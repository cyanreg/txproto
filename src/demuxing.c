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

#include <libavutil/time.h>
#include <libavutil/avstring.h>

#include <libtxproto/demuxing.h>

#include "utils.h"
#include "ctrl_template.h"

static void *demuxing_thread(void *arg)
{
    int err;
    DemuxingContext *ctx = arg;

    sp_set_thread_name_self(sp_class_get_name(ctx));

    sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_INIT, NULL);

    sp_log(ctx, SP_LOG_VERBOSE, "Muxer initialized!\n");

    while (1) {
        AVPacket *out_packet = av_packet_alloc();

        err = av_read_frame(ctx->avf, out_packet);
        if (err == AVERROR(EOF)) {
            for (int i = 0; i < ctx->avf->nb_streams; i++)
                sp_packet_fifo_push(ctx->dst_packets[i], NULL);

            sp_log(ctx, SP_LOG_VERBOSE, "Stream EOF, FIFOs flushed!\n");
            break;
        } else if (err < 0) {
            sp_log(ctx, SP_LOG_ERROR, "Failed to read packet: %s\n", av_err2str(err));
            goto fail;
        }

        sp_log(ctx, SP_LOG_TRACE, "Sending packet from stream %i\n", out_packet->stream_index);
        sp_packet_fifo_push(ctx->dst_packets[out_packet->stream_index], out_packet);

        av_packet_free(&out_packet);
    }

fail:
    return NULL;
}

static int demuxer_ioctx_ctrl_cb(AVBufferRef *event_ref, void *callback_ctx,
                                 void *_ctx, void *dep_ctx, void *data)
{
    SPCtrlTemplateCbCtx *event = callback_ctx;
    DemuxingContext *ctx = _ctx;

    if (event->ctrl & SP_EVENT_CTRL_START) {
        ctx->epoch = atomic_load(event->epoch);
        if (!ctx->demuxing_thread)
            pthread_create(&ctx->demuxing_thread, NULL, demuxing_thread, ctx);
    } else if (event->ctrl & SP_EVENT_CTRL_STOP) {
        pthread_join(ctx->demuxing_thread, NULL);
    } else if (event->ctrl & SP_EVENT_CTRL_OPTS) {

    } else if (event->ctrl & SP_EVENT_CTRL_FLUSH) {
        sp_log(ctx, SP_LOG_VERBOSE, "Flushing buffer\n");
        pthread_mutex_lock(&ctx->lock);
        avio_flush(ctx->avf->pb);
        pthread_mutex_unlock(&ctx->lock);
    } else {
        return AVERROR(ENOTSUP);
    }

    return 0;
}

int sp_demuxer_ctrl(AVBufferRef *ctx_ref, SPEventType ctrl, void *arg)
{
    DemuxingContext *ctx = (DemuxingContext *)ctx_ref->data;
    return sp_ctrl_template(ctx, ctx->events, 0x0, demuxer_ioctx_ctrl_cb, ctrl, arg);
}

int sp_demuxer_init(AVBufferRef *ctx_ref)
{
    int err;
    DemuxingContext *ctx = (DemuxingContext *)ctx_ref->data;

    if (!ctx->in_format) {
        if (!strncmp(ctx->in_url, "/dev/video", strlen("/dev/video")))
            ctx->in_format = "v4l2";
        if (!strncmp(ctx->in_url, "rtmp://", strlen("rtmp://")))
            ctx->in_format = "flv";
        if (!strncmp(ctx->in_url, "udp://", strlen("udp://")))
            ctx->in_format = "mpegts";
        if (!strncmp(ctx->in_url, "http://", strlen("http://")))
            ctx->in_format = "dash";
    }

    err = avformat_open_input(&ctx->avf, ctx->in_url, NULL, &ctx->start_options);
    if (err < 0) {
        sp_log(ctx, SP_LOG_ERROR, "Couldn't initialize demuxer: %s!\n", av_err2str(err));
        goto fail;
    }

    if (!ctx->name) {
        int len = strlen(sp_class_get_name(ctx)) + 1 + strlen(ctx->avf->iformat->name) + 1;
        char *new_name = av_mallocz(len);
        if (!new_name) {
            err = AVERROR(ENOMEM);
            goto fail;
        }
        av_strlcpy(new_name, sp_class_get_name(ctx), len);
        av_strlcat(new_name, ":", len);
        av_strlcat(new_name, ctx->avf->iformat->name, len);
        sp_class_set_name(ctx, new_name);
        av_free(new_name);
        ctx->name = sp_class_get_name(ctx);
    } else {
        sp_class_set_name(ctx, ctx->name);
    }

    err = avformat_find_stream_info(ctx->avf, NULL);
    if (err < 0) {
        sp_log(ctx, SP_LOG_ERROR, "Couldn't find stream info: %s!\n", av_err2str(err));
        goto fail;
    }

    ctx->dst_packets = av_mallocz(ctx->avf->nb_streams*sizeof(ctx->dst_packets));
    for (int i = 0; i < ctx->avf->nb_streams; i++)
        ctx->dst_packets[i] = sp_packet_fifo_create(ctx, 10, PACKET_FIFO_BLOCK_MAX_OUTPUT | PACKET_FIFO_BLOCK_NO_INPUT);

    /* Both fields alive for the duration of the avf context */
    ctx->in_format = ctx->avf->iformat->name;
    ctx->in_url = ctx->avf->url;

    return 0;

fail:
    avformat_free_context(ctx->avf);
    return err;
}

static void demuxer_free(void *opaque, uint8_t *data)
{
    DemuxingContext *ctx = (DemuxingContext *)data;

    if (ctx->demuxing_thread)
        pthread_join(ctx->demuxing_thread, NULL);

    for (int i = 0; i < ctx->avf->nb_streams; i++)
        av_buffer_unref(&ctx->dst_packets[i]);

    sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_DESTROY, NULL);
    sp_bufferlist_free(&ctx->events);

    avformat_close_input(&ctx->avf);

    pthread_mutex_destroy(&ctx->lock);

    av_free(ctx->dst_packets);

    sp_log(ctx, SP_LOG_VERBOSE, "Demuxer destroyed!\n");
    sp_class_free(ctx);
    av_free(ctx);
}

AVBufferRef *sp_demuxer_alloc(void)
{
    DemuxingContext *ctx = av_mallocz(sizeof(DemuxingContext));
    if (!ctx)
        return NULL;

    AVBufferRef *ctx_ref = av_buffer_create((uint8_t *)ctx, sizeof(*ctx),
                                            demuxer_free, NULL, 0);

    int err = sp_class_alloc(ctx, "lavf", SP_TYPE_DEMUXER, NULL);
    if (err < 0) {
        av_buffer_unref(&ctx_ref);
        return NULL;
    }

    pthread_mutex_init(&ctx->lock, NULL);
    ctx->events = sp_bufferlist_new();

    return ctx_ref;
}
