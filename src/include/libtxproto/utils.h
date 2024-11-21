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
#include <libavutil/opt.h>
#include <libavutil/avstring.h>

#include <libtxproto/events.h>

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
    assert(cond);
    return 0;
}

static inline int sp_is_number(const char *src)
{
    for (unsigned int i = 0; i < strlen(src); i++)
        if (!av_isdigit(src[i]) && src[i] != '.')
            return 0;
    return 1;
}
