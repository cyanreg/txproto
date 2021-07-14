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

#include "utils.h"

typedef struct SNAME {
    TYPE **queued;
    int num_queued;
    int max_queued;
    FNAME block_flags;
    unsigned int queued_alloc_size;
    pthread_mutex_t lock;
    pthread_cond_t cond_in;
    pthread_cond_t cond_out;

    SPBufferList *dests;
    SPBufferList *sources;
} SNAME;

static AVBufferRef *find_ref_by_data(AVBufferRef *entry, void *opaque)
{
    if (entry->data == opaque)
        return entry;
    return NULL;
}

static void PRIV_RENAME(fifo_destroy)(void *opaque, uint8_t *data)
{
    SNAME *ctx = (SNAME *)data;

    pthread_mutex_lock(&ctx->lock);

    sp_bufferlist_free(&ctx->sources);
    sp_bufferlist_free(&ctx->dests);

    for (int i = 0; i < ctx->num_queued; i++)
        FREE_FN(&ctx->queued[i]);
    av_freep(&ctx->queued);

    pthread_mutex_unlock(&ctx->lock);

    pthread_cond_destroy(&ctx->cond_in);
    pthread_cond_destroy(&ctx->cond_out);
    pthread_mutex_destroy(&ctx->lock);

    av_free(ctx);
}

AVBufferRef *RENAME(fifo_create)(void *opaque, int max_queued, FNAME block_flags)
{
    SNAME *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return NULL;

    AVBufferRef *ctx_ref = av_buffer_create((uint8_t *)ctx, sizeof(*ctx), PRIV_RENAME(fifo_destroy), opaque, 0);
    if (!ctx_ref) {
        av_free(ctx);
        return NULL;
    }

    pthread_mutex_init(&ctx->lock, NULL);

    pthread_cond_init(&ctx->cond_in, NULL);
    pthread_cond_init(&ctx->cond_out, NULL);

    ctx->block_flags = block_flags;
    ctx->max_queued = max_queued;
    ctx->dests = sp_bufferlist_new();
    if (!ctx->dests) {
        av_buffer_unref(&ctx_ref);
        return NULL;
    }

    ctx->sources = sp_bufferlist_new();
    if (!ctx->sources) {
        av_buffer_unref(&ctx_ref);
        return NULL;
    }

    return ctx_ref;
}

int RENAME(fifo_mirror)(AVBufferRef *dst, AVBufferRef *src)
{
    SNAME *dst_ctx = (SNAME *)dst->data;
    SNAME *src_ctx = (SNAME *)src->data;

    if (!dst || !src)
        return AVERROR(EINVAL);

    sp_bufferlist_append(dst_ctx->sources, src);
    sp_bufferlist_append(src_ctx->dests,   dst);

    return 0;
}

int RENAME(fifo_unmirror)(AVBufferRef *dst, AVBufferRef *src)
{
    SNAME *dst_ctx = (SNAME *)dst->data;
    SNAME *src_ctx = (SNAME *)src->data;

    AVBufferRef *dst_ref = sp_bufferlist_pop(src_ctx->dests, find_ref_by_data,
                                             dst->data);
    assert(dst_ref);
    av_buffer_unref(&dst_ref);

    AVBufferRef *src_ref = sp_bufferlist_pop(dst_ctx->sources, find_ref_by_data,
                                             src->data);
    assert(src_ref);
    av_buffer_unref(&src_ref);

    return 0;
}

int RENAME(fifo_unmirror_all)(AVBufferRef *dst)
{
    if (!dst)
        return 0;

    SNAME *dst_ctx = (SNAME *)dst->data;

    pthread_mutex_lock(&dst_ctx->lock);

    AVBufferRef *src_ref = NULL;
    while ((src_ref = sp_bufferlist_pop(dst_ctx->sources, sp_bufferlist_find_fn_first, NULL))) {
        SNAME *src_ctx = (SNAME *)src_ref->data;
        AVBufferRef *own_ref = sp_bufferlist_pop(src_ctx->dests, find_ref_by_data,
                                                 dst_ctx);
        av_buffer_unref(&own_ref);
        av_buffer_unref(&src_ref);
    }

    pthread_mutex_unlock(&dst_ctx->lock);

    return 0;
}

int RENAME(fifo_is_full)(AVBufferRef *src)
{
    if (!src)
        return 0;

    SNAME *ctx = (SNAME *)src->data;
    pthread_mutex_lock(&ctx->lock);
    int ret = 0; /* max_queued == -1 -> unlimited */
    if (!ctx->max_queued)
        ret = 1; /* max_queued = 0 -> always full */
    else if (ctx->max_queued > 0)
        ret = ctx->num_queued > (ctx->max_queued + 1);
    pthread_mutex_unlock(&ctx->lock);
    return ret;
}

int RENAME(fifo_get_size)(AVBufferRef *src)
{
    if (!src)
        return 0;

    SNAME *ctx = (SNAME *)src->data;
    pthread_mutex_lock(&ctx->lock);
    int ret = ctx->num_queued;
    pthread_mutex_unlock(&ctx->lock);
    return ret;
}

int RENAME(fifo_get_max_size)(AVBufferRef *src)
{
    if (!src)
        return INT_MAX;

    SNAME *ctx = (SNAME *)src->data;
    pthread_mutex_lock(&ctx->lock);
    int ret = ctx->max_queued == -1 ? INT_MAX : ctx->max_queued;
    pthread_mutex_unlock(&ctx->lock);
    return ret;
}

void RENAME(fifo_set_max_queued)(AVBufferRef *dst, int max_queued)
{
    SNAME *ctx = (SNAME *)dst->data;
    pthread_mutex_lock(&ctx->lock);
    ctx->max_queued = max_queued;
    pthread_mutex_unlock(&ctx->lock);
}

void RENAME(fifo_set_block_flags)(AVBufferRef *dst, FNAME block_flags)
{
    SNAME *ctx = (SNAME *)dst->data;
    pthread_mutex_lock(&ctx->lock);
    ctx->block_flags = block_flags;
    pthread_mutex_unlock(&ctx->lock);
}

int RENAME(fifo_push)(AVBufferRef *dst, TYPE *in)
{
    if (!dst)
        return 0;

    int err = 0;
    AVBufferRef *dist = NULL;

    SNAME *ctx = (SNAME *)dst->data;
    pthread_mutex_lock(&ctx->lock);

    if (ctx->max_queued == 0)
        goto distribute;

    /* Block or error, but only for non-NULL pushes */
    if (in && (ctx->max_queued != -1) &&
        (ctx->num_queued > (ctx->max_queued + 1))) {
        if (!(ctx->block_flags & FRENAME(BLOCK_MAX_OUTPUT))) {
            err = AVERROR(ENOBUFS);
            goto unlock;
        }

        pthread_cond_wait(&ctx->cond_out, &ctx->lock);
    }

    unsigned int oalloc = ctx->queued_alloc_size;
    TYPE **fq = av_fast_realloc(ctx->queued, &ctx->queued_alloc_size,
                                sizeof(TYPE *)*(ctx->num_queued + 1));
    if (!fq) {
        ctx->queued_alloc_size = oalloc;
        err = AVERROR(ENOMEM);
        goto unlock;
    }

    ctx->queued = fq;
    ctx->queued[ctx->num_queued++] = CLONE_FN(in);

    pthread_cond_signal(&ctx->cond_in);

distribute:
    while ((dist = sp_bufferlist_iter_ref(ctx->dests))) {
        int ret = RENAME(fifo_push)(dist, in);
        av_buffer_unref(&dist);
        if (ret == AVERROR(ENOMEM)) {
            sp_bufferlist_iter_halt(ctx->dests);
            err = ret;
            break;
        } else if (ret && !err) {
            err = ret;
        }
    }

unlock:
    pthread_mutex_unlock(&ctx->lock);

    return err;
}

TYPE *RENAME(fifo_pop)(AVBufferRef *src)
{
    if (!src)
        return NULL;

    TYPE *out = NULL;
    SNAME *ctx = (SNAME *)src->data;
    pthread_mutex_lock(&ctx->lock);

    if (!ctx->num_queued) {
        if (!(ctx->block_flags & FRENAME(BLOCK_NO_INPUT)))
            goto unlock;

        pthread_cond_wait(&ctx->cond_in, &ctx->lock);
    }

    out = ctx->queued[0];
    ctx->num_queued--;
    assert(ctx->num_queued >= 0);

    memmove(&ctx->queued[0], &ctx->queued[1], ctx->num_queued*sizeof(TYPE *));

    if (ctx->max_queued > 0)
        pthread_cond_signal(&ctx->cond_out);

unlock:
    pthread_mutex_unlock(&ctx->lock);

    return out;
}

TYPE *RENAME(fifo_peek)(AVBufferRef *src)
{
    if (!src)
        return NULL;

    TYPE *out = NULL;
    SNAME *ctx = (SNAME *)src->data;
    pthread_mutex_lock(&ctx->lock);

    if (!ctx->num_queued) {
        if (!(ctx->block_flags & FRENAME(BLOCK_NO_INPUT)))
            goto unlock;

        pthread_cond_wait(&ctx->cond_in, &ctx->lock);
    }

    out = CLONE_FN(ctx->queued[0]);

unlock:
    pthread_mutex_unlock(&ctx->lock);

    return out;
}
