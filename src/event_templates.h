typedef struct SPCommitCbCtx {
    ctrl_fn fn;
    AVBufferRef *fn_ctx;
} SPCommitCbCtx;

static int api_commit_cb(AVBufferRef *opaque, void *src_ctx, void *data)
{
    SPCommitCbCtx *ctx = (SPCommitCbCtx *)opaque->data;
    return ctx->fn(ctx->fn_ctx, SP_EVENT_CTRL_COMMIT, NULL);
}

static int api_discard_cb(AVBufferRef *opaque, void *src_ctx, void *data)
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

    uint32_t identifier = sp_event_gen_identifier(ctx, fn_ctx->data,
                                                  SP_EVENT_FLAG_ONESHOT | SP_EVENT_ON_COMMIT);

    /* Commit */
    AVBufferRef *commit_event = sp_event_create(api_commit_cb, NULL,
                                                SP_EVENT_FLAG_ONESHOT | SP_EVENT_ON_COMMIT,
                                                av_buffer_ref(api_commit_ctx_ref),
                                                identifier);
    sp_eventlist_add(fn_ctx->data, ctx->commit_list, commit_event);
    av_buffer_unref(&commit_event);

    /* Discard */
    AVBufferRef *discard_event = sp_event_create(api_discard_cb, NULL,
                                                 SP_EVENT_FLAG_ONESHOT | SP_EVENT_ON_COMMIT,
                                                 api_commit_ctx_ref,
                                                 identifier);
    sp_eventlist_add(fn_ctx->data, ctx->discard_list, discard_event);
    av_buffer_unref(&discard_event);

    return 0;
}

#define GENERIC_LINK(s_type, s_ref, s_ctrlfn, d_type, d_ref, d_ctrlfn, name)  \
    do {                                                                      \
        s_type *sctx = (void *)s_ref->data;                                   \
        d_type *dctx = (void *)d_ref->data;                                   \
                                                                              \
        enum SPEventType flags = SP_EVENT_FLAG_ONESHOT |                      \
                                 SP_EVENT_TYPE_LINK    |                      \
                                 SP_EVENT_TYPE_FILTER  |                      \
                                 SP_EVENT_TYPE_SOURCE;                        \
                                                                              \
        uint64_t post_init = sp_eventlist_has_dispatched(sctx->events,        \
                                                         SP_EVENT_ON_INIT);   \
        if (post_init)                                                        \
            flags |= SP_EVENT_ON_COMMIT;                                      \
        else                                                                  \
            flags |= SP_EVENT_ON_INIT;                                        \
                                                                              \
        uint32_t identifier = sp_event_gen_identifier(sctx, dctx,             \
                                                      SP_EVENT_TYPE_LINK);    \
                                                                              \
        AVBufferRef *link_event = sp_event_create(api_ ##name## _cb, NULL,    \
                                                  flags, name## _ref,         \
                                                  identifier);                \
                                                                              \
        ctrl_fn src_ctrl = s_ctrlfn;                                          \
        src_ctrl(s_ref, SP_EVENT_CTRL_NEW_EVENT, link_event);                 \
        if (!sp_eventlist_has_dispatched(dctx->events, SP_EVENT_ON_CONFIG))   \
            d_ctrlfn(d_ref, SP_EVENT_CTRL_DEP | SP_EVENT_ON_CONFIG,           \
                     link_event);                                             \
                                                                              \
        if (post_init)                                                        \
            add_commit_fn_to_list(ctx, src_ctrl, s_ref);                      \
                                                                              \
        av_buffer_unref(&link_event);                                         \
                                                                              \
        if (err < 0)                                                          \
            LUA_ERROR("Error linking: %s", av_err2str(err));                  \
                                                                              \
        LUA_INTERFACE_END(0);                                                 \
    } while (0);
