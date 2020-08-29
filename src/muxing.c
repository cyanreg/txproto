#include <libavutil/time.h>
#include <libavutil/avstring.h>

#include "muxing.h"
#include "utils.h"

static av_unused void bitrate_hr(float *bps, const char **unit, int64_t rate)
{
    if (rate > (1000000)) {
        *bps = rate / 1000000.0f;
        *unit = "Mbps";
    } else if (rate > (1000)) {
        *bps = rate / 1000.0f;
        *unit = "kbps";
    } else {
        *bps = rate;
        *unit = "bps";
    }
}

static MuxEncoderMap *enc_id_lookup(MuxingContext *ctx, int enc_id)
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

    sp_bufferlist_dispatch_events(ctx->events, ctx, SP_EVENT_ON_INIT);

    pthread_mutex_lock(&ctx->lock);

    /* Stream stats */
    SlidingWinCtx *sctx_rate = av_mallocz(ctx->avf->nb_streams * sizeof(*sctx_rate));
    SlidingWinCtx *sctx_latency = av_mallocz(ctx->avf->nb_streams * sizeof(*sctx_latency));
    int64_t *rate = av_mallocz(ctx->avf->nb_streams * sizeof(*rate));
    int64_t *latency = av_mallocz(ctx->avf->nb_streams * sizeof(*latency));

    ctx->avf->flags |= AVFMT_FLAG_AUTO_BSF;

    if (ctx->low_latency) {
        ctx->avf->flags |= AVFMT_FLAG_FLUSH_PACKETS | AVFMT_FLAG_NOBUFFER;
        ctx->avf->pb->min_packet_size = 0;
    }

	err = avformat_write_header(ctx->avf, NULL);
	if (err) {
		av_log(ctx, AV_LOG_ERROR, "Could not write header: %s!\n", av_err2str(err));
		goto fail;
	}

    atomic_store(&ctx->initialized, 1);

    sp_set_thread_name_self(ctx->class->class_name);

    sp_bufferlist_dispatch_events(ctx->events, ctx, SP_EVENT_ON_CHANGE);

    pthread_mutex_unlock(&ctx->lock);

    if (ctx->dump_info)
        av_dump_format(ctx->avf, 0, ctx->out_url, 1);

    int flush = 0;
    int fmt_can_flush = ctx->avf->oformat->flags & AVFMT_ALLOW_FLUSH;

#if 0
    fprintf(stderr, "\n");
#endif

    /* Mux stats */
    SlidingWinCtx sctx_mux = { 0 };
    int64_t last_pos_update = av_gettime_relative();
    int64_t mux_rate = 0;
    int64_t last_pos = 0;
    av_unused int64_t buf_bytes = 0;

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

        MuxEncoderMap *eid = enc_id_lookup(ctx, in_pkt->stream_index);
        AVRational src_tb = eid->encoder_tb;
        int sid = eid->stream_id;

        in_pkt->stream_index = sid;

        AVRational dst_tb = ctx->avf->streams[sid]->time_base;
        SlidingWinCtx *rate_c = &sctx_rate[sid];
        SlidingWinCtx *latency_c = &sctx_latency[sid];

        rate[sid] = sp_sliding_win(rate_c, in_pkt->size, in_pkt->pts, src_tb, src_tb.den, 0) << 3;

        latency[sid]  = av_gettime_relative() - ctx->epoch;
        latency[sid] -= av_rescale_q(in_pkt->pts, src_tb, av_make_q(1, 1000000));
        latency[sid]  = sp_sliding_win(latency_c, latency[sid], in_pkt->pts, src_tb, src_tb.den, 1);

        /* Rescale timestamps */
        in_pkt->pts = av_rescale_q(in_pkt->pts, src_tb, dst_tb);
        in_pkt->dts = av_rescale_q(in_pkt->dts, src_tb, dst_tb);
        in_pkt->duration = av_rescale_q(in_pkt->duration, src_tb, dst_tb);

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
            av_log(ctx, AV_LOG_ERROR, "Error muxing, operation timed out!\n");
            pthread_mutex_unlock(&ctx->lock);
            continue;
        } else if (err < 0) {
            av_log(ctx, AV_LOG_ERROR, "Error muxing: %s!\n", av_err2str(err));
            pthread_mutex_unlock(&ctx->lock);
	    	goto fail;
        }

        if (flush) {
            pthread_mutex_unlock(&ctx->lock);
            break;
        }
#if 0
        {
            for (int i = 0; i < ctx->avf->nb_streams; i++) {
                fprintf(stderr, "\033[F");
                fflush(stderr);
            }

            float bitrate_mux;
            const char *unit_mux;
            bitrate_hr(&bitrate_mux, &unit_mux, mux_rate);
            fprintf(stderr, "\33[2KMuxing: %.1f%s (buffer: %li Kib)\n", bitrate_mux, unit_mux, buf_bytes / 1024);
            fflush(stderr);

            for (int i = 0; i < ctx->avf->nb_streams; i++) {
                float bitrate_st;
                const char *unit_st;
                const char *type;
                switch (ctx->avf->streams[i]->codecpar->codec_type) {
                case AVMEDIA_TYPE_VIDEO: type = "video"; break;
                case AVMEDIA_TYPE_AUDIO: type = "audio"; break;
                case AVMEDIA_TYPE_SUBTITLE: type = "subtitle"; break;
                default: type = "unknown"; break;
                }
                bitrate_hr(&bitrate_st, &unit_st, rate[i]);
                fprintf(stderr, "\33[2K    Stream %i (%s): %.1f%s, Latency: %.1fms%c",
                        i, type, bitrate_st, unit_st, latency[i] / 1000.0f,
                        i == (ctx->avf->nb_streams - 1) ? '\0' : '\n');
                fflush(stderr);
            }
        }
#endif
        pthread_mutex_unlock(&ctx->lock);
    }

    pthread_mutex_lock(&ctx->lock);

fail:
    av_free(sctx_rate);
    av_free(sctx_latency);
    av_free(rate);
    av_free(latency);

    pthread_mutex_unlock(&ctx->lock);

    ctx->err = err;
    atomic_store(&ctx->running, 0);

    return NULL;
}

int sp_muxer_add_stream(AVBufferRef *ctx_ref, AVBufferRef *enc_ref)
{
    int err = 0;
    MuxingContext *ctx = (MuxingContext *)ctx_ref->data;
    EncodingContext *enc = (EncodingContext *)enc_ref->data;

    pthread_mutex_lock(&ctx->lock);

    MuxEncoderMap *enc_map_entry = NULL;
    for (int i = 0; i < ctx->enc_map_size; i++) {
        if (ctx->enc_map[i].encoder_id == enc->encoder_id && (ctx->enc_map[i].stream_id == INT_MAX)) {
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

    enc_map_entry->encoder_id = enc->encoder_id;
    enc_map_entry->encoder_tb = enc->avctx->time_base;
    enc_map_entry->stream_id  = INT_MAX;

    if (ctx->initialized) {

    } else {
        ctx->stream_has_link = av_realloc(ctx->stream_has_link, sizeof(*ctx->stream_has_link) * (ctx->avf->nb_streams + 1));
        ctx->stream_codec_id = av_realloc(ctx->stream_codec_id, sizeof(*ctx->stream_codec_id) * (ctx->avf->nb_streams + 1));
 
        AVStream *st = avformat_new_stream(ctx->avf, NULL);
        if (!st) {
            av_log(ctx, AV_LOG_ERROR, "Unable to allocate stream!\n");
            err = AVERROR(ENOMEM);
            goto end;
        }

        err = avcodec_parameters_from_context(st->codecpar, enc->avctx);
        if (err < 0) {
            av_log(ctx, AV_LOG_ERROR, "Could not copy codec params: %s!\n", av_err2str(err));
            goto end;
        }

        st->time_base = enc_map_entry->encoder_tb;
        enc_map_entry->stream_id = st->id = ctx->avf->nb_streams - 1;

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
    }

end:
    pthread_mutex_unlock(&ctx->lock);

    return err;
}

typedef struct MuxerIOCtrlCtx {
    enum SPEventType ctrl;
    AVDictionary *opts;
    atomic_int_fast64_t *epoch;
} MuxerIOCtrlCtx;

static void muxer_ioctx_ctrl_free(void *opaque, uint8_t *data)
{
    MuxerIOCtrlCtx *event = (MuxerIOCtrlCtx *)data;
    av_dict_free(&event->opts);
    av_free(data);
}

static int muxer_ioctx_ctrl_cb(AVBufferRef *opaque, void *src_ctx)
{
    MuxerIOCtrlCtx *event = (MuxerIOCtrlCtx *)opaque->data;
    MuxingContext *ctx = src_ctx;

    if (event->ctrl & SP_EVENT_CTRL_START) {
        pthread_mutex_lock(&ctx->lock);
        if (!atomic_load(&ctx->running)) {
            atomic_store(&ctx->running, 1);
            ctx->epoch = atomic_load(event->epoch);
            pthread_create(&ctx->muxing_thread, NULL, muxing_thread, ctx);
        }
        pthread_mutex_unlock(&ctx->lock);
    } else if (event->ctrl & SP_EVENT_CTRL_STOP) {
        if (atomic_load(&ctx->running)) {
            sp_packet_fifo_push(ctx->src_packets, NULL);
            pthread_join(ctx->muxing_thread, NULL);
        }
    } else if (event->ctrl & SP_EVENT_CTRL_OPTS) {
        const char *tmp_val = NULL;
        if ((tmp_val = dict_get(event->opts, "low_latency")))
            if (!strcmp(tmp_val, "true") || strtol(tmp_val, NULL, 10) != 0)
                ctx->low_latency = 1;
        if ((tmp_val = dict_get(event->opts, "dump_info")))
            if (!strcmp(tmp_val, "true") || strtol(tmp_val, NULL, 10) != 0)
                ctx->dump_info = 1;
        if ((tmp_val = dict_get(event->opts, "fifo_size"))) {
            long int len = strtol(tmp_val, NULL, 10);
            if (len < 0)
                av_log(ctx, AV_LOG_ERROR, "Invalid fifo size \"%s\"!\n", tmp_val);
            else
                sp_packet_fifo_set_max_queued(ctx->src_packets, len);
        }
    } else {
        return AVERROR(ENOTSUP);
    }

    return 0;    
}

int sp_muxer_ctrl(AVBufferRef *ctx_ref, enum SPEventType ctrl, void *arg)
{
    MuxingContext *ctx = (MuxingContext *)ctx_ref->data;

    if (ctrl & SP_EVENT_CTRL_COMMIT) {
        av_log(ctx, AV_LOG_DEBUG, "Comitting!\n");
        return sp_bufferlist_dispatch_events(ctx->events, ctx, SP_EVENT_ON_COMMIT);
    } else if (ctrl & SP_EVENT_CTRL_DISCARD) {
        av_log(ctx, AV_LOG_DEBUG, "Discarding!\n");
        sp_bufferlist_discard_new_events(ctx->events);
    } else if (ctrl & SP_EVENT_CTRL_NEW_EVENT) {
        char *fstr = sp_event_flags_to_str_buf(arg);
        av_log(ctx, AV_LOG_DEBUG, "Registering new event (%s)!\n", fstr);
        av_free(fstr);
        return sp_bufferlist_append_event(ctx->events, arg);
    } else if (ctrl & SP_EVENT_CTRL_DEP) {
        char *fstr = sp_event_flags_to_str(ctrl & ~SP_EVENT_CTRL_MASK);
        av_log(ctx, AV_LOG_DEBUG, "Registering new dependency (%s)!\n", fstr);
        av_free(fstr);
        return sp_bufferlist_append_dep(ctx->events, arg, ctrl);
    } else if (ctrl & ~(SP_EVENT_CTRL_START |
                        SP_EVENT_CTRL_STOP |
                        SP_EVENT_CTRL_OPTS |
                        SP_EVENT_FLAG_IMMEDIATE)) {
        return AVERROR(ENOTSUP);
    }

    SP_EVENT_BUFFER_CTX_ALLOC(MuxerIOCtrlCtx, ctrl_ctx, muxer_ioctx_ctrl_free, NULL)

    ctrl_ctx->ctrl = ctrl;
    if (ctrl & SP_EVENT_CTRL_OPTS)
        av_dict_copy(&ctrl_ctx->opts, arg, 0);
    if (ctrl & SP_EVENT_CTRL_START)
        ctrl_ctx->epoch = arg;

    if (ctrl & SP_EVENT_FLAG_IMMEDIATE) {
        int ret = muxer_ioctx_ctrl_cb(ctrl_ctx_ref, ctx);
        av_buffer_unref(&ctrl_ctx_ref);
        return ret;
    }

    enum SPEventType flags = SP_EVENT_FLAG_ONESHOT | SP_EVENT_ON_COMMIT | ctrl;
    AVBufferRef *ctrl_event = sp_event_create(muxer_ioctx_ctrl_cb, NULL,
                                              flags, ctrl_ctx_ref,
                                              sp_event_gen_identifier(ctx, NULL, flags));

    char *fstr = sp_event_flags_to_str_buf(ctrl_event);
    av_log(ctx, AV_LOG_DEBUG, "Registering new event (%s)!\n", fstr);
    av_free(fstr);

    int err = sp_bufferlist_append_event(ctx->events, ctrl_event);
    av_buffer_unref(&ctrl_event);
    if (err < 0)
        return err;

    return 0;
}

int sp_muxer_init(AVBufferRef *ctx_ref)
{
    int err;
    char *new_name = NULL;
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
        av_log(ctx, AV_LOG_ERROR, "Unable to init lavf context!\n");
        return err;
    }

    if (ctx->name) {
        new_name = av_strdup(ctx->name);
        if (!new_name) {
            err = AVERROR(ENOMEM);
            goto fail;
        }
    } else {
        int len = strlen(ctx->class->class_name) + 1 + strlen(ctx->avf->oformat->name) + 1;
        new_name = av_mallocz(len);
        if (!new_name) {
            err = AVERROR(ENOMEM);
            goto fail;
        }
        av_strlcpy(new_name, ctx->class->class_name, len);
        av_strlcat(new_name, ":", len);
        av_strlcat(new_name, ctx->avf->oformat->name, len);
    }

    av_free((void *)ctx->class->class_name);
    ctx->class->class_name = new_name;
    ctx->name = new_name;

    ctx->avf->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    /* Open for writing */
    err = avio_open(&ctx->avf->pb, ctx->out_url, AVIO_FLAG_WRITE);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Couldn't open %s: %s!\n", ctx->out_url,
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

    if (atomic_load(&ctx->running)) {
        sp_packet_fifo_push(ctx->src_packets, NULL);
        pthread_join(ctx->muxing_thread, NULL);
    }

    av_buffer_unref(&ctx->src_packets);
    av_free(ctx->enc_map);
    av_free(ctx->stream_has_link);
    av_free(ctx->stream_codec_id);

    if (atomic_load(&ctx->initialized)) {
        int err = av_write_trailer(ctx->avf);
        if (err < 0)
            av_log(ctx, AV_LOG_ERROR, "Error writing trailer: %s!\n",
                   av_err2str(err));

        /* Flush output data */
        avio_flush(ctx->avf->pb);
        avformat_flush(ctx->avf);

        av_log(ctx, AV_LOG_INFO, "Wrote trailer!\n");
    }

    sp_bufferlist_dispatch_events(ctx->events, ctx, SP_EVENT_ON_DESTROY);
    sp_bufferlist_free(&ctx->events);

    if (ctx->avf)
        avio_closep(&ctx->avf->pb);

    avformat_free_context(ctx->avf);

    av_free((void *)ctx->class->class_name);
    av_free(ctx->class);

    pthread_mutex_destroy(&ctx->lock);
}

AVBufferRef *sp_muxer_alloc(void)
{
    MuxingContext *ctx = av_mallocz(sizeof(MuxingContext));
    if (!ctx)
        return NULL;

    AVBufferRef *ctx_ref = av_buffer_create((uint8_t *)ctx, sizeof(*ctx),
                                            muxer_free, NULL, 0);

    ctx->class = av_mallocz(sizeof(*ctx->class));
    if (!ctx->class) {
        av_buffer_unref(&ctx_ref);
        return NULL;
    }

    *ctx->class = (AVClass) {
        .class_name   = av_strdup("lavf"),
        .item_name    = av_default_item_name,
        .get_category = av_default_get_category,
        .version      = LIBAVUTIL_VERSION_INT,
        .category     = AV_CLASS_CATEGORY_MUXER,
    };

    pthread_mutex_init(&ctx->lock, NULL);
    ctx->events = sp_bufferlist_new();
    ctx->src_packets = sp_packet_fifo_create(ctx, 256, PACKET_FIFO_BLOCK_NO_INPUT);
    ctx->initialized = ATOMIC_VAR_INIT(0);
    ctx->running = ATOMIC_VAR_INIT(0);

    return ctx_ref;
}
