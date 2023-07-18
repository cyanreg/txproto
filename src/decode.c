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

#include <libtxproto/decode.h>

#include <pthread.h>
#include <libavutil/avstring.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>

#include "ctrl_template.h"

int sp_decoding_connect(DecodingContext *dec, DemuxingContext *mux,
                        int stream_id, char *stream_desc)
{
    int err;
    int idx = -1;

    if (stream_id >= 0) {
has_stream_id:
        idx = stream_id;
        if (stream_id > (mux->avf->nb_streams - 1)) {
            sp_log(dec, SP_LOG_ERROR, "Invalid stream ID %i, demuxer only has %i streams!\n",
                   stream_id, mux->avf->nb_streams);
            return AVERROR(EINVAL);
        }
    } else if (stream_desc) {
#define FIND_STREAM(prefix_str, media_type)                                     \
        if (!strncmp(stream_desc, prefix_str, strlen(prefix_str))) {            \
            int cnt = 0;                                                        \
            stream_id = strtol(stream_desc + strlen(prefix_str), NULL, 10);     \
            for (int i = 0; i < mux->avf->nb_streams; i++) {                    \
                if (mux->avf->streams[i]->codecpar->codec_type == media_type) { \
                    if (stream_id == cnt) {                                     \
                        stream_id = i;                                          \
                        goto has_stream_id;                                     \
                    }                                                           \
                    cnt++;                                                      \
                }                                                               \
            }                                                                   \
            goto has_stream_id;                                                 \
        }

        FIND_STREAM("video=", AVMEDIA_TYPE_VIDEO)
        FIND_STREAM("vid=", AVMEDIA_TYPE_VIDEO)
        FIND_STREAM("audio=", AVMEDIA_TYPE_VIDEO)
        FIND_STREAM("aid=", AVMEDIA_TYPE_VIDEO)
        FIND_STREAM("subtitle=", AVMEDIA_TYPE_VIDEO)
        FIND_STREAM("sub=", AVMEDIA_TYPE_VIDEO)

        for (int i = 0; i < mux->avf->nb_streams; i++) {
            AVDictionaryEntry *d = av_dict_get(mux->avf->streams[i]->metadata, "title", NULL, 0);
            if (!d)
                continue;
            if (!strcmp(d->value, stream_desc)) {
                stream_id = i;
                break;
            }
        }

        sp_log(dec, SP_LOG_ERROR, "Unable to find stream with title \"%s\"\n",
               stream_desc);
        return AVERROR(EINVAL);
    } else if (mux->avf->nb_streams == 1) {
        idx = 0;
    } else {
        sp_log(dec, SP_LOG_ERROR, "No stream ID or description specified for searching!\n");
        return AVERROR(EINVAL);
    }

    AVStream *st = mux->avf->streams[idx];

    if (st->codecpar->extradata_size) {
        dec->avctx->extradata = av_mallocz(st->codecpar->extradata_size +
                                            AV_INPUT_BUFFER_PADDING_SIZE);
        if (!dec->avctx->extradata)
            return AVERROR(ENOMEM);

        memcpy(dec->avctx->extradata, st->codecpar->extradata,
               st->codecpar->extradata_size);
        dec->avctx->extradata_size = st->codecpar->extradata_size;
    }

    dec->avctx->time_base = mux->avf->streams[idx]->time_base;

    err = avcodec_parameters_to_context(dec->avctx, st->codecpar);
    if (err < 0) {
    	sp_log(dec, SP_LOG_ERROR, "Cannot copy coder parameters: %s!\n", av_err2str(err));
        return err;
    }

    if (dec->low_latency) {
        printf("ASSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSS\n");
        dec->avctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    }

    err = avcodec_open2(dec->avctx, dec->codec, NULL);
    if (err < 0) {
    	sp_log(dec, SP_LOG_ERROR, "Cannot open decoder: %s!\n", av_err2str(err));
    	return err;
    }

    sp_log(dec, SP_LOG_VERBOSE, "Linked to demuxer %s (tb: %i/%i)\n",
           sp_class_get_name(dec),
           dec->avctx->time_base.num, dec->avctx->time_base.den);

    dec->src_packets = av_buffer_ref(mux->dst_packets[idx]);

    return 0;
}

static int context_full_config(DecodingContext *ctx)
{
    int err;

    ctx->avctx = avcodec_alloc_context3(ctx->codec);
    if (!ctx->avctx)
        return AVERROR(ENOMEM);

    err = sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_CONFIG, NULL);
    if (err < 0)
        return err;

    sp_log(ctx, SP_LOG_VERBOSE, "Decoder configured!\n");

    return 0;
}

static void *decoding_thread(void *arg)
{
    DecodingContext *ctx = arg;
    int ret = 0, flush = 0;

    sp_set_thread_name_self(sp_class_get_name(ctx));

    sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_INIT, NULL);

    sp_log(ctx, SP_LOG_VERBOSE, "Decoder initialized!\n");

    do {
        pthread_mutex_lock(&ctx->lock);

        AVPacket *packet = NULL;

        if (!flush) {
            packet = sp_packet_fifo_pop(ctx->src_packets);
            flush = !packet;
        }

        /* Give packet */
        ret = avcodec_send_packet(ctx->avctx, packet);
        av_packet_free(&packet);
        if (ret < 0) {
            sp_log(ctx, SP_LOG_ERROR, "Error decoding: %s!\n", av_err2str(ret));
            pthread_mutex_unlock(&ctx->lock);
            goto fail;
        }

        /* Return */
        while (1) {
            AVFrame *out_frame = av_frame_alloc();

            ret = avcodec_receive_frame(ctx->avctx, out_frame);
            if (ret == AVERROR_EOF) {
                pthread_mutex_unlock(&ctx->lock);
                av_frame_free(&out_frame);
                ret = 0;
                goto end;
            } else if (ret == AVERROR(EAGAIN)) {
                av_frame_free(&out_frame);
                ret = 0;
                break;
            } else if (ret < 0) {
                pthread_mutex_unlock(&ctx->lock);
                sp_log(ctx, SP_LOG_ERROR, "Error decoding: %s!\n", av_err2str(ret));
                av_frame_free(&out_frame);
                goto fail;
            }

            if (!ctx->start_pts)
                ctx->start_pts = out_frame->pts;

            out_frame->pts -= ctx->start_pts;

            out_frame->opaque_ref = av_buffer_allocz(sizeof(FormatExtraData));

            FormatExtraData *fe  = (FormatExtraData *)out_frame->opaque_ref->data;
            fe->time_base        = ctx->avctx->time_base;
            fe->bits_per_sample  = 32;
            out_frame->time_base = fe->time_base;

            sp_log(ctx, SP_LOG_TRACE, "Pushing frame to FIFO, pts = %f\n",
                   av_q2d(out_frame->time_base) * out_frame->pts);

            sp_frame_fifo_push(ctx->dst_frames, out_frame);
            av_frame_free(&out_frame);
        }

        pthread_mutex_unlock(&ctx->lock);
    } while (!ctx->err);

end:
    sp_log(ctx, SP_LOG_VERBOSE, "Stream flushed!\n");

    return NULL;

fail:
    ctx->err = ret;

    atomic_store(&ctx->running, 0);
    return NULL;
}

static int decoder_ioctx_ctrl_cb(AVBufferRef *event_ref, void *callback_ctx,
                                 void *_ctx, void *dep_ctx, void *data)
{
    SPCtrlTemplateCbCtx *event = callback_ctx;
    DecodingContext *ctx = _ctx;

    if (event->ctrl & SP_EVENT_CTRL_START) {
        if (!sp_eventlist_has_dispatched(ctx->events, SP_EVENT_ON_CONFIG)) {
            int ret = context_full_config(ctx);
            if (ret < 0)
                return ret;
        }
        ctx->epoch = atomic_load(event->epoch);
        if (!ctx->decoding_thread)
            pthread_create(&ctx->decoding_thread, NULL, decoding_thread, ctx);
    } else if (event->ctrl & SP_EVENT_CTRL_OPTS) {
        const char *tmp_val = NULL;
        if ((tmp_val = dict_get(event->opts, "low_latency")))
            if (!strcmp(tmp_val, "true") || strtol(tmp_val, NULL, 10) != 0)
                ctx->low_latency = 1;
    } else if (event->ctrl & SP_EVENT_CTRL_STOP) {
        if (ctx->decoding_thread) {
            sp_packet_fifo_push(ctx->src_packets, NULL);
            pthread_join(ctx->decoding_thread, NULL);
            ctx->decoding_thread = 0;
        }
    } else {
        return AVERROR(ENOTSUP);
    }

    return 0;
}

int sp_decoder_ctrl(AVBufferRef *ctx_ref, SPEventType ctrl, void *arg)
{
    DecodingContext *ctx = (DecodingContext *)ctx_ref->data;
    return sp_ctrl_template(ctx, ctx->events, 0x0,
                            decoder_ioctx_ctrl_cb, ctrl, arg);
}

int sp_decoder_init(AVBufferRef *ctx_ref)
{
    int err;
    DecodingContext *ctx = (DecodingContext *)ctx_ref->data;

    ctx->avctx = avcodec_alloc_context3(ctx->codec);
    if (!ctx->avctx)
        return AVERROR(ENOMEM);

    char *new_name = NULL;
    if (ctx->name) {
        new_name = av_strdup(ctx->name);
        if (!new_name) {
            err = AVERROR(ENOMEM);
            goto fail;
        }
    } else {
        int len = strlen(sp_class_get_name(ctx)) + 1 + strlen(ctx->codec->name) + 1;
        new_name = av_mallocz(len);
        if (!new_name) {
            err = AVERROR(ENOMEM);
            goto fail;
        }
        av_strlcpy(new_name, sp_class_get_name(ctx), len);
        av_strlcat(new_name, ":", len);
        av_strlcat(new_name, ctx->codec->name, len);
    }

    sp_class_set_name(ctx, new_name);
    ctx->name = new_name;

    return 0;

fail:
    avcodec_free_context(&ctx->avctx);
    return err;
}

static void decoder_free(void *opaque, uint8_t *data)
{
    DecodingContext *ctx = (DecodingContext *)data;

    sp_frame_fifo_unmirror_all(ctx->src_packets);
    sp_frame_fifo_unmirror_all(ctx->dst_frames);

    if (ctx->decoding_thread) {
        sp_packet_fifo_push(ctx->src_packets, NULL);
        pthread_join(ctx->decoding_thread, NULL);
    }

    av_buffer_unref(&ctx->src_packets);
    av_buffer_unref(&ctx->dst_frames);

    sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_DESTROY, NULL);
    sp_bufferlist_free(&ctx->events);

    avcodec_free_context(&ctx->avctx);

    pthread_mutex_destroy(&ctx->lock);

    sp_log(ctx, SP_LOG_VERBOSE, "Decoder destroyed!\n");
    sp_class_free(ctx);
}

AVBufferRef *sp_decoder_alloc(void)
{
    DecodingContext *ctx = av_mallocz(sizeof(DecodingContext));
    if (!ctx)
        return NULL;

    AVBufferRef *ctx_ref = av_buffer_create((uint8_t *)ctx, sizeof(*ctx),
                                            decoder_free, NULL, 0);

    int err = sp_class_alloc(ctx, "lavc", SP_TYPE_DECODER, NULL);
    if (err < 0) {
        av_buffer_unref(&ctx_ref);
        return NULL;
    }

    pthread_mutex_init(&ctx->lock, NULL);
    ctx->events = sp_bufferlist_new();

    ctx->src_packets = sp_packet_fifo_create(ctx, 8, PACKET_FIFO_BLOCK_NO_INPUT);
    ctx->dst_frames = sp_frame_fifo_create(ctx, 0, 0);

    return ctx_ref;
}
