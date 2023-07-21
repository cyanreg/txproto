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

#include "../config.h"

#ifdef HAVE_INTERFACE
#include "interface_common.h"
#endif

#include "iosys_common.h"

#include <libtxproto/control.h>
#include <libtxproto/commit.h>
#include <libtxproto/encode.h>
#include <libtxproto/decode.h>
#include <libtxproto/mux.h>
#include <libtxproto/filter.h>

ctrl_fn sp_get_ctrl_fn(void *ctx)
{
    enum SPType type = sp_class_get_type(ctx);
    switch (type) {
    case SP_TYPE_ENCODER:
        return sp_encoder_ctrl;
    case SP_TYPE_MUXER:
        return sp_muxer_ctrl;
    case SP_TYPE_DECODER:
        return sp_decoder_ctrl;
    case SP_TYPE_DEMUXER:
        return sp_demuxer_ctrl;
    case SP_TYPE_FILTER:
        return sp_filter_ctrl;
#ifdef HAVE_INTERFACE
    case SP_TYPE_INTERFACE:
        return sp_interface_ctrl;
#endif
    case SP_TYPE_AUDIO_SOURCE:
    case SP_TYPE_AUDIO_SINK:
    case SP_TYPE_AUDIO_BIDIR:
    case SP_TYPE_VIDEO_SOURCE:
    case SP_TYPE_VIDEO_SINK:
    case SP_TYPE_VIDEO_BIDIR:
    case SP_TYPE_SUB_SOURCE:
    case SP_TYPE_SUB_SINK:
    case SP_TYPE_SUB_BIDIR:
        return ((IOSysEntry *)ctx)->ctrl;
    default:
        break;
    }
    return NULL;
}

int sp_generic_ctrl(TXMainContext *ctx,
                    AVBufferRef *ref,
                    SPEventType flags,
                    void *arg)
{
    int err;

    ctrl_fn fn = sp_get_ctrl_fn(ref->data);
    if (!fn) {
        sp_log(ctx, SP_LOG_ERROR, "Unsupported CTRL type: %s!",
               sp_class_type_string(ref->data));
        return AVERROR(EINVAL);
    }

    if (!(flags & SP_EVENT_CTRL_MASK)) {
        sp_log(ctx, SP_LOG_ERROR, "Missing ctrl: command: %s!",
               av_err2str(AVERROR(EINVAL)));
        return AVERROR(EINVAL);
    } else if (flags & SP_EVENT_ON_MASK) {
        sp_log(ctx, SP_LOG_ERROR, "Event specified but given to a ctrl, use %s.schedule: %s!",
               sp_class_get_name(ref->data), av_err2str(AVERROR(EINVAL)));
        return AVERROR(EINVAL);
    } else if ((flags & SP_EVENT_CTRL_OPTS) && (!arg)) {
        sp_log(ctx, SP_LOG_ERROR, "No options specified for ctrl:opts: %s!",
               av_err2str(AVERROR(EINVAL)));
        return AVERROR(EINVAL);
    }

    if (flags & SP_EVENT_CTRL_START)
        err = fn(ref, flags, &ctx->epoch_value);
    else
        err = fn(ref, flags, arg);
    if (err < 0) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to process CTRL: %s",
               av_err2str(err));
        return AVERROR(EINVAL);
    }

    if (!(flags & SP_EVENT_FLAG_IMMEDIATE))
        sp_add_commit_fn_to_list(ctx, fn, ref);

    return 0;
}
