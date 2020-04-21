#define _GNU_SOURCE
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
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

#include "frame_fifo.h"
#include "src_common.h"
#include "utils.h"

#include "wlr-export-dmabuf-unstable-v1-client-protocol.h"

struct capture_context {
    AVClass *class; /* For pretty logging */

    /* If something happens during capture */
    int err;
    atomic_bool quit;

    /* For quitting reliably */

    /* Lavf */
    AVFormatContext *avf;
    pthread_mutex_t avf_lock;

    /* Video */
    int video_streamid;
    pthread_t video_thread;
    AVRational video_src_tb;
    AVRational video_dst_tb;
    AVFrameFIFO video_frames;
    AVCodecContext *video_avctx;
    AVBufferRef *mapped_device_ref;
    AVBufferRef *mapped_frames_ref;

    /* Audio */
    int audio_streamid;
    pthread_t audio_thread;
    AVCodecContext *audio_avctx;
    SwrContext *swr_ctx;
    AVRational audio_dst_tb;
    AVFrameFIFO audio_frames;

    /* Output */
    char *out_filename;
    char *out_format;

    /* Video capture */
    void *video_cap_ctx;
    CaptureSource *video_capture_source;
    FormatReport video_src_format;
    const char *video_capture_target;
    AVDictionary *video_capture_opts;

    /* Video encoder */
    char *video_encoder;
    float video_bitrate;
    int video_frame_queue;
    char *hardware_device;
    bool is_software_encoder;
    enum AVHWDeviceType hw_device_type;
    AVDictionary *video_encoder_opts;

    /* Audio capture */
    void *audio_cap_ctx;
    CaptureSource *audio_capture_source;
    FormatReport audio_src_format;
    const char *audio_capture_target;
    AVDictionary *audio_capture_opts;

    /* Audio encoder */
    char *audio_encoder;
    float audio_bitrate;
    int audio_dst_samplerate;
    int audio_frame_queue;
    AVDictionary *audio_encoder_opts;
};

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
	frames_ctx->sw_format = ctx->video_avctx->pix_fmt;
	frames_ctx->width = ctx->video_avctx->width;
	frames_ctx->height = ctx->video_avctx->height;

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
    int err;
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
    ctx->video_avctx->time_base         = ctx->video_dst_tb;
    ctx->video_avctx->compression_level = 7;
    ctx->video_avctx->width             = ctx->video_src_format.width;
    ctx->video_avctx->height            = ctx->video_src_format.height;

	if (ctx->avf->oformat->flags & AVFMT_GLOBALHEADER)
		ctx->video_avctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	/* Init hw frames context */
	if (!ctx->is_software_encoder)
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

void *video_encode_thread(void *arg)
{
	int err = 0;
	struct capture_context *ctx = arg;

    peek_from_fifo(&ctx->video_frames);

    /* Mapped hwcontext */
    if (ctx->hw_device_type)
        err = av_hwdevice_ctx_create(&ctx->mapped_device_ref,
                                     ctx->hw_device_type, ctx->hardware_device,
                                     NULL, 0);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create a hardware device: %s\n",
               av_err2str(err));
        goto end;
    }

    if ((err = setup_video_avctx(ctx))) {
        goto end;
    }

	do {
		AVFrame *in_f = NULL;
		if (get_fifo_size(&ctx->video_frames) || !atomic_load(&ctx->quit))
			in_f = pop_from_fifo(&ctx->video_frames);

        if (!ctx->is_software_encoder) {
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
        }

		if (0) {
			AVFrame *soft_frame = av_frame_alloc();
			av_hwframe_transfer_data(soft_frame, in_f, 0);
			soft_frame->pts = in_f->pts;
			av_frame_free(&in_f);
			in_f = soft_frame;
		}

        if (in_f)
            in_f->pts = av_rescale_q(in_f->pts, ctx->video_src_tb,
                                     ctx->video_avctx->time_base);

		err = avcodec_send_frame(ctx->video_avctx, in_f);

		av_frame_free(&in_f);

		if (err) {
			av_log(ctx, AV_LOG_ERROR, "Error encoding: %s!\n", av_err2str(err));
			goto end;
		}

		while (1) {
			AVPacket pkt;
			av_init_packet(&pkt);

			int ret = avcodec_receive_packet(ctx->video_avctx, &pkt);
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

            /* Depends on lavf init */
            pthread_mutex_lock(&ctx->avf_lock);
            pkt.stream_index = ctx->video_streamid;
            err = av_interleaved_write_frame(ctx->avf, &pkt);
            pthread_mutex_unlock(&ctx->avf_lock);

            av_packet_unref(&pkt);

            if (err) {
                av_log(ctx, AV_LOG_ERROR, "Writing video packet fail: %s!\n",
                       av_err2str(err));
                goto end;
            }
        };

		av_log(ctx, AV_LOG_INFO, "Encoded video frame %i (%i in queue)\n",
				ctx->video_avctx->frame_number, get_fifo_size(&ctx->video_frames));

	} while (!ctx->err);

end:
    if (!ctx->err && err)
        ctx->err = err;
    return NULL;
}

static enum AVSampleFormat get_codec_sample_fmt(AVCodec *codec)
{
    int i = 0;
    if (!codec->sample_fmts)
        return AV_SAMPLE_FMT_S16;
    while (1) {
        if (codec->sample_fmts[i] == -1)
            break;
        if (av_get_bytes_per_sample(codec->sample_fmts[i]) >= 2)
            return codec->sample_fmts[i];
        i++;
    }
    return codec->sample_fmts[0];
}

static int init_lavf(struct capture_context *ctx)
{
    /* Lock this until lavf is fully inititalized */
    pthread_mutex_init(&ctx->avf_lock, NULL);
    pthread_mutex_lock(&ctx->avf_lock);

    int err = avformat_alloc_output_context2(&ctx->avf, NULL, ctx->out_format,
	                                         ctx->out_filename);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Unable to init lavf context!\n");
        return err;
    }

    return 0;
}

static int init_muxing_context(struct capture_context *ctx)
{
	int err;
	int streamid = 0;

	if (ctx->video_encoder) { /* Video output stream */
        AVStream *st = avformat_new_stream(ctx->avf, NULL);
        if (!st) {
            av_log(ctx, AV_LOG_ERROR, "Unable to alloc stream!\n");
            return 1;
        }

        st->id = ctx->video_streamid = streamid++;
        st->time_base = ctx->video_avctx->time_base;
        st->avg_frame_rate = av_inv_q(ctx->video_src_format.time_base);

        err = avcodec_parameters_from_context(st->codecpar, ctx->video_avctx);
		if (err) {
			av_log(ctx, AV_LOG_ERROR, "Couldn't copy codec params: %s!\n",
					av_err2str(err));
			return err;
		}
	}

    if (ctx->audio_encoder) { /* Audio output stream */
        AVStream *st = avformat_new_stream(ctx->avf, NULL);
		if (!st) {
			av_log(ctx, AV_LOG_ERROR, "Unable to alloc stream!\n");
			return 1;
		}

        st->id = ctx->audio_streamid = streamid++;
		st->time_base = ctx->audio_avctx->time_base;

        err = avcodec_parameters_from_context(st->codecpar, ctx->audio_avctx);
		if (err) {
			av_log(ctx, AV_LOG_ERROR, "Couldn't copy codec params: %s!\n",
					av_err2str(err));
			return err;
		}
    }

    /* Debug print */
    av_dump_format(ctx->avf, 0, ctx->out_filename, 1);

	/* Open for writing */
    err = avio_open(&ctx->avf->pb, ctx->out_filename, AVIO_FLAG_WRITE);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Couldn't open %s: %s!\n", ctx->out_filename,
               av_err2str(err));
        return err;
    }

	err = avformat_write_header(ctx->avf, NULL);
	if (err) {
		av_log(ctx, AV_LOG_ERROR, "Couldn't write header: %s!\n", av_err2str(err));
		return err;
	}

    pthread_mutex_unlock(&ctx->avf_lock);

	return err;
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

        ctx->audio_avctx->opaque                = ctx;
        ctx->audio_avctx->bit_rate              = lrintf(ctx->audio_bitrate*1000.0f);
        ctx->audio_avctx->sample_fmt            = get_codec_sample_fmt(codec);
        ctx->audio_avctx->channel_layout        = ctx->audio_src_format.channel_layout;
        ctx->audio_avctx->sample_rate           = ctx->audio_dst_samplerate;
        ctx->audio_avctx->time_base             = ctx->audio_dst_tb;
        ctx->audio_avctx->channels = av_get_channel_layout_nb_channels(ctx->audio_avctx->channel_layout);
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

static int64_t conv_audio_pts(struct capture_context *ctx, int64_t in)
{
    int64_t m = (int64_t)ctx->audio_src_format.sample_rate *ctx->audio_dst_samplerate;

    int64_t b1 = (int64_t)ctx->audio_src_format.time_base.num * m;
    int64_t c1 = ctx->audio_src_format.time_base.den;
    int64_t swr_pts = av_rescale(in, b1, c1);

    int64_t swr_out_pts = swr_next_pts(ctx->swr_ctx, in == INT64_MIN ? in : swr_pts);

    int64_t b2 = ctx->audio_dst_tb.den;
    int64_t c2 = ctx->audio_dst_tb.num * m;
    int64_t out_pts = av_rescale(swr_out_pts, b2, c2);

    return out_pts;
}

void *audio_encode_thread(void *arg)
{
    struct capture_context *ctx = arg;
    int ret = 0, flushing = 0;

    AVFrame *in_frame = peek_from_fifo(&ctx->audio_frames);
    init_audio_encoding(ctx);

    int codec_frame_size = ctx->audio_avctx->frame_size;
    if (!codec_frame_size ||
        (ctx->audio_avctx->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE))
        codec_frame_size = in_frame->nb_samples; /* Something sane hopefully */

    do {
        if (!flushing) {
            in_frame = pop_from_fifo(&ctx->audio_frames);
            flushing = !in_frame;
        }

        int64_t resampled_frame_pts = conv_audio_pts(ctx, in_frame ? in_frame->pts : INT64_MIN);

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
            AVPacket out_pkt;
            av_init_packet(&out_pkt);
            ret = avcodec_receive_packet(ctx->audio_avctx, &out_pkt);
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

            out_pkt.stream_index = 0;

            pthread_mutex_lock(&ctx->avf_lock);
            out_pkt.stream_index = ctx->audio_streamid;
			ret = av_interleaved_write_frame(ctx->avf, &out_pkt);
			pthread_mutex_unlock(&ctx->avf_lock);

            av_packet_unref(&out_pkt);
            if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR, "Error muxing packet: %s!\n", av_err2str(ret));
                goto fail;
            }
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
        pthread_create(&ctx->audio_thread, NULL, audio_encode_thread, ctx);
        pthread_setname_np(ctx->audio_thread, "audio encode thread");
    }

    if (ctx->video_encoder) {
        pthread_create(&ctx->video_thread, NULL, video_encode_thread, ctx);
        pthread_setname_np(ctx->video_thread, "video encode thread");
        while (get_fifo_size(&ctx->video_frames) < 3);
    }

    /* Inits muxer and adds the audio and video streams */
    if ((ctx->video_encoder || ctx->audio_encoder) && (err = init_muxing_context(ctx)))
        return err;

    /* Run main loop */
    while (1) {

        usleep(10000);

        if (ctx->err || atomic_load(&ctx->quit))
            break;
    }

    push_to_fifo(&ctx->audio_frames, NULL);
    push_to_fifo(&ctx->video_frames, NULL);

    /* Stop capturing */
//    if (ctx->video_capture_source) stop_video_capture(ctx);
//    if (ctx->audio_capture_source) stop_audio_capture(ctx);

    /* Join with encoder threads */
    if (ctx->video_encoder && ctx->video_thread) pthread_join(ctx->video_thread, NULL);
    if (ctx->audio_encoder && ctx->audio_thread) pthread_join(ctx->audio_thread, NULL);

    if ((err = av_write_trailer(ctx->avf))) {
        av_log(ctx, AV_LOG_ERROR, "Error writing trailer: %s!\n",
               av_err2str(err));
        return err;
    }

    av_log(ctx, AV_LOG_INFO, "Wrote trailer!\n");

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
    av_dict_set_int(&ctx.video_capture_opts, "capture_cursor", 0, 0);
    av_dict_set_int(&ctx.video_capture_opts, "use_screencopy", 1, 0);

    ctx.video_encoder = "libx264rgb";
    ctx.video_bitrate = 12.0f;
    ctx.hw_device_type = av_hwdevice_find_type_by_name("vaapi");
    ctx.hardware_device = "/dev/dri/renderD128";
    ctx.video_frame_queue = 16;
    ctx.video_src_tb = (AVRational){ 1, 1000000000 };
    ctx.video_dst_tb = (AVRational){ 1, 1000 };
    av_dict_set(&ctx.video_encoder_opts, "preset", "superfast", 0);

    ctx.audio_capture_source = &src_pulse;
    ctx.audio_capture_target = "alsa_output.pci-0000_00_1f.3.analog-stereo.monitor";
//    ctx.audio_encoder = "flac";
    ctx.audio_bitrate = 128.0f;
    ctx.audio_dst_samplerate = 48000;
    ctx.audio_dst_tb = (AVRational){ 1, 1000 };
    ctx.audio_frame_queue = 256;

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
