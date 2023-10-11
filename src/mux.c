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

#include <libtxproto/mux.h>

#include <libtxproto/utils.h>
#include "utils.h"
#include "ctrl_template.h"

typedef struct MuxEncoderMap {
    intptr_t encoder_id;
    int stream_index;
    char *name;
} MuxEncoderMap;

static MuxEncoderMap *enc_id_lookup(MuxingContext *ctx, intptr_t enc_id)
{
    for (int i = 0; i < ctx->enc_map_size; i++)
        if (ctx->enc_map[i].encoder_id == enc_id)
            return &ctx->enc_map[i];
    return NULL;
}

static void *muxing_thread(void *arg)
{
    int err = 0;
    MuxingContext *ctx = arg;
    SPGenericData *stat_entries = NULL;
    int nb_stat_entries = 0;

    /* Stream stats */
    SlidingWinCtx *sctx_rate = av_mallocz(ctx->avf->nb_streams * sizeof(*sctx_rate));
    SlidingWinCtx *sctx_latency = av_mallocz(ctx->avf->nb_streams * sizeof(*sctx_latency));
    int64_t *rate = av_mallocz(ctx->avf->nb_streams * sizeof(*rate));
    int64_t *latency = av_mallocz(ctx->avf->nb_streams * sizeof(*latency));

    sp_set_thread_name_self(sp_class_get_name(ctx));

    sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_INIT, NULL);

    if (ctx->dump_sdp_file) {
        char sdp_data[16384];
        err = av_sdp_create(&ctx->avf, 1, sdp_data, sizeof(sdp_data));
        if (err < 0) {
            sp_log(ctx, SP_LOG_WARN, "Unable to create an SDP: %s!\n", av_err2str(err));
        } else {
            FILE *sdp_file = av_fopen_utf8(ctx->dump_sdp_file, "w");
            if (!sdp_file) {
                sp_log(ctx, SP_LOG_WARN, "Unable to open SDP file for writing: %s!\n",
                av_err2str(AVERROR(errno)));
            } else {
                size_t sdp_len = strlen(sdp_data) + 1;
                fwrite(sdp_data, sdp_len, 1, sdp_file);
                fclose(sdp_file);
                sp_log(ctx, SP_LOG_INFO, "SDP file written to %s (%lu bytes)\n",
                       ctx->dump_sdp_file, sdp_len);
            }
        }
    }

    if (ctx->dump_info)
        av_dump_format(ctx->avf, 0, ctx->out_url, 1);

    int flush = 0;
    int fmt_can_flush = ctx->avf->oformat->flags & AVFMT_ALLOW_FLUSH;

    /* Mux stats */
    SlidingWinCtx sctx_mux = { 0 };
    int64_t last_pos_update = av_gettime_relative();
    int64_t mux_rate = 0;
    int64_t last_pos = ctx->avf->pb->pos;
    int64_t buf_bytes = 0;

    sp_log(ctx, SP_LOG_VERBOSE, "Muxer initialized!\n");

    while (1) {
        AVPacket *in_pkt = NULL;
        pthread_mutex_lock(&ctx->lock);

        if (!flush) {
            in_pkt = sp_packet_fifo_pop(ctx->src_packets);
            flush = !in_pkt;

            /* Format can't flush, so just exit */
            if (flush && !fmt_can_flush) {
                pthread_mutex_unlock(&ctx->lock);
                break;
            }
        }

        if (flush)
            goto send;

        AVRational src_tb = in_pkt->time_base;

        MuxEncoderMap *src_enc = enc_id_lookup(ctx, (intptr_t)in_pkt->opaque);
        int sidx = src_enc->stream_index;

        in_pkt->stream_index = sidx;

        AVRational dst_tb = ctx->avf->streams[sidx]->time_base;
        SlidingWinCtx *rate_c = &sctx_rate[sidx];
        SlidingWinCtx *latency_c = &sctx_latency[sidx];

        rate[sidx] = sp_sliding_win(rate_c, in_pkt->size, in_pkt->pts, src_tb, src_tb.den, 0) << 3;

        latency[sidx]  = av_gettime_relative() - ctx->epoch;
        latency[sidx] -= av_rescale_q(in_pkt->pts, src_tb, av_make_q(1, 1000000));
        latency[sidx]  = sp_sliding_win(latency_c, latency[sidx], in_pkt->pts, src_tb, src_tb.den, 1);

        /* Rescale timestamps */
        in_pkt->pts = av_rescale_q(in_pkt->pts, src_tb, dst_tb);
        in_pkt->dts = av_rescale_q(in_pkt->dts, src_tb, dst_tb);
        in_pkt->duration = av_rescale_q(in_pkt->duration, src_tb, dst_tb);

        sp_log(ctx, SP_LOG_TRACE, "Got packet from \"%s\", sidx = %i, out pts = %f, out_dts = %f\n",
               src_enc->name,
               sidx,
               av_q2d(dst_tb) * in_pkt->pts,
               av_q2d(dst_tb) * in_pkt->dts);

        buf_bytes = ctx->avf->pb->buf_ptr - ctx->avf->pb->buffer;
        if (last_pos != ctx->avf->pb->pos) {
            int64_t t_delta, cur_time = av_gettime_relative();
            mux_rate = (ctx->avf->pb->pos - last_pos) << 3;
            t_delta = cur_time - last_pos_update;
            mux_rate = av_rescale(mux_rate, 1000000, t_delta);
            last_pos_update = cur_time;
            last_pos = ctx->avf->pb->pos;
            mux_rate = sp_sliding_win(&sctx_mux, mux_rate, cur_time, av_make_q(1, 1000000),
                                      10000000, 1);
        }

send:
        err = av_interleaved_write_frame(ctx->avf, in_pkt);
        av_packet_free(&in_pkt);

        if (err == AVERROR(ETIMEDOUT)) {
            sp_log(ctx, SP_LOG_ERROR, "Error muxing, operation timed out!\n");
            pthread_mutex_unlock(&ctx->lock);
            continue;
        } else if (err < 0) {
            sp_log(ctx, SP_LOG_ERROR, "Error muxing: %s!\n", av_err2str(err));
            pthread_mutex_unlock(&ctx->lock);
	    	goto fail;
        }

        if (flush) {
            pthread_mutex_unlock(&ctx->lock);
            break;
        }

        int entries = 2 + 2*ctx->avf->nb_streams;
        stat_entries = av_fast_realloc(stat_entries, &nb_stat_entries, sizeof(*stat_entries) * entries);

        stat_entries[0] = D_TYPE("bitrate", NULL, mux_rate);
        stat_entries[1] = D_TYPE("cached", NULL, buf_bytes);

        for (int i = 0; i < ctx->avf->nb_streams; i++) {
            stat_entries[2 + 2*i + 0] = D_TYPE("bitrate", src_enc->name, rate[i]);
            stat_entries[2 + 2*i + 1] = D_TYPE("latency", src_enc->name, latency[i]);
        }

        stat_entries[2 + 2*ctx->avf->nb_streams] = (SPGenericData){ 0 };

        sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_STATS, stat_entries);

        pthread_mutex_unlock(&ctx->lock);
    }

    pthread_mutex_lock(&ctx->lock);

fail:
    av_free(sctx_rate);
    av_free(sctx_latency);
    av_free(rate);
    av_free(latency);
    av_free(stat_entries);

    pthread_mutex_unlock(&ctx->lock);

    ctx->err = err;

    sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_EOS, &err);

    return NULL;
}

int sp_muxer_add_stream(MuxingContext *ctx, EncodingContext *enc)
{
    int err = 0;

    pthread_mutex_lock(&ctx->lock);

    MuxEncoderMap *enc_map_entry = NULL;
    for (int i = 0; i < ctx->enc_map_size; i++) {
        if (ctx->enc_map[i].encoder_id == sp_class_get_id(enc)) {
            enc_map_entry = &ctx->enc_map[i];
            break;
        }
    }

    if (!enc_map_entry) {
        MuxEncoderMap *enc_map = av_realloc(ctx->enc_map, sizeof(*enc_map) * (ctx->enc_map_size + 1));
        if (!enc_map)
            return AVERROR(ENOMEM);
        ctx->enc_map = enc_map;
        enc_map_entry = &ctx->enc_map[ctx->enc_map_size];
        ctx->enc_map_size++;
    }

    enc_map_entry->encoder_id = (intptr_t)sp_class_get_id(enc);

    if (0) {

    } else {
        ctx->stream_has_link = av_realloc(ctx->stream_has_link, sizeof(*ctx->stream_has_link) * (ctx->avf->nb_streams + 1));
        ctx->stream_codec_id = av_realloc(ctx->stream_codec_id, sizeof(*ctx->stream_codec_id) * (ctx->avf->nb_streams + 1));

        AVStream *st = avformat_new_stream(ctx->avf, enc->codec);
        if (!st) {
            sp_log(ctx, SP_LOG_ERROR, "Unable to allocate stream!\n");
            err = AVERROR(ENOMEM);
            goto end;
        }

        err = avcodec_parameters_from_context(st->codecpar, enc->avctx);
        if (err < 0) {
            sp_log(ctx, SP_LOG_ERROR, "Could not copy codec params: %s!\n", av_err2str(err));
            goto end;
        }

        st->time_base = enc->avctx->time_base;

        enc_map_entry->stream_index = ctx->avf->nb_streams - 1;
        enc_map_entry->name = av_strdup(enc->name);

        ctx->stream_has_link[st->id] = 1;
        ctx->stream_codec_id[st->id] = enc->avctx->codec_id;

        /* Set stream metadata */
        int enc_str_len = sizeof(LIBAVCODEC_IDENT) + 1 + strlen(enc->avctx->codec->name) + 1;
        char *enc_str = av_mallocz(enc_str_len);
        av_strlcpy(enc_str, LIBAVCODEC_IDENT " ", enc_str_len);
        av_strlcat(enc_str, enc->avctx->codec->name, enc_str_len);
        av_dict_set(&st->metadata, "encoder", enc_str, AV_DICT_DONT_STRDUP_VAL);

        /* Set SAR */
        if (enc->avctx->codec->type == AVMEDIA_TYPE_VIDEO) {
            st->avg_frame_rate      = enc->avctx->framerate;
            st->sample_aspect_ratio = enc->avctx->sample_aspect_ratio;
        }

        sp_log(ctx, SP_LOG_VERBOSE, "Encoder \"%s\" registered, stream index %i!\n",
               enc_map_entry->name, st->id);
    }

end:
    pthread_mutex_unlock(&ctx->lock);

    return err;
}

static int configure_muxer(MuxingContext *ctx)
{
    int ret;

    ret = sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_CONFIG, NULL);
    if (ret < 0)
        return ret;

    ctx->avf->flags |= AVFMT_FLAG_AUTO_BSF;

    if (ctx->low_latency) {
        ctx->avf->flags |= AVFMT_FLAG_FLUSH_PACKETS | AVFMT_FLAG_NOBUFFER;
        ctx->avf->pb->min_packet_size = 0;
    }

    ret = avformat_write_header(ctx->avf, NULL);
    if (ret) {
        sp_log(ctx, SP_LOG_ERROR, "Could not write header: %s!\n", av_err2str(ret));
        return ret;
    }

    sp_log(ctx, SP_LOG_VERBOSE, "Muxer configured!\n");

    return 0;
}

static int muxer_ioctx_ctrl_cb(AVBufferRef *event_ref, void *callback_ctx,
                               void *_ctx, void *dep_ctx, void *data)
{
    SPCtrlTemplateCbCtx *event = callback_ctx;
    MuxingContext *ctx = _ctx;

    if (event->ctrl & SP_EVENT_CTRL_START) {
        if (!sp_eventlist_has_dispatched(ctx->events, SP_EVENT_ON_CONFIG)) {
            int ret = configure_muxer(ctx);
            if (ret < 0)
                return ret;
        }
        ctx->epoch = atomic_load(event->epoch);
        if (!ctx->muxing_thread)
            pthread_create(&ctx->muxing_thread, NULL, muxing_thread, ctx);
    } else if (event->ctrl & SP_EVENT_CTRL_STOP) {
        if (ctx->muxing_thread) {
            sp_packet_fifo_push(ctx->src_packets, NULL);
            pthread_join(ctx->muxing_thread, NULL);
            ctx->muxing_thread = 0;
        }
    } else if (event->ctrl & SP_EVENT_CTRL_OPTS) {
        pthread_mutex_lock(&ctx->lock);
        const char *tmp_val = NULL;
        if ((tmp_val = dict_get(event->opts, "low_latency")))
            if (!strcmp(tmp_val, "true") || strtol(tmp_val, NULL, 10) != 0)
                ctx->low_latency = 1;
        if ((tmp_val = dict_get(event->opts, "dump_info")))
            if (!strcmp(tmp_val, "true") || strtol(tmp_val, NULL, 10) != 0)
                ctx->dump_info = 1;
        if ((tmp_val = dict_get(event->opts, "sdp_file")))
            ctx->dump_sdp_file = av_strdup(tmp_val);
        if ((tmp_val = dict_get(event->opts, "fifo_size"))) {
            long int len = strtol(tmp_val, NULL, 10);
            if (len < 0)
                sp_log(ctx, SP_LOG_ERROR, "Invalid fifo size \"%s\"!\n", tmp_val);
            else
                sp_packet_fifo_set_max_queued(ctx->src_packets, len);
        }
        pthread_mutex_unlock(&ctx->lock);
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

int sp_muxer_ctrl(AVBufferRef *ctx_ref, SPEventType ctrl, void *arg)
{
    MuxingContext *ctx = (MuxingContext *)ctx_ref->data;
    return sp_ctrl_template(ctx, ctx->events, 0x0, muxer_ioctx_ctrl_cb, ctrl, arg);
}

int sp_muxer_init(AVBufferRef *ctx_ref)
{
    int err;
    MuxingContext *ctx = (MuxingContext *)ctx_ref->data;

    if (!ctx->out_format) {
        if (!strncmp(ctx->out_url, "/dev/video", strlen("/dev/video")))
            ctx->out_format = "v4l2";
        if (!strncmp(ctx->out_url, "rtmp://", strlen("rtmp://")))
            ctx->out_format = "flv";
        if (!strncmp(ctx->out_url, "udp://", strlen("udp://")))
            ctx->out_format = "mpegts";
        if (!strncmp(ctx->out_url, "http://", strlen("http://")))
            ctx->out_format = "dash";
    }

    err = avformat_alloc_output_context2(&ctx->avf, NULL, ctx->out_format,
	                                 ctx->out_url);
    if (err) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to init lavf context!\n");
        return err;
    }

    if (!ctx->name) {
        int len = strlen(sp_class_get_name(ctx)) + 1 + strlen(ctx->avf->oformat->name) + 1;
        char *new_name = av_mallocz(len);
        if (!new_name) {
            err = AVERROR(ENOMEM);
            goto fail;
        }
        av_strlcpy(new_name, sp_class_get_name(ctx), len);
        av_strlcat(new_name, ":", len);
        av_strlcat(new_name, ctx->avf->oformat->name, len);
        sp_class_set_name(ctx, new_name);
        av_free(new_name);
        ctx->name = sp_class_get_name(ctx);
    } else {
        sp_class_set_name(ctx, ctx->name);
    }

    ctx->avf->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    /* Open for writing */
    err = avio_open(&ctx->avf->pb, ctx->out_url, AVIO_FLAG_WRITE);
    if (err) {
        sp_log(ctx, SP_LOG_ERROR, "Couldn't open %s: %s!\n", ctx->out_url,
               av_err2str(err));
        goto fail;
    }

    /* Both fields alive for the duration of the avf context */
    ctx->out_format = ctx->avf->oformat->name;
    ctx->out_url = ctx->avf->url;

    return 0;

fail:
    avformat_free_context(ctx->avf);
    return err;
}

static void muxer_free(void *opaque, uint8_t *data)
{
    MuxingContext *ctx = (MuxingContext *)data;

    sp_packet_fifo_unmirror_all(ctx->src_packets);

    if (ctx->muxing_thread) {
        sp_packet_fifo_push(ctx->src_packets, NULL);
        pthread_join(ctx->muxing_thread, NULL);
    }

    for (int i = 0; i < ctx->enc_map_size; i++)
        av_free(ctx->enc_map[i].name);
    av_free(ctx->enc_map);

    av_buffer_unref(&ctx->src_packets);
    av_free(ctx->stream_has_link);
    av_free(ctx->stream_codec_id);
    av_free(ctx->dump_sdp_file);

    if (sp_eventlist_has_dispatched(ctx->events, SP_EVENT_ON_INIT)) {
        int err = av_write_trailer(ctx->avf);
        if (err < 0)
            sp_log(ctx, SP_LOG_ERROR, "Error writing trailer: %s!\n",
                   av_err2str(err));

        /* Flush output data */
        avio_flush(ctx->avf->pb);
        avformat_flush(ctx->avf);

        sp_log(ctx, SP_LOG_VERBOSE, "Wrote trailer!\n");
    }

    sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_DESTROY, NULL);
    sp_bufferlist_free(&ctx->events);

    if (ctx->avf)
        avio_closep(&ctx->avf->pb);

    avformat_free_context(ctx->avf);

    pthread_mutex_destroy(&ctx->lock);

    sp_log(ctx, SP_LOG_VERBOSE, "Muxer destroyed!\n");
    sp_class_free(ctx);
    av_free(ctx);
}

AVBufferRef *sp_muxer_alloc(void)
{
    MuxingContext *ctx = av_mallocz(sizeof(MuxingContext));
    if (!ctx)
        return NULL;

    AVBufferRef *ctx_ref = av_buffer_create((uint8_t *)ctx, sizeof(*ctx),
                                            muxer_free, NULL, 0);

    int err = sp_class_alloc(ctx, "lavf", SP_TYPE_MUXER, NULL);
    if (err < 0) {
        av_buffer_unref(&ctx_ref);
        return NULL;
    }

    pthread_mutex_init(&ctx->lock, NULL);
    ctx->events = sp_bufferlist_new();
    ctx->src_packets = sp_packet_fifo_create(ctx, 256, PACKET_FIFO_BLOCK_NO_INPUT);

    return ctx_ref;
}
