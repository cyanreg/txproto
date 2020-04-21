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
#include "packet_fifo.h"
#include "src_common.h"
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
    pthread_t video_thread;
    AVCodecContext *video_avctx;
    AVBufferRef *mapped_device_ref;
    AVBufferRef *mapped_frames_ref;

    /* Audio */
    pthread_t audio_thread;
    AVCodecContext *audio_avctx;
    SwrContext *swr_ctx;

    /* Video capture */
    void *video_cap_ctx;
    CaptureSource *video_capture_source;
    FormatReport video_src_format;
    const char *video_capture_target;
    AVDictionary *video_capture_opts;
    AVFrameFIFO video_frames;

    /* Video filter */
    struct filter_context filter_ctx;

    /* Video encoder */
    int video_width;
    int video_height;
    enum AVPixelFormat video_encoder_pix_fmt;
    char *video_encoder;
    float video_bitrate;
    int video_frame_queue;
    char *hardware_device;
    bool is_software_encoder;
    enum AVHWDeviceType hw_device_type;
    AVDictionary *video_encoder_opts;
    int keyframe_interval;

    /* Audio capture */
    void *audio_cap_ctx;
    CaptureSource *audio_capture_source;
    FormatReport audio_src_format;
    const char *audio_capture_target;
    AVDictionary *audio_capture_opts;
    AVFrameFIFO audio_frames;

    /* Audio encoder */
    char *audio_encoder;
    float audio_bitrate;
    int audio_dst_samplerate;
    int audio_frame_queue;
    AVDictionary *audio_encoder_opts;

    /* Muxing */
    pthread_t muxing_thread;
    int target_nb_streams;
    char *out_filename;
    char *out_format;
    int video_streamid;
    int audio_streamid;
    AVPacketFIFO packet_buf;
    AVFormatContext *avf;
    pthread_mutex_t avf_lock;
    pthread_mutex_t avf_conf_lock;
};

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

static int set_hwframe_ctx(struct capture_context *ctx,
                           AVBufferRef *hw_device_ctx)
{
	AVHWFramesContext *frames_ctx = NULL;
	int err = 0;

	if (!(ctx->mapped_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx)))
		return AVERROR(ENOMEM);

	AVHWFramesConstraints *cst;
	cst = av_hwdevice_get_hwframe_constraints(ctx->mapped_device_ref, NULL);
	if (!cst) {
		av_log(ctx, AV_LOG_ERROR, "Failed to get hw device constraints!\n");
		av_buffer_unref(&ctx->mapped_frames_ref);
		return AVERROR(ENOMEM);
	}

	frames_ctx = (AVHWFramesContext *)(ctx->mapped_frames_ref->data);
	frames_ctx->format = cst->valid_hw_formats[0];
	frames_ctx->sw_format = ctx->video_src_format.sw_format;
	frames_ctx->width = ctx->video_src_format.width;
	frames_ctx->height = ctx->video_src_format.height;

	av_hwframe_constraints_free(&cst);

	if ((err = av_hwframe_ctx_init(ctx->mapped_frames_ref))) {
		av_log(ctx, AV_LOG_ERROR, "Failed to initialize hw frame context: %s!\n",
				av_err2str(err));
		av_buffer_unref(&ctx->mapped_frames_ref);
		return err;
	}

	if (!ctx->is_software_encoder) {
		ctx->video_avctx->pix_fmt = frames_ctx->format;
		ctx->video_avctx->hw_frames_ctx = av_buffer_ref(ctx->mapped_frames_ref);
		if (!ctx->video_avctx->hw_frames_ctx) {
			av_buffer_unref(&ctx->mapped_frames_ref);
			err = AVERROR(ENOMEM);
		}
	}

	return err;
}

static int setup_video_avctx(struct capture_context *ctx)
{
    int err = 0;
    AVCodec *codec = avcodec_find_encoder_by_name(ctx->video_encoder);
    if (!codec) {
        av_log(ctx, AV_LOG_ERROR, "Codec not found (not compiled in lavc?)!\n");
        return AVERROR(EINVAL);
    }
    ctx->avf->oformat->video_codec = codec->id;
    ctx->is_software_encoder = !(codec->capabilities & AV_CODEC_CAP_HARDWARE);

    ctx->video_avctx = avcodec_alloc_context3(codec);
    if (!ctx->video_avctx)
        return 1;

    ctx->video_avctx->opaque            = ctx;
    ctx->video_avctx->bit_rate          = (int64_t)ctx->video_bitrate*1000000.0f;
    ctx->video_avctx->pix_fmt           = ctx->video_src_format.pix_fmt;
    ctx->video_avctx->time_base         = ctx->video_src_format.time_base;
    ctx->video_avctx->compression_level = 7;
    ctx->video_avctx->width             = ctx->video_src_format.width;
    ctx->video_avctx->height            = ctx->video_src_format.height;
    ctx->video_avctx->gop_size          = ctx->keyframe_interval;

    if (ctx->video_width)
        ctx->video_avctx->width = ctx->video_width;
    if (ctx->video_height)
        ctx->video_avctx->height = ctx->video_height;
    if (ctx->video_encoder_pix_fmt)
        ctx->video_avctx->pix_fmt = ctx->video_encoder_pix_fmt;

    if (ctx->avf->oformat->flags & AVFMT_GLOBALHEADER)
        ctx->video_avctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    /* Mapped hwcontext */
    if (ctx->hw_device_type)
        err = av_hwdevice_ctx_create(&ctx->mapped_device_ref,
                                     ctx->hw_device_type, ctx->hardware_device,
                                     NULL, 0);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create a hardware device: %s\n",
               av_err2str(err));
        return err;
    }

	/* Init hw frames context */
	if (ctx->hw_device_type != AV_HWDEVICE_TYPE_NONE)
        if ((err = set_hwframe_ctx(ctx, ctx->mapped_device_ref)))
            return err;

    err = avcodec_open2(ctx->video_avctx, codec, &ctx->video_encoder_opts);
	if (err) {
		av_log(ctx, AV_LOG_ERROR, "Cannot open encoder: %s!\n",
				av_err2str(err));
		return err;
	}

	return 0;
}

static int init_video_muxing(struct capture_context *ctx)
{
    int old_nb_streams = ctx->avf->nb_streams;

    AVStream *st = avformat_new_stream(ctx->avf, NULL);
    if (!st) {
        av_log(ctx, AV_LOG_ERROR, "Unable to alloc stream!\n");
        return AVERROR(ENOMEM);
    }

    st->id = ctx->video_streamid = old_nb_streams;
    st->time_base = ctx->video_avctx->time_base;
    st->avg_frame_rate = ctx->video_src_format.avg_frame_rate;
    st->sample_aspect_ratio = av_make_q(1, 1);

    int err = avcodec_parameters_from_context(st->codecpar, ctx->video_avctx);
	if (err) {
		av_log(ctx, AV_LOG_ERROR, "Couldn't copy codec params: %s!\n",
				av_err2str(err));
		return err;
	}

    return 0;
}

void *video_encode_thread(void *arg)
{
	int err = 0;
	struct capture_context *ctx = arg;

    pthread_setname_np(pthread_self(), "video encode thread");

	do {
		AVFrame *in_f = NULL;
		if (get_fifo_size(&ctx->video_frames) || !atomic_load(&ctx->quit))
			in_f = pop_from_fifo(&ctx->video_frames);

        /* If we have to filter, upload the frame */
        if (ctx->filter_ctx.graph && in_f && !in_f->hw_frames_ctx) {
            AVFrame *hard_frame = av_frame_alloc();
	        if (!hard_frame) {
		        err = AVERROR(ENOMEM);
		        goto end;
	        }

	        AVHWFramesContext *mapped_hwfc;
	        mapped_hwfc = (AVHWFramesContext *)ctx->mapped_frames_ref->data;
	        hard_frame->format = mapped_hwfc->format;
	        hard_frame->pts = in_f->pts;
	        hard_frame->width = mapped_hwfc->width;
	        hard_frame->height = mapped_hwfc->height;

	        /* Set frame hardware context referencce */
	        hard_frame->hw_frames_ctx = av_buffer_ref(ctx->mapped_frames_ref);
	        if (!hard_frame->hw_frames_ctx) {
		        err = AVERROR(ENOMEM);
		        goto end;
	        }

            av_hwframe_get_buffer(ctx->mapped_frames_ref, hard_frame, 0);

			av_hwframe_transfer_data(hard_frame, in_f, 0);
			hard_frame->pts = in_f->pts;
			av_frame_free(&in_f);
			in_f = hard_frame;
			in_f->colorspace = AVCOL_SPC_BT709;
        }  else if (in_f && in_f->hw_frames_ctx && ctx->mapped_frames_ref) {
            /* If we have a hardware frame and a mapping context, map the frame */

            AVFrame *mapped_frame = av_frame_alloc();
	        if (!mapped_frame) {
		        err = AVERROR(ENOMEM);
		        goto end;
	        }

	        AVHWFramesContext *mapped_hwfc;
	        mapped_hwfc = (AVHWFramesContext *)ctx->mapped_frames_ref->data;
	        mapped_frame->format = mapped_hwfc->format;
	        mapped_frame->pts = in_f->pts;

	        /* Set frame hardware context referencce */
	        mapped_frame->hw_frames_ctx = av_buffer_ref(ctx->mapped_frames_ref);
	        if (!mapped_frame->hw_frames_ctx) {
		        err = AVERROR(ENOMEM);
		        goto end;
	        }

            err = av_hwframe_map(mapped_frame, in_f, AV_HWFRAME_MAP_READ);
            if (err) {
                av_log(ctx, AV_LOG_ERROR, "Error mapping: %s!\n", av_err2str(err));
                goto end;
            }

            /* Replace original frame with mapped */
            av_frame_free(&in_f);
            in_f = mapped_frame;
            in_f->colorspace = AVCOL_SPC_BT709;
        }

        /* Filter */
        if (ctx->filter_ctx.graph) {
            err = av_buffersrc_add_frame_flags(ctx->filter_ctx.buffersrc_ctx, in_f,
                                   AV_BUFFERSRC_FLAG_NO_CHECK_FORMAT |
                                   AV_BUFFERSRC_FLAG_KEEP_REF);
            if (err) {
		        av_log(ctx, AV_LOG_ERROR, "Error filtering: %s!\n", av_err2str(err));
		        goto end;
	        }

	        av_frame_free(&in_f);

            AVFrame *filt_frame = av_frame_alloc();

            err = av_buffersink_get_frame(ctx->filter_ctx.buffersink_ctx, filt_frame);
            if (err == AVERROR(EAGAIN)) {
                av_frame_free(&filt_frame);
            } else if (err == AVERROR_EOF) {
                av_frame_free(&filt_frame);
            } else if (err) {
		        av_log(ctx, AV_LOG_ERROR, "Error filtering: %s!\n", av_err2str(err));
		        goto end;
	        } else {
	            in_f = filt_frame;
	        }
        }

        /* Download the mapped frame if its hardware and we have a software encoder */
		if (ctx->is_software_encoder && in_f && in_f->hw_frames_ctx) {
			AVFrame *soft_frame = av_frame_alloc();
			av_hwframe_transfer_data(soft_frame, in_f, 0);
			soft_frame->pts = in_f->pts;
			av_frame_free(&in_f);
			in_f = soft_frame;
		}

        if (in_f)
            in_f->pts = av_rescale_q(in_f->pts, ctx->video_src_format.time_base,
                                     ctx->video_avctx->time_base);

		err = avcodec_send_frame(ctx->video_avctx, in_f);

		av_frame_free(&in_f);

		if (err) {
			av_log(ctx, AV_LOG_ERROR, "Error encoding: %s!\n", av_err2str(err));
			goto end;
		}

		while (1) {
			AVPacket *out_pkt = av_packet_alloc();;

			int ret = avcodec_receive_packet(ctx->video_avctx, out_pkt);
			if (ret == AVERROR(EAGAIN)) {
				break;
			} else if (ret == AVERROR_EOF) {
				av_log(ctx, AV_LOG_INFO, "Encoder flushed (video)!\n");
				goto end;
			} else if (ret) {
				av_log(ctx, AV_LOG_ERROR, "Error encoding: %s!\n",
						av_err2str(ret));
				err = ret;
				goto end;
            }

            out_pkt->stream_index = ctx->video_streamid;

            pkt_push_to_fifo(&ctx->packet_buf, out_pkt);
        };
	} while (!ctx->err);

end:
    if (!ctx->err && err)
        ctx->err = err;
    return NULL;
}

static int init_lavf(struct capture_context *ctx)
{
    pkt_init_fifo(&ctx->packet_buf, 0);

    pthread_mutex_init(&ctx->avf_lock, NULL);
    pthread_mutex_init(&ctx->avf_conf_lock, NULL);
    pthread_mutex_lock(&ctx->avf_conf_lock);

    int err = avformat_alloc_output_context2(&ctx->avf, NULL, ctx->out_format,
	                                         ctx->out_filename);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Unable to init lavf context!\n");
        return err;
    }

    return 0;
}

void *muxing_thread(void *arg)
{
    struct capture_context *ctx = arg;

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
        AVPacket *in_pkt = pkt_pop_from_fifo(&ctx->packet_buf);
        if (!in_pkt)
            break;

        AVRational dst_tb = ctx->avf->streams[in_pkt->stream_index]->time_base;
        AVRational src_tb;
        if (in_pkt->stream_index == ctx->video_streamid) {
            src_tb = ctx->video_avctx->time_base;
            video_packets++;
        } else if (in_pkt->stream_index == ctx->audio_streamid) {
            src_tb = ctx->audio_avctx->time_base;
            audio_packets++;
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

        fprintf(stderr, "\rPackets muxed: %liv, %lia, cache: %iv, %ia, %ip", video_packets,
                audio_packets, get_fifo_size(&ctx->video_frames),
                get_fifo_size(&ctx->audio_frames), pkt_get_fifo_size(&ctx->packet_buf));
    }

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

    init_fifo(&ctx->video_frames, ctx->video_frame_queue);

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
                                           ctx->video_encoder ? &ctx->video_frames : NULL,
                                           &get_video_info, ctx);
    if (err)
        return err;

    return 0;
}

static enum AVSampleFormat pick_codec_sample_fmt(AVCodec *codec,
                                                 enum AVSampleFormat ifmt, int ibps)
{
    int i = 0;
    int max_bps = 0;
    enum AVSampleFormat max_bps_fmt = AV_SAMPLE_FMT_NONE;

    /* Accepts anything */
    if (!codec->sample_fmts)
        return ifmt;

    /* Try to match bits per sample */
    while (1) {
        if (codec->sample_fmts[i] == -1)
            break;
        int bps = av_get_bytes_per_sample(codec->sample_fmts[i]);
        if (bps > max_bps) {
            max_bps = bps;
            max_bps_fmt = codec->sample_fmts[i];
        }
        if (bps >= (ibps >> 3))
            return codec->sample_fmts[i];
        i++;
    }

    /* Return the best one */
    return max_bps_fmt;
}

static int pick_codec_sample_rate(AVCodec *codec, int irate)
{
    int i = 0, ret;
    if (!codec->supported_samplerates)
        return irate;

    /* Go to the array terminator (0) */
    while (codec->supported_samplerates[++i] > 0);
    /* Alloc, copy and sort array upwards */
    int *tmp = av_malloc(i*sizeof(int));
    memcpy(tmp, codec->supported_samplerates, i*sizeof(int));
    qsort(tmp, i, sizeof(int), cmp_numbers);

    /* Pick lowest one above the input rate, otherwise just use the highest one */
    for (int j = 0; j < i; j++) {
        ret = tmp[j];
        if (ret >= irate)
            break;
    }

    av_free(tmp);

    return ret;
}

static const uint64_t pick_codec_channel_layout(AVCodec *codec, uint64_t ilayout)
{
    int i = 0;
    int in_channels = av_get_channel_layout_nb_channels(ilayout);

    /* Supports anything */
    if (!codec->channel_layouts)
        return ilayout;

    /* Try to match */
    while (1) {
        if (!codec->channel_layouts[i])
            break;
        if (codec->channel_layouts[i] == ilayout)
            return codec->channel_layouts[i];
        i++;
    }

    i = 0;

    /* Try to match channel counts */
    while (1) {
        if (!codec->channel_layouts[i])
            break;
        if (av_get_channel_layout_nb_channels(codec->channel_layouts[i]) == in_channels)
            return codec->channel_layouts[i];
        i++;
    }

    /* Whatever */
    return codec->channel_layouts[0];
}

static void stop_video_capture(struct capture_context *ctx)
{
    SourceInfo *infos;
    int num_infos;
    ctx->video_capture_source->sources(ctx->video_cap_ctx, &infos, &num_infos);
    for (int i = 0; i < num_infos; i++)
	    ctx->video_capture_source->stop(ctx->video_cap_ctx, infos[i].identifier);
}

static int init_audio_encoding(struct capture_context *ctx)
{
    int err = 0;

    { /* Encoding avctx */
	    AVCodec *codec = avcodec_find_encoder_by_name(ctx->audio_encoder);
	    if (!codec) {
		    av_log(ctx, AV_LOG_ERROR, "Codec not found (not compiled in lavc?)!\n");
		    err = AVERROR(EINVAL);
		    goto fail;
	    }

	    ctx->avf->oformat->audio_codec = codec->id;

        ctx->audio_avctx = avcodec_alloc_context3(codec);
        if (!ctx->audio_avctx) {
            err = AVERROR(ENOMEM);
            goto fail;
        }

        ctx->audio_avctx->sample_fmt     = pick_codec_sample_fmt(codec, ctx->audio_src_format.sample_fmt,
                                                                 ctx->audio_src_format.bits_per_sample);
        ctx->audio_avctx->channel_layout = pick_codec_channel_layout(codec, ctx->audio_src_format.channel_layout);
        ctx->audio_avctx->sample_rate    = pick_codec_sample_rate(codec, ctx->audio_src_format.sample_rate);

        ctx->audio_avctx->channels       = av_get_channel_layout_nb_channels(ctx->audio_avctx->channel_layout);

        if (ctx->audio_avctx->sample_fmt == ctx->audio_src_format.sample_fmt)
            ctx->audio_avctx->bits_per_raw_sample = ctx->audio_src_format.bits_per_sample;

        ctx->audio_avctx->opaque              = ctx;
        ctx->audio_avctx->bit_rate            = lrintf(ctx->audio_bitrate*1000.0f);
        ctx->audio_avctx->time_base           = ctx->audio_src_format.time_base;
        ctx->audio_avctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

        if (ctx->avf->oformat->flags & AVFMT_GLOBALHEADER)
            ctx->audio_avctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        err = avcodec_open2(ctx->audio_avctx, codec, &ctx->audio_encoder_opts);
        if (err) {
            av_log(ctx, AV_LOG_ERROR, "Cannot open encoder: %s!\n",
                   av_err2str(err));
            goto fail;
        }
    }

    { /* SWR */
        if (!(ctx->swr_ctx = swr_alloc())) {
            av_log(ctx, AV_LOG_ERROR, "Could not alloc swr context!\n");
            err = AVERROR(ENOMEM);
            goto fail;
        }

        av_opt_set_int           (ctx->swr_ctx, "in_sample_rate",     ctx->audio_src_format.sample_rate,    0);
        av_opt_set_channel_layout(ctx->swr_ctx, "in_channel_layout",  ctx->audio_src_format.channel_layout, 0);
        av_opt_set_sample_fmt    (ctx->swr_ctx, "in_sample_fmt",      ctx->audio_src_format.sample_fmt,     0);

        av_opt_set_int           (ctx->swr_ctx, "out_sample_rate",    ctx->audio_avctx->sample_rate,        0);
        av_opt_set_channel_layout(ctx->swr_ctx, "out_channel_layout", ctx->audio_avctx->channel_layout,     0);
        av_opt_set_sample_fmt    (ctx->swr_ctx, "out_sample_fmt",     ctx->audio_avctx->sample_fmt,         0);

        if ((err = swr_init(ctx->swr_ctx))) {
            av_log(ctx, AV_LOG_ERROR, "Could not init swr context: %s!\n",
                   av_err2str(err));
            goto fail;
        }
    }

fail:
    return 0;
}

int64_t last_pts = 0;

static int64_t conv_audio_pts(struct capture_context *ctx, AVFrame *in)
{
    int64_t b, c, npts;

    int64_t m = (int64_t)ctx->audio_src_format.sample_rate * ctx->audio_dst_samplerate;

    int64_t in_pts = in ? in->pts : INT64_MIN;

    b    = (int64_t)ctx->audio_src_format.time_base.num * m;
    c    = ctx->audio_src_format.time_base.den;
    npts = in_pts == INT64_MIN ? INT64_MIN : av_rescale(in_pts, b, c);

    npts = swr_next_pts(ctx->swr_ctx, npts);

    b = ctx->audio_avctx->time_base.den;
    c = ctx->audio_avctx->time_base.num * m;
    int64_t out_pts = av_rescale(npts, b, c);

    return out_pts;
}

static int init_audio_muxing(struct capture_context *ctx)
{
    int old_nb_streams = ctx->avf->nb_streams;

    AVStream *st = avformat_new_stream(ctx->avf, NULL);
	if (!st) {
		av_log(ctx, AV_LOG_ERROR, "Unable to alloc stream!\n");
		return AVERROR(ENOMEM);
	}

    st->id = ctx->audio_streamid = old_nb_streams;
	st->time_base = ctx->audio_avctx->time_base;

    int err = avcodec_parameters_from_context(st->codecpar, ctx->audio_avctx);
	if (err) {
		av_log(ctx, AV_LOG_ERROR, "Couldn't copy codec params: %s!\n",
				av_err2str(err));
		return err;
	}

    return 0;
}

void *audio_encode_thread(void *arg)
{
    struct capture_context *ctx = arg;
    int ret = 0, flushing = 0;

    pthread_setname_np(pthread_self(), "audio encode thread");

    AVFrame *in_frame = peek_from_fifo(&ctx->audio_frames);

    int codec_frame_size = ctx->audio_avctx->frame_size;
    if (!codec_frame_size ||
        (ctx->audio_avctx->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE))
        codec_frame_size = in_frame->nb_samples; /* Something sane hopefully */

    do {
        if (!flushing) {
            in_frame = pop_from_fifo(&ctx->audio_frames);
            flushing = !in_frame;
        }

        int64_t resampled_frame_pts = conv_audio_pts(ctx, in_frame);

        /* Resample the frame, can be NULL */
        ret = swr_convert_frame(ctx->swr_ctx, NULL, in_frame);
        av_frame_free(&in_frame);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Error pushing audio for resampling: %s!\n", av_err2str(ret));
            goto fail;
        }

        /* Not enough output samples to get a frame */
        if (!flushing && (swr_get_out_samples(ctx->swr_ctx, 0) < codec_frame_size))
            continue;

        AVFrame *out_frame = NULL;
        if (!flushing || ctx->swr_ctx) {
            if (!(out_frame = av_frame_alloc())) {
                av_log(ctx, AV_LOG_ERROR, "Error allocating frame!\n");
                ret = AVERROR(ENOMEM);
                goto fail;
            }

            out_frame->format         = ctx->audio_avctx->sample_fmt;
            out_frame->channel_layout = ctx->audio_avctx->channel_layout;
            out_frame->sample_rate    = ctx->audio_avctx->sample_rate;
            out_frame->nb_samples     = codec_frame_size;
            out_frame->pts            = resampled_frame_pts;

            /* Get frame buffer */
            ret = av_frame_get_buffer(out_frame, 0);
            if (ret) {
                av_log(ctx, AV_LOG_ERROR, "Error allocating frame: %s!\n", av_err2str(ret));
                av_frame_free(&out_frame);
                goto fail;
            }

            /* Resample */
            ret = swr_convert_frame(ctx->swr_ctx, out_frame, NULL);
            if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR, "Error pulling resampled audio: %s!\n", av_err2str(ret));
                av_frame_free(&out_frame);
                goto fail;
            }

            /* swr has been drained */
            if (!out_frame->nb_samples) {
                av_frame_free(&out_frame);
                swr_free(&ctx->swr_ctx);
            }
        }

        /* Give frame */
        ret = avcodec_send_frame(ctx->audio_avctx, out_frame);
        av_frame_free(&out_frame);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Error encoding: %s!\n", av_err2str(ret));
            goto fail;
        }

        /* Return */
        while (1) {
            AVPacket *out_pkt = av_packet_alloc();

            ret = avcodec_receive_packet(ctx->audio_avctx, out_pkt);
            if (ret == AVERROR_EOF) {
                ret = 0;
                goto end;
            } else if (ret == AVERROR(EAGAIN)) {
                ret = 0;
                break;
            } else if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR, "Error encoding: %s!\n", av_err2str(ret));
                goto fail;
            }

            out_pkt->stream_index = ctx->audio_streamid;

            pkt_push_to_fifo(&ctx->packet_buf, out_pkt);
        }
    } while (!ctx->err);

end:
    av_log(ctx, AV_LOG_INFO, "Audio stream flushed!\n");

    return NULL;

fail:
    ctx->err = ret;

    return NULL;
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
    init_fifo(&ctx->audio_frames, ctx->audio_frame_queue);

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
                                           ctx->audio_encoder ? &ctx->audio_frames : NULL,
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

    if (ctx->audio_encoder) {
        /* Wait for frames */
        peek_from_fifo(&ctx->audio_frames);

        err = init_audio_encoding(ctx);
        if (err)
            return err;

        err = init_audio_muxing(ctx);
        if (err)
            return err;

        av_log(ctx, AV_LOG_INFO, "Starting audio encoding thread!\n");
        pthread_create(&ctx->audio_thread, NULL, audio_encode_thread, ctx);
    }

    if (ctx->video_encoder) {
        /* Wait for frames */
        peek_from_fifo(&ctx->video_frames);

        if ((err = setup_video_avctx(ctx)))
            return err;

        if (ctx->hw_device_type != AV_HWDEVICE_TYPE_NONE &&
            (ctx->video_width  != ctx->video_src_format.width ||
             ctx->video_height != ctx->video_src_format.height ||
             ctx->video_encoder_pix_fmt != ctx->video_src_format.sw_format))
            init_filtering(ctx, &ctx->filter_ctx);

        if ((err = init_video_muxing(ctx))) {
            return err;
        }

        av_log(ctx, AV_LOG_INFO, "Starting video encoding thread!\n");
        pthread_create(&ctx->video_thread, NULL, video_encode_thread, ctx);
    }

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

    /* Stop encoding threads */
    push_to_fifo(&ctx->audio_frames, NULL);
    push_to_fifo(&ctx->video_frames, NULL);

    /* Join with encoder threads */
    if (ctx->video_encoder && ctx->video_thread) pthread_join(ctx->video_thread, NULL);
    if (ctx->audio_encoder && ctx->audio_thread) pthread_join(ctx->audio_thread, NULL);

    pkt_push_to_fifo(&ctx->packet_buf, NULL);

    if (ctx->audio_encoder || ctx->video_encoder) pthread_join(ctx->muxing_thread, NULL);

    exit(0);

    return ctx->err;
}



static void uninit(struct capture_context *ctx)
{
#if PULSE_SRC
    src_pulse.free(&q_ctx->audio_cap_ctx);
#endif

    free_fifo(&ctx->video_frames);
    free_fifo(&ctx->audio_frames);

    av_buffer_unref(&ctx->mapped_frames_ref);
    av_buffer_unref(&ctx->mapped_device_ref);

    av_dict_free(&ctx->video_encoder_opts);
    av_dict_free(&ctx->audio_encoder_opts);

    avcodec_free_context(&ctx->video_avctx);
    avcodec_free_context(&ctx->audio_avctx);

    swr_free(&ctx->swr_ctx);

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

    /* Capture settings */
    ctx.video_capture_source = &src_wayland;
    ctx.video_capture_target = "0x0502";
    av_dict_set_int(&ctx.video_capture_opts, "capture_cursor", 1, 0);
    av_dict_set_int(&ctx.video_capture_opts, "use_screencopy", 1, 0);

    ctx.video_encoder = !ctx.video_capture_source ? NULL : "libx264";
    ctx.video_width = 1280*1;
    ctx.video_height = 720*1;
    ctx.video_encoder_pix_fmt = AV_PIX_FMT_NV12;
    ctx.video_bitrate = 24.0f;
    ctx.hw_device_type = av_hwdevice_find_type_by_name("vulkan");
    ctx.hardware_device = "0";
    ctx.keyframe_interval = 30;
    ctx.video_frame_queue = 16;
    av_dict_set(&ctx.video_encoder_opts, "preset", "superfast", 0);

    ctx.audio_capture_source = &src_pulse;
    ctx.audio_capture_target = "alsa_output.pci-0000_00_1f.3.analog-stereo.monitor";
    ctx.audio_encoder = !ctx.audio_capture_source ? NULL : "libopus";
    ctx.audio_bitrate = 164.0f;
    ctx.audio_dst_samplerate = 48000;
    ctx.audio_frame_queue = 256;

    ctx.target_nb_streams = !!ctx.video_encoder + !!ctx.audio_encoder;
    ctx.video_streamid = ctx.audio_streamid = -1;

    av_dict_set(&ctx.audio_encoder_opts, "application", "lowdelay", 0);
    av_dict_set(&ctx.audio_encoder_opts, "vbr", "off", 0);
    av_dict_set(&ctx.audio_encoder_opts, "frame_duration", "2.5", 0);
    av_dict_set(&ctx.audio_encoder_opts, "opus_delay", "2.5", 0);

	if ((err = main_loop(&ctx)))
		goto end;

end:
	uninit(&ctx);
	return err;
}
