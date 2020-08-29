#pragma once

#include <stdatomic.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>

#include "fifo_packet.h"
#include "fifo_frame.h"
#include "utils.h"

/* Video encoder - we scale and convert in the encoding thread */
typedef struct EncodingContext {
    AVClass *class;
    const char *name;
    int encoder_id;
    pthread_mutex_t lock;

    /* Needed to start */
    AVBufferRef *src_frames;
    AVBufferRef *dst_packets;
    AVCodec *codec;
    int need_global_header; /* Will be forced 0 on config reinit */

    /* Events */
    SPBufferList *events;

    /* State */
    atomic_int initialized;
    atomic_int running;

    /* Options */
    int crf;
    int keyframe_interval;

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
int sp_encoder_ctrl(AVBufferRef *ctx_ref, enum SPEventType ctrl, void *arg);
