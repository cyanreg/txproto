#pragma once

#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/samplefmt.h>

#include "fifo_frame.h"

typedef struct FormatExtraData {
    AVRational avg_frame_rate;
    int bits_per_sample;
    AVRational time_base;
    int64_t clock_time;
} FormatExtraData;

typedef struct SourceInfo {
    char *name;
    char *desc;
    uint64_t identifier;
    uint64_t opaque; /* Opaque info to be used by the capture source only */
} SourceInfo;

typedef void (error_handler)(void *opaque, int error_code);

typedef const struct CaptureSource {
    /**
     * Capture API name
     */
    const char *name;

    /**
     * Initialize the API
     * The report_error callback may be called at any time from any thread
     */
    int  (*init)(void **s);

    /**
     * Get a list of sources, memory is handled by the capture source
     */
    void (*sources)(void *s, SourceInfo **sources, int *num);

    /**
     * Start encoding and dump frames into the FIFO given.
     * Will error out if called multiple times on the same identifier.
     * The ownership of the AVDict is transferred to the CaptureSource
     */
    int  (*start)(void *s, uint64_t identifier, AVDictionary *opts, SPFrameFIFO *dst,
                  error_handler *err_cb, void *error_handler_ctx);

    /**
     * Stops capture from a given identifier. Returns an error if not found.
     */
    int  (*stop)(void  *s, uint64_t identifier);

    /**
     * Stops and frees the capture API
     */
    void (*free)(void **s);
} CaptureSource;

extern const CaptureSource src_pulse;
extern const CaptureSource src_wayland;
