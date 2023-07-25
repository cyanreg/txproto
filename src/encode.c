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

#include <libavutil/avstring.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/cpu.h>

#include <libtxproto/encode.h>

#include "encoding_utils.h"
#include "os_compat.h"
#include <libtxproto/utils.h>
#include "utils.h"
#include "ctrl_template.h"

static int swr_configure(EncodingContext *ctx, AVFrame *conf)
{
    if (!conf)
        return 0;

    if (ctx->swr_configured_rate   == conf->sample_rate    &&
        ctx->swr_configured_layout == conf->channel_layout &&
        ctx->swr_configured_format == conf->format)
        return 0;

    av_opt_set_int           (ctx->swr, "in_sample_rate",     conf->sample_rate,              0);
    av_opt_set_channel_layout(ctx->swr, "in_channel_layout",  conf->channel_layout,           0);
    av_opt_set_sample_fmt    (ctx->swr, "in_sample_fmt",      conf->format,                   0);

    av_opt_set_int           (ctx->swr, "out_sample_rate",    ctx->avctx->sample_rate,        0);
    av_opt_set_channel_layout(ctx->swr, "out_channel_layout", ctx->avctx->channel_layout,     0);
    av_opt_set_sample_fmt    (ctx->swr, "out_sample_fmt",     ctx->avctx->sample_fmt,         0);

    av_opt_set_int(ctx->swr, "output_sample_bits", ctx->avctx->bits_per_raw_sample, 0);

    int err = swr_init(ctx->swr);
    if (err) {
        sp_log(ctx, SP_LOG_ERROR, "Could not init swr context: %s!\n", av_err2str(err));
        swr_free(&ctx->swr);
        return err;
    }

    ctx->swr_configured_rate = conf->sample_rate;
    ctx->swr_configured_layout = conf->channel_layout;
    ctx->swr_configured_format = conf->format;

    return 0;
}

static int init_avctx(EncodingContext *ctx, AVFrame *conf)
{
    FormatExtraData *fe = (FormatExtraData *)conf->opaque_ref->data;

    ctx->avctx->opaque                = ctx;
    ctx->avctx->time_base             = fe->time_base;
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

        ctx->avctx->sample_aspect_ratio = conf->sample_aspect_ratio;

        if (ctx->width && ctx->height) {
            ctx->avctx->width  = ctx->width;
            ctx->avctx->height = ctx->height;
        }

        if (ctx->pix_fmt != AV_PIX_FMT_NONE) {
            ctx->avctx->pix_fmt = ctx->pix_fmt;
        } else {
            const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(ctx->avctx->pix_fmt);
            if (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) {
                AVBufferRef *input_frames_ref = conf->hw_frames_ctx;
                AVHWFramesContext *hwfc = (AVHWFramesContext *)input_frames_ref->data;
                ctx->avctx->pix_fmt = hwfc->sw_format;
            }
        }

        if (fe->avg_frame_rate.num && fe->avg_frame_rate.den)
            ctx->avctx->framerate = fe->avg_frame_rate;
    }

    if (ctx->codec->type == AVMEDIA_TYPE_AUDIO) {
        /* Pick selected or input sample rate, then the one closest which the codec supports */
        ctx->avctx->sample_rate = ctx->sample_rate ? ctx->sample_rate : conf->sample_rate;
        ctx->avctx->sample_rate = pick_codec_sample_rate(ctx->codec, ctx->avctx->sample_rate);

        /* Same with the sample format */
        if (ctx->sample_fmt != AV_SAMPLE_FMT_NONE) {
            int bpsf = av_get_bytes_per_sample(ctx->avctx->sample_fmt) * 8;
            ctx->avctx->sample_fmt = pick_codec_sample_fmt(ctx->codec, ctx->sample_fmt, bpsf);
            ctx->avctx->bits_per_raw_sample = bpsf;
        } else {
            ctx->avctx->sample_fmt = pick_codec_sample_fmt(ctx->codec, conf->format, fe->bits_per_sample);
            ctx->avctx->bits_per_raw_sample = SPMIN(av_get_bytes_per_sample(ctx->avctx->sample_fmt) * 8,
                                                    fe->bits_per_sample);
        }

        if (ctx->channel_layout)
            ctx->avctx->channel_layout = pick_codec_channel_layout(ctx->codec, ctx->channel_layout);
        else
            ctx->avctx->channel_layout = pick_codec_channel_layout(ctx->codec, conf->channel_layout);

        ctx->avctx->channels = av_get_channel_layout_nb_channels(ctx->avctx->channel_layout);
    }

    if (ctx->need_global_header)
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
        if (err < 0) {
            sp_log(ctx, SP_LOG_ERROR, "Could not derive hardware device: %s!\n", av_err2str(err));
            goto end;
        }
    } else {
        err = av_hwdevice_ctx_create(&enc_device_ref, hwcfg->device_type,
                                     NULL, NULL, 0);
        if (err < 0) {
            sp_log(ctx, SP_LOG_ERROR, "Could not init hardware device: %s!\n", av_err2str(err));
            goto end;
        }
    }

    /* Derive only if there's a ref, and the width, height and format of the
     * source format match the encoding width, height and format. */
    if (input_frames_ref && hwfc && (hwfc->sw_format == ctx->avctx->pix_fmt) &&
        (hwfc->width == ctx->avctx->width) && (hwfc->height == ctx->avctx->height)) {
        err = av_hwframe_ctx_create_derived(&ctx->enc_frames_ref, hwcfg->pix_fmt,
                                            enc_device_ref, input_frames_ref, 0);
        if (err < 0)
            sp_log(ctx, SP_LOG_WARN, "Could not derive hardware frames context: %s!\n",
                   av_err2str(err));
        else
            goto set;
    }

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
    if (err < 0) {
        sp_log(ctx, SP_LOG_ERROR, "Could not init hardware frames context: %s!\n", av_err2str(err));
        goto end;
    }

set:
    hwfc = (AVHWFramesContext*)ctx->enc_frames_ref->data;

    ctx->avctx->pix_fmt = hwfc->format;
    ctx->avctx->thread_count = 1; /* Otherwise we spawn N threads */
    ctx->avctx->thread_type = 0;

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

static int context_full_config(EncodingContext *ctx)
{
    int err;

    err = sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_CONFIG, NULL);
    if (err < 0)
        return err;

    av_buffer_unref(&ctx->mode_negotiate_event);

    sp_log(ctx, SP_LOG_VERBOSE, "Getting a frame to configure...\n");
    AVFrame *conf = sp_frame_fifo_peek(ctx->src_frames);
    if (!conf) {
        sp_log(ctx, SP_LOG_ERROR, "No input frame to configure with!\n");
        return AVERROR(EINVAL);
    }

    if ((err = init_avctx(ctx, conf))) {
        av_frame_free(&conf);
        return err;
    }

    if (ctx->codec->type == AVMEDIA_TYPE_VIDEO) {
        if ((ctx->codec->capabilities & (AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_HYBRID))) {
            err = init_hwcontext(ctx, conf);
            if (err < 0) {
                av_frame_free(&conf);
                return err;
            }
        }
    }

    /* SWR */
    if (ctx->codec->type == AVMEDIA_TYPE_AUDIO) {
        err = swr_configure(ctx, conf);
        if (err < 0) {
            av_frame_free(&conf);
            return err;
        }
    }

    err = avcodec_open2(ctx->avctx, ctx->codec, NULL);
	if (err < 0) {
		sp_log(ctx, SP_LOG_ERROR, "Cannot open encoder: %s!\n", av_err2str(err));
		return err;
	}

    av_frame_free(&conf);

    sp_log(ctx, SP_LOG_VERBOSE, "Encoder configured!\n");

    return 0;
}

static int audio_process_frame(EncodingContext *ctx, AVFrame **input, int flush)
{
    int ret;
    int frame_size = ctx->avctx->frame_size;

    ret = swr_configure(ctx, *input);
    if (ret < 0)
        return ret;

    int64_t resampled_frame_pts = get_next_audio_pts(ctx, *input);

    /* Resample the frame, can be NULL */
    ret = swr_convert_frame(ctx->swr, NULL, *input);

    av_frame_free(input);

    if (ret < 0) {
        sp_log(ctx, SP_LOG_ERROR, "Error pushing audio for resampling: %s!\n", av_err2str(ret));
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
        sp_log(ctx, SP_LOG_ERROR, "Error allocating frame!\n");
        return AVERROR(ENOMEM);
    }

    out_frame->format                = ctx->avctx->sample_fmt;
    out_frame->channel_layout        = ctx->avctx->channel_layout;
    out_frame->sample_rate           = ctx->avctx->sample_rate;
    out_frame->pts                   = resampled_frame_pts;

    /* SWR sets this field to whatever it can output if it can't this much */
    out_frame->nb_samples            = frame_size;

    /* Get frame buffer */
    ret = av_frame_get_buffer(out_frame, 0);
    if (ret < 0) {
        sp_log(ctx, SP_LOG_ERROR, "Error allocating frame: %s!\n", av_err2str(ret));
        av_frame_free(&out_frame);
        return ret;
    }

    /* Resample */
    ret = swr_convert_frame(ctx->swr, out_frame, NULL);
    if (ret < 0) {
        sp_log(ctx, SP_LOG_ERROR, "Error pulling resampled audio: %s!\n", av_err2str(ret));
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
            if (err < 0) {
                sp_log(ctx, SP_LOG_ERROR, "Error mapping: %s!\n", av_err2str(err));
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

            tx_frame->width = in_f->width;
            tx_frame->height = in_f->height;

            if (ed->flags & AV_PIX_FMT_FLAG_HWACCEL) {
                AVHWFramesContext *enc_hwfc = (AVHWFramesContext *)ctx->enc_frames_ref->data;
                tx_frame->format = enc_hwfc->format;

                err = av_hwframe_get_buffer(ctx->enc_frames_ref, tx_frame, 0);
                if (err < 0) {
                    sp_log(ctx, SP_LOG_ERROR, "Error allocating frame: %s!\n", av_err2str(err));
                    return err;
                }
            } else {
                AVHWFramesContext *in_hwfc = (AVHWFramesContext *)in_f->hw_frames_ctx->data;
                tx_frame->format = in_hwfc->sw_format;

                err = av_frame_get_buffer(tx_frame, 4096);
                if (err < 0) {
                    sp_log(ctx, SP_LOG_ERROR, "Error allocating frame: %s!\n", av_err2str(err));
                    return err;
                }
            }

            err = av_hwframe_transfer_data(tx_frame, in_f, 0);
            if (err < 0) {
                sp_log(ctx, SP_LOG_ERROR, "Error transferring: %s!\n", av_err2str(err));
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
    AVPacket *out_pkt = NULL;

    sp_set_thread_name_self(sp_class_get_name(ctx));

    sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_INIT, NULL);

    sp_log(ctx, SP_LOG_VERBOSE, "Encoder initialized!\n");

    do {
        pthread_mutex_lock(&ctx->lock);

        AVFrame *frame = NULL;

        if (!flush) {
            frame = sp_frame_fifo_pop(ctx->src_frames);
            flush = !frame;
        }

        if (ctx->codec->type == AVMEDIA_TYPE_VIDEO) {
            ret = video_process_frame(ctx, &frame);
            if (ret < 0) {
                pthread_mutex_unlock(&ctx->lock);
                goto fail;
            }
        } else if (ctx->codec->type == AVMEDIA_TYPE_AUDIO) {
            ret = audio_process_frame(ctx, &frame, flush);
            if (ret == AVERROR(EAGAIN)) {
                pthread_mutex_unlock(&ctx->lock);
                continue;
            } else if (ret < 0) {
                pthread_mutex_unlock(&ctx->lock);
                goto fail;
            }
        }

        if (!atomic_load(&ctx->soft_flush)) {
            /* Give frame */
            ret = avcodec_send_frame(ctx->avctx, frame);
            av_frame_free(&frame);
            if (ret < 0) {
                sp_log(ctx, SP_LOG_ERROR, "Error encoding: %s!\n", av_err2str(ret));
                pthread_mutex_unlock(&ctx->lock);
                goto fail;
            }
        } else {
            sp_log(ctx, SP_LOG_VERBOSE, "Soft-flushing encoder\n");
            avcodec_flush_buffers(ctx->avctx);
            atomic_store(&ctx->soft_flush, 0);
            av_frame_free(&frame);
        }

        /* Return */
        while (1) {
            if (!out_pkt)
                out_pkt = av_packet_alloc();

            ret = avcodec_receive_packet(ctx->avctx, out_pkt);
            if (ret == AVERROR_EOF) {
                pthread_mutex_unlock(&ctx->lock);
                goto end;
            } else if (ret == AVERROR(EAGAIN)) {
                ret = 0;
                break;
            } else if (ret < 0) {
                sp_log(ctx, SP_LOG_ERROR, "Error encoding: %s!\n", av_err2str(ret));
                pthread_mutex_unlock(&ctx->lock);
                goto fail;
            }

            out_pkt->opaque = (void *)(intptr_t)sp_class_get_id(ctx);
            out_pkt->time_base = ctx->avctx->time_base;

            sp_log(ctx, SP_LOG_TRACE, "Pushing packet to FIFO, pts = %f\n",
                   av_q2d(ctx->avctx->time_base) * out_pkt->pts);

            sp_packet_fifo_push(ctx->dst_packets, out_pkt);
            av_packet_free(&out_pkt);
        }

        pthread_mutex_unlock(&ctx->lock);
    } while (!ctx->err);

end:
    av_packet_free(&out_pkt);
    sp_log(ctx, SP_LOG_VERBOSE, "Stream flushed!\n");

    sp_event_send_eos_packet(ctx, ctx->events, ctx->dst_packets, ret);

    return NULL;

fail:
    av_packet_free(&out_pkt);
    ctx->err = ret;

    sp_event_send_eos_packet(ctx, ctx->events, ctx->dst_packets, ret);

    atomic_store(&ctx->running, 0);
    return NULL;
}

static int encoder_ioctx_ctrl_cb(AVBufferRef *event_ref, void *callback_ctx,
                                 void *_ctx, void *dep_ctx, void *data)
{
    SPCtrlTemplateCbCtx *event = callback_ctx;
    EncodingContext *ctx = _ctx;

    if (event->ctrl & SP_EVENT_CTRL_START) {
        if (!sp_eventlist_has_dispatched(ctx->events, SP_EVENT_ON_CONFIG)) {
            int ret = context_full_config(ctx);
            if (ret < 0)
                return ret;
        }
        ctx->epoch = atomic_load(event->epoch);
        if (!ctx->encoding_thread)
            pthread_create(&ctx->encoding_thread, NULL, encoding_thread, ctx);
    } else if (event->ctrl & SP_EVENT_CTRL_STOP) {
        if (ctx->encoding_thread) {
            sp_frame_fifo_push(ctx->src_frames, NULL);
            pthread_join(ctx->encoding_thread, NULL);
            ctx->encoding_thread = 0;
        }
    } else if (event->ctrl & SP_EVENT_CTRL_OPTS) {
        const char *tmp_val = NULL;
        if ((tmp_val = dict_get(event->opts, "fifo_size"))) {
            long int len = strtol(tmp_val, NULL, 10);
            if (len < 0)
                sp_log(ctx, SP_LOG_ERROR, "Invalid fifo size \"%s\"!\n", tmp_val);
            else
                sp_packet_fifo_set_max_queued(ctx->src_frames, len);
        }
    } else if (event->ctrl & SP_EVENT_CTRL_FLUSH) {
        if (ctx->codec->capabilities & AV_CODEC_CAP_ENCODER_FLUSH) {
            atomic_store(&ctx->soft_flush, 1);
            sp_frame_fifo_push(ctx->src_frames, NULL);
        }
    } else {
        return AVERROR(ENOTSUP);
    }

    return 0;
}

int sp_encoder_ctrl(AVBufferRef *ctx_ref, SPEventType ctrl, void *arg)
{
    EncodingContext *ctx = (EncodingContext *)ctx_ref->data;
    return sp_ctrl_template(ctx, ctx->events, 0x0,
                            encoder_ioctx_ctrl_cb, ctrl, arg);
}

int sp_encoder_init(AVBufferRef *ctx_ref)
{
    int err;
    EncodingContext *ctx = (EncodingContext *)ctx_ref->data;

    if (!ctx->codec) {
        sp_log(ctx, SP_LOG_ERROR, "Missing codec!\n");
        return AVERROR(EINVAL);
    }

    ctx->avctx = avcodec_alloc_context3(ctx->codec);
    if (!ctx->avctx)
        return AVERROR(ENOMEM);

    if (!ctx->name) {
        int len = strlen(sp_class_get_name(ctx)) + 1 + strlen(ctx->codec->name) + 1;
        char *new_name = av_mallocz(len);
        if (!new_name) {
            err = AVERROR(ENOMEM);
            goto fail;
        }
        av_strlcpy(new_name, sp_class_get_name(ctx), len);
        av_strlcat(new_name, ":", len);
        av_strlcat(new_name, ctx->codec->name, len);
        sp_class_set_name(ctx, new_name);
        av_free(new_name);
        ctx->name = sp_class_get_name(ctx);
    } else {
        sp_class_set_name(ctx, ctx->name);
    }

    return 0;

fail:
    avcodec_free_context(&ctx->avctx);
    return err;
}

static void encoder_free(void *opaque, uint8_t *data)
{
    EncodingContext *ctx = (EncodingContext *)data;

    sp_frame_fifo_unmirror_all(ctx->src_frames);
    sp_frame_fifo_unmirror_all(ctx->dst_packets);

    if (ctx->encoding_thread) {
        sp_frame_fifo_push(ctx->src_frames, NULL);
        pthread_join(ctx->encoding_thread, NULL);
    }

    av_buffer_unref(&ctx->src_frames);
    av_buffer_unref(&ctx->dst_packets);

    sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_DESTROY, NULL);
    sp_bufferlist_free(&ctx->events);

    if (ctx->swr)
        swr_free(&ctx->swr);

    if (ctx->enc_frames_ref)
        av_buffer_unref(&ctx->enc_frames_ref);

    avcodec_free_context(&ctx->avctx);

    pthread_mutex_destroy(&ctx->lock);

    sp_log(ctx, SP_LOG_VERBOSE, "Encoder destroyed!\n");
    sp_class_free(ctx);
    av_free(ctx);
}

AVBufferRef *sp_encoder_alloc(void)
{
    EncodingContext *ctx = av_mallocz(sizeof(EncodingContext));
    if (!ctx)
        return NULL;

    AVBufferRef *ctx_ref = av_buffer_create((uint8_t *)ctx, sizeof(*ctx),
                                            encoder_free, NULL, 0);

    int err = sp_class_alloc(ctx, "lavc", SP_TYPE_ENCODER, NULL);
    if (err < 0) {
        av_buffer_unref(&ctx_ref);
        return NULL;
    }

    pthread_mutex_init(&ctx->lock, NULL);
    ctx->pix_fmt = AV_PIX_FMT_NONE;
    ctx->sample_fmt = AV_SAMPLE_FMT_NONE;
    ctx->events = sp_bufferlist_new();
    ctx->swr = swr_alloc();
    ctx->soft_flush = ATOMIC_VAR_INIT(0);

    ctx->src_frames = sp_frame_fifo_create(ctx, 8, FRAME_FIFO_BLOCK_NO_INPUT);
    ctx->dst_packets = sp_packet_fifo_create(ctx, 0, 0);

    return ctx_ref;
}
