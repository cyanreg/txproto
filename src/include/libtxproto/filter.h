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

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/dict.h>
#include <libavutil/hwcontext.h>

#include <libtxproto/utils.h>
#include "log.h"

typedef struct FilterPad {
    struct FilterContext *main;
    char *name;
    int is_out;
    enum AVMediaType type;
    AVDictionary *metadata; /* For filters outputting metadata, num_out_pads should be 1 */

    AVFilterContext *buffer;
    AVFilterContext *filter;
    int filter_pad;

    /* Input only */
    int eos;

    /* Output only */
    int dropped_frames;

    AVBufferRef *fifo;
    AVFrame *in_fmt; /* Used to track format changes */
} FilterPad;

typedef struct FilterContext {
    SPClass *class;

    SPBufferList *events;
    AVFilterGraph *graph;

    char **in_pad_names;
    char **out_pad_names;

    int dump_graph;
    int send_eos;

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
    enum AVPixelFormat direct_filter_fmt;
    AVDictionary *graph_opts;
    int direct_filter;

    /* I/O thread */
    pthread_t filter_thread;
    pthread_mutex_t lock;

    int64_t epoch;

    /* Failure */
    int err;
} FilterContext;

/* Allocate a filtering context */
AVBufferRef *sp_filter_alloc(void);

/**
 * Initializes a single filter, such as a scaling filter
 * name - custom name used for logging
 * filt - the name of the filter (ownership transferred, so must be av_malloc'd)
 * opts - an AVDictionary with the options for the filter, ownership goes to FilterContext
 * graph_opts - options for the filtergraph (threads, thread_type, etc, look: avfiltergraph.c),
 *              ownership goes to FilterContext
 * derive_device - derive a hardware device from the input frames and use it
 */
int sp_init_filter_single(AVBufferRef *ctx_ref, const char *name, const char *filt,
                          char **in_pad_names, char **out_pad_names, enum AVPixelFormat req_fmt,
                          AVDictionary *opts, AVDictionary *graph_opts,
                          enum AVHWDeviceType derive_device);

/**
 * Initializes a filtergraph, such as a complex multi-in, multi-out graph
 * name - custom name used for logging
 * graph - the filtergraph expression to be passed to lavfi (ownership transferred, so must be av_malloc'd)
 * opts - same as sp_init_filter_single
 * graph_opts - same as sp_init_filter_single
 * derive_device - same as sp_init_filter_single but iterate over all the sources with
 *                 a device and and try each until it succeeds
 */
int sp_init_filter_graph(AVBufferRef *ctx_ref, const char *name, const char *graph,
                         char **in_pad_names, char **out_pad_names,
                         AVDictionary *graph_opts, enum AVHWDeviceType derive_device);

/**
 * Maps a FIFO to a pad.
 */
int sp_map_fifo_to_pad(FilterContext *ctx, AVBufferRef *fifo,
                       const char *name, int is_out);

/**
 * Maps a pad to another pad (from another context).
 */
int sp_map_pad_to_pad(FilterContext *dst, const char *dst_pad,
                      FilterContext *src, const char *src_pad);

/**
 * Filter control.
 */
int sp_filter_ctrl(AVBufferRef *ctx_ref, SPEventType ctrl, void *arg);
