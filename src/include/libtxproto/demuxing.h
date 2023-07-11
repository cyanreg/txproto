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

#include <libtxproto/utils.h>
#include "fifo_packet.h"
#include "logging.h"

typedef struct DemuxingContext {
    SPClass *class;

    const char *name;
    pthread_mutex_t lock;

    AVFormatContext *avf;
    pthread_t demuxing_thread;

    /* Events */
    SPBufferList *events;

    int64_t epoch;
    const char *in_url;
    const char *in_format;
    AVDictionary *start_options;

    AVBufferRef **dst_packets; // One per output stream
    AVCodecParameters par;

    int err;
} DemuxingContext;

AVBufferRef *sp_demuxer_alloc(void);
int  sp_demuxer_init(AVBufferRef *ctx_ref);
int  sp_demuxer_ctrl(AVBufferRef *ctx_ref, SPEventType ctrl, void *arg);
