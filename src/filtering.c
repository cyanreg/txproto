#define _GNU_SOURCE
#include <pthread.h>

#include <libavutil/time.h>
#include <libavfilter/buffersink.h>

#include "fifo_frame.h"
#include "filtering.h"
#include "utils.h"

FN_CREATING(FilterContext, FilterPad, in_pad, in_pads, num_in_pads)
FN_CREATING(FilterContext, FilterPad, out_pad, out_pads, num_out_pads)

static void free_graph(FilterContext *ctx)
{
    avfilter_graph_free(&ctx->graph);
    for (int n = 0; n < ctx->num_in_pads; n++) {
        FilterPad *pad = ctx->in_pads[n];
        pad->filter = NULL;
        pad->filter_pad = -1;
        pad->buffer = NULL;
        av_frame_free(&pad->in_fmt);
    }
    for (int n = 0; n < ctx->num_out_pads; n++) {
        FilterPad *pad = ctx->out_pads[n];
        pad->filter = NULL;
        pad->filter_pad = -1;
        pad->buffer = NULL;
        av_frame_free(&pad->in_fmt);
    }
}

int sp_map_fifo_to_pad(FilterContext *ctx, SPFrameFIFO *fifo, int pad_idx, int is_out)
{
    if (is_out) {
        for (int n = 0; n < ctx->num_out_pads; n++) {
            if (ctx->out_pads[n]->fifo == fifo) {
                av_log(ctx, AV_LOG_ERROR, "Duplicate output FIFO!\n");
                return AVERROR(EINVAL);
            }
        }
        if (ctx->out_pads[pad_idx]->fifo) {
            av_log(ctx, AV_LOG_ERROR, "Output FIFO for pad %i already specified!\n", pad_idx);
            return AVERROR(EINVAL);
        }
        ctx->out_pads[pad_idx]->fifo = fifo;
    } else {
        int fifo_refcount = 1;
        for (int n = 0; n < ctx->num_out_pads; n++) {
            if (ctx->out_pads[n]->fifo == fifo)
                fifo_refcount++;
        }
        if (ctx->in_pads[pad_idx]->fifo) {
            av_log(ctx, AV_LOG_ERROR, "Input FIFO for pad %i already specified!\n", pad_idx);
            return AVERROR(EINVAL);
        }
        sp_frame_fifo_set_refs(fifo, fifo_refcount);
        ctx->in_pads[pad_idx]->fifo = fifo;
    }

    return 0;
}

static void add_pad(FilterContext *ctx, int is_out, int index,
                    AVFilterContext *filter, int filter_pad, const char *name,
                    int first_init)
{
    if (ctx->err)
        return;

    enum AVMediaType type;
    if (is_out)
        type = avfilter_pad_get_type(filter->output_pads, filter_pad);
    else
        type = avfilter_pad_get_type(filter->input_pads, filter_pad);

    // For anonymous pads, just make something up. libavfilter allows duplicate
    // pad names (while we don't), so we check for collisions along with normal
    // duplicate pads below.
    char tmp[80];
    const char *dir_string = !is_out ? "in" : "out";
    if (name) {
        if (ctx->direct_filter) {
            // libavfilter has this very unpleasant thing that filter labels
            // don't have to be unique - in particular, both input and output
            // are usually named "default". With direct filters, the user has
            // no chance to provide better names, so do something to resolve it.
            snprintf(tmp, sizeof(tmp), "%s_%s", name, dir_string);
            name = tmp;
        }
    } else {
        snprintf(tmp, sizeof(tmp), "%s%d", dir_string, index);
        name = tmp;
    }

    /* Check if pad already exists */
    FilterPad *pad = NULL;
    if (!is_out) {
        for (int n = 0; n < ctx->num_in_pads; n++) {
            if (!strcmp(ctx->in_pads[n]->name, name)) {
                pad = ctx->in_pads[n];
                break;
            }
        }
    } else {
        for (int n = 0; n < ctx->num_out_pads; n++) {
            if (!strcmp(ctx->out_pads[n]->name, name)) {
                pad = ctx->out_pads[n];
                break;
            }
        }
    }

    if (pad) {
        /* Graph recreation case: reassociate an existing pad. */
        if (pad->filter) {
            /* Collision due to duplicate names. */
            av_log(ctx, AV_LOG_ERROR, "More than one pad with label \"%s\"!\n", name);
            ctx->err = AVERROR(EINVAL);
            return;
        }
        if (pad->is_out != is_out || pad->type != type) {
            /* libavfilter graph parser behavior not deterministic. */
            av_log(ctx, AV_LOG_ERROR, "Pad \"%s\" changed type or direction!\n", name);
            ctx->err = AVERROR(EINVAL);
            return;
        }
    } else {
        if (!first_init) {
            av_log(ctx, AV_LOG_ERROR, "Pad \"%s\" added after filter init!\n", name);
            ctx->err = AVERROR(EINVAL);
            return;
        }

        pad = is_out ? create_out_pad(ctx) : create_in_pad(ctx);
        if (!pad) {
            ctx->err = AVERROR(ENOMEM);
            return;
        }

        pad->main = ctx;
        pad->is_out = is_out;
        pad->name = av_strdup(name);
        pad->type = type;
        pad->metadata = NULL;
    }

    pad->filter = filter;
    pad->filter_pad = filter_pad;
}

static void add_pads(FilterContext *ctx, int is_out, AVFilterInOut *fio, int first_init)
{
    int index = 0;
    for (; fio; fio = fio->next)
        add_pad(ctx, is_out, index++, fio->filter_ctx, fio->pad_idx, fio->name, first_init);
}

static void add_pads_direct(FilterContext *ctx, int is_out, AVFilterContext *fctx,
                            AVFilterPad *pads, int num_pads, int first_init)
{
    for (int idx = 0; idx < num_pads; idx++)
        add_pad(ctx, is_out, idx, fctx, idx, avfilter_pad_get_name(pads, idx), first_init);
}

int re_create_filtering(FilterContext *ctx, int first_init)
{
    int err = 0;

    ctx->graph = avfilter_graph_alloc();
    if (!ctx->graph) {
        av_log(ctx, AV_LOG_ERROR, "Unable to allocate, no memory!\n");
        err = AVERROR(ENOMEM);
        goto end;
    }

    if ((err = sp_set_avopts(ctx, ctx->graph, ctx->graph_opts)))
        goto end;

    if (ctx->direct_filter) {
        const AVFilter *ftype = avfilter_get_by_name(ctx->graph_str);
        AVFilterContext *filt = avfilter_graph_alloc_filter(ctx->graph,
                                                            ftype, "filter");

        if (!filt) {
            av_log(ctx, AV_LOG_ERROR, "Filter \"%s\" not found!\n", ctx->graph_str);
            err = AVERROR(EINVAL);
            goto end;
        }

        sp_set_avopts_pos(ctx, filt, filt->priv, ctx->direct_filter_opts);

        err = avfilter_init_str(filt, NULL);
        if (err) {
            av_log(ctx, AV_LOG_ERROR, "Unable to init filter: %s!\n", av_err2str(err));
            goto end;
        }

        add_pads_direct(ctx, 0, filt, filt->input_pads, filt->nb_inputs, first_init);
        add_pads_direct(ctx, 1, filt, filt->output_pads, filt->nb_outputs, first_init);
    } else {
        AVFilterInOut *in = NULL, *out = NULL;

        err = avfilter_graph_parse2(ctx->graph, ctx->graph_str, &in, &out);
        if (err) {
            av_log(ctx, AV_LOG_ERROR, "Unable to parse filter graph: %s!\n", av_err2str(err));
            goto end;
        }

        add_pads(ctx, 0, in, first_init);
        add_pads(ctx, 1, out, first_init);
        avfilter_inout_free(&in);
        avfilter_inout_free(&out);
    }

end:
    /* This resets the error state */
    ctx->err = err;

    return err;
}

int sp_init_filter_single(FilterContext *ctx, const char *name,
                          AVDictionary *opts, AVDictionary *graph_opts,
                          enum AVHWDeviceType derive_device)
{
    ctx->direct_filter = 1;
    ctx->graph_str = name;
    ctx->graph_opts = graph_opts;
    ctx->direct_filter_opts = opts;
    ctx->device_type = derive_device;
    return re_create_filtering(ctx, 1);
}

int sp_init_filter_graph(FilterContext *ctx, const char *graph, AVDictionary *graph_opts,
                         enum AVHWDeviceType derive_device)
{
    ctx->direct_filter = 0;
    ctx->graph_str = graph;
    ctx->graph_opts = graph_opts;
    ctx->device_type = derive_device;
    return re_create_filtering(ctx, 1);
}

static void attempt_derive_device(FilterContext *ctx, AVBufferRef *frames_ref)
{
    int err;

    /* Device already derived, skip */
    if (ctx->hw_device_ref || (ctx->device_type == AV_HWDEVICE_TYPE_NONE))
        return;

    AVHWFramesContext *hwfc = (AVHWFramesContext *)frames_ref->data;
    AVBufferRef *input_device_ref = hwfc->device_ref;

    err = av_hwdevice_ctx_create_derived(&ctx->hw_device_ref, ctx->device_type,
                                         input_device_ref, 0);
    if (err < 0) {
        av_log(ctx, AV_LOG_ERROR, "Could not derive hardware device: %s!\n",
               av_err2str(err));
        if (ctx->hw_device_ref)
            av_buffer_unref(&ctx->hw_device_ref);
    }
}

static int init_pads(FilterContext *ctx)
{
    int err = 0;

    if (!ctx->graph) {
        err = AVERROR(EINVAL);
        goto error;
    }

    /* Output pads first */
    for (int i = 0; i < ctx->num_out_pads; i++) {
        FilterPad *pad = ctx->out_pads[i];
        if (pad->buffer)
            continue;

        const AVFilter *dst_filter = NULL;
        if (pad->type == AVMEDIA_TYPE_AUDIO)
            dst_filter = avfilter_get_by_name("abuffersink");
        else if (pad->type == AVMEDIA_TYPE_VIDEO)
            dst_filter = avfilter_get_by_name("buffersink");

        if (!dst_filter)
            goto error;

        char name[256];
        snprintf(name, sizeof(name), "sink_%s", pad->name);

        if ((err = avfilter_graph_create_filter(&pad->buffer, dst_filter,
                                               name, NULL, NULL, ctx->graph))) {
            av_log(ctx, AV_LOG_ERROR, "Unable to create pad graph: %s!\n", av_err2str(err));
            goto error;
        }

        if ((err = avfilter_link(pad->filter, pad->filter_pad, pad->buffer, 0))) {
            av_log(ctx, AV_LOG_ERROR, "Unable to link graph: %s!\n", av_err2str(err));
            goto error;
        }
    }

    for (int i = 0; i < ctx->num_in_pads; i++) {
        FilterPad *pad = ctx->in_pads[i];
        if (pad->buffer)
            continue;

        av_frame_free(&pad->in_fmt);

        AVFrame *in_fmt = sp_frame_fifo_peek(pad->fifo);
        pad->in_fmt = av_frame_alloc();
        av_frame_copy_props(pad->in_fmt, in_fmt);

        /* Set the filter buffer source paramters */
        AVBufferSrcParameters *params = av_buffersrc_parameters_alloc();
        if (!params) {
            err = AVERROR(ENOMEM);
            goto error;
        }

        FormatExtraData *fe = (FormatExtraData *)in_fmt->opaque_ref->data;
        params->format    = in_fmt->format;
        params->time_base = fe->time_base;

        char *filter_name = NULL;
        if (pad->type == AVMEDIA_TYPE_AUDIO) {
            params->sample_rate = in_fmt->sample_rate;
            params->channel_layout = in_fmt->channel_layout;
            filter_name = "abuffer";
        } else if (pad->type == AVMEDIA_TYPE_VIDEO) {
            params->width = in_fmt->width;
            params->height = in_fmt->height;
            params->sample_aspect_ratio = in_fmt->sample_aspect_ratio;
            if (in_fmt->hw_frames_ctx) {
                params->hw_frames_ctx = av_buffer_ref(in_fmt->hw_frames_ctx);
                attempt_derive_device(ctx, params->hw_frames_ctx);
            }
            params->frame_rate = fe->avg_frame_rate;
            filter_name = "buffer";
        }

        const AVFilter *filter = avfilter_get_by_name(filter_name);
        if (filter) {
            char name[256];
            snprintf(name, sizeof(name), "source_%s", pad->name);
            pad->buffer = avfilter_graph_alloc_filter(ctx->graph, filter, name);
        }

        if (!pad->buffer) {
            av_log(ctx, AV_LOG_ERROR, "Unable to create buffer for input pad %s!\n", pad->name);
            av_free(params);
            err = AVERROR(ENOMEM);
            goto error;
        }

        err = av_buffersrc_parameters_set(pad->buffer, params);
        av_free(params);
        if (err < 0) {
            av_log(ctx, AV_LOG_ERROR, "Unable to set buffer params: %s!\n", av_err2str(err));
            goto error;
        }

        if ((err = avfilter_init_str(pad->buffer, NULL)) < 0) {
            av_log(ctx, AV_LOG_ERROR, "Unable to initialize filter: %s!\n", av_err2str(err));
            goto error;
        }

        if ((err = avfilter_link(pad->buffer, 0, pad->filter, pad->filter_pad)) < 0) {
            av_log(ctx, AV_LOG_ERROR, "Unable to link graph: %s!\n", av_err2str(err));
            goto error;
        }
    }

    return 0;

error:
    ctx->err = err;

    return err;
}

void *filtering_thread(void *data)
{
    int err = 0, flushing = 0;
    FilterContext *ctx = data;

    pthread_setname_np(pthread_self(), "filtering");

    while (1) {
        for (int i = 0; i < ctx->num_in_pads; i++) {
            FilterPad *in_pad = ctx->in_pads[i];
            if (!flushing && (av_buffersrc_get_nb_failed_requests(in_pad->buffer) < 1))
                continue;

            AVFrame *in_frame = NULL;
            if (!flushing) {
                in_frame = sp_frame_fifo_pop(in_pad->fifo);
                flushing = !in_frame;
            }

            err = av_buffersrc_add_frame_flags(in_pad->buffer, in_frame,
                                               AV_BUFFERSRC_FLAG_PUSH);
            if (err < 0) {
                av_log(ctx, AV_LOG_ERROR, "Error pushing frame: %s!\n", av_err2str(err));
                goto end;
            }
        }

        for (int i = 0; i < ctx->num_out_pads; i++) {
            FilterPad *out_pad = ctx->out_pads[i];

            AVFrame *filt_frame = av_frame_alloc();
            err = av_buffersink_get_frame(out_pad->buffer, filt_frame);
            if (err == AVERROR(EAGAIN)) {
                av_frame_free(&filt_frame);
            } else if (err == AVERROR_EOF) {
                av_log(ctx, AV_LOG_INFO, "Filter flushed!\n");
                av_frame_free(&filt_frame);
                goto end;
            } else if (err < 0) {
                av_log(ctx, AV_LOG_ERROR, "Error pulling filtered frame: %s!\n", av_err2str(err));
                av_frame_free(&filt_frame);
                goto end;
            } else {
                filt_frame->opaque_ref = av_buffer_allocz(sizeof(FormatExtraData));

                FormatExtraData *fe = (FormatExtraData *)filt_frame->opaque_ref->data;
                fe->time_base = out_pad->buffer->inputs[0]->time_base;
                if (out_pad->buffer->inputs[0]->type == AVMEDIA_TYPE_VIDEO)
                    fe->avg_frame_rate  = out_pad->buffer->inputs[0]->frame_rate;
                else if (out_pad->buffer->inputs[0]->type == AVMEDIA_TYPE_AUDIO)
                    fe->bits_per_sample = av_get_bytes_per_sample(out_pad->buffer->inputs[0]->format) * 8;

                if (sp_frame_fifo_is_full(out_pad->fifo)) {
                    out_pad->dropped_frames++;
                    av_log_once(ctx, AV_LOG_WARNING, AV_LOG_DEBUG, &out_pad->dropped_frames_msg_state,
                                "Dropping filtered frame (%i dropped)!\n", out_pad->dropped_frames);
                    av_frame_free(&filt_frame);
                } else {
                    sp_frame_fifo_push(out_pad->fifo, filt_frame);
                    out_pad->dropped_frames_msg_state = 0;
                }
            }
        }
    }

end:
    sp_frame_fifo_push(ctx->out_pads[0]->fifo, NULL);
    return NULL;
}

int sp_filter_init_graph(FilterContext *ctx)
{
    int err = 0;
    if (!ctx->graph)
        err = re_create_filtering(ctx, 1);

    if (err)
        return err;

    if ((err = init_pads(ctx)))
        return err;

    if (ctx->hw_device_ref) {
        for (int i = 0; i < ctx->graph->nb_filters; i++) {
            AVFilterContext *filter = ctx->graph->filters[i];
            filter->hw_device_ctx = av_buffer_ref(ctx->hw_device_ref);
        }
    }

    if ((err = avfilter_graph_config(ctx->graph, NULL)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Unable to configure graph: %s!\n", av_err2str(err));
        free_graph(ctx);
        ctx->err = err;
        return err;
    }

    char *graph_dump = avfilter_graph_dump(ctx->graph, NULL);
    if (graph_dump)
        av_log(NULL, AV_LOG_INFO, "\n%s", graph_dump);
    av_free(graph_dump);

    pthread_create(&ctx->filter_thread, NULL, filtering_thread, ctx);

    return err;
}

FilterContext *alloc_filtering_ctx(void)
{
    FilterContext *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->class = av_mallocz(sizeof(*ctx->class));
    if (!ctx->class) {
        av_free(ctx);
        return NULL;
    }

    *ctx->class = (AVClass) {
        .class_name = "filtering",
        .item_name  = av_default_item_name,
        .version    = LIBAVUTIL_VERSION_INT,
    };

    return ctx;
}
