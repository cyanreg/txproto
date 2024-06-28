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

#include <libavutil/buffer.h>
#include <libavutil/dict.h>
#include <libavutil/hwcontext.h>

typedef struct TXMainContext TXMainContext;

#include <libtxproto/events.h>
#include <libtxproto/io.h>

TXMainContext *tx_new(void);

int tx_init(TXMainContext *ctx);

void tx_free(TXMainContext *ctx);

int tx_epoch_set(TXMainContext *ctx, int64_t value);

int tx_commit(TXMainContext *ctx);

int tx_ctrl(TXMainContext *ctx, AVBufferRef *ref, SPEventType flags, void *arg);

AVBufferRef *tx_demuxer_create(
    TXMainContext *ctx,
    const char *name,
    const char *in_url,
    const char *in_format,
    AVDictionary *start_options,
    AVDictionary *init_opts
);

AVBufferRef *tx_decoder_create(
    TXMainContext *ctx,
    const char *dec_name,
    AVDictionary *init_opts
);

AVBufferRef *tx_encoder_create(
    TXMainContext *ctx,
    const char *enc_name,
    const char *name,
    AVDictionary *options,
    AVDictionary *init_opts
);

AVBufferRef *tx_muxer_create(
    TXMainContext *ctx,
    const char *out_url,
    const char *out_format,
    AVDictionary *options,
    AVDictionary *init_opts
);

AVBufferRef *tx_filtergraph_create(
    TXMainContext *ctx,
    const char *graph,
    enum AVHWDeviceType hwctx_type,
    AVDictionary *init_opts
);

int tx_link(
    TXMainContext *ctx,
    AVBufferRef *src,
    AVBufferRef *dst,
    int src_stream_id
);

int tx_filtergraph_command(TXMainContext *ctx, AVBufferRef *graph,
                           const char *filter_target, AVDictionary *commands);

int tx_destroy(
    TXMainContext *ctx,
    AVBufferRef **ref
);

int tx_event_register(
    TXMainContext *ctx,
    AVBufferRef *target,
    AVBufferRef *event
);

int tx_event_destroy(
    TXMainContext *ctx,
    AVBufferRef *event
);

AVBufferRef *tx_io_register_cb(
    TXMainContext *ctx,
    const char **api_list,
    int (*source_event_cb)(IOSysEntry *entry, void *userdata),
    void *userdata
);

AVBufferRef *tx_io_create(TXMainContext *ctx,
                          uint32_t identifier,
                          AVDictionary *opts);
