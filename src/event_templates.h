typedef struct SPCommitCbCtx {
    ctrl_fn fn;
    AVBufferRef *fn_ctx;
} SPCommitCbCtx;

static int api_commit_cb(AVBufferRef *opaque, void *src_ctx)
{
    SPCommitCbCtx *ctx = (SPCommitCbCtx *)opaque->data;
    return ctx->fn(ctx->fn_ctx, SP_EVENT_CTRL_COMMIT, NULL);
}

static int api_discard_cb(AVBufferRef *opaque, void *src_ctx)
{
    SPCommitCbCtx *ctx = (SPCommitCbCtx *)opaque->data;
    return ctx->fn(ctx->fn_ctx, SP_EVENT_CTRL_DISCARD, NULL);
}

static void api_commit_discard_free(void *opaque, uint8_t *data)
{
    SPCommitCbCtx *ctx = (SPCommitCbCtx *)data;
    av_buffer_unref(&ctx->fn_ctx);
    av_free(data);
}

static int add_commit_fn_to_list(MainContext *ctx, ctrl_fn fn, AVBufferRef *fn_ctx)
{
    SP_EVENT_BUFFER_CTX_ALLOC(SPCommitCbCtx, api_commit_ctx, api_commit_discard_free, NULL)

    api_commit_ctx->fn = fn;
    api_commit_ctx->fn_ctx = av_buffer_ref(fn_ctx);

    uint32_t identifier = sp_event_gen_identifier(ctx, NULL, SP_EVENT_FLAG_ONESHOT | SP_EVENT_ON_COMMIT);

    /* Commit */
    AVBufferRef *commit_event = sp_event_create(api_commit_cb, NULL,
                                                SP_EVENT_FLAG_ONESHOT | SP_EVENT_ON_COMMIT,
                                                av_buffer_ref(api_commit_ctx_ref),
                                                identifier);
    sp_bufferlist_append(ctx->commit_list, commit_event);
    av_buffer_unref(&commit_event);

    /* Discard */
    AVBufferRef *discard_event = sp_event_create(api_discard_cb, NULL,
                                                 SP_EVENT_FLAG_ONESHOT | SP_EVENT_ON_COMMIT,
                                                 api_commit_ctx_ref,
                                                 identifier);
    sp_bufferlist_append(ctx->commit_list, discard_event);
    av_buffer_unref(&discard_event);

    return 0;
}
