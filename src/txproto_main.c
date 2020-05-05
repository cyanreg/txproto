#define _GNU_SOURCE
#include <libavformat/avformat.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libavutil/avstring.h>

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "src_common.h"
#include "encoding.h"
#include "filtering.h"
#include "utils.h"
#include "../config.h"
#include "version.h"

typedef struct VideoCapturing {
    void *video_cap_ctx;
    int video_frame_queue;
    CaptureSource *video_capture_source;
    const char *video_capture_target;
    AVDictionary *video_capture_opts;
    SPFrameFIFO video_frames;
} VideoCapturing;

struct capture_context {
    AVClass *class; /* For pretty logging */

    /* If something happens during capture */
    int err;
    atomic_bool quit;

    int64_t epoch;

    /* Filtering */
    FilterContext *fctx_video;
    SPFrameFIFO filtered_video;

    FilterContext *fctx_audio;
    SPFrameFIFO filtered_audio;

    AVDictionary *muxer_options;

    /* Encoders */
    EncodingContext *video_encoder;
    EncodingContext *audio_encoder;

    /* Video capture */
    VideoCapturing vcap[16];
    int vcap_nums;

    /* Audio capture - we resample and convert in the encoding thread */
    void *audio_cap_ctx;
    int audio_frame_queue;
    CaptureSource *audio_capture_source;
    const char *audio_capture_targets[16];
    int audio_capture_targets_num;
    AVDictionary *audio_capture_opts[16];
    SPFrameFIFO audio_frames[16];



    /* Muxing */
    AVFormatContext *avf;
    pthread_t muxing_thread; /* Its own thread to deal better with blocking */
    char *out_filename;
    char *out_format;
    int video_streamid;
    int audio_streamid;
    SPPacketFIFO packet_buf;
};

static int init_lavf(struct capture_context *ctx)
{
    sp_packet_fifo_init(&ctx->packet_buf, 0, PACKET_FIFO_BLOCK_NO_INPUT);

    int err = avformat_alloc_output_context2(&ctx->avf, NULL, ctx->out_format,
	                                         ctx->out_filename);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Unable to init lavf context!\n");
        return err;
    }

    ctx->avf->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    return 0;
}

void *muxing_thread(void *arg)
{
    struct capture_context *ctx = arg;
    SlidingWinCtx sctx_video = { 0 };
    SlidingWinCtx sctx_audio = { 0 };
    int64_t rate_video = 0, rate_audio = 0;

	/* Open for writing */
    int err = avio_open(&ctx->avf->pb, ctx->out_filename, AVIO_FLAG_WRITE);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Couldn't open %s: %s!\n", ctx->out_filename,
               av_err2str(err));
        goto fail;
    }

	err = avformat_write_header(ctx->avf, &ctx->muxer_options);
	if (err) {
		av_log(ctx, AV_LOG_ERROR, "Couldn't write header: %s!\n", av_err2str(err));
		goto fail;
	}

    pthread_setname_np(pthread_self(), ctx->avf->oformat->name);

    /* Debug print */
    av_dump_format(ctx->avf, 0, ctx->out_filename, 1);

    int flush = 0;
    int64_t audio_packets = 0;
    int64_t video_packets = 0;
    SlidingWinCtx sctx_delay_video = { 0 };
    SlidingWinCtx sctx_delay_audio = { 0 };
    int64_t delay_video = 0;
    int64_t delay_audio = 0;
    fprintf(stderr, "\n");

    while (1) {
        AVPacket *in_pkt = NULL;

        if (!flush) {
            in_pkt = sp_packet_fifo_pop(&ctx->packet_buf);
            flush = !in_pkt;
        }

        if (flush)
            goto send;

        AVRational dst_tb = ctx->avf->streams[in_pkt->stream_index]->time_base;
        AVRational src_tb;
        if (in_pkt->stream_index == ctx->video_streamid) {
            src_tb = ctx->video_encoder->avctx->time_base;
            video_packets++;
            rate_video = sliding_win_sum(&sctx_video, in_pkt->size, in_pkt->pts, src_tb, src_tb.den, 0);
            delay_video = av_gettime_relative() - ctx->epoch; /* Time since epoch */
            delay_video -= av_rescale_q(in_pkt->pts, src_tb, av_make_q(1, 1000000));
            delay_video = sliding_win_sum(&sctx_delay_video, delay_video, in_pkt->pts, src_tb, src_tb.den, 1);
        } else if (in_pkt->stream_index == ctx->audio_streamid) {
            src_tb = ctx->audio_encoder->avctx->time_base;
            audio_packets++;
            rate_audio = sliding_win_sum(&sctx_audio, in_pkt->size, in_pkt->pts, src_tb, src_tb.den, 0);
            delay_audio = av_gettime_relative() - ctx->epoch; /* Time since epoch */
            delay_audio -= av_rescale_q(in_pkt->pts, src_tb, av_make_q(1, 1000000));
            delay_audio = sliding_win_sum(&sctx_delay_audio, delay_audio, in_pkt->pts, src_tb, src_tb.den, 1);
        }

        in_pkt->pts = av_rescale_q(in_pkt->pts, src_tb, dst_tb);
        in_pkt->dts = av_rescale_q(in_pkt->dts, src_tb, dst_tb);
        in_pkt->duration = av_rescale_q(in_pkt->duration, src_tb, dst_tb);

send:
        err = av_interleaved_write_frame(ctx->avf, in_pkt);
        av_packet_free(&in_pkt);
        if (err) {
            av_log(ctx, AV_LOG_ERROR, "Error muxing: %s!\n", av_err2str(err));
	    	goto fail;
        }

        if (flush)
            break;
#if 1
        fprintf(stderr, "\rRate: %.1fMbps (video), %likbps (audio), Packets muxed: "
                "%liv, %lia, Delay: %.1fms (v), %.1fms (a)",
                (rate_video << 3) / 1000000.0f, av_rescale(rate_audio, 8, 1000),
                video_packets, audio_packets, delay_video / 1000.0f, delay_audio / 1000.0f);
#endif
    }

    if ((err = av_write_trailer(ctx->avf))) {
        av_log(ctx, AV_LOG_ERROR, "Error writing trailer: %s!\n",
               av_err2str(err));
        goto fail;
    }

    /* Flush output data */
    avio_flush(ctx->avf->pb);
    avformat_flush(ctx->avf);

    av_log(ctx, AV_LOG_INFO, "Wrote trailer!\n");

    return NULL;

fail:
    ctx->err = err;
    return NULL;
}

static int init_video_capture(struct capture_context *ctx, VideoCapturing *vctx,
                              int64_t epoch)
{
    int err;

    sp_frame_fifo_init(&vctx->video_frames, vctx->video_frame_queue, FRAME_FIFO_BLOCK_NO_INPUT);

    /* Init pulse */
    vctx->video_capture_source->init(&vctx->video_cap_ctx, epoch);

    /* Get all sources */
    SourceInfo *infos;
    int num_infos;
    vctx->video_capture_source->sources(vctx->video_cap_ctx, &infos, &num_infos);

    for (int i = 0; i < num_infos; i++)
        av_log(ctx, AV_LOG_WARNING, "Video source %i: %s (%s)\n", i,
               infos[i].name, infos[i].desc);

    const char *target = vctx->video_capture_target;
    if (!target)
        return 0;

    char *end = NULL;
    uint64_t identifier = infos[0].identifier;
    int target_idx = strtol(target, &end, 10);
    if (end != target && target_idx >= 0 && target_idx < num_infos) {
        identifier = infos[target_idx].identifier;
    } else {
        for (int i = 0; i < num_infos; i++) {
            if (!av_strncasecmp(target, infos[i].name, strlen(target))) {
                identifier = infos[i].identifier;
                break;
            }
        }
    }

    /* Start capturing and init video encoder */
    err = vctx->video_capture_source->start(vctx->video_cap_ctx, identifier,
                                            vctx->video_capture_opts,
                                            (ctx->video_encoder || ctx->fctx_video) ? &vctx->video_frames : NULL,
                                            NULL, NULL);
    if (err)
        return err;

    return 0;
}

static void stop_video_capture(struct capture_context *ctx, VideoCapturing *vctx)
{
    SourceInfo *infos;
    int num_infos;
    vctx->video_capture_source->sources(vctx->video_cap_ctx, &infos, &num_infos);
    for (int i = 0; i < num_infos; i++)
	    vctx->video_capture_source->stop(vctx->video_cap_ctx, infos[i].identifier);
}

static int init_enc_muxing(struct capture_context *ctx, EncodingContext *enc)
{
    int old_nb_streams = ctx->avf->nb_streams;

    AVStream *st = avformat_new_stream(ctx->avf, NULL);
    if (!st) {
        av_log(ctx, AV_LOG_ERROR, "Unable to alloc stream!\n");
        return AVERROR(ENOMEM);
    }

    st->id = enc->stream_id = old_nb_streams;
    st->time_base = enc->avctx->time_base;

    /* Set stream metadata */
    int enc_str_len = sizeof(LIBAVCODEC_IDENT) + 1 + strlen(enc->avctx->codec->name) + 1;
    char *enc_str = av_mallocz(enc_str_len);
    av_strlcpy(enc_str, LIBAVCODEC_IDENT " ", enc_str_len);
    av_strlcat(enc_str, enc->avctx->codec->name, enc_str_len);
    av_dict_set(&st->metadata, "encoder", enc_str, AV_DICT_DONT_STRDUP_VAL);

    /* Set SAR */
    if (enc->codec->type == AVMEDIA_TYPE_VIDEO) {
        ctx->video_streamid     = st->id;
        st->avg_frame_rate      = enc->avctx->framerate;
        st->sample_aspect_ratio = enc->avctx->sample_aspect_ratio;
    }

    if (enc->codec->type == AVMEDIA_TYPE_AUDIO)
        ctx->audio_streamid = st->id;

    int err = avcodec_parameters_from_context(st->codecpar, enc->avctx);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Couldn't copy codec params: %s!\n", av_err2str(err));
         return err;
     }

    return 0;
}

static void stop_audio_capture(struct capture_context *ctx)
{
    SourceInfo *infos;
    int num_infos;
    ctx->audio_capture_source->sources(ctx->audio_cap_ctx, &infos, &num_infos);
    for (int i = 0; i < num_infos; i++)
	    ctx->audio_capture_source->stop(ctx->audio_cap_ctx, infos[i].identifier);
}

static int init_audio_capture(struct capture_context *ctx, int64_t epoch)
{
    int err;

    /* Init the FIFO buffers */
    for (int i = 0; i < 16; i++) {
        sp_frame_fifo_init(&ctx->audio_frames[i], ctx->audio_frame_queue,
                           FRAME_FIFO_BLOCK_NO_INPUT);
    }

    /* Init pulse */
    ctx->audio_capture_source->init(&ctx->audio_cap_ctx, epoch);

    /* Get all sources */
    SourceInfo *infos;
    int num_infos;
    ctx->audio_capture_source->sources(ctx->audio_cap_ctx, &infos, &num_infos);

    for (int i = 0; i < num_infos; i++)
        av_log(ctx, AV_LOG_WARNING, "Audio source %i: %s (%s)\n", i,
               infos[i].name, infos[i].desc);

    if (!ctx->audio_capture_targets_num)
        return 0;

    for (int i = 0; i < ctx->audio_capture_targets_num; i++) {
        const char *target = ctx->audio_capture_targets[i];

        if (!target)
            continue;

        char *end = NULL;
        uint64_t identifier = infos[0].identifier;
        int target_idx = strtol(target, &end, 10);
        if (end != target && target_idx >= 0 && target_idx < num_infos) {
            identifier = infos[target_idx].identifier;
        } else {
            for (int i = 0; i < num_infos; i++) {
                if (!av_strncasecmp(target, infos[i].name, strlen(target))) {
                    identifier = infos[i].identifier;
                    break;
                }
            }
        }

        /* Start capturing and init audio encoder */
        err = ctx->audio_capture_source->start(ctx->audio_cap_ctx, identifier,
                                               ctx->audio_capture_opts[i],
                                               ctx->audio_encoder ? &ctx->audio_frames[i] : NULL,
                                               NULL, NULL);
        if (err)
            return err;
    }

    return 0;
}

struct capture_context *q_ctx = NULL;

void on_quit_signal(int signo) {
	printf("\r");
	av_log(q_ctx, AV_LOG_WARNING, "Quitting!\n");
	atomic_store(&q_ctx->quit, true);
}

static int main_loop(struct capture_context *ctx) {
	int err;

	q_ctx = ctx;

	if (signal(SIGINT, on_quit_signal) == SIG_ERR) {
		av_log(ctx, AV_LOG_ERROR, "Unable to install signal handler!\n");
		return AVERROR(EINVAL);
	}

    if ((err = init_lavf(ctx)))
        return err;

    ctx->epoch = av_gettime_relative();

    /* If existing, init video capture */
    for (int i = 0; i < ctx->vcap_nums; i++) {
        if (ctx->vcap[i].video_capture_source && (err = init_video_capture(ctx, &ctx->vcap[i], ctx->epoch)))
            return err;
    }

    /* If existing, init audio capture */
    if (ctx->audio_capture_source && (err = init_audio_capture(ctx, ctx->epoch)))
        return err;

    /* Filtering should go here */
    if (ctx->fctx_video) {
        sp_filter_init_graph(ctx->fctx_video);
    }

    if (ctx->fctx_audio) {
        sp_filter_init_graph(ctx->fctx_audio);
    }

    /* Init video encoding */
    if (ctx->vcap_nums && ctx->video_encoder) {
        ctx->video_encoder->global_header_needed = ctx->avf->oformat->flags & AVFMT_GLOBALHEADER;
        init_encoder(ctx->video_encoder);
        init_enc_muxing(ctx, ctx->video_encoder);
        start_encoding_thread(ctx->video_encoder);
    }

    /* Init audio encoding */
    if (ctx->audio_capture_targets_num && ctx->audio_encoder && ctx->audio_capture_source) {
        ctx->audio_encoder->global_header_needed = ctx->avf->oformat->flags & AVFMT_GLOBALHEADER;
        init_encoder(ctx->audio_encoder);
        init_enc_muxing(ctx, ctx->audio_encoder);
        start_encoding_thread(ctx->audio_encoder);
    }

    if (ctx->video_encoder || ctx->audio_encoder) {
        av_log(ctx, AV_LOG_INFO, "Starting muxing thread!\n");
        pthread_create(&ctx->muxing_thread, NULL, muxing_thread, ctx);
    }

    /* Run main loop */
    while (!ctx->err && !atomic_load(&ctx->quit))
    {
        usleep(500000);
    }

    /* Stop capturing */
    for (int i = 0; i < ctx->vcap_nums; i++)
        if (ctx->vcap[i].video_capture_source && ctx->vcap[i].video_capture_target)
            stop_video_capture(ctx, &ctx->vcap[i]);

    if (ctx->audio_capture_source && ctx->audio_capture_targets_num)
        stop_audio_capture(ctx);

    /* Stop encoding */
    stop_encoding_thread(ctx->video_encoder);
    stop_encoding_thread(ctx->audio_encoder);

    /* Stop muxing */
    sp_packet_fifo_push(&ctx->packet_buf, NULL);
    if (ctx->video_encoder || ctx->audio_encoder)
        pthread_join(ctx->muxing_thread, NULL);

    return ctx->err;
}

static void uninit(struct capture_context *ctx)
{
    for (int i = 0; i < ctx->vcap_nums; i++) {

        /* Free all frames before destroying capture context */
        sp_frame_fifo_free(&ctx->vcap[i].video_frames);

        if (ctx->vcap[i].video_capture_source)
            ctx->vcap[i].video_capture_source->free(&ctx->vcap[i].video_cap_ctx);
    }

    if (ctx->audio_capture_source)
        ctx->audio_capture_source->free(&ctx->audio_cap_ctx);

    for (int i = 0; i < ctx->audio_capture_targets_num; i++)
        sp_frame_fifo_free(&ctx->audio_frames[i]);

    sp_packet_fifo_free(&ctx->packet_buf);

    free_encoder(&ctx->video_encoder);
    free_encoder(&ctx->audio_encoder);

    if (ctx->avf)
        avio_closep(&ctx->avf->pb);

    avformat_free_context(ctx->avf);
}

int main(int argc, char *argv[])
{
    int err;
    struct capture_context ctx = {
        .quit = ATOMIC_VAR_INIT(0),
    };
    ctx.class = &((AVClass) {
        .class_name  = "txproto",
        .item_name   = av_default_item_name,
        .version     = LIBAVUTIL_VERSION_INT,
    });

    av_log(&ctx, AV_LOG_INFO, "Starting %s %s (%s)!\n", PROJECT_NAME,
           PROJECT_VERSION_STRING, vcstag);

    ctx.out_filename = argv[1];
    ctx.video_streamid = ctx.audio_streamid = -1;

    ctx.out_format = NULL;
    if (!strncmp(ctx.out_filename, "rtmp://", 7))
        ctx.out_format = "flv";
    if (!strncmp(ctx.out_filename, "udp://", 6))
        ctx.out_format = "mpegts";
    if (!strncmp(ctx.out_filename, "http://", 7)) {
        ctx.out_format = "dash";
        av_dict_set(&ctx.muxer_options, "seg_duration", "2", 0);
        av_dict_set(&ctx.muxer_options, "dash_segment_type", "mp4", 0);
        av_dict_set(&ctx.muxer_options, "window_size", "2", 0);
        av_dict_set(&ctx.muxer_options, "remove_at_exit", "1", 0);
    }

#if 1
    /* Video capture (screen) */
    ctx.vcap[ctx.vcap_nums].video_capture_source = &src_wayland;
    ctx.vcap[ctx.vcap_nums].video_capture_target = "0";
    ctx.vcap[ctx.vcap_nums].video_frame_queue = 4;
    av_dict_set_int(&ctx.vcap[ctx.vcap_nums].video_capture_opts, "capture_cursor",   1, 0);
    av_dict_set_int(&ctx.vcap[ctx.vcap_nums].video_capture_opts, "use_screencopy",   0, 0);
//    av_dict_set_int(&ctx.vcap[ctx.vcap_nums].video_capture_opts, "framerate_num",   30, 0);
//    av_dict_set_int(&ctx.vcap[ctx.vcap_nums].video_capture_opts, "framerate_den",    1, 0);
    ctx.vcap_nums++;
#endif

#if 0
    /* Video capture (webcam) */
    ctx.vcap[ctx.vcap_nums].video_capture_source = &src_lavd;
    ctx.vcap[ctx.vcap_nums].video_capture_target = "4";
    ctx.vcap[ctx.vcap_nums].video_frame_queue = 16;
    av_dict_set(&ctx.vcap[ctx.vcap_nums].video_capture_opts, "video_size", "640x480", 0);
//    av_dict_set(&ctx.vcap[ctx.vcap_nums].video_capture_opts, "pixel_format", "mjpeg", 0);
    av_dict_set(&ctx.vcap[ctx.vcap_nums].video_capture_opts, "framerate",  "30", 0);
    ctx.vcap_nums++;
#endif

#if 1
    sp_frame_fifo_init(&ctx.filtered_video, 4, FRAME_FIFO_BLOCK_NO_INPUT);
    ctx.fctx_video = alloc_filtering_ctx();

    sp_init_filter_graph(ctx.fctx_video, "[in1] hwmap,scale_vaapi=w=1280:h=720:format=nv12 [out1]"
                         , NULL, AV_HWDEVICE_TYPE_VAAPI);

    sp_map_fifo_to_pad(ctx.fctx_video, &ctx.vcap[0].video_frames, 0, 0);
//    sp_map_fifo_to_pad(ctx.fctx_video, &ctx.vcap[1].video_frames, 1, 0);
    sp_map_fifo_to_pad(ctx.fctx_video, &ctx.filtered_video, 0, 1);
#endif

#if 1
    /* Video encoder */
    ctx.video_encoder = alloc_encoding_ctx();
    ctx.video_encoder->codec = avcodec_find_encoder_by_name("h264_vaapi");
    ctx.video_encoder->source_frames = ctx.fctx_video ? &ctx.filtered_video : &ctx.vcap[0].video_frames;
    ctx.video_encoder->dest_packets = &ctx.packet_buf;
    ctx.video_encoder->width = 0; /* 0 - Use input */
    ctx.video_encoder->height = 0; /* 0 - Use input */
    ctx.video_encoder->pix_fmt = AV_PIX_FMT_NONE; /* AV_PIX_FMT_NONE - Use input */
    ctx.video_encoder->bitrate = 1000000 * /* Mbps */ 10;
    ctx.video_encoder->keyframe_interval = 40;
//    ctx.video_encoder->crf = 10;
    av_dict_set(&ctx.video_encoder->encoder_opts, "preset", "veryfast", 0);
#endif

#if 1
    /* Audio capture */
    ctx.audio_capture_source = &src_pulse;
    ctx.audio_capture_targets_num++; ctx.audio_capture_targets[0] = "0";
    ctx.audio_capture_targets_num++; ctx.audio_capture_targets[1] = "1";
    ctx.audio_frame_queue = 256;
    av_dict_set_int(&ctx.audio_capture_opts[0], "buffer_ms", 20, 0);
    av_dict_set_int(&ctx.audio_capture_opts[1], "buffer_ms", 20, 0);
#endif

#if 1
    /* Audio filtering */
    sp_frame_fifo_init(&ctx.filtered_audio, 32, FRAME_FIFO_BLOCK_NO_INPUT);
    ctx.fctx_audio = alloc_filtering_ctx();

    sp_init_filter_graph(ctx.fctx_audio, "[in1] [in2] amix=inputs=2:weights='1 1' [out1]",
                         NULL, AV_HWDEVICE_TYPE_NONE);
    sp_map_fifo_to_pad(ctx.fctx_audio, &ctx.audio_frames[0], 0, 0);
    sp_map_fifo_to_pad(ctx.fctx_audio, &ctx.audio_frames[1], 1, 0);
    sp_map_fifo_to_pad(ctx.fctx_audio, &ctx.filtered_audio, 0, 1);
#endif

#if 1
    /* Audio encoder */
    ctx.audio_encoder = alloc_encoding_ctx();
    ctx.audio_encoder->codec = avcodec_find_encoder_by_name("libopus");
    ctx.audio_encoder->source_frames = ctx.fctx_audio ? &ctx.filtered_audio : &ctx.audio_frames[0];
    ctx.audio_encoder->dest_packets = &ctx.packet_buf;
    ctx.audio_encoder->sample_rate = 48000;
    ctx.audio_encoder->bitrate = 1000 * /* Kbps */ 128;
    av_dict_set(&ctx.audio_encoder->encoder_opts, "application", "audio", 0);
    av_dict_set(&ctx.audio_encoder->encoder_opts, "vbr", "off", 0);
    av_dict_set(&ctx.audio_encoder->encoder_opts, "frame_duration", "20", 0);
#endif

	if ((err = main_loop(&ctx)))
		goto end;

end:
	uninit(&ctx);
	return err;
}
