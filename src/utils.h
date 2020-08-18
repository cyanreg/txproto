#pragma once

#include <pthread.h>
#include <libavutil/opt.h>
#include <libavutil/buffer.h>

int      sp_make_wakeup_pipe (int pipes[2]);
void     sp_write_wakeup_pipe(int pipes[2], int64_t val);
int64_t  sp_flush_wakeup_pipe(int pipes[2]);
void     sp_close_wakeup_pipe(int pipes[2]);

#define SPMIN(a,b) ((a) > (b) ? (b) : (a))
#define SPMAX(a,b) ((a) > (b) ? (a) : (b))
#define SP_ARRAY_ELEMS(a) (sizeof(a) / sizeof((a)[0]))
#define SPALIGN(x, a) (((x)+(a)-1)&~((a)-1))

#define FREE_CLASSED(s)                               \
    do {                                              \
        if (!(*s))                                    \
            break;                                    \
        av_free((void *)((*(s))->class->class_name)); \
        av_free((*(s))->class);                       \
        av_freep((s));                                \
    } while (0)

/* Bufferlist */
typedef struct SPBufferList SPBufferList;
typedef AVBufferRef *(*sp_buflist_find_fn)(AVBufferRef *entry, void *opaque);
SPBufferList *sp_bufferlist_new(void);
void          sp_bufferlist_free(SPBufferList **s);
int           sp_bufferlist_len(SPBufferList *list);
int           sp_bufferlist_copy(SPBufferList *dst, SPBufferList *src);
int           sp_bufferlist_copy_locked(SPBufferList *dst, SPBufferList *src);
int           sp_bufferlist_append(SPBufferList *list, AVBufferRef *entry);
int           sp_bufferlist_append_noref(SPBufferList *list, AVBufferRef *entry);

/* Find */
AVBufferRef  *sp_bufferlist_ref(SPBufferList *list, sp_buflist_find_fn find, void *find_opaque);
AVBufferRef  *sp_bufferlist_pop(SPBufferList *list, sp_buflist_find_fn find, void *find_opaque);
AVBufferRef  *sp_bufferlist_find_fn_first(AVBufferRef *entry, void *opaque);

/* Iterate */
AVBufferRef  *sp_bufferlist_iter_ref(SPBufferList *list);
void          sp_bufferlist_iter_halt(SPBufferList *list);

enum SPEventType {
    /* Triggers */
    SP_EVENT_ON_COMMIT      = (1ULL <<  0),
    SP_EVENT_ON_INIT        = (1ULL <<  1),
    SP_EVENT_ON_CHANGE      = (1ULL <<  2),
    SP_EVENT_ON_STATS_OUT   = (1ULL <<  3),
    SP_EVENT_ON_EOS         = (1ULL <<  4),
    SP_EVENT_ON_ERROR       = (1ULL <<  5),
    SP_EVENT_ON_DESTROY     = (1ULL <<  6),
    SP_EVENT_ON_MASK        = (((1ULL << 16) - 1) <<  0), /* 16 bits reserved for event triggers */

    /* If ORd when comitting, will only run events with that type */
    SP_EVENT_TYPE_SOURCE    = (1ULL << 16),
    SP_EVENT_TYPE_SINK      = (1ULL << 17),
    SP_EVENT_TYPE_LINK      = (1ULL << 18),
    SP_EVENT_TYPE_MASK      = (((1ULL << 16) - 1) << 16), /* 16 bits reserved for event type */

    /* Same as type, but at least either a type or a ctrl must be present */
    SP_EVENT_CTRL_START     = (1ULL << 32), /* Pointer to an atomic int64_t that must be valid at the time of commit */
    SP_EVENT_CTRL_STOP      = (1ULL << 33),
    SP_EVENT_CTRL_NEW_EVENT = (1ULL << 34), /* Immediate */ /* Pointer to an AVBufferRef */
    SP_EVENT_CTRL_DEL_EVENT = (1ULL << 35), /* Immediate */ /* Pointer to an uint32_t identifier */
    SP_EVENT_CTRL_OPTS      = (1ULL << 36), /* Pointer to an AVDictionary, will be validated before returning */
    SP_EVENT_CTRL_COMMIT    = (1ULL << 37), /* Immediate */ /* Optional pointer to an enum SPEventType for filtering */
    SP_EVENT_CTRL_DISCARD   = (1ULL << 38),
    SP_EVENT_CTRL_DEP       = (1ULL << 39), /* Immediate */ /* Must be ORd with a trigger, arg must be an AVBufferRef event */
    SP_EVENT_CTRL_MASK      = (((1ULL << 16) - 1) << 32), /* 16 bits reserved for control events */

    SP_EVENT_FLAG_INTERNAL1 = (1ULL << 59),
    SP_EVENT_FLAG_INTERNAL2 = (1ULL << 60),

    SP_EVENT_FLAG_EPOCH     = (1ULL << 60),
    SP_EVENT_FLAG_IMMEDIATE = (1ULL << 61), /* If added to a ctrl will run the event immediately */
    SP_EVENT_FLAG_EXPIRED   = (1ULL << 62),
    SP_EVENT_FLAG_ONESHOT   = (1ULL << 63), /* If ORd when comitting will unref all events that ran */
    SP_EVENT_FLAG_MASK      = (SP_EVENT_FLAG_EPOCH     |
                               SP_EVENT_FLAG_IMMEDIATE |
                               SP_EVENT_FLAG_EXPIRED   |
                               SP_EVENT_FLAG_ONESHOT   |
                               SP_EVENT_FLAG_INTERNAL1 |
                               SP_EVENT_FLAG_INTERNAL2),
};

typedef struct SPEvent SPEvent;
uint32_t sp_event_gen_identifier(void *src, void *dst, enum SPEventType type);
AVBufferRef *sp_event_create(int (*fn)(AVBufferRef *opaque, void *src_ctx),
                             pthread_mutex_t *lock, enum SPEventType type,
                             AVBufferRef *opaque, uint32_t identifier);

int sp_bufferlist_append_event(SPBufferList *list, AVBufferRef *event);
int sp_bufferlist_append_dep(SPBufferList *list, AVBufferRef *event,
                             enum SPEventType when);
void sp_event_unref_expire(AVBufferRef **buf);

/* Unrefs all events on DESTROY, only runs those which are marked as destroy */
int sp_bufferlist_dispatch_events(SPBufferList *list, void *src_ctx,
                                  enum SPEventType type);

/* Unrefs any events added since the last dispatch */
void sp_bufferlist_discard_new_events(SPBufferList *list);

/* Returns a string of all flags, must be freed */
char *sp_event_flags_to_str_buf(AVBufferRef *event);
char *sp_event_flags_to_str(enum SPEventType flags);

typedef int (*ctrl_fn)(AVBufferRef *ctx_ref, enum SPEventType ctrl, void *arg);

#define SP_EVENT_BUFFER_CTX_ALLOC(struc, name, destr, opaque)                  \
    struc *name = av_mallocz(sizeof(struc));                                   \
    AVBufferRef *name ## _ref = av_buffer_create((uint8_t *)name,              \
                                                 sizeof(struc),                \
                                                 destr,                        \
                                                 opaque, 0);

/* Generic macro for creating contexts which need to keep their addresses
 * if another context is created. */
#define FN_CREATING_INIT(ctx, type, shortname, array, num, spawn_fn, destr_fn) \
static av_always_inline type *create_ ##shortname(ctx *dctx)                   \
{                                                                              \
    type **array, *sctx = spawn_fn;                                            \
    if (!sctx)                                                                 \
        return NULL;                                                           \
                                                                               \
    array = av_realloc_array(dctx->array, sizeof(*dctx->array), dctx->num + 1);\
    if (!array) {                                                              \
        av_free(sctx);                                                         \
        return NULL;                                                           \
    }                                                                          \
                                                                               \
    dctx->array = array;                                                       \
    dctx->array[dctx->num++] = sctx;                                           \
                                                                               \
    return sctx;                                                               \
}                                                                              \
                                                                               \
static av_unused av_always_inline void remove_ ##shortname(ctx *dctx, type *entry)       \
{                                                                              \
    for (int i = 0; i < dctx->num; i++) {                                      \
        if (dctx->array[i] != entry)                                           \
            continue;                                                          \
        dctx->num--;                                                           \
        destr_fn(&dctx->array[i]);                                             \
        memcpy(&dctx->array[i], &dctx->array[i + 1],                           \
               (dctx->num - i) * sizeof(*dctx->array));                        \
        if (dctx->num)                                                         \
            dctx->array = av_realloc_array(dctx->array, sizeof(*dctx->array),  \
                                           dctx->num);                         \
        else                                                                   \
            av_freep(&dctx->array);                                            \
        return;                                                                \
    }                                                                          \
}                                                                              \
                                                                               \
static av_unused av_always_inline type *get_at_index_ ##shortname(ctx *dctx, int idx)    \
{                                                                              \
    if (idx > (dctx->num - 1))                                                 \
        return NULL;                                                           \
    return dctx->array[idx];                                                   \
}

#define FN_CREATING(ctx, type, shortname, array, num)                          \
    FN_CREATING_INIT(ctx, type, shortname, array, num,                         \
                     av_mallocz(sizeof(*sctx)), av_freep)

static inline const char *dict_get(AVDictionary *dict, const char *key)
{
    AVDictionaryEntry *e = av_dict_get(dict, key, NULL, 0);
    return e ? e->value : NULL;
}

static inline int cmp_numbers(const void *a, const void *b)
{
    return *((int *)a) > *((int *)b);
}

/* Sliding window */
#define MAX_ROLLING_WIN_ENTRIES 4096
typedef struct SlidingWinCtx {
    struct SPSlidingWinEntry {
        int64_t num;
        int64_t pts;
        AVRational tb;
    } entries[MAX_ROLLING_WIN_ENTRIES];
    int num_entries;
    unsigned int num_entries_alloc;
} SlidingWinCtx;

int64_t sp_sliding_win(SlidingWinCtx *ctx, int64_t num, int64_t pts,
                       AVRational tb, int64_t len, int do_avg);

/* AVDictionary to AVOption */
int sp_set_avopts_pos(void *log, void *avobj, void *posargs, AVDictionary *dict);
int sp_set_avopts(void *log, void *avobj, AVDictionary *dict);

/* Thread name setter wrapper */
void sp_set_thread_name_self(const char *name);

/* Class type to string */
const char *sp_map_class_to_string(AVClassCategory class_type);
enum SPEventType sp_classed_ctx_to_type(void *ctx);
enum AVMediaType sp_classed_ctx_to_media_type(void *ctx);

/* parent and log offset MUST be contained in ctx, will set log_offset if name matches in sp_component_log_levels */
int sp_alloc_class(void *ctx, const char *name, AVClassCategory category, void *parent, void *log_offset);
void sp_free_class(void *ctx);

/* From the end, generate random bytes. */
void sp_gen_random_data(void *dst, size_t dst_size, int bytes);

/* Component log levels */
extern AVDictionary *sp_component_log_levels;

/* Converts string to AV_LOG... or returns INT_MAX if invalid */
int sp_str_to_log_lvl(const char *str);

/* Gets the component log offset */
int sp_get_log_lvl_offset(const char *component);
