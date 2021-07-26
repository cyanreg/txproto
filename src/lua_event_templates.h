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

typedef struct SPCommitCbCtx {
    ctrl_fn fn;
    AVBufferRef *fn_ctx;
} SPCommitCbCtx;

static inline int api_commit_cb(AVBufferRef *opaque, void *src_ctx, void *data)
{
    SPCommitCbCtx *ctx = (SPCommitCbCtx *)opaque->data;
    return ctx->fn(ctx->fn_ctx, SP_EVENT_CTRL_COMMIT, NULL);
}

static inline int api_discard_cb(AVBufferRef *opaque, void *src_ctx, void *data)
{
    SPCommitCbCtx *ctx = (SPCommitCbCtx *)opaque->data;
    return ctx->fn(ctx->fn_ctx, SP_EVENT_CTRL_DISCARD, NULL);
}

static inline void api_commit_discard_free(void *opaque, uint8_t *data)
{
    SPCommitCbCtx *ctx = (SPCommitCbCtx *)data;
    av_buffer_unref(&ctx->fn_ctx);
    av_free(data);
}

static inline int add_commit_fn_to_list(TXMainContext *ctx, ctrl_fn fn, AVBufferRef *fn_ctx)
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

#define GENERIC_CTRL(ref, flags, arg)                                             \
    do {                                                                          \
        ctrl_fn fn = NULL;                                                        \
        enum SPType type = sp_class_get_type(ref->data);                          \
        switch (type) {                                                           \
        case SP_TYPE_ENCODER:                                                     \
            fn = sp_encoder_ctrl;                                                 \
            break;                                                                \
        case SP_TYPE_MUXER:                                                       \
            fn = sp_muxer_ctrl;                                                   \
            break;                                                                \
        case SP_TYPE_FILTER:                                                      \
            fn = sp_filter_ctrl;                                                  \
            break;                                                                \
        case SP_TYPE_AUDIO_SOURCE:                                                \
        case SP_TYPE_AUDIO_SINK:                                                  \
        case SP_TYPE_AUDIO_BIDIR:                                                 \
        case SP_TYPE_VIDEO_SOURCE:                                                \
        case SP_TYPE_VIDEO_SINK:                                                  \
        case SP_TYPE_VIDEO_BIDIR:                                                 \
        case SP_TYPE_SUB_SOURCE:                                                  \
        case SP_TYPE_SUB_SINK:                                                    \
        case SP_TYPE_SUB_BIDIR:                                                   \
            fn = ((IOSysEntry *)ref->data)->ctrl;                                 \
            break;                                                                \
        default:                                                                  \
            LUA_ERROR("Unsupported CTRL type: %s!",                               \
                      sp_class_type_string(ref->data));                           \
        }                                                                         \
                                                                                  \
        if (!(flags & SP_EVENT_CTRL_MASK)) {                                      \
            LUA_ERROR("Missing ctrl: command: %s!", av_err2str(AVERROR(EINVAL))); \
        } else if (flags & SP_EVENT_ON_MASK) {                                    \
            LUA_ERROR("Event specified but given to a ctrl, use %s.hook: %s!",    \
                      sp_class_get_name(ref->data), av_err2str(AVERROR(EINVAL))); \
        } else if ((flags & SP_EVENT_CTRL_OPTS) && (!arg)) {                      \
            LUA_ERROR("No options specified for ctrl:opts: %s!",                  \
                      av_err2str(AVERROR(EINVAL)));                               \
        }                                                                         \
                                                                                  \
        if (flags & SP_EVENT_CTRL_START)                                          \
            err = fn(ref, flags, &ctx->epoch_value);                              \
        else                                                                      \
            err = fn(ref, flags, arg);                                            \
        if (err < 0)                                                              \
             LUA_ERROR("Unable to process CTRL: %s", av_err2str(err));            \
                                                                                  \
        if (!(flags & SP_EVENT_FLAG_IMMEDIATE))                                   \
            add_commit_fn_to_list(ctx, fn, ref);                                  \
                                                                                  \
    } while (0)

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
        if (autostart) {                                                      \
            GENERIC_CTRL(s_ref, SP_EVENT_CTRL_START, NULL);                   \
            GENERIC_CTRL(d_ref, SP_EVENT_CTRL_START, NULL);                   \
        }                                                                     \
                                                                              \
        return sp_lua_unlock_interface(ctx->lua, 0);                          \
    } while (0);
