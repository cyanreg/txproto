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
#include "log.h"

/* Video encoder - we scale and convert in the encoding thread */
typedef struct EncodingContext {
    SPClass *class;

    const char *name;
    pthread_mutex_t lock;

    int64_t epoch;
    atomic_int soft_flush;

    /* Needed to start */
    AVDictionary *codec_config;
    AVBufferRef *src_frames;
    AVBufferRef *dst_packets;
    const AVCodec *codec;
    int need_global_header;
    AVBufferRef *mode_negotiate_event; /* To negotiate need_global_header */

    /* Events */
    SPBufferList *events;

    /* State */
    atomic_int initialized;
    atomic_int running;

    /* Video options only */
    int width, height;
    enum AVPixelFormat pix_fmt;

    /* Audio options only */
    int sample_rate;
    enum AVSampleFormat sample_fmt;
    uint64_t channel_layout;

    /* Internals below */
    pthread_t encoding_thread;
    AVCodecContext *avctx;

    /* Video */
    AVBufferRef *enc_frames_ref;

    /* Audio */
    SwrContext *swr;
    int swr_configured_rate;
    uint64_t swr_configured_layout;
    int swr_configured_format;

    int err;
} EncodingContext;

AVBufferRef *sp_encoder_alloc(void);
int sp_encoder_init(AVBufferRef *ctx_ref);
int sp_encoder_ctrl(AVBufferRef *ctx_ref, SPEventType ctrl, void *arg);
