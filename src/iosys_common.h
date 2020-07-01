#pragma once

#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>

#include "fifo_frame.h"
#include "utils.h"

typedef const struct IOSysAPI {
    /**
     * API name
     */
    const char *name;

    /**
     * Initialize the API
     */
    int (*init_sys)(AVBufferRef **ctx);

    /**
     * Initialize a source/sink, but don't start it.
     */
    int (*init_io)(AVBufferRef *ctx_ref, AVBufferRef *entry, AVDictionary *opts);

    /**
     * Refs an IOSysEntry by identifier or returns NULL if none found.
     */
    AVBufferRef *(*ref_entry)(AVBufferRef *ctx_ref, uint32_t identifier);

    /**
     * IOCTL interface.
     */
    int (*ctrl)(AVBufferRef *ctx, enum SPEventType ctrl, void *arg);
} IOSysAPI;

typedef struct IOSysEntry {
    AVClass *class;
    int log_lvl_offset;
    void *parent;

    char *name;
    char *desc;
    uint32_t identifier;
    uint32_t api_id;
    int is_default;
    int gone; /* Indicates a removed device */

    IOSysAPI *api;
    AVBufferRef *api_ctx;

    /* Video specific input properties */
    int scale;
    int width;
    int height;
    AVRational framerate;

    /* Audio specific input properties */
    int sample_rate;
    int sample_fmt;
    int channels;
    uint64_t channel_layout;
    float volume;

    /* Input/output FIFO */
    AVBufferRef *frames;

    /* Command interface */
    int (*ctrl)(AVBufferRef *entry, enum SPEventType ctrl, void *arg);
    SPBufferList *events;

    /* API-specific private data */
    void *api_priv;
    void *io_priv;
} IOSysEntry;

AVBufferRef *sp_bufferlist_iosysentry_by_id(AVBufferRef *ref, void *opaque);
uint32_t sp_iosys_gen_identifier(void *ctx, uint32_t num, uint32_t extra);

extern const IOSysAPI *sp_compiled_apis[];
extern const int sp_compiled_apis_len;
