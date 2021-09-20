/*
 * This file is part of txproto.
 *
 * txproto is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * txproto is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with txproto; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <unistd.h>
#include <stdbool.h>
#include <fcntl.h>

#include <libavutil/crc.h>
#include <libavutil/opt.h>
#include <libavutil/bprint.h>
#include <libavutil/random_seed.h>

#include "utils.h"
#include "os_compat.h"
#include "logging.h"

/* Set the CLOEXEC flag on the given fd.
 * On error, false is returned (and errno set). */
static bool set_cloexec(int fd)
{
    if (fd >= 0) {
        int flags = fcntl(fd, F_GETFD);
        if (flags == -1)
            return false;
        if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1)
            return false;
    }

    return true;
}

static int sp_make_cloexec_pipe(int pipes[2])
{
    if (pipe(pipes) != 0) {
        pipes[0] = pipes[1] = -1;
        return -1;
    }

    for (int i = 0; i < 2; i++)
        set_cloexec(pipes[i]);

    return 0;
}

/* create a pipe, and set it to non-blocking (and also set FD_CLOEXEC) */
int sp_make_wakeup_pipe(int pipes[2])
{
    if (sp_make_cloexec_pipe(pipes) < 0)
        return -1;

    for (int i = 0; i < 2; i++) {
        int val = fcntl(pipes[i], F_GETFL) | O_NONBLOCK;
        fcntl(pipes[i], F_SETFL, val);
    }
    return 0;
}

void sp_write_wakeup_pipe(int pipes[2], int64_t val)
{
    if ((pipes[0] == -1) || (pipes[1] == -1))
        return;

    (void)write(pipes[1], &val, sizeof(val));
}

int64_t sp_flush_wakeup_pipe(int pipes[2])
{
    sp_assert((pipes[0] >= 0) && (pipes[1] >= 0));
    int64_t res = INT64_MIN;
    (void)read(pipes[0], &res, sizeof(res));
    return res;
}

void sp_close_wakeup_pipe(int pipes[2])
{
    for (int i = 0; i < 2; i++) {
        if (pipes[i] >= 0)
            close(pipes[i]);
    }
}

int64_t sp_sliding_win(SlidingWinCtx *ctx, int64_t num, int64_t pts,
                       AVRational tb, int64_t len, int do_avg)
{
    struct SPSlidingWinEntry *top, *last;
    int64_t sum = 0;

    if (pts == INT64_MIN)
        goto calc;

    if (!ctx->num_entries)
        goto add;

    for (int i = 0; i < ctx->num_entries; i++) {
        last = &ctx->entries[i];
        int64_t test = av_add_stable(last->tb, last->pts, tb, len);

        if (((ctx->num_entries + 1) > MAX_ROLLING_WIN_ENTRIES) ||
            av_compare_ts(test, last->tb, pts, tb) < 0) {
            ctx->num_entries--;
            memmove(last, last + 1, sizeof(*last) * ctx->num_entries);
        }
    }

add:
    top = &ctx->entries[ctx->num_entries++];
    top->num = num;
    top->pts = pts;
    top->tb  = tb;

calc:
    /* Calculate the average */
    for (int i = 0; i < ctx->num_entries; i++)
        sum += ctx->entries[i].num;

    if (do_avg && ctx->num_entries)
        sum /= ctx->num_entries;

    return sum;
}

// If the name starts with "@", try to interpret it as a number, and set *name
// to the name of the n-th parameter.
static void resolve_positional_arg(void *avobj, char **name)
{
    if (!*name || (*name)[0] != '@' || !avobj)
        return;

    char *end = NULL;
    int pos = strtol(*name + 1, &end, 10);
    if (!end || *end)
        return;

    const AVOption *opt = NULL;
    int offset = -1;
    while (1) {
        opt = av_opt_next(avobj, opt);
        if (!opt)
            return;
        // This is what libavfilter's parser does to skip aliases.
        if (opt->offset != offset && opt->type != AV_OPT_TYPE_CONST)
            pos--;
        if (pos < 0) {
            *name = (char *)opt->name;
            return;
        }
        offset = opt->offset;
    }
}

// Like mp_set_avopts(), but the posargs argument is used to resolve positional
// arguments. If posargs==NULL, positional args are disabled.
int sp_set_avopts_pos(void *log, void *avobj, void *posargs, AVDictionary *dict)
{
    int success = 0;
    const AVDictionaryEntry *d = NULL;
    while ((d = av_dict_get(dict, "", d, AV_DICT_IGNORE_SUFFIX))) {
        char *k = d->key;
        char *v = d->value;
        resolve_positional_arg(posargs, &k);
        int r = av_opt_set(avobj, k, v, AV_OPT_SEARCH_CHILDREN);
        if (r == AVERROR_OPTION_NOT_FOUND) {
            sp_log(log, SP_LOG_ERROR, "AVOption \"%s\" not found!\n", k);
            success = -1;
        } else if (r < 0) {
            char errstr[80];
            av_strerror(r, errstr, sizeof(errstr));
            sp_log(log, SP_LOG_ERROR, "Could not set AVOption \"%s\" to \"%s\": "
                   "%s!\n", k, v, errstr);
            success = -1;
        }
    }

    return success;
}

// Set these options on given avobj (using av_opt_set..., meaning avobj must
// point to a struct that has AVClass as first member).
// Options which fail to set (error or not found) are printed to log.
// Returns: >=0 success, <0 failed to set an option
int sp_set_avopts(void *log, void *avobj, AVDictionary *dict)
{
    return sp_set_avopts_pos(log, avobj, avobj, dict);
}

struct SPBufferList {
    pthread_mutex_t lock;
    AVBufferRef **entries;
    int entries_num;
    unsigned int entries_size;

    enum SPEventType *priv_flags;
    unsigned int priv_flags_size;

    int iter_idx;

    enum SPEventType dispatched;
    enum SPEventType queued;
};

SPBufferList *sp_bufferlist_new(void)
{
    SPBufferList *list = av_mallocz(sizeof(*list));

    pthread_mutexattr_t lock_attr;
    pthread_mutexattr_init(&lock_attr);
    pthread_mutexattr_settype(&lock_attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&list->lock, &lock_attr);

    list->iter_idx = -1;
    return list;
}

int sp_bufferlist_len(SPBufferList *list)
{
    pthread_mutex_lock(&list->lock);
    int len = list->entries_num;
    pthread_mutex_unlock(&list->lock);
    return len;
}

AVBufferRef *sp_bufferlist_find_fn_first(AVBufferRef *entry, void *opaque)
{
    return entry;
}

AVBufferRef *sp_bufferlist_find_fn_data(AVBufferRef *entry, void *opaque)
{
    AVBufferRef *needle = opaque;
    return needle->data == entry->data ? entry : NULL;
}

#define SP_BUF_PRIV_PRIORITY ((SP_EVENT_TYPE_MASK) | (SP_EVENT_CTRL_MASK))
#define SP_BUF_PRIV_NEW      (1ULL << 48)
#define SP_BUF_PRIV_DEP      (1ULL << 49)
#define SP_BUF_PRIV_RUNNING  (1ULL << 50)
#define SP_BUF_PRIV_ON_MASK  (SP_EVENT_ON_MASK)

static int internal_bufferlist_append(SPBufferList *list, AVBufferRef *entry,
                                      enum SPEventType pflags, int ref)
{
    int err = 0;
    if (!list)
        return 0;

    pthread_mutex_lock(&list->lock);

    if (ref) {
        entry = av_buffer_ref(entry);
        if (!entry)
            return AVERROR(ENOMEM);
    }

    AVBufferRef **new_entries = av_fast_realloc(list->entries, &list->entries_size,
                                                sizeof(*new_entries) * (list->entries_num + 1));
    if (!new_entries) {
        err = AVERROR(ENOMEM);
        if (ref)
            av_buffer_unref(&entry);
        goto end;
    }

    enum SPEventType *priv_flags = av_fast_realloc(list->priv_flags, &list->priv_flags_size,
                                                   sizeof(*priv_flags) * (list->entries_num + 1));
    if (!priv_flags) {
        err = AVERROR(ENOMEM);
        list->entries = new_entries;
        if (ref)
            av_buffer_unref(&entry);
        goto end;
    }

    enum SPEventType prio = pflags & SP_BUF_PRIV_PRIORITY;

    int i;
    if (prio == SP_BUF_PRIV_PRIORITY) {
        i = list->entries_num;
    } else {
        for (i = 0; i < list->entries_num; i++)
            if (prio > (priv_flags[i] & SP_BUF_PRIV_PRIORITY))
                break;
    }

    memmove(&new_entries[i + 1], &new_entries[i], (list->entries_num - i)*sizeof(*new_entries));
    memmove(&priv_flags[i + 1], &priv_flags[i], (list->entries_num - i)*sizeof(*priv_flags));

    priv_flags[i] = pflags;
    new_entries[i] = entry;
    list->entries_num++;
    list->entries = new_entries;
    list->priv_flags = priv_flags;

end:
    pthread_mutex_unlock(&list->lock);
    return err;
}

int sp_bufferlist_append(SPBufferList *list, AVBufferRef *entry)
{
    return internal_bufferlist_append(list, entry, SP_BUF_PRIV_NEW | SP_BUF_PRIV_PRIORITY, 1);
}

int sp_bufferlist_append_noref(SPBufferList *list, AVBufferRef *entry)
{
    return internal_bufferlist_append(list, entry, SP_BUF_PRIV_NEW | SP_BUF_PRIV_PRIORITY, 0);
}

AVBufferRef *sp_bufferlist_ref(SPBufferList *list, sp_buflist_find_fn find, void *find_opaque)
{
    int i = 0;
    AVBufferRef *sel = NULL;
    pthread_mutex_lock(&list->lock);
    for (i = 0; i < list->entries_num; i++)
        if ((sel = find(list->entries[i], find_opaque)))
            break;
    if (!sel)
        goto end;

    sel = av_buffer_ref(sel);

end:
    pthread_mutex_unlock(&list->lock);

    return sel;
}

static inline void buflist_remove_idx(SPBufferList *list, int index)
{
    list->entries_num--;
    memmove(&list->entries[index], &list->entries[index + 1],
            (list->entries_num - index) * sizeof(*list->entries));
    memmove(&list->priv_flags[index], &list->priv_flags[index + 1],
            (list->entries_num - index) * sizeof(*list->priv_flags));

    list->entries = av_fast_realloc(list->entries, &list->entries_size,
                                    list->entries_num * sizeof(*list->entries));
    list->priv_flags = av_fast_realloc(list->priv_flags, &list->priv_flags_size,
                                       list->entries_num * sizeof(*list->priv_flags));
}

AVBufferRef *sp_bufferlist_pop(SPBufferList *list, sp_buflist_find_fn find, void *find_opaque)
{
    int i = 0;
    AVBufferRef *sel = NULL;
    pthread_mutex_lock(&list->lock);
    for (i = 0; i < list->entries_num; i++)
        if ((sel = find(list->entries[i], find_opaque)))
            break;
    if (!sel)
        goto end;

    buflist_remove_idx(list, i);

end:
    pthread_mutex_unlock(&list->lock);

    return sel;
}

AVBufferRef *sp_bufferlist_iter_ref(SPBufferList *list)
{
    AVBufferRef *ref = NULL;

    /* Recursive lock */
    pthread_mutex_lock(&list->lock);

    if (list->iter_idx < 0)
        pthread_mutex_lock(&list->lock);

    list->iter_idx++;

    if (list->iter_idx >= list->entries_num) {
        list->iter_idx = -1;
        pthread_mutex_unlock(&list->lock);
        goto end;
    }

    ref = av_buffer_ref(list->entries[list->iter_idx]);

end:
    pthread_mutex_unlock(&list->lock);
    return ref;
}

void sp_bufferlist_iter_halt(SPBufferList *list)
{
    pthread_mutex_lock(&list->lock);

    if (list->iter_idx >= 0) {
        list->iter_idx = -1;
        pthread_mutex_unlock(&list->lock);
    }

    pthread_mutex_unlock(&list->lock);
}

void sp_bufferlist_free(SPBufferList **s)
{
    if (!s || !(*s))
        return;

    SPBufferList *list = *s;

    pthread_mutex_lock(&list->lock);

    for (int i = 0; i < list->entries_num; i++)
        av_buffer_unref(&list->entries[i]);

    av_freep(&list->entries);
    av_freep(&list->priv_flags);

    pthread_mutex_unlock(&list->lock);
    pthread_mutex_destroy(&list->lock);

    av_freep(s);
}

int sp_bufferlist_copy(SPBufferList *dst, SPBufferList *src)
{
    int err = 0;
    pthread_mutex_lock(&src->lock);

    for (int i = 0; i < src->entries_num; i++) {
        AVBufferRef *cloned = av_buffer_ref(src->entries[i]);
        if (!cloned)
            goto end;
        err = sp_bufferlist_append(dst, cloned);
        if (err < 0) {
            av_buffer_unref(&cloned);
            goto end;
        }
    }

end:
    if (err < 0)
        sp_bufferlist_free(&dst);

    pthread_mutex_unlock(&src->lock);
    return err;
}

struct SPEvent {
    enum SPEventType type;
    pthread_mutex_t *lock;
    pthread_cond_t cond;
    uint32_t identifier;
    int external_lock;
    int (*fn)(AVBufferRef *opaque, void *src_ctx, void *data);
};

static void destroy_event(void *opaque, uint8_t *data)
{
    SPEvent *event = (SPEvent *)data;
    AVBufferRef *opaque_ref = opaque;
    pthread_mutex_lock(event->lock);
    int external_lock = event->external_lock;
    pthread_mutex_unlock(event->lock);
    if (!external_lock) {
        pthread_mutex_destroy(event->lock);
        av_free(event->lock);
    }
    av_free(event);
    av_buffer_unref(&opaque_ref);
}

AVBufferRef *sp_event_create(int (*fn)(AVBufferRef *opaque, void *src_ctx, void *data),
                             pthread_mutex_t *lock, enum SPEventType type,
                             AVBufferRef *opaque, uint32_t identifier)
{
    SPEvent *event = av_mallocz(sizeof(SPEvent));
    if (!event)
        return NULL;

    event->fn   = fn;
    event->type = type;
    event->lock = lock;
    event->identifier = identifier;
    event->external_lock = !!lock;

    pthread_cond_init(&event->cond, NULL);

    if (!event->external_lock) {
        event->lock = av_mallocz(sizeof(pthread_mutex_t));
        if (!event->lock) {
            av_free(event);
            return NULL;
        }
        pthread_mutex_init(event->lock, NULL);
    }

    AVBufferRef *entry = av_buffer_create((uint8_t *)event, sizeof(SPEvent),
                                          destroy_event, opaque, 0);
    if (!entry) {
        if (!event->external_lock) {
            pthread_mutex_destroy(event->lock);
            av_free(event->lock);
        }
        av_free(event);
        return NULL;
    }

    return entry;
}

void sp_event_unref_expire(AVBufferRef **buf)
{
    if (!buf || !*buf)
        return;

    SPEvent *event = (SPEvent *)(*buf)->data;
    pthread_mutex_lock(event->lock);
    event->type |= SP_EVENT_FLAG_EXPIRED;
    pthread_mutex_unlock(event->lock);

    av_buffer_unref(buf);
}

void sp_event_unref_await(AVBufferRef **buf)
{
    if (!buf || !*buf)
        return;

    SPEvent *event = (SPEvent *)(*buf)->data;
    pthread_mutex_lock(event->lock);
    if (event->type & SP_EVENT_FLAG_EXPIRED)
        goto end;

    pthread_cond_wait(&event->cond, event->lock);

end:
    pthread_mutex_unlock(event->lock);

    av_buffer_unref(buf);
}

static inline AVBufferRef *find_event_identifier(AVBufferRef *entry, void *opaque)
{
    SPEvent *event = (SPEvent *)entry->data;
    if (event->identifier == *((uint32_t *)opaque))
        return entry;

    return NULL;
}

/* 31 bits for priority, must be unsigned, 0 means highest priority */
static int get_event_priority(void *src_ctx, AVBufferRef *event)
{
    enum SPType ctype = sp_class_get_type(src_ctx);
    SPEvent *event_ctx = (SPEvent *)event->data;
    enum SPEventType f = event_ctx->type;

    int prio = 128;
    if (ctype & SP_TYPE_CLOCK_SOURCE)
        prio += 128 * 1;
    if (ctype & SP_TYPE_VIDEO_SOURCE ||
        ctype & SP_TYPE_AUDIO_SOURCE ||
        ctype & SP_TYPE_SUB_SOURCE)
        prio += 128 * 2;
    if (ctype & SP_TYPE_FILTER)
        prio += 128 * 3;
    if (ctype & SP_TYPE_ENCODER)
        prio += 128 * 4;
    if (ctype & SP_TYPE_BSF)
        prio += 128 * 5;
    if (ctype & SP_TYPE_MUXER)
        prio += 128 * 6;
    if (ctype & SP_TYPE_DEMUXER)
        prio += 128 * 7;
    if (ctype & SP_TYPE_DECODER)
        prio += 128 * 8;
    if (ctype & SP_TYPE_VIDEO_SINK ||
        ctype & SP_TYPE_AUDIO_SINK ||
        ctype & SP_TYPE_SUB_SINK)
        prio += 128 * 9;
    if (ctype & SP_TYPE_CONTEXT)
        prio += 128 * 10;
    if (ctype & SP_TYPE_INTERFACE)
        prio += 128 * 11;
    if (ctype & SP_TYPE_SCRIPT)
        prio += 128 * 12;

    if (f & SP_EVENT_CTRL_STOP)
        prio += 1;

    if (f & SP_EVENT_TYPE_LINK)
        prio += 2;

    if (f & SP_EVENT_CTRL_OPTS)
        prio += 3;

    if (f & SP_EVENT_CTRL_START)
        prio += 8;

    if (f & SP_EVENT_CTRL_COMMAND)
        prio += 16;

    prio += f & SP_EVENT_FLAG_HIGH_PRIO ? -1 :
            f & SP_EVENT_FLAG_LOW_PRIO  ? +1 : 0;

    return ((prio) & 31);
}

static int eventlist_add_internal(void *src_ctx, SPBufferList *list,
                                  AVBufferRef *event, enum SPEventType when)
{
    SPEvent *event_ctx = (SPEvent *)event->data;
    if (!list)
        return 0;

    AVBufferRef *dup = NULL;
    if (!(event_ctx->type & SP_EVENT_FLAG_NO_DEDUP))
        dup = sp_bufferlist_pop(list, find_event_identifier, &event_ctx->identifier);

    if (dup) {
        char *fstr = sp_event_flags_to_str(when ? when : event_ctx->type);
        sp_log(src_ctx, SP_LOG_DEBUG, "Deduplicating %s (%s id: 0x%x)!\n",
               when ? "dependency" : "event", fstr, event_ctx->identifier);
        av_free(fstr);
        av_buffer_unref(&dup);
    }

    int priority = get_event_priority(src_ctx, event);
    enum SPEventType pflags = priority << 16;
    if (!(event_ctx->type & SP_EVENT_FLAG_IMMEDIATE))
        pflags |= SP_BUF_PRIV_NEW;
    if (when) {
        pflags |= SP_BUF_PRIV_DEP;
        pflags |= when & SP_BUF_PRIV_ON_MASK;
    }

    int ret = internal_bufferlist_append(list, event, pflags, 1);
    if (ret < 0)
        return ret;

    list->queued |= event_ctx->type;

    return priority;
}

int sp_eventlist_add(void *src_ctx, SPBufferList *list, AVBufferRef *event)
{
    return eventlist_add_internal(src_ctx, list, event, 0x0);
}

int sp_eventlist_add_with_dep(void *src_ctx, SPBufferList *list,
                              AVBufferRef *event, enum SPEventType when)
{
    return eventlist_add_internal(src_ctx, list, event, when);
}

#define MASK_ERR_DESTROY (SP_EVENT_ON_DESTROY | SP_EVENT_ON_ERROR)

int sp_eventlist_dispatch(void *src_ctx, SPBufferList *list, uint64_t type, void *data)
{
    int ret = 0, dispatched = 0, num_events;
    if (!list && type & SP_EVENT_ON_DESTROY)
        return 0;
    else if (!list)
        return AVERROR(EINVAL);

    pthread_mutex_lock(&list->lock);

    num_events = list->entries_num;

    list->dispatched |= type;
    list->queued &= ~type;

    for (int i = 0; i < list->entries_num; i++) {
        if (type & SP_EVENT_ON_COMMIT)
            list->priv_flags[i] &= ~SP_BUF_PRIV_NEW;
        else if (list->priv_flags[i] & SP_BUF_PRIV_NEW)
            continue;

        if ((list->priv_flags[i] & SP_BUF_PRIV_DEP) &&
            ((list->priv_flags[i] & SP_BUF_PRIV_ON_MASK) & type)) {
            SPEvent *event = (SPEvent *)list->entries[i]->data;
            pthread_mutex_lock(event->lock);

            enum SPEventType expired = event->type & SP_EVENT_FLAG_EXPIRED;
            char *fstr = sp_event_flags_to_str(event->type);
            sp_log(src_ctx, SP_LOG_DEBUG, "%s on event (%s)!\n",
                   expired ? "Skipping waiting" : "Waiting", fstr);
            av_free(fstr);

            if (!expired)
                pthread_cond_wait(&event->cond, event->lock);

            pthread_mutex_unlock(event->lock);
            av_buffer_unref(&list->entries[i]);
            buflist_remove_idx(list, i);
            i -= 1;
        }
    }

    int i;
    for (i = 0; i < list->entries_num; i++) {
        SPEvent *event = (SPEvent *)list->entries[i]->data;

        /* To prevent recursion. The list is threadsafe, and its execution is
         * threadsafe as well, however the dispatching of commands may modify
         * the list, and even dispatch events. */
        if (list->priv_flags[i] & SP_BUF_PRIV_RUNNING)
            continue;
        list->priv_flags[i] |= SP_BUF_PRIV_RUNNING;

        pthread_mutex_lock(event->lock);

        enum SPEventType destroy_now = 0;
        destroy_now |= event->type & SP_EVENT_FLAG_EXPIRED;
        destroy_now |= (type & MASK_ERR_DESTROY) && !(event->type & MASK_ERR_DESTROY);

        if (destroy_now) {
            if (event->type & SP_EVENT_FLAG_ONESHOT)
                event->type |= SP_EVENT_FLAG_EXPIRED;
            pthread_mutex_unlock(event->lock);
            av_buffer_unref(&list->entries[i]);
            buflist_remove_idx(list, i); /* No need to remove IS_RUNNING */
            i -= 1;
            continue;
        } else if (list->priv_flags[i] & SP_BUF_PRIV_NEW) {
            goto end;
        }

        enum SPEventType filter_type = type & ~(SP_EVENT_FLAG_MASK | SP_EVENT_ON_MASK);
        enum SPEventType filter_on = type & ~(SP_EVENT_FLAG_MASK | SP_EVENT_TYPE_MASK | SP_EVENT_CTRL_MASK);

        enum SPEventType run_now;
        if (event->type & SP_EVENT_FLAG_IMMEDIATE)
            run_now = 1;
        else if (filter_type && filter_on)
            run_now = (!!(event->type & filter_type)) && (!!(event->type & filter_on));
        else if (filter_on)
            run_now = !!(event->type & filter_on);
        else if (filter_type)
            run_now = !!(event->type & filter_type);
        else
            run_now = 0;

        if (!run_now)
            goto end;

        dispatched++;
        destroy_now = 0;
        destroy_now |= type & SP_EVENT_FLAG_ONESHOT;
        destroy_now |= type & MASK_ERR_DESTROY;

        char *fstr = sp_event_flags_to_str(event->type);
        sp_log(src_ctx, SP_LOG_DEBUG, "Dispatching event (%s)%s!\n", fstr, destroy_now ? ", destroying" : "");
        av_free(fstr);

        destroy_now |= event->type & SP_EVENT_FLAG_ONESHOT;

        ret = event->fn((AVBufferRef *)av_buffer_get_opaque(list->entries[i]), src_ctx, data);
        if (event->type & SP_EVENT_FLAG_ONESHOT)
            event->type |= SP_EVENT_FLAG_EXPIRED;

        pthread_cond_broadcast(&event->cond);

        if (destroy_now) {
            pthread_mutex_unlock(event->lock);

            /* The list may have been modified when running events within. */
            AVBufferRef *ev_old = sp_bufferlist_pop(list_orig, sp_bufferlist_find_fn_data,
                                                    list->entries[i]);
            av_buffer_unref(&ev_old);

            continue;
        }

end:
        pthread_mutex_unlock(event->lock);
        list->priv_flags[i] &= ~SP_BUF_PRIV_RUNNING;
    }

    pthread_mutex_unlock(&list->lock);

    char *fstr = sp_event_flags_to_str(type);
    sp_log(src_ctx, !dispatched ? SP_LOG_TRACE : SP_LOG_DEBUG,
           "Dispatched %i/%i requested events (%s)!\n",
           dispatched, num_events, fstr);
    av_free(fstr);

    return ret;
}

enum SPEventType sp_eventlist_has_dispatched(SPBufferList *list, enum SPEventType type)
{
    enum SPEventType ret;
    pthread_mutex_lock(&list->lock);
    ret = list->dispatched & type;
    pthread_mutex_unlock(&list->lock);
    return ret;
}

enum SPEventType sp_eventlist_has_queued(SPBufferList *list, enum SPEventType type)
{
    enum SPEventType ret;
    pthread_mutex_lock(&list->lock);
    ret = list->queued & type;
    pthread_mutex_unlock(&list->lock);
    return ret;
}

void sp_eventlist_discard(SPBufferList *list)
{
    pthread_mutex_lock(&list->lock);

    for (int i = 0; i < list->entries_num; i++) {
        if (list->priv_flags[i] & SP_BUF_PRIV_NEW) {
            av_buffer_unref(&list->entries[i]);
            buflist_remove_idx(list, i);
            i -= 1;
        }
    }

    pthread_mutex_unlock(&list->lock);
}

uint32_t sp_event_gen_identifier(void *src, void *dst, uint64_t type)
{
    uint64_t tmp;
    uint32_t id = 0x0;
    const AVCRC *tab = av_crc_get_table(AV_CRC_32_IEEE);

    /* Never deduplicate any plain events */
    uint64_t no_dedup = 0x0;
    no_dedup |= type & SP_EVENT_FLAG_NO_DEDUP;
    no_dedup |= (type & SP_EVENT_ON_MASK) && !(type & (SP_EVENT_CTRL_MASK | SP_EVENT_TYPE_MASK));

    if (no_dedup) {
        tmp = av_get_random_seed();
        id = av_crc(tab, id, (uint8_t *)&tmp, sizeof(tmp));
    } else {
        type &= ~SP_EVENT_FLAG_MASK; /* Flags don't matter */
        id = av_crc(tab, id, (uint8_t *)&type, sizeof(type));
    }

    if (src) {
        tmp = sp_class_get_id(src);
        id = av_crc(tab, id, (uint8_t *)&tmp, sizeof(tmp));
    }

    if (dst) {
        tmp = sp_class_get_id(dst);
        id = av_crc(tab, id, (uint8_t *)&tmp, sizeof(tmp));
    }

    return id;
}

char *sp_event_flags_to_str(uint64_t flags)
{
    AVBPrint bp;
    av_bprint_init(&bp, 32, AV_BPRINT_SIZE_UNLIMITED);

    int on_prefix   = 0;
    int type_prefix = 0;
    int ctrl_prefix = 0;
    int flag_prefix = 0;

#define COND(flag, type, str)                                              \
    if (flag & flags) {                                                    \
        flags &= ~flag;                                                    \
        int sflags = on_prefix + type_prefix + ctrl_prefix + flag_prefix;  \
        av_bprintf(&bp, "%s%s%s%s",                                        \
                   type## _prefix ? "+" : !sflags ? "" : " ",              \
                   type## _prefix ? "" : #type,                            \
                   type## _prefix ? "" : ":", str);                        \
        on_prefix = type_prefix = ctrl_prefix = flag_prefix = 0;           \
        type## _prefix = 1;                                                \
    }

    COND(SP_EVENT_ON_COMMIT,         on, "commit")
    COND(SP_EVENT_ON_CONFIG,         on, "config")
    COND(SP_EVENT_ON_INIT,           on, "init")
    COND(SP_EVENT_ON_CHANGE,         on, "change")
    COND(SP_EVENT_ON_STATS,          on, "stats")
    COND(SP_EVENT_ON_DESTROY,        on, "destroy")
    COND(SP_EVENT_ON_EOS,            on, "eos")
    COND(SP_EVENT_ON_ERROR,          on, "error")
    COND(SP_EVENT_ON_OUTPUT,         on, "output")

    COND(SP_EVENT_TYPE_LINK,       type, "link")
    COND(SP_EVENT_TYPE_SOURCE,     type, "source")
    COND(SP_EVENT_TYPE_FILTER,     type, "filter")
    COND(SP_EVENT_TYPE_BSF,        type, "bsf")
    COND(SP_EVENT_TYPE_ENCODER,    type, "encoder")
    COND(SP_EVENT_TYPE_MUXER,      type, "muxer")
    COND(SP_EVENT_TYPE_DEMUXER,    type, "demuxer")
    COND(SP_EVENT_TYPE_DECODER,    type, "decoder")
    COND(SP_EVENT_TYPE_SINK,       type, "sink")

    COND(SP_EVENT_CTRL_START,      ctrl, "start")
    COND(SP_EVENT_CTRL_STOP,       ctrl, "stop")
    COND(SP_EVENT_CTRL_COMMAND,    ctrl, "command")
    COND(SP_EVENT_CTRL_NEW_EVENT,  ctrl, "new_event")
    COND(SP_EVENT_CTRL_DEL_EVENT,  ctrl, "del_event")
    COND(SP_EVENT_CTRL_OPTS,       ctrl, "opts")
    COND(SP_EVENT_CTRL_COMMIT,     ctrl, "commit")
    COND(SP_EVENT_CTRL_DISCARD,    ctrl, "discard")
    COND(SP_EVENT_CTRL_DEP,        ctrl, "dependency")
    COND(SP_EVENT_CTRL_FLUSH,      ctrl, "flush")

    COND(SP_EVENT_FLAG_NO_REORDER, flag, "no_reorder")
    COND(SP_EVENT_FLAG_NO_DEDUP,   flag, "no_dedup")
    COND(SP_EVENT_FLAG_HIGH_PRIO,  flag, "high_prio")
    COND(SP_EVENT_FLAG_LOW_PRIO,   flag, "low_prio")
    COND(SP_EVENT_FLAG_IMMEDIATE,  flag, "immediate")
    COND(SP_EVENT_FLAG_EXPIRED,    flag, "expired")
    COND(SP_EVENT_FLAG_ONESHOT,    flag, "oneshot")

    if (flags)
        av_bprintf(&bp, "UNKNOWN(0x%lx)!", flags);

#undef COND

    char *buf;
    av_bprint_finalize(&bp, &buf);
    return buf;
}

char *sp_event_flags_to_str_buf(AVBufferRef *event)
{
    return sp_event_flags_to_str(((SPEvent *)event->data)->type);
}

enum SPEventType sp_class_to_event_type(void *ctx)
{
    switch (sp_class_get_type(ctx)) {
    case SP_TYPE_VIDEO_SOURCE:
    case SP_TYPE_AUDIO_SOURCE:
    case SP_TYPE_SUB_SOURCE:
    case SP_TYPE_CLOCK_SOURCE:
        return SP_EVENT_TYPE_SOURCE;

    case SP_TYPE_VIDEO_SINK:
    case SP_TYPE_AUDIO_SINK:
    case SP_TYPE_SUB_SINK:
    case SP_TYPE_CLOCK_SINK:
        return SP_EVENT_TYPE_SINK;

    case SP_TYPE_VIDEO_BIDIR:
    case SP_TYPE_AUDIO_BIDIR:
    case SP_TYPE_SUB_BIDIR:
        return SP_EVENT_TYPE_SINK | SP_EVENT_TYPE_SOURCE;

    case SP_TYPE_FILTER:
        return SP_EVENT_TYPE_FILTER;

    case SP_TYPE_BSF:
        return SP_EVENT_TYPE_BSF;

    case SP_TYPE_ENCODER:
        return SP_EVENT_TYPE_ENCODER;
    case SP_TYPE_DECODER:
        return SP_EVENT_TYPE_DECODER;

    case SP_TYPE_MUXER:
        return SP_EVENT_TYPE_MUXER;
    case SP_TYPE_DEMUXER:
        return SP_EVENT_TYPE_DEMUXER;


    default:
        return 0x0;
    }
}
