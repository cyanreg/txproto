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

#pragma once

#include <stdatomic.h>
#include <libavformat/avformat.h>

#include "encoding.h"
#include "logging.h"

typedef struct MuxingContext {
    SPClass *class;

    const char *name;
    pthread_mutex_t lock;

    AVFormatContext *avf;
    pthread_t muxing_thread;

    /* Events */
    SPBufferList *events;

    int64_t epoch;
    const char *out_url;
    const char *out_format;
    int low_latency;
    int dump_info;
    char *dump_sdp_file;

    AVBufferRef *src_packets;

    /* State */
    struct MuxEncoderMap *enc_map;
    int enc_map_size;

    int *stream_has_link;
    enum AVCodecID *stream_codec_id;

    int err;
} MuxingContext;

AVBufferRef *sp_muxer_alloc(void);
int  sp_muxer_init(AVBufferRef *ctx_ref);
int  sp_muxer_add_stream(MuxingContext *ctx, EncodingContext *enc);
int  sp_muxer_ctrl(AVBufferRef *ctx_ref, SPEventType ctrl, void *arg);
