#pragma once

#include "src_common.h"
#include "frame_fifo.h"

#include <libswresample/swresample.h>
#include <libavutil/dict.h>

/* Video encoder - we scale and convert in the encoding thread */
typedef struct EncodingContext {
    AVClass *class; /* For pretty logging */

    /* Data needed to init */
    AVCodec *codec;
    AVFrameFIFO *source_frames;
    AVFrameFIFO *dest_packets;
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

int stop_encoding_thread(EncodingContext *ctx);
