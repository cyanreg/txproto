#define _GNU_SOURCE

#include "encoding.h"
#include "encoding_utils.h"

#include <pthread.h>
#include <libavutil/opt.h>

static int swr_configure(EncodingContext *ctx)
{
    if (!(ctx->swr = swr_alloc())) {
        av_log(ctx, AV_LOG_ERROR, "Could not alloc swr context!\n");
        return AVERROR(ENOMEM);
    }

    av_opt_set_int           (ctx->swr, "in_sample_rate",     ctx->input_fmt->sample_rate,    0);
    av_opt_set_channel_layout(ctx->swr, "in_channel_layout",  ctx->input_fmt->channel_layout, 0);
    av_opt_set_sample_fmt    (ctx->swr, "in_sample_fmt",      ctx->input_fmt->sample_fmt,     0);

    av_opt_set_int           (ctx->swr, "out_sample_rate",    ctx->avctx->sample_rate,        0);
    av_opt_set_channel_layout(ctx->swr, "out_channel_layout", ctx->avctx->channel_layout,     0);
    av_opt_set_sample_fmt    (ctx->swr, "out_sample_fmt",     ctx->avctx->sample_fmt,         0);

    int err = swr_init(ctx->swr);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Could not init swr context: %s!\n", av_err2str(err));
        swr_free(&ctx->swr);
        return err;
    }

    ctx->swr_configured_rate = ctx->input_fmt->sample_rate;

    return 0;
}

static int init_avctx(EncodingContext *ctx)
{
    ctx->avctx = avcodec_alloc_context3(ctx->codec);
    if (!ctx->avctx)
        return AVERROR(ENOMEM);

    ctx->avctx->opaque                = ctx;
    ctx->avctx->time_base             = ctx->input_fmt->time_base;
    ctx->avctx->bit_rate              = ctx->bitrate;
    ctx->avctx->compression_level     = 7;
    ctx->avctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    if (ctx->codec->type == AVMEDIA_TYPE_VIDEO) {
        ctx->avctx->width      = ctx->input_fmt->width;
        ctx->avctx->height     = ctx->input_fmt->height;
        ctx->avctx->pix_fmt    = ctx->input_fmt->sw_format;
        ctx->avctx->gop_size   = ctx->keyframe_interval;

        if (ctx->width && ctx->height) {
            ctx->avctx->width  = ctx->width;
            ctx->avctx->height = ctx->height;
        }

        if (ctx->pix_fmt != AV_PIX_FMT_NONE)
            ctx->avctx->pix_fmt = ctx->pix_fmt;
    }

    if (ctx->codec->type == AVMEDIA_TYPE_AUDIO) {
        ctx->avctx->sample_fmt     = pick_codec_sample_fmt(ctx->codec, ctx->input_fmt);
        ctx->avctx->channel_layout = pick_codec_channel_layout(ctx->codec, ctx->input_fmt);
        ctx->avctx->sample_rate    = pick_codec_sample_rate(ctx->codec, ctx->input_fmt);
        ctx->avctx->channels       = av_get_channel_layout_nb_channels(ctx->avctx->channel_layout);
    }

    if (ctx->global_header_needed)
        ctx->avctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    return 0;
}

static int init_hwcontext(EncodingContext *ctx)
{
    int err;
    AVHWFramesContext *hwfc;

    if (!ctx->input_device_ref && ctx->enc_frames_ref) {
        hwfc = (AVHWFramesContext*)ctx->input_frames_ref->data;
        ctx->input_device_ref = hwfc->device_ref;
    }

    if (ctx->input_device_ref) {
        err = av_hwdevice_ctx_create_derived(&ctx->enc_device_ref, ctx->hwcfg->device_type,
                                             ctx->input_device_ref, 0);
        if (err) {
            av_log(ctx, AV_LOG_ERROR, "Could not derive hardware device: %s!\n", av_err2str(err));
            return err;
        }
    } else {
        err = av_hwdevice_ctx_create(&ctx->enc_device_ref, ctx->hwcfg->device_type,
                                     NULL, NULL, 0);
        if (err) {
            av_log(ctx, AV_LOG_ERROR, "Could not init hardware device: %s!\n", av_err2str(err));
            return err;
        }
    }

    if (ctx->input_frames_ref) {
        err = av_hwframe_ctx_create_derived(&ctx->enc_frames_ref, ctx->hwcfg->pix_fmt,
                                            ctx->enc_device_ref, ctx->input_frames_ref, 0);
        if (err) {
            av_log(ctx, AV_LOG_ERROR, "Could not derive hardware frames context: %s!\n", av_err2str(err));
            return err;
        }
    } else {
        ctx->enc_frames_ref = av_hwframe_ctx_alloc(ctx->enc_device_ref);
        if (!ctx->enc_frames_ref)
            return AVERROR(ENOMEM);

        hwfc = (AVHWFramesContext*)ctx->enc_frames_ref->data;

        hwfc->format = ctx->hwcfg->pix_fmt;
        hwfc->sw_format = ctx->avctx->pix_fmt;
        hwfc->width = ctx->avctx->width;
        hwfc->height = ctx->avctx->height;

        err = av_hwframe_ctx_init(ctx->enc_frames_ref);
        if (err) {
            av_log(ctx, AV_LOG_ERROR, "Could not init hardware frames context: %s!\n", av_err2str(err));
            return err;
        }
    }

    hwfc = (AVHWFramesContext*)ctx->enc_frames_ref->data;
    ctx->avctx->pix_fmt = hwfc->format;

    ctx->avctx->hw_frames_ctx = av_buffer_ref(ctx->enc_frames_ref);
	if (!ctx->avctx->hw_frames_ctx) {
		av_buffer_unref(&ctx->enc_frames_ref);
		err = AVERROR(ENOMEM);
	}

    return 0;
}

int init_encoder(EncodingContext *ctx)
{
    int err;

    if (!ctx->codec) {
        av_log(ctx, AV_LOG_ERROR, "Missing codec!\n");
        return AVERROR(EINVAL);
    }
    if (!ctx->source_frames) {
        av_log(ctx, AV_LOG_ERROR, "No source frame FIFO!\n");
        return AVERROR(EINVAL);
    }
    if (!ctx->dest_packets) {
        av_log(ctx, AV_LOG_ERROR, "No destination packet FIFO!\n");
        return AVERROR(EINVAL);
    }

    init_avctx(ctx);

    if (ctx->codec->type == AVMEDIA_TYPE_VIDEO) {

        //ctx->hwcfg = get_codec_hw_config(ctx);

        AVCodecHWConfig *cfg = av_mallocz(sizeof(*ctx->hwcfg));
        cfg->pix_fmt = AV_PIX_FMT_VAAPI;
        cfg->device_type = av_hwdevice_find_type_by_name("vaapi");
        ctx->hwcfg = cfg;

        if ((ctx->codec->capabilities & AV_CODEC_CAP_HARDWARE) &&
            (ctx->hwcfg)) {
            err = init_hwcontext(ctx);
            if (err)
                return err;
        }
    }

    /* SWR */
    if (ctx->codec->type == AVMEDIA_TYPE_AUDIO) {
        err = swr_configure(ctx);
        if (err)
            return err;
    }

    err = avcodec_open2(ctx->avctx, ctx->codec, &ctx->encoder_opts);
	if (err) {
		av_log(ctx, AV_LOG_ERROR, "Cannot open encoder: %s!\n", av_err2str(err));
		return err;
	}

    return 0;
}

static int audio_process_frame(EncodingContext *ctx, AVFrame **input, int flush)
{
    int ret;
    int frame_size = ctx->avctx->frame_size;
 
    int64_t resampled_frame_pts = get_next_audio_pts(ctx, *input);

    /* Resample the frame, can be NULL */
    ret = swr_convert_frame(ctx->swr, NULL, *input);

    av_frame_free(input);

    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error pushing audio for resampling: %s!\n", av_err2str(ret));
        return ret;
    }

    /* Not enough output samples to get a frame */
    if (!flush) {
        int out_samples = swr_get_out_samples(ctx->swr, 0);
        if (!frame_size && out_samples)
            frame_size = out_samples;
        else if (!out_samples || ((frame_size) && (out_samples < frame_size)))
            return AVERROR(EAGAIN);
    } else if (!frame_size && ctx->swr) {
        frame_size = swr_get_out_samples(ctx->swr, 0);
        if (!frame_size)
            return 0;
    }

    if (flush && !ctx->swr)
        return 0;

    AVFrame *out_frame = NULL;
    if (!(out_frame = av_frame_alloc())) {
        av_log(ctx, AV_LOG_ERROR, "Error allocating frame!\n");
        return AVERROR(ENOMEM);
    }

    out_frame->format         = ctx->avctx->sample_fmt;
    out_frame->channel_layout = ctx->avctx->channel_layout;
    out_frame->sample_rate    = ctx->avctx->sample_rate;
    out_frame->pts            = resampled_frame_pts;

    /* SWR sets this field to whatever it can output if it can't this much */
    out_frame->nb_samples     = frame_size;

    /* Get frame buffer */
    ret = av_frame_get_buffer(out_frame, 0);
    if (ret) {
        av_log(ctx, AV_LOG_ERROR, "Error allocating frame: %s!\n", av_err2str(ret));
        av_frame_free(&out_frame);
        return ret;
    }

    /* Resample */
    ret = swr_convert_frame(ctx->swr, out_frame, NULL);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error pulling resampled audio: %s!\n", av_err2str(ret));
        av_frame_free(&out_frame);
        return ret;
    }

    /* swr has been drained */
    if (!out_frame->nb_samples) {
        av_frame_free(&out_frame);
        swr_free(&ctx->swr);
    }

    *input = out_frame;

    return 0;
}

static int video_process_frame(EncodingContext *ctx, AVFrame **input)
{
    int err;
    AVFrame *in_f = *input;
    if (!in_f)
        return 0;

    AVFrame *mapped_frame = av_frame_alloc();
    if (!mapped_frame)
        return AVERROR(ENOMEM);

    AVHWFramesContext *mapped_hwfc;
    mapped_hwfc = (AVHWFramesContext *)ctx->enc_frames_ref->data;
    mapped_frame->format = mapped_hwfc->format;
    mapped_frame->pts = in_f->pts;

    /* Set frame hardware context referencce */
    mapped_frame->hw_frames_ctx = av_buffer_ref(ctx->enc_frames_ref);
    if (!mapped_frame->hw_frames_ctx)
        return AVERROR(ENOMEM);

    err = av_hwframe_map(mapped_frame, in_f, AV_HWFRAME_MAP_READ);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Error mapping: %s!\n", av_err2str(err));
        return err;
    }

    /* Replace original frame with mapped */
    av_frame_free(&in_f);
    in_f = mapped_frame;
    in_f->colorspace = AVCOL_SPC_BT709;
    *input = in_f;

    return 0;
}

static void *encoding_thread(void *arg)
{
    EncodingContext *ctx = arg;
    int ret = 0, flush = 0;

    pthread_setname_np(pthread_self(), "audio encode thread");

    do {
        AVFrame *frame = NULL;

        if (!flush) {
            frame = pop_from_fifo(ctx->source_frames);
            flush = !frame;
        }

        if (ctx->codec->type == AVMEDIA_TYPE_VIDEO) {
            ret = video_process_frame(ctx, &frame);
            if (ret)
                goto fail;
        } else if (ctx->codec->type == AVMEDIA_TYPE_AUDIO) {
            ret = audio_process_frame(ctx, &frame, flush);
            if (ret == AVERROR(EAGAIN))
                continue;
            else if (ret)
                goto fail;
        }

        /* Give frame */
        ret = avcodec_send_frame(ctx->avctx, frame);
        av_frame_free(&frame);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Error encoding: %s!\n", av_err2str(ret));
            goto fail;
        }

        /* Return */
        while (1) {
            AVPacket *out_pkt = av_packet_alloc();

            ret = avcodec_receive_packet(ctx->avctx, out_pkt);
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

            out_pkt->stream_index = ctx->stream_id;

            push_to_fifo(ctx->dest_packets, out_pkt);
        }
    } while (!ctx->err);

end:
    av_log(ctx, AV_LOG_INFO, "Stream flushed!\n");

    return NULL;

fail:
    ctx->err = ret;

    return NULL;
}

int stop_encoding_thread(EncodingContext *ctx)
{
    if (!ctx || !ctx->avctx)
        return 0;

    push_to_fifo(ctx->source_frames, NULL);
    pthread_join(ctx->encoding_thread, NULL);

    return 0;
}

int start_encoding_thread(EncodingContext *ctx)
{
    if (!ctx || !ctx->avctx)
        return 0;

    av_log(ctx, AV_LOG_INFO, "Starting encoding thread!\n");
    pthread_create(&ctx->encoding_thread, NULL, encoding_thread, ctx);

    return 0;
}

EncodingContext *alloc_encoding_ctx(void)
{
    EncodingContext *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->class = av_mallocz(sizeof(*ctx->class));
    if (!ctx->class) {
        av_free(ctx);
        return NULL;
    }

    *ctx->class = (AVClass) {
        .class_name = "encoding",
        .item_name  = av_default_item_name,
        .version    = LIBAVUTIL_VERSION_INT,
    };

    ctx->pix_fmt = AV_PIX_FMT_NONE;

    return ctx;
}
