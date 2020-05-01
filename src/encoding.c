#define _GNU_SOURCE

#include "encoding.h"
#include "encoding_utils.h"

#include <pthread.h>
#include <libavutil/avstring.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>

static int swr_configure(EncodingContext *ctx, AVFrame *conf)
{
    /* Don't reallocate if unneded. */
    if (!ctx->swr && !(ctx->swr = swr_alloc())) {
        av_log(ctx, AV_LOG_ERROR, "Could not alloc swr context!\n");
        return AVERROR(ENOMEM);
    }

    av_opt_set_int           (ctx->swr, "in_sample_rate",     conf->sample_rate,              0);
    av_opt_set_channel_layout(ctx->swr, "in_channel_layout",  conf->channel_layout,           0);
    av_opt_set_sample_fmt    (ctx->swr, "in_sample_fmt",      conf->format,                   0);

    av_opt_set_int           (ctx->swr, "out_sample_rate",    ctx->avctx->sample_rate,        0);
    av_opt_set_channel_layout(ctx->swr, "out_channel_layout", ctx->avctx->channel_layout,     0);
    av_opt_set_sample_fmt    (ctx->swr, "out_sample_fmt",     ctx->avctx->sample_fmt,         0);

    int err = swr_init(ctx->swr);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Could not init swr context: %s!\n", av_err2str(err));
        swr_free(&ctx->swr);
        return err;
    }

    ctx->swr_configured_rate = conf->sample_rate;

    return 0;
}

static int init_avctx(EncodingContext *ctx, AVFrame *conf)
{
    FormatExtraData *fe = (FormatExtraData *)conf->opaque_ref->data;

    ctx->avctx = avcodec_alloc_context3(ctx->codec);
    if (!ctx->avctx)
        return AVERROR(ENOMEM);

    ctx->avctx->opaque                = ctx;
    ctx->avctx->time_base             = fe->time_base;
    ctx->avctx->bit_rate              = ctx->bitrate;
    ctx->avctx->compression_level     = 7;
    ctx->avctx->thread_count          = av_cpu_count();
    ctx->avctx->thread_type           = FF_THREAD_FRAME | FF_THREAD_SLICE;
    ctx->avctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    if (ctx->codec->type == AVMEDIA_TYPE_VIDEO) {
        ctx->avctx->width           = conf->width;
        ctx->avctx->height          = conf->height;
        ctx->avctx->pix_fmt         = conf->format;
        ctx->avctx->color_range     = conf->color_range;
        ctx->avctx->colorspace      = conf->colorspace;
        ctx->avctx->color_trc       = conf->color_trc;
        ctx->avctx->color_primaries = conf->color_primaries;

        ctx->avctx->gop_size        = ctx->keyframe_interval;
        ctx->avctx->global_quality  = ctx->crf;

        ctx->avctx->sample_aspect_ratio = conf->sample_aspect_ratio;

        if (ctx->width && ctx->height) {
            ctx->avctx->width  = ctx->width;
            ctx->avctx->height = ctx->height;
        }

        if (conf->hw_frames_ctx) {
            AVHWFramesContext *in_hwfc = (AVHWFramesContext *)conf->hw_frames_ctx->data;
            ctx->avctx->pix_fmt = in_hwfc->sw_format;
        }

        if (ctx->pix_fmt != AV_PIX_FMT_NONE)
            ctx->avctx->pix_fmt = ctx->pix_fmt;

        if (fe->avg_frame_rate.num && fe->avg_frame_rate.den)
            ctx->avctx->framerate = fe->avg_frame_rate;
    }

    if (ctx->codec->type == AVMEDIA_TYPE_AUDIO) {
        ctx->avctx->sample_fmt     = pick_codec_sample_fmt(ctx->codec, conf->format, fe->bits_per_sample);
        ctx->avctx->channel_layout = pick_codec_channel_layout(ctx->codec, conf->channel_layout);
        ctx->avctx->sample_rate    = pick_codec_sample_rate(ctx->codec, conf->sample_rate);
        ctx->avctx->channels       = av_get_channel_layout_nb_channels(ctx->avctx->channel_layout);

        if (ctx->sample_rate)
            ctx->avctx->sample_rate = ctx->sample_rate;
    }

    if (ctx->global_header_needed)
        ctx->avctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    return 0;
}

static int init_hwcontext(EncodingContext *ctx, AVFrame *conf)
{
    int err = 0;
    AVHWFramesContext *hwfc;
    AVBufferRef *input_device_ref = NULL;
    AVBufferRef *input_frames_ref = conf->hw_frames_ctx;
    AVBufferRef *enc_device_ref   = NULL;

    const AVCodecHWConfig *hwcfg = get_codec_hw_config(ctx);
    if (!hwcfg)
        return 0;

    if (input_frames_ref) {
        hwfc = (AVHWFramesContext *)input_frames_ref->data;
        input_device_ref = hwfc->device_ref;
    }

    if (input_device_ref) {
        err = av_hwdevice_ctx_create_derived(&enc_device_ref, hwcfg->device_type,
                                             input_device_ref, 0);
        if (err) {
            av_log(ctx, AV_LOG_ERROR, "Could not derive hardware device: %s!\n", av_err2str(err));
            goto end;
        }
    } else {
        err = av_hwdevice_ctx_create(&enc_device_ref, hwcfg->device_type,
                                     NULL, NULL, 0);
        if (err) {
            av_log(ctx, AV_LOG_ERROR, "Could not init hardware device: %s!\n", av_err2str(err));
            goto end;
        }
    }

    /* Derive only if there's a ref, and the width, height and format of the
     * source format match the encoding width, height and format. */
    if (input_frames_ref && hwfc && (hwfc->sw_format == ctx->avctx->pix_fmt) &&
        (hwfc->width == ctx->avctx->width) && (hwfc->height == ctx->avctx->height)) {
        err = av_hwframe_ctx_create_derived(&ctx->enc_frames_ref, hwcfg->pix_fmt,
                                            enc_device_ref, input_frames_ref, 0);
        if (err) {
            av_log(ctx, AV_LOG_ERROR, "Could not derive hardware frames context: %s!\n", av_err2str(err));
            goto end;
        }
    } else {
        ctx->enc_frames_ref = av_hwframe_ctx_alloc(enc_device_ref);
        if (!ctx->enc_frames_ref) {
            err = AVERROR(ENOMEM);
            goto end;
        }

        hwfc = (AVHWFramesContext*)ctx->enc_frames_ref->data;

        hwfc->format = hwcfg->pix_fmt;
        hwfc->sw_format = ctx->avctx->pix_fmt;
        hwfc->width = ctx->avctx->width;
        hwfc->height = ctx->avctx->height;

        err = av_hwframe_ctx_init(ctx->enc_frames_ref);
        if (err) {
            av_log(ctx, AV_LOG_ERROR, "Could not init hardware frames context: %s!\n", av_err2str(err));
            goto end;
        }
    }

    hwfc = (AVHWFramesContext*)ctx->enc_frames_ref->data;
    ctx->avctx->pix_fmt = hwfc->format;

    ctx->avctx->hw_frames_ctx = av_buffer_ref(ctx->enc_frames_ref);
	if (!ctx->avctx->hw_frames_ctx) {
		av_buffer_unref(&ctx->enc_frames_ref);
		err = AVERROR(ENOMEM);
	}

end:
    /* Hardware frames make their own ref */
    av_buffer_unref(&enc_device_ref);

    return err;
}

int init_encoder(EncodingContext *ctx)
{
    int err;

    if (!ctx->codec) {
        av_log(ctx, AV_LOG_ERROR, "Missing codec!\n");
        return AVERROR(EINVAL);
    }

    /* Also used as thread name */
    ctx->class->class_name = av_mallocz(16);
    av_strlcpy((char *)ctx->class->class_name, "lavc_", 16);
    av_strlcat((char *)ctx->class->class_name, ctx->codec->name, 16);

    if (!ctx->source_frames) {
        av_log(ctx, AV_LOG_ERROR, "No source frame FIFO!\n");
        return AVERROR(EINVAL);
    }
    if (!ctx->dest_packets) {
        av_log(ctx, AV_LOG_ERROR, "No destination packet FIFO!\n");
        return AVERROR(EINVAL);
    }

    AVFrame *conf = sp_frame_fifo_peek(ctx->source_frames);

    init_avctx(ctx, conf);

    if (ctx->codec->type == AVMEDIA_TYPE_VIDEO) {
        if ((ctx->codec->capabilities & AV_CODEC_CAP_HARDWARE)) {
            err = init_hwcontext(ctx, conf);
            if (err)
                return err;
        }
    }

    /* SWR */
    if (ctx->codec->type == AVMEDIA_TYPE_AUDIO) {
        err = swr_configure(ctx, conf);
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

    out_frame->format                = ctx->avctx->sample_fmt;
    out_frame->channel_layout        = ctx->avctx->channel_layout;
    out_frame->sample_rate           = ctx->avctx->sample_rate;
    out_frame->pts                   = resampled_frame_pts;

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

    /* Hardware mapping and transferring */
    if ((in_f->hw_frames_ctx || ctx->enc_frames_ref) &&
        (in_f->format != ctx->avctx->pix_fmt)) {
        const AVPixFmtDescriptor *id = av_pix_fmt_desc_get(in_f->format);
        const AVPixFmtDescriptor *ed = av_pix_fmt_desc_get(ctx->avctx->pix_fmt);
        if (in_f->hw_frames_ctx && ctx->enc_frames_ref) { /* Map */
            AVFrame *mapped_frame = av_frame_alloc();
            if (!mapped_frame)
                return AVERROR(ENOMEM);

            AVHWFramesContext *mapped_hwfc;
            mapped_hwfc = (AVHWFramesContext *)ctx->enc_frames_ref->data;
            mapped_frame->format = mapped_hwfc->format;

            /* Set frame hardware context referencce */
            mapped_frame->hw_frames_ctx = av_buffer_ref(ctx->enc_frames_ref);
            if (!mapped_frame->hw_frames_ctx)
                return AVERROR(ENOMEM);

            err = av_hwframe_map(mapped_frame, in_f, AV_HWFRAME_MAP_READ);
            if (err) {
                av_log(ctx, AV_LOG_ERROR, "Error mapping: %s!\n", av_err2str(err));
                return err;
            }

            /* Copy properties */
            av_frame_copy_props(mapped_frame, in_f);

            /* Replace original frame with mapped */
            av_frame_free(&in_f);
            in_f = mapped_frame;
        } else if ((id->flags ^ ed->flags) & AV_PIX_FMT_FLAG_HWACCEL) {
            AVFrame *tx_frame = av_frame_alloc();
            if (!tx_frame)
                return AVERROR(ENOMEM);

            if (ed->flags & AV_PIX_FMT_FLAG_HWACCEL) {
                AVHWFramesContext *enc_hwfc;
                enc_hwfc = (AVHWFramesContext *)ctx->enc_frames_ref->data;
                tx_frame->format = enc_hwfc->format;
                tx_frame->width = enc_hwfc->width;
                tx_frame->height = enc_hwfc->height;

                /* Set frame hardware context referencce */
                tx_frame->hw_frames_ctx = av_buffer_ref(ctx->enc_frames_ref);
                if (!tx_frame->hw_frames_ctx) {
                    err = AVERROR(ENOMEM);
                    return err;
                }

                av_hwframe_get_buffer(ctx->enc_frames_ref, tx_frame, 0);
            }

            err = av_hwframe_transfer_data(tx_frame, in_f, 0);
            if (err) {
                av_log(ctx, AV_LOG_ERROR, "Error transferring: %s!\n", av_err2str(err));
                return err;
            }

            av_frame_copy_props(tx_frame, in_f);
            av_frame_free(&in_f);
            in_f = tx_frame;
        }
    }

    /* Both or neither must have a hardware reference now */
    assert(!((!!in_f->hw_frames_ctx) ^ (!!ctx->enc_frames_ref)));

    int needed_scale_software = 0, needed_scale_hardware = 0;

    if (in_f->hw_frames_ctx && ctx->enc_frames_ref) {
        AVHWFramesContext *in_hwfc = (AVHWFramesContext *)in_f->hw_frames_ctx->data;
        AVHWFramesContext *enc_hwfc = (AVHWFramesContext *)ctx->enc_frames_ref->data;

        /* Must have been mapped by now */
        assert(in_f->format == ctx->avctx->pix_fmt);

        if ((in_hwfc->width     != enc_hwfc->width)     ||
            (in_hwfc->height    != enc_hwfc->height)    ||
            (in_hwfc->sw_format != enc_hwfc->sw_format))
            needed_scale_hardware = 1;
    } else if ((in_f->width  != ctx->avctx->width)   ||
               (in_f->height != ctx->avctx->height)  ||
               (in_f->format != ctx->avctx->pix_fmt)) {
        needed_scale_software = 1;
    }

    if (needed_scale_software == 1) {

    } else if (needed_scale_hardware == 1) {

    }

    *input = in_f;

    return 0;
}

static void *encoding_thread(void *arg)
{
    EncodingContext *ctx = arg;
    int ret = 0, flush = 0;

    pthread_setname_np(pthread_self(), ctx->class->class_name);

    do {
        AVFrame *frame = NULL;

        if (!flush) {
            frame = sp_frame_fifo_pop(ctx->source_frames);
            flush = !frame;
        }

        if (frame)
            frame->pts = av_add_stable(ctx->avctx->time_base, frame->pts,
                                       av_make_q(1, 1000000), ctx->ts_offset_us);

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

            sp_packet_fifo_push(ctx->dest_packets, out_pkt);
        }
    } while (!ctx->err);

end:
    av_log(ctx, AV_LOG_INFO, "Stream flushed!\n");

    return NULL;

fail:
    ctx->err = ret;

    return NULL;
}

void free_encoder(EncodingContext **s)
{
    if (!s || !*s)
        return;

    EncodingContext *ctx = *s;

    if (ctx->swr)
        swr_free(&ctx->swr);

    if (ctx->enc_frames_ref)
        av_buffer_unref(&ctx->enc_frames_ref);

    avcodec_free_context(&ctx->avctx);

    av_free((char *)ctx->class->class_name);
    av_free(ctx->class);
    av_freep(s);
}

int stop_encoding_thread(EncodingContext *ctx)
{
    if (!ctx || !ctx->avctx)
        return 0;

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
        .item_name  = av_default_item_name,
        .version    = LIBAVUTIL_VERSION_INT,
    };

    ctx->pix_fmt = AV_PIX_FMT_NONE;

    return ctx;
}
