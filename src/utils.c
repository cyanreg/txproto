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

#include <pthread.h>
#include <stdatomic.h>

#include <libavutil/crc.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libavutil/bprint.h>
#include <libavutil/random_seed.h>

#include <libtxproto/utils.h>
#include "os_compat.h"
#include <libtxproto/log.h>

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

    SPEventType *priv_flags;
    unsigned int priv_flags_size;

    int iter_idx;

    SPEventType dispatched;
    SPEventType queued;
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

#define SP_BUF_PRIV_NEW      (1ULL << 48)
#define SP_BUF_PRIV_SIGNAL   (1ULL << 49)
#define SP_BUF_PRIV_RUNNING  (1ULL << 50)
#define SP_BUF_PRIV_ON_MASK  (SP_EVENT_ON_MASK)

static int internal_bufferlist_append(SPBufferList *list, AVBufferRef *entry,
                                      SPEventType pflags, int ref,
                                      int position)
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

    SPEventType *priv_flags = av_fast_realloc(list->priv_flags, &list->priv_flags_size,
                                                   sizeof(*priv_flags) * (list->entries_num + 1));
    if (!priv_flags) {
        err = AVERROR(ENOMEM);
        list->entries = new_entries;
        if (ref)
            av_buffer_unref(&entry);
        goto end;
    }

    int i;
    if (position > list->entries_num) {
        i = list->entries_num;
    } else {
        i = position;
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
    return internal_bufferlist_append(list, entry, SP_BUF_PRIV_NEW, 1, INT_MAX);
}

int sp_bufferlist_append_noref(SPBufferList *list, AVBufferRef *entry)
{
    return internal_bufferlist_append(list, entry, SP_BUF_PRIV_NEW, 0, INT_MAX);
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

static AVBufferRef *bufferlist_pop_internal(SPBufferList *list,
                                            sp_buflist_find_fn find,
                                            void *find_opaque, int *idx)
{
    int i = 0;
    AVBufferRef *sel = NULL;
    pthread_mutex_lock(&list->lock);
    for (i = 0; i < list->entries_num; i++)
        if ((sel = find(list->entries[i], find_opaque)))
            break;
    if (!sel) {
        *idx = -1;
    } else {
        *idx = i;
        buflist_remove_idx(list, i);
    }

    pthread_mutex_unlock(&list->lock);

    return sel;
}

AVBufferRef *sp_bufferlist_pop(SPBufferList *list,
                               sp_buflist_find_fn find,
                               void *find_opaque)
{
    int tmp;
    return bufferlist_pop_internal(list, find, find_opaque, &tmp);
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

    /* TODO: check for duplicates on event lists */

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
    /* Event identification */
    uint64_t id;
    SPEventType type;
    void *ctx;
    void *dep_ctx;

    /* Event dependency */
    pthread_cond_t cond;
    int dep_done;

    /* Event parameters */
    event_fn cb;
    event_free destroy_cb;
    pthread_mutex_t *lock;
    int external_lock;
};

static void destroy_event(void *opaque, uint8_t *data)
{
    SPEvent *event = (SPEvent *)data;

    if (event->destroy_cb)
        event->destroy_cb(opaque, event->ctx, event->dep_ctx);

    if (!event->external_lock) {
        pthread_mutex_destroy(event->lock);
        av_free(event->lock);
    }

    av_free(event);
    av_free(opaque);
}

atomic_uint_fast64_t global_event_counter = ATOMIC_VAR_INIT(0);

AVBufferRef *sp_event_create(event_fn cb,
                             event_free destroy_cb,
                             size_t callback_ctx_size,
                             pthread_mutex_t *lock,
                             SPEventType type,
                             void *ctx,
                             void *dep_ctx)
{
    /* TODO: Cache the allocations - as a starter, bypass all if the
     * callback_ctx_size is below a threshold and use a preallocated event */
    SPEvent *event = av_mallocz(sizeof(SPEvent));
    if (!event)
        return NULL;

    event->cb            = cb;
    event->destroy_cb    = destroy_cb;
    event->type          = type;
    event->dep_ctx       = dep_ctx;
    event->ctx           = ctx;
    event->lock          = lock;
    event->external_lock = !!lock;
    event->id            = atomic_fetch_add(&global_event_counter, 1);

    /* Initialize the cond which will be used if there's a dependency */
    pthread_cond_init(&event->cond, NULL);

    if (!event->external_lock) {
        event->lock = av_mallocz(sizeof(pthread_mutex_t));
        if (!event->lock) {
            av_free(event);
            return NULL;
        }
        pthread_mutex_init(event->lock, NULL);
    }

    void *opaque = av_mallocz(callback_ctx_size);

    AVBufferRef *entry = av_buffer_create((uint8_t *)event, sizeof(SPEvent),
                                          destroy_event, opaque, 0);
    if (!entry) {
        if (!event->external_lock) {
            pthread_mutex_destroy(event->lock);
            av_free(event->lock);
        }
        av_free(event);
        av_free(opaque);
        return NULL;
    }

    char *fstr = sp_event_flags_to_str(event->type);
    sp_log(ctx, SP_LOG_VERBOSE, "Created event ID: %li (%s)\n", event->id, fstr);
    av_free(fstr);

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

    if (!event->dep_done && !(event->type & SP_EVENT_FLAG_EXPIRED))
        pthread_cond_wait(&event->cond, event->lock);

    pthread_mutex_unlock(event->lock);

    av_buffer_unref(buf);
}

static inline AVBufferRef *find_event_duplicate(AVBufferRef *entry, void *opaque)
{
    SPEvent *ref_ctx = (SPEvent *)entry->data;
    SPEvent *cmp_ctx = (SPEvent *)opaque;

    SPEventType mask;
    SPEventType event_ref_type = ref_ctx->type;
    SPEventType event_cmp_type = cmp_ctx->type;

    /* First, mark matching flags */
    mask  = event_ref_type & event_cmp_type;

    /* Filter out those we're not interested in */
    mask &= SP_EVENT_CTRL_MASK | SP_EVENT_TYPE_LINK;

    /* Then, exclude them if either has a UNIQUE flag. */
    mask &= ~((event_ref_type | event_cmp_type) & SP_EVENT_FLAG_UNIQUE);

    if (mask &&
        (ref_ctx->ctx == cmp_ctx->ctx) &&
        (ref_ctx->dep_ctx == cmp_ctx->dep_ctx))
        return entry;

    return NULL;
}

static int eventlist_add_internal(void *ctx, SPBufferList *list,
                                  AVBufferRef *event, int ref, SPEventType when)
{
    SPEvent *event_ctx = (SPEvent *)event->data;
    if (!list)
        return 0;

    AVBufferRef *dup = sp_bufferlist_pop(list, find_event_duplicate, event_ctx);

    if (dup) {
        char *fstr = sp_event_flags_to_str(when ? when : event_ctx->type);
        const char *e_ctx_name = sp_class_get_name(event_ctx->ctx);
        const char *e_dep_ctx_name = sp_class_get_name(event_ctx->dep_ctx);
        sp_log(ctx, SP_LOG_DEBUG, "Deduplicating %s (id:%lu %s, contexts: %s%s%s)!\n",
               when ? "dependency" : "event", event_ctx->id, fstr,
               e_ctx_name,
               e_ctx_name && e_dep_ctx_name ? "," : "",
               e_dep_ctx_name);
        av_free(fstr);
        av_buffer_unref(&dup);
    }

    SPEventType flags = when ? (when | SP_BUF_PRIV_SIGNAL) : 0x0;
    if (!(event_ctx->type & SP_EVENT_FLAG_IMMEDIATE))
        flags |= SP_BUF_PRIV_NEW;

    int ret = internal_bufferlist_append(list, event, flags, ref, INT_MAX);
    if (ret < 0)
        return ret;

    list->queued |= event_ctx->type;

    return 0;
}

int sp_eventlist_add(void *ctx, SPBufferList *list, AVBufferRef *event, int ref)
{
    return eventlist_add_internal(ctx, list, event, ref, 0x0);
}

int sp_eventlist_add_signal(void *ctx, SPBufferList *list,
                            AVBufferRef *event, SPEventType when, int ref)
{
    return eventlist_add_internal(ctx, list, event, ref,
                                  when & (SP_EVENT_ON_MASK | SP_EVENT_FLAG_DEPENDENCY));
}

#define MASK_ERR_DESTROY (SP_EVENT_ON_DESTROY | SP_EVENT_ON_ERROR)

int sp_eventlist_dispatch(void *ctx, SPBufferList *list, SPEventType type, void *data)
{
    int ret = 0, dispatched = 0, num_events;
    if (!list && type & SP_EVENT_ON_DESTROY)
        return 0;
    else if (!list)
        return AVERROR(EINVAL);

    pthread_mutex_lock(&list->lock);
    num_events = list->entries_num;

    if (sp_log_get_ctx_lvl(sp_class_get_name(ctx)) >= SP_LOG_TRACE) {
        char *fstrs = sp_event_flags_to_str(type);
        enum SPLogLevel lvl = num_events ? SP_LOG_LIST : 0;

        sp_log(ctx, SP_LOG_DEBUG | lvl, "Dispatching (%s), list contains %i events\n",
               fstrs, num_events);

        av_free(fstrs);

        for (int i = 0; i < num_events; i++) {
            SPEvent *event = (SPEvent *)list->entries[i]->data;
            char *fstr = sp_event_flags_to_str(event->type);
            lvl = (i == (num_events - 1)) ? SP_LOG_LIST_END : SP_LOG_LIST;
            sp_log(ctx, SP_LOG_DEBUG | lvl, "id:%lu %s%s%s\n", event->id, fstr,
                   (list->priv_flags[i] & SP_BUF_PRIV_SIGNAL) ? " | signalling" : "",
                   (list->priv_flags[i] & SP_BUF_PRIV_RUNNING) ? " | running" : "");
            av_free(fstr);
        }
    }

    list->dispatched |= type;
    list->queued &= ~type;

    for (int i = 0; i < list->entries_num; i++) {
        /* TODO: fix potential race */
        SPEvent *event = (SPEvent *)list->entries[i]->data;

        if (type & SP_EVENT_ON_COMMIT) {
            if ((list->priv_flags[i] & SP_BUF_PRIV_NEW) &&
                (event->type & SP_EVENT_ON_DISCARD)) {
                av_buffer_unref(&list->entries[i]);
                buflist_remove_idx(list, i);
                i -= 1;
                continue;
            } else {
                list->priv_flags[i] &= ~SP_BUF_PRIV_NEW;
            }
        } else if (type & SP_EVENT_ON_DISCARD) {
            if ((list->priv_flags[i] & SP_BUF_PRIV_NEW) &&
                !(event->type & SP_EVENT_ON_DISCARD)) {
                av_buffer_unref(&list->entries[i]);
                buflist_remove_idx(list, i);
                i -= 1;
                continue;
            } else {
                list->priv_flags[i] &= ~SP_BUF_PRIV_NEW;
            }
        }

        if ((event->type & SP_EVENT_FLAG_DEPENDENCY) &&
            (list->priv_flags[i] & SP_BUF_PRIV_SIGNAL) &&
            ((list->priv_flags[i] & SP_BUF_PRIV_ON_MASK) & type)) {

            SPEventType expired = event->type & SP_EVENT_FLAG_EXPIRED;

            char *fstr = sp_event_flags_to_str(event->type);
            sp_log(ctx, SP_LOG_DEBUG, "%s event (id:%lu %s)!\n",
                   expired ? "Expired, not signalling" : "Signalling",
                   event->id, fstr);
            av_free(fstr);

            if (!expired) {
                event->dep_done = 1;
                pthread_cond_broadcast(&event->cond);
            }

            av_buffer_unref(&list->entries[i]);
            buflist_remove_idx(list, i);
            i -= 1;
        }
    }

    for (int i = 0; i < list->entries_num; i++) {
        AVBufferRef *event_ref = list->entries[i];
        SPEvent *event = (SPEvent *)event_ref->data;

        if ((list->priv_flags[i] & SP_BUF_PRIV_SIGNAL) &&
            (event->type & SP_EVENT_FLAG_DEPENDENCY))
            continue;

        /* To prevent recursion. The list is threadsafe, and its execution is
         * threadsafe as well, however the dispatching of commands may modify
         * the list, and even dispatch events. */
        if (list->priv_flags[i] & SP_BUF_PRIV_RUNNING)
            continue;

        list->priv_flags[i] |= SP_BUF_PRIV_RUNNING;

        pthread_mutex_lock(event->lock);

        SPEventType destroy_now = 0;
        destroy_now |= event->type & SP_EVENT_FLAG_EXPIRED;
        destroy_now |= (type & MASK_ERR_DESTROY) && !(event->type & MASK_ERR_DESTROY);

        if (destroy_now) {
            if (event->type & SP_EVENT_FLAG_DEPENDENCY)
                sp_log(ctx, SP_LOG_DEBUG, "Event with dependency (id:%lu) "
                       "already expired!\n", event->id);

            if (event->type & SP_EVENT_FLAG_ONESHOT)
                event->type |= SP_EVENT_FLAG_EXPIRED;
            pthread_mutex_unlock(event->lock);
            av_buffer_unref(&event_ref);
            buflist_remove_idx(list, i); /* No need to remove IS_RUNNING */
            i -= 1;
            continue;
        } else if (list->priv_flags[i] & SP_BUF_PRIV_NEW) {
            goto end;
        }

        SPEventType filter_type = type & ~(SP_EVENT_FLAG_MASK | SP_EVENT_ON_MASK);
        SPEventType filter_on = type & ~(SP_EVENT_FLAG_MASK | SP_EVENT_TYPE_MASK | SP_EVENT_CTRL_MASK);

        SPEventType run_now;
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
        destroy_now |= event->type & SP_EVENT_FLAG_ONESHOT;

        char *fstr = sp_event_flags_to_str(event->type);
        if (event->type & SP_EVENT_FLAG_DEPENDENCY) {
            if (!event->dep_done) {
                sp_log(ctx, SP_LOG_DEBUG, "Waiting on event (id:%lu %s)!\n",
                       event->id, fstr);

                int64_t event_wait_start = av_gettime_relative();
                pthread_cond_wait(&event->cond, event->lock);
                int64_t event_wait_done = av_gettime_relative();

                sp_log(ctx, SP_LOG_DEBUG, "Done waiting after %.2f ms, dispatching "
                       "event (id:%lu %s)%s!\n",
                       (event_wait_done - event_wait_start)/1000.0f, event->id,
                       fstr, destroy_now ? ", destroying" : "");
            } else {
                sp_log(ctx, SP_LOG_DEBUG, "Event dependency done, dispatching "
                       "event (id:%lu %s)%s!\n",
                       event->id, fstr, destroy_now ? ", destroying" : "");
            }
        } else {
            sp_log(ctx, SP_LOG_DEBUG, "Dispatching event (id:%lu %s)%s!\n",
                   event->id, fstr, destroy_now ? ", destroying" : "");
        }

        ret = event->cb(event_ref, av_buffer_get_opaque(event_ref),
                        event->ctx, event->dep_ctx ? event->dep_ctx : ctx, data);
        if (type & SP_EVENT_ON_COMMIT && ret < 0) {
            av_free(fstr);
            pthread_mutex_unlock(event->lock);
            pthread_mutex_unlock(&list->lock);

            return ret;
        }

        /* Signal any events with no dependencies right after completing them */
        if (!(event->type & SP_EVENT_FLAG_DEPENDENCY) &&
            (list->priv_flags[i] & SP_BUF_PRIV_SIGNAL)) {
            sp_log(ctx, SP_LOG_DEBUG, "Signalling non-dependant event (id:%lu %s)!\n",
                   event->id, fstr);
            pthread_cond_broadcast(&event->cond);
            event->dep_done = 1;
        }

        av_free(fstr);

        if (event->type & SP_EVENT_FLAG_ONESHOT)
            event->type |= SP_EVENT_FLAG_EXPIRED;

        if (destroy_now) {
            pthread_mutex_unlock(event->lock);

            /* The list may have been modified when running events within.
             * TODO: this may not be entirely correct. */
            AVBufferRef *ev_old = bufferlist_pop_internal(list, sp_bufferlist_find_fn_data,
                                                          event_ref, &i);
            sp_log(ctx, SP_LOG_VERBOSE, "Removed event ID: %li\n", ((SPEvent *)ev_old->data)->id);
            av_buffer_unref(&ev_old);
            i -= 1;

            continue;
        }

end:
        pthread_mutex_unlock(event->lock);
        list->priv_flags[i] &= ~SP_BUF_PRIV_RUNNING;
    }

    pthread_mutex_unlock(&list->lock);

    char *fstr = sp_event_flags_to_str(type);
    sp_log(ctx, !dispatched ? SP_LOG_TRACE : SP_LOG_DEBUG,
           "Dispatched %i/%i requested events (%s)!\n",
           dispatched, num_events, fstr);
    av_free(fstr);

    return ret;
}

SPEventType sp_eventlist_has_dispatched(SPBufferList *list, SPEventType type)
{
    if (!list)
        return 0;

    pthread_mutex_lock(&list->lock);
    SPEventType ret = list->dispatched & type;
    pthread_mutex_unlock(&list->lock);
    return ret;
}

SPEventType sp_eventlist_has_queued(SPBufferList *list, SPEventType type)
{
    if (!list)
        return 0;

    pthread_mutex_lock(&list->lock);
    SPEventType ret = list->queued & type;
    pthread_mutex_unlock(&list->lock);
    return ret;
}

char *sp_event_flags_to_str(SPEventType flags)
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
    COND(SP_EVENT_ON_DISCARD,        on, "discard")
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
    COND(SP_EVENT_CTRL_SIGNAL,     ctrl, "dependency")
    COND(SP_EVENT_CTRL_FLUSH,      ctrl, "flush")

    COND(SP_EVENT_FLAG_UNIQUE,     flag, "unique")
    COND(SP_EVENT_FLAG_DEPENDENCY, flag, "dependency")
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

int sp_event_string_to_flags(void *ctx, SPEventType *dst, const char *in_str)
{
    uint64_t flags  = 0x0;
    int on_prefix   = 0;
    int type_prefix = 0;
    int ctrl_prefix = 0;
    int flag_prefix = 0;

    /* We have to modify it */
    char *str = av_strdup(in_str);

    *dst = 0x0;

#define FLAG(flag, prf, name)                                    \
    if (!strcmp(tok, #prf ":" name) || !strcmp(tok, name)) {     \
        flags |= flag;                                           \
        on_prefix = type_prefix = ctrl_prefix = flag_prefix = 0; \
        prf## _prefix = 1;                                       \
    } else if (!strcmp(tok, ":" name)) {                         \
        if (prf## _prefix) {                                     \
            flags |= flag;                                       \
        } else {                                                 \
            sp_log(ctx, SP_LOG_ERROR, "Error parsing flags, no " \
                   "%s prefix specified for %s!\n", #prf, name); \
            av_free(str);                                        \
            return AVERROR(EINVAL);                              \
        }                                                        \
    } else

    char *save, *tok = av_strtok(str, " ,+", &save);
    while (tok) {
        FLAG(SP_EVENT_ON_COMMIT,       on,   "commit")
        FLAG(SP_EVENT_ON_DISCARD,      on,   "discard")
        FLAG(SP_EVENT_ON_CONFIG,       on,   "config")
        FLAG(SP_EVENT_ON_INIT,         on,   "init")
        FLAG(SP_EVENT_ON_CHANGE,       on,   "change")
        FLAG(SP_EVENT_ON_STATS,        on,   "stats")
        FLAG(SP_EVENT_ON_EOS,          on,   "eos")
        FLAG(SP_EVENT_ON_ERROR,        on,   "error")
        FLAG(SP_EVENT_ON_DESTROY,      on,   "destroy")
        FLAG(SP_EVENT_ON_OUTPUT,       on,   "output")
        FLAG(SP_EVENT_ON_MASK,         on,   "all")

        FLAG(SP_EVENT_TYPE_SOURCE,     type, "source")
        FLAG(SP_EVENT_TYPE_SINK,       type, "sink")
        FLAG(SP_EVENT_TYPE_LINK,       type, "link")
        FLAG(SP_EVENT_TYPE_MASK,       type, "all")

        FLAG(SP_EVENT_CTRL_START,      ctrl, "start")
        FLAG(SP_EVENT_CTRL_STOP,       ctrl, "stop")
        FLAG(SP_EVENT_CTRL_OPTS,       ctrl, "opts")
        FLAG(SP_EVENT_CTRL_COMMIT,     ctrl, "commit")
        FLAG(SP_EVENT_CTRL_DISCARD,    ctrl, "discard")
        FLAG(SP_EVENT_CTRL_FLUSH,      ctrl, "flush")

        FLAG(SP_EVENT_FLAG_UNIQUE,     flag, "unique")
        FLAG(SP_EVENT_FLAG_DEPENDENCY, flag, "dependency")
        FLAG(SP_EVENT_FLAG_IMMEDIATE,  flag, "immediate")
        FLAG(SP_EVENT_FLAG_EXPIRED,    flag, "expired")
        FLAG(SP_EVENT_FLAG_ONESHOT,    flag, "oneshot")

        /* else comes from the macro */ if (!strcmp(tok, "on:")) {
            type_prefix = ctrl_prefix = flag_prefix = 0;
            on_prefix = 1;
        } else if (!strcmp(tok, "type:")) {
            on_prefix = ctrl_prefix = flag_prefix = 0;
            type_prefix = 1;
        } else if (!strcmp(tok, "ctrl:")) {
            on_prefix = type_prefix = flag_prefix = 0;
            ctrl_prefix = 1;
        } else if (!strcmp(tok, "type:")) {
            on_prefix = type_prefix = ctrl_prefix = 0;
            flag_prefix = 1;
        } else {
            sp_log(ctx, SP_LOG_ERROR, "Error parsing flags, \"%s\" not found!\n", tok);
            av_free(str);
            return AVERROR(EINVAL);
        }

        tok = av_strtok(NULL, " ,+", &save);
    }

    av_free(str);
    *dst = flags;

#undef FLAG
    return 0;
}

uint64_t sp_event_get_id(AVBufferRef *event)
{
    return ((SPEvent *)event->data)->id;
}

char *sp_event_flags_to_str_buf(AVBufferRef *event)
{
    return sp_event_flags_to_str(((SPEvent *)event->data)->type);
}

SPEventType sp_class_to_event_type(void *ctx)
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
