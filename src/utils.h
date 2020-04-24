
int  wls_make_wakeup_pipe (int pipes[2]);
void wls_write_wakeup_pipe(int pipes[2]);
void wls_flush_wakeup_pipe(int pipes[2]);
void wls_close_wakeup_pipe(int pipes[2]);

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

typedef struct SlidingWinCtx {
    struct entry {
        int64_t num;
        int64_t pts;
        AVRational tb;
    } *entries;
    int num_entries;
    unsigned int num_entries_alloc;
} SlidingWinCtx;

/* len is the window size in tb */
static inline int64_t sliding_win_sum(SlidingWinCtx *ctx, int64_t num, int64_t pts,
                                      AVRational tb, int64_t len)
{
    int64_t sum = 0;

    if (!ctx->num_entries)
        goto add;

    struct entry *last = &ctx->entries[0];
    int64_t test = av_add_stable(last->tb, last->pts, tb, len);

    /* Older than the window, remove */
    if (av_compare_ts(test, last->tb, pts, tb) == -1) {
        ctx->num_entries--;
        memmove(last, last + 1, sizeof(*last) * ctx->num_entries);
    }

add:
    ctx->entries = av_fast_realloc(ctx->entries, &ctx->num_entries_alloc,
                                   sizeof(*ctx->entries) * (ctx->num_entries + 1));

    struct entry *top = &ctx->entries[ctx->num_entries++];
    top->num = num;
    top->pts = pts;
    top->tb  = tb;

    /* Calculate the average */
    for (int i = 0; i < ctx->num_entries; i++)
        sum += ctx->entries[i].num;

    return sum;
}

static inline void free_sliding_win(SlidingWinCtx *ctx)
{
    if (!ctx)
        return;
    av_freep(&ctx->entries);
}
