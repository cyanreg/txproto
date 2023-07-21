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

#include <libtxproto/commit.h>

typedef struct SPCommitCbCtx {
    ctrl_fn fn;
    AVBufferRef *fn_ctx;
} SPCommitCbCtx;

/**
 * This is what, on commit, asks sub-comonents to also commit.
 */
static int api_commit_cb(AVBufferRef *event_ref, void *callback_ctx,
                         void *ctx, void *dep_ctx, void *data)
{
    SPCommitCbCtx *cb_ctx = callback_ctx;
    return cb_ctx->fn(cb_ctx->fn_ctx, SP_EVENT_CTRL_COMMIT, NULL);
}

/**
 * Same, but with discards.
 */
static int api_discard_cb(AVBufferRef *event_ref, void *callback_ctx,
                          void *ctx, void *dep_ctx, void *data)
{
    SPCommitCbCtx *cb_ctx = callback_ctx;
    return cb_ctx->fn(cb_ctx->fn_ctx, SP_EVENT_CTRL_DISCARD, NULL);
}

static void api_commit_free(void *callback_ctx, void *ctx, void *dep_ctx)
{
    SPCommitCbCtx *cb_ctx = callback_ctx;
    av_buffer_unref(&cb_ctx->fn_ctx);
}

int sp_add_discard_fn_to_list(TXMainContext *ctx, ctrl_fn fn,
                              AVBufferRef *fn_ctx)
{
    int err;
    SPEventType type = sp_class_to_event_type(fn_ctx->data);
    type |= SP_EVENT_FLAG_ONESHOT | SP_EVENT_ON_DISCARD;

    AVBufferRef *discard_event = sp_event_create(api_discard_cb,
                                                 api_commit_free,
                                                 sizeof(SPCommitCbCtx),
                                                 NULL,
                                                 type,
                                                 fn_ctx->data,
                                                 NULL);

    SPCommitCbCtx *api_discard_ctx = av_buffer_get_opaque(discard_event);
    api_discard_ctx->fn = fn;
    api_discard_ctx->fn_ctx = av_buffer_ref(fn_ctx);

    if ((err = sp_eventlist_add(ctx, ctx->events, discard_event, 0)) < 0) {
        av_buffer_unref(&discard_event);
        return err;
    }

    return 0;
}

int sp_add_commit_fn_to_list(TXMainContext *ctx, ctrl_fn fn,
                             AVBufferRef *fn_ctx)
{
    int err;
    SPEventType type = sp_class_to_event_type(fn_ctx->data);
    type |= SP_EVENT_FLAG_ONESHOT | SP_EVENT_ON_COMMIT;

    AVBufferRef *commit_event = sp_event_create(api_commit_cb,
                                                 api_commit_free,
                                                 sizeof(SPCommitCbCtx),
                                                 NULL,
                                                 type,
                                                 fn_ctx->data,
                                                 NULL);

    SPCommitCbCtx *api_commit_ctx = av_buffer_get_opaque(commit_event);
    api_commit_ctx->fn = fn;
    api_commit_ctx->fn_ctx = av_buffer_ref(fn_ctx);

    if ((err = sp_eventlist_add(ctx, ctx->events, commit_event, 0)) < 0) {
        av_buffer_unref(&commit_event);
        return err;
    }

    sp_add_discard_fn_to_list(ctx, fn, fn_ctx);

    return 0;
}
