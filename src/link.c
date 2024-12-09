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

#include <libtxproto/commit.h>
#include <libtxproto/control.h>
#include <libtxproto/decode.h>
#include <libtxproto/demux.h>
#include <libtxproto/encode.h>
#include <libtxproto/filter.h>
#include <libtxproto/link.h>
#include <libtxproto/mux.h>

#include "iosys_common.h"
#ifdef HAVE_INTERFACE
#include "interface_common.h"
#endif

typedef struct EncoderModeNegotiate {
    int need_global;
} EncoderModeNegotiate;

static int encoder_mode_event_cb(AVBufferRef *event_ref, void *callback_ctx,
                                 void *ctx, void *dep_ctx, void *data)
{
    EncoderModeNegotiate *mode_ctx = callback_ctx;
    EncodingContext *enc = ctx;

    if (mode_ctx->need_global)
        enc->need_global_header = 1;

    return 0;
}

static int encoder_mode_negotiate(AVBufferRef *enc_ref, int want_global)
{
    EncodingContext *enc = (EncodingContext *)enc_ref->data;

    if (sp_eventlist_has_dispatched(enc->events, SP_EVENT_ON_CONFIG))
        return AVERROR(EINVAL);

    if (!enc->mode_negotiate_event) {
        AVBufferRef *event = sp_event_create(encoder_mode_event_cb, NULL,
                                             sizeof(EncoderModeNegotiate),
                                             NULL, SP_EVENT_FLAG_ONESHOT |
                                                   SP_EVENT_ON_CONFIG,
                                             enc, NULL);
        if (!event)
            return AVERROR(ENOMEM);

        int ret = sp_encoder_ctrl(enc_ref, SP_EVENT_CTRL_NEW_EVENT, event);
        if (ret < 0) {
            av_buffer_unref(&event);
            return ret;
        }

        enc->mode_negotiate_event = event;
    }

    EncoderModeNegotiate *neg_ctx = av_buffer_get_opaque(enc->mode_negotiate_event);
    neg_ctx->need_global |= !!want_global;

    return 0;
}

static SPBufferList *sp_ctx_get_events_list(void *ctx)
{
    enum SPType type = sp_class_get_type(ctx);
    switch (type) {
    case SP_TYPE_AUDIO_SOURCE:
    case SP_TYPE_AUDIO_SINK:
    case SP_TYPE_AUDIO_BIDIR:
    case SP_TYPE_VIDEO_SOURCE:
    case SP_TYPE_VIDEO_SINK:
    case SP_TYPE_VIDEO_BIDIR:
        return ((IOSysEntry *)ctx)->events;
    case SP_TYPE_MUXER:
        return ((MuxingContext *)ctx)->events;
    case SP_TYPE_FILTER:
        return ((FilterContext *)ctx)->events;
    case SP_TYPE_ENCODER:
        return ((EncodingContext *)ctx)->events;
    case SP_TYPE_DECODER:
        return ((DecodingContext *)ctx)->events;
    case SP_TYPE_DEMUXER:
        return ((DemuxingContext *)ctx)->events;
    default:
        break;
    }
    return NULL;
}

static AVBufferRef *sp_ctx_get_fifo(void *ctx, int out)
{
    enum SPType type = sp_class_get_type(ctx);
    switch (type) {
    case SP_TYPE_AUDIO_SOURCE:
    case SP_TYPE_AUDIO_SINK:
    case SP_TYPE_VIDEO_SOURCE:
    case SP_TYPE_VIDEO_SINK:
    case SP_TYPE_SUB_SOURCE:
    case SP_TYPE_SUB_SINK:
    case SP_TYPE_VIDEO_BIDIR:
    case SP_TYPE_AUDIO_BIDIR:
    case SP_TYPE_SUB_BIDIR:
    case SP_TYPE_SOURCE:
    case SP_TYPE_SINK:
    case SP_TYPE_INOUT:
        return ((IOSysEntry *)ctx)->frames;
    case SP_TYPE_MUXER:
        sp_assert(!out);
        return ((MuxingContext *)ctx)->src_packets;
    case SP_TYPE_FILTER:
        return NULL;
    case SP_TYPE_ENCODER:
        if (out)
            return ((EncodingContext *)ctx)->dst_packets;
        else
            return ((EncodingContext *)ctx)->src_frames;
#ifdef HAVE_INTERFACE
    case SP_TYPE_INTERFACE:
        return sp_interface_get_fifo(ctx);
#endif
    case SP_TYPE_DECODER:
        if (out)
            return ((DecodingContext *)ctx)->dst_frames;
        else
            return ((DecodingContext *)ctx)->src_packets;
    case SP_TYPE_DEMUXER:
        return NULL;
    default:
        sp_assert(0); /* Should never happen */
        return NULL;
    }
    sp_assert(0);
    return NULL;
}

typedef struct SPLinkCtx {
    char *src_filt_pad;
    char *dst_filt_pad;
    AVBufferRef *src_ref;
    AVBufferRef *dst_ref;

    int src_stream_id;
    char *src_stream_desc;
} SPLinkCtx;

static int link_fn(AVBufferRef *event_ref, void *callback_ctx, void *dst_ctx,
                   void *src_ctx, void *data)
{
    SPLinkCtx *cb_ctx = callback_ctx;

    enum SPType s_type = sp_class_get_type(src_ctx);
    enum SPType d_type = sp_class_get_type(dst_ctx);

    AVBufferRef *src_fifo = sp_ctx_get_fifo(src_ctx, 1);
    AVBufferRef *dst_fifo = sp_ctx_get_fifo(dst_ctx, 0);

    sp_log(dst_ctx, SP_LOG_VERBOSE, "Linking %s \"%s\"%s%s%s to "
                                            "%s \"%s\"%s%s%s\n",
           sp_class_type_string(src_ctx), sp_class_get_name(src_ctx),
           s_type != SP_TYPE_FILTER ? "" : " (pad: ",
           s_type != SP_TYPE_FILTER ? "" : (cb_ctx->src_filt_pad ? cb_ctx->src_filt_pad : "default"),
           s_type != SP_TYPE_FILTER ? "" : ")",

           sp_class_type_string(dst_ctx), sp_class_get_name(dst_ctx),
           d_type != SP_TYPE_FILTER ? "" : " (pad: ",
           d_type != SP_TYPE_FILTER ? "" : (cb_ctx->dst_filt_pad ? cb_ctx->dst_filt_pad : "default"),
           d_type != SP_TYPE_FILTER ? "" : ")");

    if ((s_type == SP_TYPE_FILTER) && (d_type == SP_TYPE_FILTER)) {
        return sp_map_pad_to_pad((FilterContext *)dst_ctx, cb_ctx->dst_filt_pad,
                                 (FilterContext *)src_ctx, cb_ctx->src_filt_pad);
    } else if (s_type == SP_TYPE_FILTER && (d_type == SP_TYPE_ENCODER)) {
        return sp_map_fifo_to_pad((FilterContext *)src_ctx, dst_fifo,
                                  cb_ctx->src_filt_pad, 1);
    } else if ((s_type & SP_TYPE_INOUT) && (d_type == SP_TYPE_FILTER)) {
        return sp_map_fifo_to_pad((FilterContext *)dst_ctx, src_fifo,
                                  cb_ctx->dst_filt_pad, 0);
    } else if ((s_type == SP_TYPE_ENCODER) && (d_type == SP_TYPE_MUXER)) {
        EncodingContext *src_enc_ctx  = src_ctx;
        MuxingContext   *dst_mux_ctx  = dst_ctx;

        sp_assert(dst_fifo && src_fifo);

        int err = sp_muxer_add_stream(dst_mux_ctx, src_enc_ctx);
        if (err < 0)
            return err;

        return sp_packet_fifo_mirror(dst_fifo, src_fifo);
    } else if ((s_type == SP_TYPE_DEMUXER) && (d_type == SP_TYPE_DECODER)) {
        DemuxingContext *src_mux_ctx = src_ctx;
        DecodingContext *dst_dec_ctx = dst_ctx;

        return sp_decoding_connect(dst_dec_ctx, src_mux_ctx,
                                   cb_ctx->src_stream_id, cb_ctx->src_stream_desc);
    } else if ((s_type & SP_TYPE_DECODER) && (d_type == SP_TYPE_ENCODER)) {
        sp_assert(dst_fifo && src_fifo);

        return sp_frame_fifo_mirror(dst_fifo, src_fifo);
    } else if ((s_type & SP_TYPE_DECODER) && (d_type == SP_TYPE_INTERFACE)) {
        sp_assert(dst_fifo && src_fifo);

        return sp_frame_fifo_mirror(dst_fifo, src_fifo);
    } else if (s_type == SP_TYPE_DECODER && (d_type == SP_TYPE_FILTER)) {
        sp_assert(!!src_fifo);

        return sp_map_fifo_to_pad((FilterContext *)dst_ctx, src_fifo,
                                  cb_ctx->dst_filt_pad, 0);
    } else if ((s_type & SP_TYPE_INOUT) && (d_type == SP_TYPE_ENCODER)) {
        if (!dst_fifo) {
            sp_log(dst_ctx, SP_LOG_VERBOSE, "Unable to get FIFO from interface, unsupported!\n");
            return AVERROR(EINVAL);
        }

        return sp_frame_fifo_mirror(dst_fifo, src_fifo);
    } else if ((s_type == SP_TYPE_FILTER) && (d_type == SP_TYPE_INTERFACE)) {
        if (!dst_fifo) {
            sp_log(dst_ctx, SP_LOG_VERBOSE, "Unable to get FIFO from interface, unsupported!\n");
            return AVERROR(EINVAL);
        }

        return sp_map_fifo_to_pad((FilterContext *)src_ctx, dst_fifo,
                                  cb_ctx->src_filt_pad, 1);
    } else if ((s_type & SP_TYPE_INOUT) && (d_type == SP_TYPE_INTERFACE)) {
        if (!dst_fifo) {
            sp_log(dst_ctx, SP_LOG_VERBOSE, "Unable to get FIFO from interface, unsupported!\n");
            return AVERROR(EINVAL);
        }

        return sp_frame_fifo_mirror(dst_fifo, src_fifo);
    } else {
        sp_assert(1); /* Should never happen */
    }

    sp_assert(0);

    return 0;
}

static void link_free(void *callback_ctx, void *dst_ctx, void *src_ctx)
{
    SPLinkCtx *cb_ctx = callback_ctx;
    av_free(cb_ctx->src_filt_pad);
    av_free(cb_ctx->dst_filt_pad);
    av_free(cb_ctx->src_stream_desc);
    av_buffer_unref(&cb_ctx->src_ref);
    av_buffer_unref(&cb_ctx->dst_ref);
}

int sp_generic_link(TXMainContext *ctx,
                    AVBufferRef *obj1,
                    AVBufferRef *obj2,
                    int autostart,
                    const char *src_pad_name,
                    const char *dst_pad_name,
                    int src_stream_id,
                    const char *src_stream_desc)
{
    int err;
    AVBufferRef *src_ref;
    AVBufferRef *dst_ref;
    char *src_filt_pad = NULL;
    char *dst_filt_pad = NULL;
    int stream_id = -1;
    char *stream_desc = NULL;
    ctrl_fn src_ctrl_fn = NULL;
    ctrl_fn dst_ctrl_fn = NULL;

#define EITHER(o1, o2, t1, t2)                                                                             \
    ((((sp_class_get_type(o1->data) & (t1)) == (t1)) && ((sp_class_get_type(o2->data) & (t2)) == (t2))) || \
     (((sp_class_get_type(o1->data) & (t2)) == (t2)) && ((sp_class_get_type(o2->data) & (t1)) == (t1))))

#define PICK_REF(o1, o2, type)                                   \
    av_buffer_ref(sp_class_get_type(o1->data) == type ? o1 : o2)

#define PICK_REF_INV(o1, o2, type)                               \
    av_buffer_ref(sp_class_get_type(o1->data) != type ? o1 : o2)

    if (EITHER(obj1, obj2, SP_TYPE_ENCODER, SP_TYPE_MUXER)) {
        src_ref = PICK_REF(obj1, obj2, SP_TYPE_ENCODER);
        dst_ref = PICK_REF(obj1, obj2, SP_TYPE_MUXER);
        src_ctrl_fn = sp_encoder_ctrl;
        dst_ctrl_fn = sp_muxer_ctrl;

        MuxingContext *dst_mux_ctx = (MuxingContext *)dst_ref->data;
        int mux_needs_global = dst_mux_ctx->avf->oformat->flags & AVFMT_GLOBALHEADER;

        err = encoder_mode_negotiate(src_ref, mux_needs_global);
        if (err != AVERROR(EINVAL) && err < 0)
            return err;
    } else if (EITHER(obj1, obj2, SP_TYPE_ENCODER, SP_TYPE_VIDEO_SOURCE) ||
               EITHER(obj1, obj2, SP_TYPE_ENCODER, SP_TYPE_AUDIO_SOURCE)) {
        src_ref = PICK_REF_INV(obj1, obj2, SP_TYPE_ENCODER);
        dst_ref = PICK_REF(obj1, obj2, SP_TYPE_ENCODER);
        src_ctrl_fn = ((IOSysEntry *)src_ref->data)->ctrl;
        dst_ctrl_fn = sp_encoder_ctrl;
    } else if (EITHER(obj1, obj2, SP_TYPE_ENCODER, SP_TYPE_FILTER)) {
        src_ref = PICK_REF(obj1, obj2, SP_TYPE_FILTER);
        dst_ref = PICK_REF(obj1, obj2, SP_TYPE_ENCODER);
        src_filt_pad = av_strdup(src_pad_name);
        src_ctrl_fn = sp_filter_ctrl;
        dst_ctrl_fn = sp_encoder_ctrl;
    } else if (EITHER(obj1, obj2, SP_TYPE_DECODER, SP_TYPE_FILTER)) {
        src_ref = PICK_REF(obj1, obj2, SP_TYPE_DECODER);
        dst_ref = PICK_REF(obj1, obj2, SP_TYPE_FILTER);
        dst_filt_pad = av_strdup(dst_pad_name);
        src_ctrl_fn = sp_decoder_ctrl;
        dst_ctrl_fn = sp_filter_ctrl;
    } else if (EITHER(obj1, obj2, SP_TYPE_FILTER, SP_TYPE_VIDEO_SOURCE) ||
               EITHER(obj1, obj2, SP_TYPE_FILTER, SP_TYPE_AUDIO_SOURCE)) {
        src_ref = PICK_REF_INV(obj1, obj2, SP_TYPE_FILTER);
        dst_ref = PICK_REF(obj1, obj2, SP_TYPE_FILTER);
        dst_filt_pad = av_strdup(dst_pad_name);
        src_ctrl_fn = ((IOSysEntry *)src_ref->data)->ctrl;
        dst_ctrl_fn = sp_filter_ctrl;
    } else if ((sp_class_get_type(obj1->data) == SP_TYPE_FILTER) &&
               (sp_class_get_type(obj2->data) == SP_TYPE_FILTER)) {
        /* NOTE: we're unable to determine order for filter to filter connections.
         * Assume that src is 1, dst is 2.
         * The API should be improved to remove such ambiguities. */
        src_ref = av_buffer_ref(obj1);
        dst_ref = av_buffer_ref(obj2);
        src_filt_pad = av_strdup(src_pad_name);
        dst_filt_pad = av_strdup(dst_pad_name);
        src_ctrl_fn = sp_filter_ctrl;
        dst_ctrl_fn = sp_filter_ctrl;
#ifdef HAVE_INTERFACE
    } else if (EITHER(obj1, obj2, SP_TYPE_INTERFACE, SP_TYPE_FILTER)) {
        src_ref = PICK_REF(obj1, obj2, SP_TYPE_FILTER);
        dst_ref = PICK_REF(obj1, obj2, SP_TYPE_INTERFACE);
        src_filt_pad = av_strdup(src_pad_name);
        src_ctrl_fn = sp_filter_ctrl;
        dst_ctrl_fn = sp_interface_ctrl;
    } else if (EITHER(obj1, obj2, SP_TYPE_INTERFACE, SP_TYPE_DECODER)) {
        src_ref = PICK_REF(obj1, obj2, SP_TYPE_DECODER);
        dst_ref = PICK_REF(obj1, obj2, SP_TYPE_INTERFACE);
        src_ctrl_fn = sp_decoder_ctrl;
        dst_ctrl_fn = sp_interface_ctrl;
    } else if (EITHER(obj1, obj2, SP_TYPE_INTERFACE, SP_TYPE_VIDEO_SOURCE)) {
        src_ref = PICK_REF_INV(obj1, obj2, SP_TYPE_INTERFACE);
        dst_ref = PICK_REF(obj1, obj2, SP_TYPE_INTERFACE);
        src_ctrl_fn = ((IOSysEntry *)src_ref->data)->ctrl;
        dst_ctrl_fn = sp_interface_ctrl;
#endif
    } else if (EITHER(obj1, obj2, SP_TYPE_ENCODER, SP_TYPE_DECODER)) {
        src_ref = PICK_REF(obj1, obj2, SP_TYPE_DECODER);
        dst_ref = PICK_REF(obj1, obj2, SP_TYPE_ENCODER);
        src_ctrl_fn = sp_decoder_ctrl;
        dst_ctrl_fn = sp_encoder_ctrl;
    } else if (EITHER(obj1, obj2, SP_TYPE_DEMUXER, SP_TYPE_DECODER)) {
        src_ref = PICK_REF(obj1, obj2, SP_TYPE_DEMUXER);
        dst_ref = PICK_REF(obj1, obj2, SP_TYPE_DECODER);
        stream_id = src_stream_id;
        stream_desc = av_strdup(src_stream_desc);
        src_ctrl_fn = sp_demuxer_ctrl;
        dst_ctrl_fn = sp_decoder_ctrl;
    } else {
        sp_log(ctx, SP_LOG_ERROR, "Unable to link \"%s\" (%s) to \"%s\" (%s)!\n",
               sp_class_get_name(obj1->data), sp_class_type_string(obj1->data),
               sp_class_get_name(obj2->data), sp_class_type_string(obj2->data));
        return AVERROR(EINVAL);
    }

    sp_log(ctx, SP_LOG_VERBOSE, "Linking \"%s\" (%s) to \"%s\" (%s)\n",
           sp_class_get_name(obj1->data), sp_class_type_string(obj1->data),
           sp_class_get_name(obj2->data), sp_class_type_string(obj2->data));

    void *sctx = (void *)src_ref->data;
    void *dctx = (void *)dst_ref->data;

    SPEventType flags = SP_EVENT_FLAG_ONESHOT        |
                        SP_EVENT_TYPE_LINK           |
                        sp_class_to_event_type(sctx) |
                        sp_class_to_event_type(dctx);

    SPBufferList *src_events = sp_ctx_get_events_list(sctx);
    if (!src_events) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to link \"%s\" (%s) to \"%s\" (%s)!",
               sp_class_get_name(obj1->data), sp_class_type_string(obj1->data),
               sp_class_get_name(obj2->data), sp_class_type_string(obj2->data));
        return AVERROR(EINVAL);
    }

    SPEventType src_post_init = sp_eventlist_has_dispatched(src_events,
                                                            SP_EVENT_ON_INIT);
    if (src_post_init)
        flags |= SP_EVENT_ON_COMMIT;
    else
        flags |= SP_EVENT_ON_CONFIG;

    if (!src_post_init)
        flags |= SP_EVENT_FLAG_DEPENDENCY;

    AVBufferRef *link_event = sp_event_create(link_fn, link_free,
                                              sizeof(SPLinkCtx), NULL, flags,
                                              dctx, sctx);

    SPLinkCtx *link_event_ctx = av_buffer_get_opaque(link_event);
    link_event_ctx->src_filt_pad = src_filt_pad;
    link_event_ctx->dst_filt_pad = dst_filt_pad;
    link_event_ctx->src_ref = src_ref;
    link_event_ctx->dst_ref = dst_ref;
    link_event_ctx->src_stream_id = stream_id;
    link_event_ctx->src_stream_desc = stream_desc;

    /* Add event to destination context */
    dst_ctrl_fn(dst_ref, SP_EVENT_CTRL_NEW_EVENT, link_event);

    /* Add dependency to source context, if needed */
    if (!src_post_init) {
        err = src_ctrl_fn(src_ref, SP_EVENT_CTRL_SIGNAL | SP_EVENT_ON_INIT, link_event);
        if (err < 0) {
            av_buffer_unref(&link_event);
            sp_log(ctx, SP_LOG_ERROR, "Unable to add linking event: %s!\n", av_err2str(err));
            return AVERROR(EINVAL);
        }
    }

    /* We don't need our reference any more */
    av_buffer_unref(&link_event);

    if (autostart) { /* These add a discard event, so we don't need to */
        err = sp_generic_ctrl(ctx, src_ref, SP_EVENT_CTRL_START, NULL);
        if (err < 0)
            return err;

        err = sp_generic_ctrl(ctx, dst_ref, SP_EVENT_CTRL_START, NULL);
        if (err < 0)
            return err;
    } else { /* But if we're not auto-starting them, we need to. */
        sp_add_discard_fn_to_list(ctx, sp_get_ctrl_fn(src_ref->data), src_ref);
        sp_add_discard_fn_to_list(ctx, sp_get_ctrl_fn(dst_ref->data), dst_ref);
    }

    return 0;
}
