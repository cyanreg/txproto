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

#include <stdbool.h>

#include <libavutil/buffer.h>
#include <libtxproto/log.h>
#include <libtxproto/utils.h>

typedef struct SNAME {
    TYPE **queued;
    int num_queued;
    int max_queued;
    FNAME block_flags;
    unsigned int queued_alloc_size;
    bool poked;
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
    if (!dst || !src)
        return AVERROR(EINVAL);

    SNAME *dst_ctx = (SNAME *)dst->data;
    SNAME *src_ctx = (SNAME *)src->data;

    void *src_class = av_buffer_get_opaque(src);
    const char *src_class_name = sp_class_get_name(src_class);

    if (sp_log_get_ctx_lvl(src_class_name) >= SP_LOG_VERBOSE) {
        void *dst_class = av_buffer_get_opaque(dst);
        const char *dst_class_name = sp_class_get_name(dst_class);
        const char *dst_class_type = sp_class_type_string(dst_class);
        const char *src_class_type = sp_class_type_string(src_class);

        sp_log(src_class, SP_LOG_VERBOSE,
               "Mirroring output FIFO from \"%s\" (%s) to \"%s\" (%s)\n",
               src_class_name ? src_class_name : "unknown",
               src_class_type ? src_class_type : "unknown",
               dst_class_name ? dst_class_name : "unknown",
               dst_class_type ? dst_class_type : "unknown");
    }

    sp_bufferlist_append(dst_ctx->sources, src);
    sp_bufferlist_append(src_ctx->dests,   dst);

    return 0;
}

int RENAME(fifo_unmirror)(AVBufferRef *dst, AVBufferRef *src)
{
    if (!dst || !src)
        return AVERROR(EINVAL);

    void *src_class = av_buffer_get_opaque(src);
    const char *src_class_name = sp_class_get_name(src_class);

    if (sp_log_get_ctx_lvl(src_class_name) >= SP_LOG_VERBOSE) {
        void *dst_class = av_buffer_get_opaque(dst);
        const char *dst_class_name = sp_class_get_name(dst_class);
        const char *dst_class_type = sp_class_type_string(dst_class);
        const char *src_class_type = sp_class_type_string(src_class);

        sp_log(src_class, SP_LOG_VERBOSE,
               "Unmirroring output FIFO from \"%s\" (%s) to \"%s\" (%s)\n",
               src_class_name ? src_class_name : "unknown",
               src_class_type ? src_class_type : "unknown",
               dst_class_name ? dst_class_name : "unknown",
               dst_class_type ? dst_class_type : "unknown");
    }

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

int RENAME(fifo_unmirror_all)(AVBufferRef *ref)
{
    if (!ref)
        return 0;

    void *ref_class = av_buffer_get_opaque(ref);
    const char *ref_class_name = sp_class_get_name(ref_class);
    enum SPLogLevel log_lvl = sp_log_get_ctx_lvl(ref_class_name);

    if (log_lvl >= SP_LOG_VERBOSE) {
        const char *ref_class_type = sp_class_type_string(ref_class);

        sp_log(ref_class, SP_LOG_VERBOSE, "Unmirroring all from \"%s\" (%s)...\n",
               ref_class_name ? ref_class_name : "unknown",
               ref_class_type ? ref_class_type : "unknown");
    }

    SNAME *ref_ctx = (SNAME *)ref->data;

    pthread_mutex_lock(&ref_ctx->lock);

    AVBufferRef *src_ref = NULL;
    while ((src_ref = sp_bufferlist_pop(ref_ctx->sources, sp_bufferlist_find_fn_first, NULL))) {
        SNAME *src_ctx = (SNAME *)src_ref->data;
        AVBufferRef *own_ref = sp_bufferlist_pop(src_ctx->dests, find_ref_by_data,
                                                 ref_ctx);
        if (log_lvl >= SP_LOG_VERBOSE) {
            void *src_class = av_buffer_get_opaque(src_ref);
            const char *src_class_name = sp_class_get_name(src_class);
            const char *src_class_type = sp_class_type_string(src_class);

            sp_log(ref_class, SP_LOG_VERBOSE, " ...from source \"%s\" (%s)\n",
                   src_class_name ? src_class_name : "unknown",
                   src_class_type ? src_class_type : "unknown");
        }
        av_buffer_unref(&own_ref);
        av_buffer_unref(&src_ref);
    }

    AVBufferRef *dst_ref = NULL;
    while ((dst_ref = sp_bufferlist_pop(ref_ctx->dests, sp_bufferlist_find_fn_first, NULL))) {
        SNAME *dst_ctx = (SNAME *)dst_ref->data;
        AVBufferRef *own_ref = sp_bufferlist_pop(dst_ctx->sources, find_ref_by_data,
                                                 ref_ctx);
        if (log_lvl >= SP_LOG_VERBOSE) {
            void *dst_class = av_buffer_get_opaque(dst_ref);
            const char *dst_class_name = sp_class_get_name(dst_class);
            const char *dst_class_type = sp_class_type_string(dst_class);

            sp_log(ref_class, SP_LOG_VERBOSE, " ...from dest \"%s\" (%s)\n",
                   dst_class_name ? dst_class_name : "unknown",
                   dst_class_type ? dst_class_type : "unknown");
        }

        /* unblock anyone pulling this dest */
        pthread_cond_signal(&dst_ctx->cond_in);

        av_buffer_unref(&own_ref);
        av_buffer_unref(&dst_ref);
    }

    pthread_mutex_unlock(&ref_ctx->lock);

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

// convert a lowercase, comma-separated list of block flags to actual flags
int RENAME(fifo_string_to_block_flags)(FNAME *dst, const char *in_str)
{
    *dst = 0;
    int err = 0;
    char *saveptr;
    char *copy = strdup(in_str);
    char *ptr = strtok_r(copy, ",", &saveptr);
    while (ptr != NULL) {
        if (!strcmp(ptr, "block_no_input")) {
            *dst |= FRENAME(BLOCK_NO_INPUT);
        } else if (!strcmp(ptr, "block_max_output")) {
            *dst |= FRENAME(BLOCK_MAX_OUTPUT);
        } else if (!strcmp(ptr, "pull_no_block")) {
            *dst |= FRENAME(PULL_NO_BLOCK);
        } else {
            err = AVERROR(EINVAL); // error
            goto end;
        }
        ptr = strtok_r(NULL, ",", &saveptr);
    }
end:
    free(copy);
    return err;
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

int RENAME(fifo_poke)(AVBufferRef *ref)
{
    SNAME *ctx = (SNAME *)ref->data;
    void *ref_class = av_buffer_get_opaque(ref);
    const char *ref_class_name = sp_class_get_name(ref_class);
    const char *ref_class_type = sp_class_type_string(ref_class);
    sp_log(ref_class, SP_LOG_VERBOSE, "Poking FIFO \"%s\" (%s)...\n",
           ref_class_name ? ref_class_name : "unknown",
           ref_class_type ? ref_class_type : "unknown");
    pthread_mutex_lock(&ctx->lock);
    ctx->poked = true;
    pthread_mutex_unlock(&ctx->lock);
    pthread_cond_signal(&ctx->cond_in);
    return 0;
}

static int PRIV_RENAME(fifo_pull_flags_template)(AVBufferRef *src, TYPE **dst,
                                                 FNAME flags, int pop)
{
    int ret = 0;

    if (!src) {
        *dst = NULL;
        return 0;
    }

    TYPE *out = NULL;
    SNAME *ctx = (SNAME *)src->data;
    pthread_mutex_lock(&ctx->lock);

    int pull_poke = flags & FRENAME(PULL_POKE);
    int pull_no_block = flags & FRENAME(PULL_NO_BLOCK);

    while (!ctx->num_queued) {
        /* this one might change while we wait for `cond_in` */
        int block_no_input = ctx->block_flags & FRENAME(BLOCK_NO_INPUT);

        if (!block_no_input || pull_no_block) {
            ret = AVERROR(EAGAIN);
            goto unlock;
        }

        if (!ctx->poked) {
            pthread_cond_wait(&ctx->cond_in, &ctx->lock);
        }

        /* if the `PULL_POKE` flag is set, return on poke */
        if (pull_poke && ctx->poked) {
            ctx->poked = false;
            ret = AVERROR(EAGAIN);
            goto unlock;
        }
        ctx->poked = false;
    }

    if (pop) {
        out = ctx->queued[0];
        ctx->num_queued--;
        assert(ctx->num_queued >= 0);

        memmove(&ctx->queued[0], &ctx->queued[1], ctx->num_queued*sizeof(TYPE *));

        if (ctx->max_queued > 0)
            pthread_cond_signal(&ctx->cond_out);
    } else {
        out = CLONE_FN(ctx->queued[0]);
    }

unlock:
    pthread_mutex_unlock(&ctx->lock);

    *dst = out;

    return ret;
}

int RENAME(fifo_pop_flags)(AVBufferRef *src, TYPE **dst, FNAME flags)
{
    return PRIV_RENAME(fifo_pull_flags_template)(src, dst, flags, 1);
}

TYPE *RENAME(fifo_pop)(AVBufferRef *src)
{
    TYPE *val = NULL;
    PRIV_RENAME(fifo_pull_flags_template)(src, &val, 0x0, 1);
    return val;
}

int RENAME(fifo_peek_flags)(AVBufferRef *src, TYPE **dst, FNAME flags)
{
    return PRIV_RENAME(fifo_pull_flags_template)(src, dst, flags, 0);
}

TYPE *RENAME(fifo_peek)(AVBufferRef *src)
{
    TYPE *val = NULL;
    PRIV_RENAME(fifo_pull_flags_template)(src, &val, 0x0, 0);
    return val;
}

#undef TYPE
#undef CLONE_FN
#undef FREE_FN
#undef SNAME
#undef FNAME
#undef PRIV_RENAME
#undef RENAME
#undef FRENAME
