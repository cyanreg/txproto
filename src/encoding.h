#pragma once

#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>

#include "src_common.h"
#include "fifo_packet.h"

/* Video encoder - we scale and convert in the encoding thread */
typedef struct EncodingContext {
    AVClass *class; /* For pretty logging */

    /* Data needed to init */
    AVCodec *codec;
    SPFrameFIFO *source_frames;
    SPPacketFIFO *dest_packets;
    int global_header_needed;

    /* Options */
    int64_t bitrate;
    int crf;
    int keyframe_interval;

    /* Set this before you start the thread */
    int stream_id;
    int64_t ts_offset_us;

    /* Video only */
    AVDictionary *encoder_opts;
    int sample_rate; /* If 0, use input or closes supported */
    int width, height; /* If either is 0, don't scale, use input */
    /* If AV_PIX_FMT_NONE, don't convert, use input pixfmt
     * If encoder is hardware, this is used as the sw_format */
    enum AVPixelFormat pix_fmt;

    /* Internals below */
    pthread_t encoding_thread;
    AVCodecContext *avctx;

    /* Video */
    AVBufferRef *enc_frames_ref;

    /* Audio */
    SwrContext *swr;
    int swr_configured_rate;

    int err;
} EncodingContext;

EncodingContext *alloc_encoding_ctx(void);

/* Fill in the options and the FIFO in the context, then call this */
int init_encoder(EncodingContext *ctx);

int start_encoding_thread(EncodingContext *ctx);

/* Once a thread is stopped it cannot be restarted */
int stop_encoding_thread(EncodingContext *ctx);

void free_encoder(EncodingContext **s);
