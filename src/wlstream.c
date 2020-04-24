#define _GNU_SOURCE
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libavutil/avstring.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "frame_fifo.h"
#include "src_common.h"
#include "encoding.h"
#include "utils.h"


#include "wlr-export-dmabuf-unstable-v1-client-protocol.h"

struct filter_context {
    AVFilterGraph *graph;
    AVFilterContext *buffersink_ctx;
    AVFilterContext *buffersrc_ctx;
    AVFilterGraph *filter_graph;
};

struct capture_context {
    AVClass *class; /* For pretty logging */

    /* If something happens during capture */
    int err;
    atomic_bool quit;

    /* For quitting reliably */


    /* Video */


    /* Encoders */
    EncodingContext *video_encoder;
    EncodingContext *audio_encoder;




    /* Video capture */
    void *video_cap_ctx;
    int video_frame_queue;
    CaptureSource *video_capture_source;
    FormatReport video_src_format;
    const char *video_capture_target;
    AVDictionary *video_capture_opts;
    AVFrameFIFO video_frames;

    /* Audio capture - we resample and convert in the encoding thread */
    void *audio_cap_ctx;
    int audio_frame_queue;
    CaptureSource *audio_capture_source;
    FormatReport audio_src_format;
    const char *audio_capture_target;
    AVDictionary *audio_capture_opts;
    AVFrameFIFO audio_frames;

    /* Muxing */
    AVFormatContext *avf;
    pthread_t muxing_thread; /* Its own thread to deal better with blocking */
    char *out_filename;
    char *out_format;
    int video_streamid;
    int audio_streamid;
    AVFrameFIFO packet_buf;
    pthread_mutex_t avf_conf_lock;
};

#if 0
static int init_filtering(struct capture_context *ctx, struct filter_context *s)
{
    int ret = 0;
    AVFilterInOut *outputs = NULL;
    AVFilterInOut *inputs = NULL;

    s->graph = avfilter_graph_alloc();
    if (!s->graph)
        return AVERROR(ENOMEM);

    const AVFilter *buffersrc  = avfilter_get_by_name("buffer");

    char args[512];

    AVBufferSrcParameters *params = av_buffersrc_parameters_alloc();
    params->format = AV_PIX_FMT_VULKAN;
    params->width = ctx->video_src_format.width;
    params->height = ctx->video_src_format.height;
    params->sample_aspect_ratio.num = 1;
    params->sample_aspect_ratio.den = 1;
    params->frame_rate = ctx->video_src_format.avg_frame_rate;
    params->hw_frames_ctx = av_buffer_ref(ctx->mapped_frames_ref);

    snprintf(args, sizeof(args),
            "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
            params->width, params->height,
            AV_PIX_FMT_VULKAN,
            params->frame_rate.den, params->frame_rate.num,
            1, 1);

    ret = avfilter_graph_create_filter(&s->buffersrc_ctx, buffersrc, "in",
                                       args, NULL, s->graph);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error creating filter source: %s!\n", av_err2str(ret));
        goto fail;
    }

    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    ret = avfilter_graph_create_filter(&s->buffersink_ctx, buffersink, "out",
                                       NULL, NULL, s->graph);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error creating filter sink: %s!\n", av_err2str(ret));
        goto fail;
    }

    enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_VULKAN, AV_PIX_FMT_NONE };
    ret = av_opt_set_int_list(s->buffersink_ctx, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE,
                              AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error setting filter sample format: %s!\n", av_err2str(ret));
        goto fail;
    }

    outputs = avfilter_inout_alloc();
    if (!outputs) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    outputs->name          = av_strdup("in");
    outputs->filter_ctx    = s->buffersrc_ctx;
    outputs->pad_idx       = 0;
    outputs->next          = NULL;

    inputs = avfilter_inout_alloc();
    if (!inputs) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    inputs->name          = av_strdup("out");
    inputs->filter_ctx    = s->buffersink_ctx;
    inputs->pad_idx       = 0;
    inputs->next          = NULL;

    char filter_desc[256];
    snprintf(filter_desc, sizeof(filter_desc), "scale_vulkan=w=%i:h=%i:format=%s",
             ctx->video_width, ctx->video_height, av_get_pix_fmt_name(ctx->video_encoder_pix_fmt));

    ret = av_buffersrc_parameters_set(s->buffersrc_ctx, params);
    av_free(params);
    if (ret < 0)
        goto fail;

    ret = avfilter_graph_parse_ptr(s->graph, filter_desc, &inputs, &outputs, NULL);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error parsing filter graph: %s!\n", av_err2str(ret));
        goto fail;
    }

    ret = avfilter_graph_config(s->graph, NULL);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error configuring filter graph: %s!\n", av_err2str(ret));
        goto fail;
    }

    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return 0;

fail:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return ret;
}
#endif


static int init_lavf(struct capture_context *ctx)
{
    init_fifo(&ctx->packet_buf, 0, FIFO_BLOCK_NO_INPUT);

    pthread_mutex_init(&ctx->avf_conf_lock, NULL);
    pthread_mutex_lock(&ctx->avf_conf_lock);

    int err = avformat_alloc_output_context2(&ctx->avf, NULL, ctx->out_format,
	                                         ctx->out_filename);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Unable to init lavf context!\n");
        return err;
    }

    ctx->avf->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

//    ctx->avf->oformat->video_codec = codec->id;
//    (int64_t)(ctx->video_bitrate*1000000.0f);

    return 0;
}







void *muxing_thread(void *arg)
{
    struct capture_context *ctx = arg;
    SlidingWinCtx sctx_video = { 0 };
    SlidingWinCtx sctx_audio = { 0 };
    int64_t rate_video = 0, rate_audio = 0;

    pthread_setname_np(pthread_self(), "muxing thread");

	/* Open for writing */
    int err = avio_open(&ctx->avf->pb, ctx->out_filename, AVIO_FLAG_WRITE);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Couldn't open %s: %s!\n", ctx->out_filename,
               av_err2str(err));
        goto fail;
    }

	err = avformat_write_header(ctx->avf, NULL);
	if (err) {
		av_log(ctx, AV_LOG_ERROR, "Couldn't write header: %s!\n", av_err2str(err));
		goto fail;
	}

    /* Debug print */
    av_dump_format(ctx->avf, 0, ctx->out_filename, 1);

    int64_t audio_packets = 0;
    int64_t video_packets = 0;
    fprintf(stderr, "\n");

    while (1) {
        AVPacket *in_pkt = pop_from_fifo(&ctx->packet_buf);
        if (!in_pkt)
            break;

        AVRational dst_tb = ctx->avf->streams[in_pkt->stream_index]->time_base;
        AVRational src_tb;
        if (in_pkt->stream_index == ctx->video_streamid) {
            src_tb = ctx->video_encoder->avctx->time_base;
            video_packets++;
            rate_video = sliding_win_sum(&sctx_video, in_pkt->size, in_pkt->pts, src_tb, src_tb.den);
        } else if (in_pkt->stream_index == ctx->audio_streamid) {
            src_tb = ctx->audio_encoder->avctx->time_base;
            audio_packets++;
            rate_audio = sliding_win_sum(&sctx_audio, in_pkt->size, in_pkt->pts, src_tb, src_tb.den);
        }

        in_pkt->pts = av_rescale_q(in_pkt->pts, src_tb, dst_tb);
        in_pkt->dts = av_rescale_q(in_pkt->dts, src_tb, dst_tb);
        in_pkt->duration = av_rescale_q(in_pkt->duration, src_tb, dst_tb);

        err = av_interleaved_write_frame(ctx->avf, in_pkt);
        if (err) {
            av_log(ctx, AV_LOG_ERROR, "Error muxing: %s!\n", av_err2str(err));
	    	goto fail;
        }

        av_packet_free(&in_pkt);

        fprintf(stderr, "\rRate: %liMbps (video), %likbps (audio), Packets muxed: "
                "%liv, %lia, cache: %iv, %ia, %ip",
                av_rescale(rate_video, 8, 1000000), av_rescale(rate_audio, 8, 1000),
                video_packets, audio_packets, get_fifo_size(&ctx->video_frames),
                get_fifo_size(&ctx->audio_frames), get_fifo_size(&ctx->packet_buf));
    }

    free_sliding_win(&sctx_video);
    free_sliding_win(&sctx_audio);

    if ((err = av_write_trailer(ctx->avf))) {
        av_log(ctx, AV_LOG_ERROR, "Error writing trailer: %s!\n",
               av_err2str(err));
        goto fail;
    }

    av_log(ctx, AV_LOG_INFO, "Wrote trailer!\n");

    return NULL;

fail:
    ctx->err = err;
    return NULL;
}

static int get_video_info(void *opaque, FormatReport *info)
{
    struct capture_context *ctx = opaque;
    ctx->video_src_format = *info;
    return 0;
}

static int init_video_capture(struct capture_context *ctx)
{
    int err;
    uint64_t identifier = 0;

    init_fifo(&ctx->video_frames, ctx->video_frame_queue, FIFO_BLOCK_NO_INPUT);

    /* Init pulse */
    ctx->video_capture_source->init(&ctx->video_cap_ctx, NULL, ctx);

    /* Get all sources */
    SourceInfo *infos;
    int num_infos;
    ctx->video_capture_source->sources(ctx->video_cap_ctx, &infos, &num_infos);

    for (int i = 0; i < num_infos; i++) {
        av_log(ctx, AV_LOG_WARNING, "Video source %i: %s\n", i, infos[i].name);
        if (!av_strncasecmp(ctx->video_capture_target, infos[i].name, sizeof(ctx->video_capture_target)))
            identifier = infos[i].identifier;
    }

    /* Start capturing and init video encoder */
    err = ctx->video_capture_source->start(ctx->video_cap_ctx, identifier,
                                           ctx->video_capture_opts,
                                           &ctx->video_frames,
                                           &get_video_info, ctx);
    if (err)
        return err;

    return 0;
}

static void stop_video_capture(struct capture_context *ctx)
{
    SourceInfo *infos;
    int num_infos;
    ctx->video_capture_source->sources(ctx->video_cap_ctx, &infos, &num_infos);
    for (int i = 0; i < num_infos; i++)
	    ctx->video_capture_source->stop(ctx->video_cap_ctx, infos[i].identifier);
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
// TODO:    st->avg_frame_rate = ctx->video_src_format.avg_frame_rate;
    st->sample_aspect_ratio = av_make_q(1, 1);

    if (enc->codec->type == AVMEDIA_TYPE_VIDEO)
        ctx->video_streamid = st->id;

    if (enc->codec->type == AVMEDIA_TYPE_AUDIO)
        ctx->audio_streamid = st->id;

    int err = avcodec_parameters_from_context(st->codecpar, enc->avctx);
       if (err) {
               av_log(ctx, AV_LOG_ERROR, "Couldn't copy codec params: %s!\n",
                               av_err2str(err));
               return err;
       }

    return 0;
}

static int get_audio_info(void *opaque, FormatReport *info)
{
    struct capture_context *ctx = opaque;
    ctx->audio_src_format = *info;
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

static int init_audio_capture(struct capture_context *ctx)
{
    int err;
    uint64_t identifier = 0;

    /* Init the FIFO buffer */
    init_fifo(&ctx->audio_frames, ctx->audio_frame_queue, FIFO_BLOCK_NO_INPUT);

    /* Init pulse */
    ctx->audio_capture_source->init(&ctx->audio_cap_ctx, NULL, ctx);

    /* Get all sources */
    SourceInfo *infos;
    int num_infos;
    ctx->audio_capture_source->sources(ctx->audio_cap_ctx, &infos, &num_infos);

    for (int i = 0; i < num_infos; i++) {
        av_log(ctx, AV_LOG_WARNING, "Audio source %i: %s\n", i, infos[i].name);
        if (!av_strncasecmp(ctx->audio_capture_target, infos[i].name, sizeof(ctx->audio_capture_target)))
            identifier = infos[i].identifier;
    }

    /* Start capturing and init audio encoder */
    err = ctx->audio_capture_source->start(ctx->audio_cap_ctx, identifier,
                                           ctx->audio_capture_opts,
                                           &ctx->audio_frames,
                                           &get_audio_info, ctx);
    if (err)
        return err;

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

    /* If existing, init video capture */
    if (ctx->video_capture_source && (err = init_video_capture(ctx)))
        return err;

    /* If existing, init audio capture */
    if (ctx->audio_capture_source && (err = init_audio_capture(ctx)))
        return err;

    if (ctx->audio_capture_source) {
        /* Wait for frames */
        peek_from_fifo(&ctx->audio_frames);

        ctx->audio_encoder = alloc_encoding_ctx();

        ctx->audio_encoder->codec = avcodec_find_encoder_by_name("aac");
        ctx->audio_encoder->source_frames = &ctx->audio_frames;
        ctx->audio_encoder->dest_packets = &ctx->packet_buf;
        ctx->audio_encoder->input_fmt = &ctx->audio_src_format;
        ctx->audio_encoder->bitrate = lrintf(1000.0f * (160.0));
        ctx->audio_encoder->global_header_needed = ctx->avf->oformat->flags & AVFMT_GLOBALHEADER;

//        av_dict_set(&ctx->audio_encoder->encoder_opts, "application", "lowdelay", 0);
//        av_dict_set(&ctx->audio_encoder->encoder_opts, "vbr", "off", 0);
//        av_dict_set(&ctx->audio_encoder->encoder_opts, "frame_duration", "2.5", 0);

        init_encoder(ctx->audio_encoder);

        init_enc_muxing(ctx, ctx->audio_encoder);
    }

    if (ctx->video_capture_source) {
        /* Wait for frames */
        peek_from_fifo(&ctx->video_frames);

        ctx->video_encoder = alloc_encoding_ctx();

        ctx->video_encoder->codec = avcodec_find_encoder_by_name("h264_vaapi");
        ctx->video_encoder->source_frames = &ctx->video_frames;
        ctx->video_encoder->dest_packets = &ctx->packet_buf;
        ctx->video_encoder->pix_fmt = AV_PIX_FMT_NV12;
        ctx->video_encoder->input_fmt = &ctx->video_src_format;
        ctx->video_encoder->bitrate = lrintf(1000000.0f * (10.0));
        ctx->video_encoder->global_header_needed = ctx->avf->oformat->flags & AVFMT_GLOBALHEADER;

        av_dict_set(&ctx->video_encoder->encoder_opts, "preset", "superfast", 0);

        init_encoder(ctx->video_encoder);

        init_enc_muxing(ctx, ctx->video_encoder);
    }

    start_encoding_thread(ctx->video_encoder);
    start_encoding_thread(ctx->audio_encoder);

    if (ctx->video_encoder || ctx->audio_encoder) {
        av_log(ctx, AV_LOG_INFO, "Starting muxing thread!\n");
        pthread_create(&ctx->muxing_thread, NULL, muxing_thread, ctx);
        pthread_setname_np(ctx->muxing_thread, "muxing thread");
    }

    /* Run main loop */
    while (1) {

        usleep(10000);

        if (ctx->err || atomic_load(&ctx->quit))
            break;
    }

    /* Stop capturing */
    if (ctx->video_capture_source) stop_video_capture(ctx);
    if (ctx->audio_capture_source) stop_audio_capture(ctx);

    stop_encoding_thread(ctx->video_encoder);
    stop_encoding_thread(ctx->audio_encoder);

    push_to_fifo(&ctx->packet_buf, NULL);
    if (ctx->video_encoder || ctx->audio_encoder) pthread_join(ctx->muxing_thread, NULL);

    exit(0);

    return ctx->err;
}

static void uninit(struct capture_context *ctx)
{
#if PULSE_SRC
    src_pulse.free(&q_ctx->audio_cap_ctx);
#endif

    free_fifo(&ctx->video_frames, 0);
    free_fifo(&ctx->audio_frames, 0);
    free_fifo(&ctx->packet_buf, 0);

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
        .class_name  = "wlstream",
        .item_name   = av_default_item_name,
        .version     = LIBAVUTIL_VERSION_INT,
    });

    ctx.out_filename = argv[1];
    ctx.out_format = NULL;

    if (!strncmp(ctx.out_filename, "rtmp", 4))
        ctx.out_format = "flv";
    if (!strncmp(ctx.out_filename, "udp", 3))
        ctx.out_format = "mpegts";
    if (!strncmp(ctx.out_filename, "http", 3))
        ctx.out_format = "dash";

    /* Capture settings */
    ctx.video_capture_source = &src_wayland;
    ctx.video_capture_target = "0x0502";
    ctx.video_frame_queue = 16;
    av_dict_set_int(&ctx.video_capture_opts, "capture_cursor", 1, 0);
    av_dict_set_int(&ctx.video_capture_opts, "use_screencopy", 0, 0);

    ctx.audio_capture_source = &src_pulse;
    ctx.audio_capture_target = "alsa_output.pci-0000_00_1f.3.analog-stereo.monitor";
    ctx.audio_frame_queue = 256;
    av_dict_set_int(&ctx.video_capture_opts, "buffer_ms", 1, 0);

    ctx.video_streamid = ctx.audio_streamid = -1;

	if ((err = main_loop(&ctx)))
		goto end;

end:
	uninit(&ctx);
	return err;
}
