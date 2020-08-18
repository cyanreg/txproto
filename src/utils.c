#include "utils.h"
#include "os_compat.h"

#include <unistd.h>
#include <stdbool.h>
#include <fcntl.h>

#include <libavutil/opt.h>
#include <libavutil/crc.h>
#include <libavutil/bprint.h>
#include <libavutil/random_seed.h>

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
    (void)write(pipes[1], &val, sizeof(val));
}

int64_t sp_flush_wakeup_pipe(int pipes[2])
{
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
            av_log(log, AV_LOG_ERROR, "AVOption \"%s\" not found!\n", k);
            success = -1;
        } else if (r < 0) {
            char errstr[80];
            av_strerror(r, errstr, sizeof(errstr));
            av_log(log, AV_LOG_ERROR, "Could not set AVOption \"%s\" to \"%s\": "
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

    pthread_mutex_t *unlock_on_delete;
    enum SPEventType *priv_flags;
    unsigned int priv_flags_size;

    int iter_idx;
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

static int internal_bufferlist_append(SPBufferList *list, AVBufferRef *entry,
                                      enum SPEventType pflags, int ref)
{
    int err = 0;

    pthread_mutex_lock(&list->lock);

    AVBufferRef **new_entries = av_fast_realloc(list->entries, &list->entries_size,
                                                sizeof(*new_entries) * (list->entries_num + 1));
    if (!new_entries) {
        err = AVERROR(ENOMEM);
        goto end;
    }

    enum SPEventType *priv_flags = av_fast_realloc(list->priv_flags, &list->priv_flags_size,
                                                   sizeof(*priv_flags) * (list->entries_num + 1));
    if (!priv_flags) {
        err = AVERROR(ENOMEM);
        goto end;
    }

    priv_flags[list->entries_num] = pflags;
    new_entries[list->entries_num] = ref ? av_buffer_ref(entry) : entry;
    list->entries_num++;
    list->entries = new_entries;
    list->priv_flags = priv_flags;

end:
    pthread_mutex_unlock(&list->lock);
    return err;
}

#define SP_BUF_PRIV_NEW SP_EVENT_FLAG_INTERNAL1
#define SP_BUF_PRIV_DEP SP_EVENT_FLAG_INTERNAL2

int sp_bufferlist_append(SPBufferList *list, AVBufferRef *entry)
{
    return internal_bufferlist_append(list, entry, SP_BUF_PRIV_NEW, 1);
}

int sp_bufferlist_append_noref(SPBufferList *list, AVBufferRef *entry)
{
    return internal_bufferlist_append(list, entry, SP_BUF_PRIV_NEW, 0);
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
    SPBufferList *list = *s;

    pthread_mutex_lock(&list->lock);

    for (int i = 0; i < list->entries_num; i++)
        av_buffer_unref(&list->entries[i]);

    av_freep(&list->entries);
    av_freep(&list->priv_flags);

    pthread_mutex_unlock(&list->lock);
    pthread_mutex_destroy(&list->lock);

    if (list->unlock_on_delete)
        pthread_mutex_unlock(list->unlock_on_delete);

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

int sp_bufferlist_copy_locked(SPBufferList *dst, SPBufferList *src)
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
    if (err < 0) {
        sp_bufferlist_free(&dst);
        pthread_mutex_unlock(&src->lock);
    }

    dst->unlock_on_delete = &src->lock;

    return err;
}

struct SPEvent {
    enum SPEventType type;
    pthread_mutex_t *lock;
    pthread_cond_t cond;
    uint32_t identifier;
    int external_lock;
    int (*fn)(AVBufferRef *opaque, void *src_ctx);
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

AVBufferRef *sp_event_create(int (*fn)(AVBufferRef *opaque, void *src_ctx),
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

static inline AVBufferRef *find_event_identifier(AVBufferRef *entry, void *opaque)
{
    SPEvent *event = (SPEvent *)entry->data;
    if (event->identifier == *((uint32_t *)opaque))
        return entry;

    return NULL;
}

int sp_bufferlist_append_event(SPBufferList *list, AVBufferRef *event)
{
    SPEvent *event_ctx = (SPEvent *)event->data;
    AVBufferRef *dup = sp_bufferlist_pop(list, find_event_identifier, &event_ctx->identifier);
    if (dup) {
        char *fstr = sp_event_flags_to_str(event_ctx->type);
        av_log(NULL, AV_LOG_WARNING, "Deduplicating event (%s)!\n", fstr);
        av_free(fstr);
        av_buffer_unref(&dup);
    }
    return internal_bufferlist_append(list, event, SP_BUF_PRIV_NEW, 1);
}

int sp_bufferlist_append_dep(SPBufferList *list, AVBufferRef *event,
                             enum SPEventType when)
{
    return internal_bufferlist_append(list, event, SP_BUF_PRIV_NEW | SP_BUF_PRIV_DEP | (when & SP_EVENT_ON_MASK), 1);
}

int sp_bufferlist_dispatch_events(SPBufferList *list, void *src_ctx,
                                  enum SPEventType type)
{
    int ret = 0;

    pthread_mutex_lock(&list->lock);

    for (int i = 0; i < list->entries_num; i++) {
        list->priv_flags[i] &= ~SP_BUF_PRIV_NEW;
        if ((list->priv_flags[i] & SP_BUF_PRIV_DEP) &&
            ((list->priv_flags[i] & SP_EVENT_ON_MASK) & type)) {
            SPEvent *event = (SPEvent *)list->entries[i]->data;
            pthread_mutex_lock(event->lock);

            if (!(event->type & SP_EVENT_FLAG_EXPIRED)) {
                char *fstr = sp_event_flags_to_str(type & SP_EVENT_ON_MASK);
                av_log(src_ctx, AV_LOG_DEBUG, "Waiting on event (%s)!\n", fstr);
                av_free(fstr);

                pthread_cond_wait(&event->cond, event->lock);
            }

            pthread_mutex_unlock(event->lock);
            av_buffer_unref(&list->entries[i]);
            buflist_remove_idx(list, i);
            i -= 1;
        }
    }

    for (int i = 0; i < list->entries_num; i++) {
        SPEvent *event = (SPEvent *)list->entries[i]->data;
        pthread_mutex_lock(event->lock);

        enum SPEventType destroy_now = 0;
        destroy_now |= event->type & SP_EVENT_FLAG_EXPIRED;
        destroy_now |= (type & SP_EVENT_ON_DESTROY) && !(event->type & SP_EVENT_ON_DESTROY);

        if (destroy_now) {
            pthread_mutex_unlock(event->lock);
            av_buffer_unref(&list->entries[i]);
            buflist_remove_idx(list, i);
            i -= 1;
            continue;
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

        if (!run_now) {
            pthread_mutex_unlock(event->lock);
            continue;
        }

        destroy_now = 0;
        destroy_now |= type & SP_EVENT_FLAG_ONESHOT;
        destroy_now |= type & SP_EVENT_ON_DESTROY;

        char *fstr = sp_event_flags_to_str(event->type);
        av_log(src_ctx, AV_LOG_DEBUG, "Dispatching event (%s)%s!\n", fstr, destroy_now ? ", destroying" : "");
        av_free(fstr);

        destroy_now |= event->type & SP_EVENT_FLAG_ONESHOT;

        ret = event->fn((AVBufferRef *)av_buffer_get_opaque(list->entries[i]), src_ctx);
        if (event->type & SP_EVENT_FLAG_ONESHOT)
            event->type |= SP_EVENT_FLAG_EXPIRED;

        pthread_cond_broadcast(&event->cond);

        if (destroy_now) {
            pthread_mutex_unlock(event->lock);
            av_buffer_unref(&list->entries[i]);
            buflist_remove_idx(list, i);
            i -= 1;
            continue;
        }

        pthread_mutex_unlock(event->lock);
    }
    pthread_mutex_unlock(&list->lock);

    return ret;
}

void sp_bufferlist_discard_new_events(SPBufferList *list)
{
    pthread_mutex_lock(&list->lock);

    for (int i = 0; i < list->entries_num; i++) {
        if (!(list->priv_flags[i] & SP_BUF_PRIV_NEW))
            continue;

        av_buffer_unref(&list->entries[i]);
        buflist_remove_idx(list, i);
    }

    pthread_mutex_unlock(&list->lock);
}

uint32_t sp_event_gen_identifier(void *src, void *dst, enum SPEventType type)
{
    uint32_t crc = UINT32_MAX;
    const AVCRC *table = av_crc_get_table(AV_CRC_32_IEEE);

    type &= SP_EVENT_TYPE_MASK | SP_EVENT_CTRL_MASK;

    crc = av_crc(table, crc, (void *)&type, sizeof(type));
    if (src) {
        AVClass *src_class = *((AVClass **)src);
        crc = av_crc(table, crc, (void *)&src, sizeof(src));
        crc = av_crc(table, crc, src_class->class_name, strlen(src_class->class_name));
    }
    if (dst) {
        AVClass *dst_class = *((AVClass **)dst);
        crc = av_crc(table, crc, (void *)&dst, sizeof(dst));
        crc = av_crc(table, crc, dst_class->class_name, strlen(dst_class->class_name));
    }
    return crc;
}

const char *sp_map_class_to_string(AVClassCategory class_type)
{
#define MAP(X, s) case X: return s;
    switch (class_type) {
    MAP(AV_CLASS_CATEGORY_NA, "not available")
    MAP(AV_CLASS_CATEGORY_INPUT, "input")
    MAP(AV_CLASS_CATEGORY_OUTPUT, "output")
    MAP(AV_CLASS_CATEGORY_MUXER, "muxer")
    MAP(AV_CLASS_CATEGORY_DEMUXER, "demuxer")
    MAP(AV_CLASS_CATEGORY_ENCODER, "encoder")
    MAP(AV_CLASS_CATEGORY_DECODER, "decoder")
    MAP(AV_CLASS_CATEGORY_FILTER, "filter")
    MAP(AV_CLASS_CATEGORY_BITSTREAM_FILTER, "bitstream filter")
    MAP(AV_CLASS_CATEGORY_SWSCALER, "scaler")
    MAP(AV_CLASS_CATEGORY_SWRESAMPLER, "resampler")
    MAP(AV_CLASS_CATEGORY_DEVICE_VIDEO_OUTPUT, "video output")
    MAP(AV_CLASS_CATEGORY_DEVICE_VIDEO_INPUT, "video input")
    MAP(AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT, "audio output")
    MAP(AV_CLASS_CATEGORY_DEVICE_AUDIO_INPUT, "audio input")
    MAP(AV_CLASS_CATEGORY_DEVICE_OUTPUT, "device output")
    MAP(AV_CLASS_CATEGORY_DEVICE_INPUT, "device input")
    default: return "unknown";
    }
#undef MAP
}

enum SPEventType sp_classed_ctx_to_type(void *ctx)
{
    AVClass *class = *((AVClass **)ctx);
    switch (class->category) {
    case AV_CLASS_CATEGORY_INPUT:
    case AV_CLASS_CATEGORY_DEVICE_VIDEO_INPUT:
    case AV_CLASS_CATEGORY_DEVICE_AUDIO_INPUT:
    case AV_CLASS_CATEGORY_DEVICE_INPUT:
        return SP_EVENT_TYPE_SOURCE;
    case AV_CLASS_CATEGORY_OUTPUT:
    case AV_CLASS_CATEGORY_DEVICE_VIDEO_OUTPUT:
    case AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT:
    case AV_CLASS_CATEGORY_DEVICE_OUTPUT:
        return SP_EVENT_TYPE_SINK;
    default:
        return 0x0;
    }
}

enum AVMediaType sp_classed_ctx_to_media_type(void *ctx)
{
    AVClass *class = *((AVClass **)ctx);
    switch (class->category) {
    case AV_CLASS_CATEGORY_DEVICE_VIDEO_INPUT:
    case AV_CLASS_CATEGORY_DEVICE_VIDEO_OUTPUT:
        return AVMEDIA_TYPE_VIDEO;
    case AV_CLASS_CATEGORY_DEVICE_AUDIO_INPUT:
    case AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT:
        return AVMEDIA_TYPE_AUDIO;
    default:
        return AVMEDIA_TYPE_UNKNOWN;
    }
}

char *sp_event_flags_to_str(enum SPEventType flags)
{
    AVBPrint bp;
    av_bprint_init(&bp, 32, AV_BPRINT_SIZE_UNLIMITED);

#define COND(flag, str)                                   \
    if (flag & flags) {                                   \
        flags &= ~flag;                                   \
        av_bprintf(&bp, "%s%c", str, flags ? ' ' : '\0'); \
    }

    COND(SP_EVENT_ON_COMMIT,      "o_commit")
    COND(SP_EVENT_ON_INIT,        "o_init")
    COND(SP_EVENT_ON_CHANGE,      "o_change")
    COND(SP_EVENT_ON_STATS_OUT,   "o_stats")
    COND(SP_EVENT_ON_DESTROY,     "o_destroy")

    COND(SP_EVENT_TYPE_SOURCE,    "t_source")
    COND(SP_EVENT_TYPE_SINK,      "t_sink")
    COND(SP_EVENT_TYPE_LINK,      "t_link")

    COND(SP_EVENT_CTRL_START,     "c_start")
    COND(SP_EVENT_CTRL_STOP,      "c_stop")
    COND(SP_EVENT_CTRL_NEW_EVENT, "c_new_event")
    COND(SP_EVENT_CTRL_DEL_EVENT, "c_del_event")
    COND(SP_EVENT_CTRL_OPTS,      "c_opts")
    COND(SP_EVENT_CTRL_COMMIT,    "c_commit")
    COND(SP_EVENT_CTRL_DISCARD,   "c_discard")

    COND(SP_EVENT_FLAG_EXPIRED,   "f_expired")
    COND(SP_EVENT_FLAG_ONESHOT,   "f_oneshot")

    if (flags)
        av_bprintf(&bp, "unknown(0x%lx)!", flags);

#undef COND

    char *buf;
    av_bprint_finalize(&bp, &buf);
    return buf;
}

char *sp_event_flags_to_str_buf(AVBufferRef *event)
{
    return sp_event_flags_to_str(((SPEvent *)event->data)->type);
}

int sp_alloc_class(void *ctx, const char *name, AVClassCategory category, void *log_lvl, void *parent)
{
    if (!ctx)
        return 0;

    struct {
        AVClass *class;
    } *s = ctx;

    s->class = av_mallocz(sizeof(AVClass));
    if (!s->class)
        return AVERROR(ENOMEM);

    *s->class = (AVClass){
        .class_name   = av_strdup(name),
        .item_name    = av_default_item_name,
        .get_category = av_default_get_category,
        .version      = LIBAVUTIL_VERSION_INT,
        .category     = category,
        .log_level_offset_offset = log_lvl ? (uintptr_t)log_lvl - (uintptr_t)ctx : 0,
        .parent_log_context_offset = parent ? (uintptr_t)parent - (uintptr_t)ctx : 0,
    };

    if (!s->class->class_name) {
        av_free(s->class);
        return AVERROR(ENOMEM);
    }

    if (s->class->log_level_offset_offset <     0 || s->class->parent_log_context_offset <     0 ||
        s->class->log_level_offset_offset >= 4096 || s->class->parent_log_context_offset >= 4096) {
        av_log(s, AV_LOG_ERROR, "Unreasonable log_lvl or parent context locations!\n");
        av_free((void *)s->class->class_name);
        av_free(s->class);
        return AVERROR(EINVAL);
    }

    if (log_lvl)
        *((int *)log_lvl) = sp_get_log_lvl_offset(name);

    return 0;
}

void sp_free_class(void *ctx)
{
    if (!ctx)
        return;

    struct {
        AVClass *class;
    } *s = ctx;

    av_free((void *)s->class->class_name);
    av_free(s->class);
}

uint32_t random_state;
int bytes_in_random_state = 0;
pthread_mutex_t random_state_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_once_t random_state_ctrl = PTHREAD_ONCE_INIT;

static void gen_random_state()
{
    random_state = av_get_random_seed();
    bytes_in_random_state = 4;
}

void sp_gen_random_data(void *dst, size_t dst_size, int bytes)
{
    if (!dst_size || !bytes)
        return;

    pthread_once(&random_state_ctrl, gen_random_state);
    pthread_mutex_lock(&random_state_lock);

    uint8_t *end = (uint8_t *)dst + dst_size - 1;
    while (bytes > 0 && end >= (uint8_t *)dst) {
        if (bytes_in_random_state <= 0)
            gen_random_state();

        *end = random_state & 0xff;
        end--;
        bytes--;

        random_state >>= 8;
        bytes_in_random_state--;
    }

    pthread_mutex_unlock(&random_state_lock);
}

int sp_str_to_log_lvl(const char *str)
{
    if (!strcmp(str, "quiet"))
        return AV_LOG_QUIET;
    else if (!strcmp(str, "panic"))
        return AV_LOG_PANIC;
    else if (!strcmp(str, "fatal"))
        return AV_LOG_FATAL;
    else if (!strcmp(str, "error"))
        return AV_LOG_ERROR;
    else if (!strcmp(str, "warn"))
        return AV_LOG_WARNING;
    else if (!strcmp(str, "info"))
        return AV_LOG_INFO;
    else if (!strcmp(str, "verbose"))
        return AV_LOG_VERBOSE;
    else if (!strcmp(str, "debug"))
        return AV_LOG_DEBUG;
    else if (!strcmp(str, "trace"))
        return AV_LOG_TRACE;
    else
        return INT_MAX;
}

AVDictionary *sp_component_log_levels = NULL;

int sp_get_log_lvl_offset(const char *component)
{
    const char *lvl_str = dict_get(sp_component_log_levels, component);
    if (!lvl_str)
        return 0;

    const char *glvl_str = dict_get(sp_component_log_levels, "global");
    int lvl = sp_str_to_log_lvl(lvl_str);
    int glvl = sp_str_to_log_lvl(glvl_str);

    return lvl - glvl;
}
