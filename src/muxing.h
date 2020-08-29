#pragma once

#include "encoding.h"
#include <libavformat/avformat.h>
#include <stdatomic.h>

typedef struct MuxEncoderMap {
    int encoder_id;
    AVRational encoder_tb;

    int stream_id;
} MuxEncoderMap;

typedef struct MuxingContext {
    AVClass *class;
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

    AVBufferRef *src_packets;

    /* State */
    MuxEncoderMap *enc_map;
    int enc_map_size;

    int *stream_has_link;
    enum AVCodecID *stream_codec_id;
    atomic_int initialized;
    atomic_int running;

    int err;
} MuxingContext;

AVBufferRef *sp_muxer_alloc(void);
int  sp_muxer_init(AVBufferRef *ctx_ref);
int  sp_muxer_add_stream(AVBufferRef *ctx_ref, AVBufferRef *enc_ref);
int  sp_muxer_ctrl(AVBufferRef *ctx_ref, enum SPEventType ctrl, void *arg);
