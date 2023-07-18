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
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>

#include <libtxproto/fifo_packet.h>
#include <libtxproto/fifo_frame.h>
#include <libtxproto/utils.h>
#include <libtxproto/log.h>
#include <libtxproto/demux.h>

/* Video encoder - we scale and convert in the encoding thread */
typedef struct DecodingContext {
    SPClass *class;

    const char *name;
    pthread_mutex_t lock;

    int64_t start_pts;
    int64_t epoch;

    /* Options */
    int low_latency;

    /* Needed to start */
    AVBufferRef *src_packets;
    AVBufferRef *dst_frames;
    const AVCodec *codec;

    /* Events */
    SPBufferList *events;

    /* State */
    atomic_int initialized;
    atomic_int running;

    /* Internals below */
    pthread_t decoding_thread;
    AVCodecContext *avctx;
    AVCodecParserContext *parser;

    /* Video */
    AVBufferRef *dec_frames_ref;

    int err;
} DecodingContext;

AVBufferRef *sp_decoder_alloc(void);
int sp_decoder_init(AVBufferRef *ctx_ref);
int sp_decoding_connect(DecodingContext *dec, DemuxingContext *mux,
                        int stream_id, char *stream_desc);
int sp_decoder_ctrl(AVBufferRef *ctx_ref, enum SPEventType ctrl, void *arg);
