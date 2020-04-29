#pragma once

#include <libavfilter/avfilter.h>
#include "frame_fifo.h"

typedef struct FilterPad {
    struct FilterContext *main;
    char *name;
    enum AVMediaType type;
    int is_out;

    AVFrameFIFO *fifo;

    AVFilterContext *buffer;
    AVFilterContext *filter;
    int filter_pad;

    AVDictionary *metadata;
} FilterPad;

typedef struct FilterContext {
    AVClass *class;

    char *graph_str;
    int direct_filter;
    AVDictionary *direct_filter_opts;

    AVFilterGraph *graph;
    AVDictionary *graph_opts;

    FilterPad **in_pads;
    int num_in_pads;
    FilterPad **out_pads;
    int num_out_pads;

    // Graph is draining to either handle format changes (if input format
    // changes for one pad, recreate the graph after draining all buffered
    // frames), or undo previously sent EOF (libavfilter does not accept
    // input anymore after sending EOF, so recreate the graph to "unstuck" it).
    int draining_recover;

    int err;
} FilterContext;


FilterContext *alloc_filtering_ctx(void);
int re_create_filtering(FilterContext *ctx, int first_init);


int map_fifo_to_filtergraph(FilterContext ctx, const char *name,
                            int is_output, AVFrameFIFO *fifo);


