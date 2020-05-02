#pragma once

#include <libavutil/opt.h>

int  sp_make_wakeup_pipe (int pipes[2]);
void sp_write_wakeup_pipe(int pipes[2]);
void sp_flush_wakeup_pipe(int pipes[2]);
void sp_close_wakeup_pipe(int pipes[2]);

/* Generic macro for creating contexts which need to keep their addresses
 * if another context is created. */
#define FN_CREATING(ctx, type, shortname, array, num)                          \
static av_always_inline type *create_ ##shortname(ctx *dctx)                   \
{                                                                              \
    type **array, *sctx = av_mallocz(sizeof(*sctx));                           \
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
static av_always_inline void remove_ ##shortname(ctx *dctx, type *entry)       \
{                                                                              \
    for (int i = 0; i < dctx->num; i++) {                                      \
        if (dctx->array[i] != entry)                                           \
            continue;                                                          \
        dctx->num--;                                                           \
        av_free(dctx->array[i]);                                               \
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
static av_always_inline type *get_at_index_ ##shortname(ctx *dctx, int idx)    \
{                                                                              \
    if (idx > (dctx->num - 1))                                                 \
        return NULL;                                                           \
    return dctx->array[idx];                                                   \
}

static inline const char *dict_get(AVDictionary *dict, const char *key)
{
    AVDictionaryEntry *e = av_dict_get(dict, key, NULL, 0);
    return e ? e->value : NULL;
}

static inline int cmp_numbers(const void *a, const void *b)
{
    return *((int *)a) > *((int *)b);
}

/* Opus at 2.5 ms frame size has 400 packets a second */
#ifndef MAX_ROLLING_WIN_ENTRIES
#define MAX_ROLLING_WIN_ENTRIES 512
#endif

typedef struct SlidingWinCtx {
    struct sliding_win_entry {
        int64_t num;
        int64_t pts;
        AVRational tb;
    } entries[MAX_ROLLING_WIN_ENTRIES];
    int num_entries;
    unsigned int num_entries_alloc;
} SlidingWinCtx;

/* len is the window size in tb */
static inline int64_t sliding_win_sum(SlidingWinCtx *ctx, int64_t num, int64_t pts,
                                      AVRational tb, int64_t len, int is_avg)
{
    struct sliding_win_entry *top, *last;
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

    if (is_avg && ctx->num_entries)
        sum /= ctx->num_entries;

    return sum;
}

int sp_set_avopts_pos(void *log, void *avobj, void *posargs, AVDictionary *dict);
int sp_set_avopts(void *log, void *avobj, AVDictionary *dict);
