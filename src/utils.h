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

#pragma once

#include <stdbool.h>
#include <assert.h>
#include <pthread.h>
#include <libavutil/opt.h>
#include <libavutil/buffer.h>
#include <libavutil/avstring.h>

#include "os_compat.h"

int      sp_make_wakeup_pipe (int pipes[2]);
void     sp_write_wakeup_pipe(int pipes[2], int64_t val);
int64_t  sp_flush_wakeup_pipe(int pipes[2]);
void     sp_close_wakeup_pipe(int pipes[2]);

#define SPMIN(a,b) ((a) > (b) ? (b) : (a))
#define SPMAX(a,b) ((a) > (b) ? (a) : (b))
#define SP_ARRAY_ELEMS(a) (sizeof(a) / sizeof((a)[0]))
#define SPSWAP(type, a, b) do { type SWAP_tmp = b; b = a; a= SWAP_tmp; } while(0)
#define SPALIGN(x, a) (((x)+(a)-1)&~((a)-1))

typedef struct SPRationalValue {
    int64_t value;
    AVRational base;
} SPRationalValue;

typedef struct SPRect {
    int x; /* All values are scaled, this is the actual number of pixels on a surface */
    int y;
    int w;
    int h;
    float scale; /* Scaling factor applied to all values */
} SPRect;

enum SPDataType {
    SP_DATA_TYPE_NONE         = (0 << 0),
    SP_DATA_TYPE_FLOAT        = (1 << 1),
    SP_DATA_TYPE_DOUBLE       = (1 << 2),
    SP_DATA_TYPE_INT          = (1 << 3),
    SP_DATA_TYPE_UINT         = (1 << 4),
    SP_DATA_TYPE_U16          = (1 << 5),
    SP_DATA_TYPE_I16          = (1 << 6),
    SP_DATA_TYPE_I64          = (1 << 7),
    SP_DATA_TYPE_U64          = (1 << 8),
    SP_DATA_TYPE_BOOL         = (1 << 9),
    SP_DATA_TYPE_NUM          = (SP_DATA_TYPE_BOOL - 1) & (~1),

    SP_DATA_TYPE_STRING       = (1 << 10),
    SP_DATA_TYPE_RATIONAL_VAL = (1 << 11),
    SP_DATA_TYPE_RECTANGLE    = (1 << 12),
    SP_DATA_TYPE_UNKNOWN      = (1 << 31),
};

#define D_TYPE(name, parent, x) (SPGenericData)                                 \
    {   (name),                                                                 \
        (parent),                                                               \
        _Generic((x),                                                           \
                 char *: ((x)),                                                 \
                 default: (&(x))),                                              \
        _Generic((x),                                                           \
        bool:            SP_DATA_TYPE_BOOL,                                     \
        float:           SP_DATA_TYPE_FLOAT,                                    \
        double:          SP_DATA_TYPE_DOUBLE,                                   \
        int32_t:         SP_DATA_TYPE_INT,                                      \
        uint32_t:        SP_DATA_TYPE_UINT,                                     \
        uint16_t:        SP_DATA_TYPE_U16,                                      \
        int16_t:         SP_DATA_TYPE_I16,                                      \
        int64_t:         SP_DATA_TYPE_I64,                                      \
        uint64_t:        SP_DATA_TYPE_U64,                                      \
        char *:          SP_DATA_TYPE_STRING,                                   \
        SPRationalValue: SP_DATA_TYPE_RATIONAL_VAL,                             \
        SPRect:          SP_DATA_TYPE_RECTANGLE,                                \
        default:         SP_DATA_TYPE_UNKNOWN)                                  \
    }

#define LOAD_GEN_DATA_NUM(entry) (                                         \
    ((entry)->type == SP_DATA_TYPE_BOOL  ) ? *((bool     *)(entry)->ptr) : \
    ((entry)->type == SP_DATA_TYPE_FLOAT ) ? *((float    *)(entry)->ptr) : \
    ((entry)->type == SP_DATA_TYPE_DOUBLE) ? *((double   *)(entry)->ptr) : \
    ((entry)->type == SP_DATA_TYPE_INT   ) ? *((int32_t  *)(entry)->ptr) : \
    ((entry)->type == SP_DATA_TYPE_UINT  ) ? *((uint32_t *)(entry)->ptr) : \
    ((entry)->type == SP_DATA_TYPE_U16   ) ? *((uint16_t *)(entry)->ptr) : \
    ((entry)->type == SP_DATA_TYPE_I16   ) ? *((int16_t  *)(entry)->ptr) : \
    ((entry)->type == SP_DATA_TYPE_I64   ) ? *((int64_t  *)(entry)->ptr) : \
    ((entry)->type == SP_DATA_TYPE_U64   ) ? *((uint64_t *)(entry)->ptr) : \
    sp_assert(0)                                                           \
)

#define GEN_DATA_TYPE_STRING(entry) (                             \
    ((entry)->type == SP_DATA_TYPE_BOOL         ) ? "bool" :      \
    ((entry)->type == SP_DATA_TYPE_FLOAT        ) ? "f32" :       \
    ((entry)->type == SP_DATA_TYPE_DOUBLE       ) ? "f64" :       \
    ((entry)->type == SP_DATA_TYPE_INT          ) ? * "int" :     \
    ((entry)->type == SP_DATA_TYPE_UINT         ) ? "uint" :      \
    ((entry)->type == SP_DATA_TYPE_U16          ) ? "u16" :       \
    ((entry)->type == SP_DATA_TYPE_I16          ) ? "i16" :       \
    ((entry)->type == SP_DATA_TYPE_I64          ) ? "i64" :       \
    ((entry)->type == SP_DATA_TYPE_U64          ) ? "u64" :       \
    ((entry)->type == SP_DATA_TYPE_STRING       ) ? "string" :    \
    ((entry)->type == SP_DATA_TYPE_RATIONAL_VAL ) ? "rational" :  \
    ((entry)->type == SP_DATA_TYPE_RECTANGLE_VAL) ? "rectangle" : \
    sp_assert(0)                                                  \
)

typedef struct SPGenericData {
    const char *name;
    const char *sub;
    void *ptr;
    enum SPDataType type;
} SPGenericData;

/* Bufferlist */
typedef struct SPBufferList SPBufferList;
typedef AVBufferRef *(*sp_buflist_find_fn)(AVBufferRef *entry, void *opaque);
SPBufferList *sp_bufferlist_new(void);
void          sp_bufferlist_free(SPBufferList **s);
int           sp_bufferlist_len(SPBufferList *list);
int           sp_bufferlist_copy(SPBufferList *dst, SPBufferList *src);
int           sp_bufferlist_append(SPBufferList *list, AVBufferRef *entry);
int           sp_bufferlist_append_noref(SPBufferList *list, AVBufferRef *entry);

/* Find */
AVBufferRef  *sp_bufferlist_ref(SPBufferList *list, sp_buflist_find_fn find, void *find_opaque);
AVBufferRef  *sp_bufferlist_pop(SPBufferList *list, sp_buflist_find_fn find, void *find_opaque);
AVBufferRef  *sp_bufferlist_find_fn_first(AVBufferRef *entry, void *opaque);
AVBufferRef  *sp_bufferlist_find_fn_data(AVBufferRef *entry, void *opaque);

/* Iterate */
AVBufferRef  *sp_bufferlist_iter_ref(SPBufferList *list);
void          sp_bufferlist_iter_halt(SPBufferList *list);

enum SPEventType {
    /* [0:15] - Triggers */
    SP_EVENT_ON_COMMIT       = (1ULL <<  0), /* NULL data */
    SP_EVENT_ON_CONFIG       = (1ULL <<  1), /* NULL data, emitted before configuring */
    SP_EVENT_ON_INIT         = (1ULL <<  2), /* NULL data, emitted after initialization */
    SP_EVENT_ON_CHANGE       = (1ULL <<  3), /* NULL data */
    SP_EVENT_ON_STATS        = (1ULL <<  4), /* 0-terminated SPGenericData array */
    SP_EVENT_ON_EOS          = (1ULL <<  5), /* NULL data */
    SP_EVENT_ON_ERROR        = (1ULL <<  6), /* 32-bit integer */
    SP_EVENT_ON_DESTROY      = (1ULL <<  7), /* NULL data, or a 0-terminated SPGenericData array */
    SP_EVENT_ON_OUTPUT       = (1ULL <<  8), /* SPRationalValue */
    SP_EVENT_ON_MASK         = (((1ULL << 16) - 1) <<  0), /* 16 bits reserved for event triggers */

    /* [16:31] - Types, if ORd when comitting, will only run events with that type */
    SP_EVENT_TYPE_LINK       = (1ULL << 16),
    SP_EVENT_TYPE_SOURCE     = (1ULL << 17),
    SP_EVENT_TYPE_FILTER     = (1ULL << 18),
    SP_EVENT_TYPE_BSF        = (1ULL << 19),
    SP_EVENT_TYPE_ENCODER    = (1ULL << 20),
    SP_EVENT_TYPE_MUXER      = (1ULL << 21),
    SP_EVENT_TYPE_DEMUXER    = (1ULL << 22),
    SP_EVENT_TYPE_DECODER    = (1ULL << 23),
    SP_EVENT_TYPE_SINK       = (1ULL << 24),
    SP_EVENT_TYPE_MASK       = (((1ULL << 16) - 1) << 16), /* 16 bits reserved for event type */

    /* [32: 47] - Controls - same as type, but at least either a type or a ctrl must be present */
    SP_EVENT_CTRL_START      = (1ULL << 32), /* Pointer to an atomic int64_t that must be valid at the time of commit */
    SP_EVENT_CTRL_STOP       = (1ULL << 33),
    SP_EVENT_CTRL_COMMAND    = (1ULL << 34), /* AVDictionary * */ /* For custom lavf commands */
    SP_EVENT_CTRL_NEW_EVENT  = (1ULL << 35), /* Immediate */ /* Pointer to an AVBufferRef */
    SP_EVENT_CTRL_DEL_EVENT  = (1ULL << 36), /* Immediate */ /* Pointer to an uint32_t identifier */
    SP_EVENT_CTRL_OPTS       = (1ULL << 37), /* Pointer to an AVDictionary, will be validated before returning */
    SP_EVENT_CTRL_COMMIT     = (1ULL << 38), /* Immediate */ /* Optional pointer to an enum SPEventType for filtering */
    SP_EVENT_CTRL_DISCARD    = (1ULL << 39), /* Immediate */ /* Unrefs all uncommited events */
    SP_EVENT_CTRL_DEP        = (1ULL << 40), /* Immediate */ /* Must be ORd with a trigger, arg must be an AVBufferRef event */
    SP_EVENT_CTRL_FLUSH      = (1ULL << 41),
    SP_EVENT_CTRL_MASK       = (((1ULL << 16) - 1) << 32), /* 16 bits reserved for control events */

    /* [48: 63] - Flags */
    SP_EVENT_FLAG_NO_REORDER = (1ULL << 48), /* Do not logically reorder CTRLs, append event to bottom-of-pipe */
    SP_EVENT_FLAG_NO_DEDUP   = (1ULL << 49), /* Do not deduplicate an event with the same ID */
    SP_EVENT_FLAG_HIGH_PRIO  = (1ULL << 50), /* Event will be ordered first in the flagged ON/CTRL chain */
    SP_EVENT_FLAG_LOW_PRIO   = (1ULL << 51), /* Event will be ordered last in the flagged ON/CTRL chain */
    SP_EVENT_FLAG_IMMEDIATE  = (1ULL << 52), /* If added to a ctrl will run the event immediately */
    SP_EVENT_FLAG_EXPIRED    = (1ULL << 53), /* Event will not be run and will be deleted as soon as possible */
    SP_EVENT_FLAG_ONESHOT    = (1ULL << 54), /* If ORd when comitting will unref all events that ran */
    SP_EVENT_FLAG_MASK       = (((1ULL << 16) - 1) << 48), /* 16 bits reserved for flags */
};

typedef struct SPEvent SPEvent;
enum SPEventType sp_class_to_event_type(void *ctx);

uint32_t sp_event_gen_identifier(void *src, void *dst, uint64_t type);
AVBufferRef *sp_event_create(int (*fn)(AVBufferRef *opaque, void *src_ctx, void *data),
                             pthread_mutex_t *lock, enum SPEventType type,
                             AVBufferRef *opaque, uint32_t identifier);

int sp_eventlist_add(void *src_ctx, SPBufferList *list, AVBufferRef *event);
int sp_eventlist_add_with_dep(void *src_ctx, SPBufferList *list, AVBufferRef *event, enum SPEventType when);
void sp_event_unref_expire(AVBufferRef **buf);
void sp_event_unref_await(AVBufferRef **buf);

/* Unrefs all events on DESTROY, only runs those which are marked as destroy */
int sp_eventlist_dispatch(void *src_ctx, SPBufferList *list, uint64_t type, void *data);

/* Returns type(s, if type is a mask) if sp_eventlist_dispatch has been called at least once with type */
enum SPEventType sp_eventlist_has_dispatched(SPBufferList *list, enum SPEventType type);
/* Same as above, but if there's an outstanding one */
enum SPEventType sp_eventlist_has_queued(SPBufferList *list, enum SPEventType type);

/* Unrefs any events added since the last dispatch */
void sp_eventlist_discard(SPBufferList *list);

/* Returns a string of all flags, must be freed */
char *sp_event_flags_to_str_buf(AVBufferRef *event);
char *sp_event_flags_to_str(uint64_t flags);

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
} SlidingWinCtx;

int64_t sp_sliding_win(SlidingWinCtx *ctx, int64_t num, int64_t pts,
                       AVRational tb, int64_t len, int do_avg);

/* AVDictionary to AVOption */
int sp_set_avopts_pos(void *log, void *avobj, void *posargs, AVDictionary *dict);
int sp_set_avopts(void *log, void *avobj, AVDictionary *dict);

/* Workaround */
static inline int sp_assert(int cond)
{
    assert(1);
    return 0;
}

static inline int sp_is_number(const char *src)
{
    for (int i = 0; i < strlen(src); i++)
        if (!av_isdigit(src[i]) && src[i] != '.')
            return 0;
    return 1;
}
