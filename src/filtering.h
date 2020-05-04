#pragma once

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/dict.h>
#include <libavutil/hwcontext.h>

#include "src_common.h"

typedef struct FilterPad {
    struct FilterContext *main;
    const char *name;
    int is_out;
    enum AVMediaType type;
    AVDictionary *metadata; /* For filters outputting metadata, num_out_pads should be 1 */

    AVFilterContext *buffer;
    AVFilterContext *filter;
    int filter_pad;

    /* Output only */
    int dropped_frames;
    int dropped_frames_msg_state;

    SPFrameFIFO *fifo;
    AVFrame *in_fmt; /* Used to track format changes */
} FilterPad;

typedef struct FilterContext {
    AVClass *class;
    AVFilterGraph *graph;

    /* Derived from input device reference */
    enum AVHWDeviceType device_type;
    AVBufferRef *hw_device_ref;

    /* Pads - inputs */
    FilterPad **in_pads;
    int num_in_pads;

    /* Pads - outputs */
    FilterPad **out_pads;
    int num_out_pads;

    /* Need these for reinit */
    const char *graph_str;
    AVDictionary *direct_filter_opts;
    AVDictionary *graph_opts;
    int direct_filter;

    /* I/O thread */
    pthread_t filter_thread;

    /* Failure */
    int err;
} FilterContext;

/* Allocate a filtering context */
FilterContext *alloc_filtering_ctx(void);

/**
 * Initializes a single filter, such as a scaling filter
 * name - the name of the filter, without options (ownership transferred, must be av_malloc'd)
 * opts - an AVDictionary with the options for the filter, ownership goes to FilterContext
 * graph_opts - options for the filtergraph (threads, thread_type, etc, look: avfiltergraph.c),
 *              ownership goes to FilterContext
 * derive_device - derive a hardware device from the input frames and use it
 *                 
 */
int sp_init_filter_single(FilterContext *ctx, const char *name,
                          AVDictionary *opts, AVDictionary *graph_opts,
                          enum AVHWDeviceType derive_device);

/**
 * Initializes a filtergraph, such as a complex multi-in, multi-out graph
 * graph - the filtergraph expression to be passed to lavfi (ownership transferred, must be av_malloc'd)
 * opts - same as sp_init_filter_single
 * graph_opts - same as sp_init_filter_single
 * derive_device - same as sp_init_filter_single but iterate over all the sources with
 *                 a device and and try each until it succeeds
 */
int sp_init_filter_graph(FilterContext *ctx, const char *graph, AVDictionary *graph_opts,
                         enum AVHWDeviceType derive_device);

/**
 * Maps a FIFO to a pad. A single FIFO may be mapped to multiple input pads,
 * but output FIFOs must be unique.
 */
int sp_map_fifo_to_pad(FilterContext *ctx, SPFrameFIFO *fifo, int pad_idx, int is_out);


int sp_filter_init_graph(FilterContext *ctx);



