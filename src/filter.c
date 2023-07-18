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

#include <stdatomic.h>

#include <libavutil/time.h>
#include <libavutil/avstring.h>
#include <libavfilter/buffersink.h>

#include <libtxproto/filter.h>

#include "fifo_frame.h"
#include "os_compat.h"
#include <libtxproto/utils.h>
#include "ctrl_template.h"

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

int sp_map_fifo_to_pad(FilterContext *ctx, AVBufferRef *fifo,
                       const char *name, int is_out)
{
    int pad_idx = 0;

    if (is_out) {
        if (ctx->num_out_pads > 1) {
            for (; pad_idx < ctx->num_out_pads; pad_idx++)
                if (!strcmp(ctx->out_pads[pad_idx]->name, name))
                    break;
            if (pad_idx == ctx->num_out_pads) {
                sp_log(ctx, SP_LOG_ERROR, "Output pad with name \"%s\" not found!\n", name);
                return AVERROR(EINVAL);
            }
        }
        sp_frame_fifo_mirror(fifo, ctx->out_pads[pad_idx]->fifo);
    } else {
        if (ctx->num_in_pads > 1) {
            for (; pad_idx < ctx->num_in_pads; pad_idx++)
                if (!strcmp(ctx->in_pads[pad_idx]->name, name))
                    break;
            if (pad_idx == ctx->num_in_pads) {
                sp_log(ctx, SP_LOG_ERROR, "Input pad with name \"%s\" not found!\n", name);
                return AVERROR(EINVAL);
            }
        }
        sp_frame_fifo_mirror(ctx->in_pads[pad_idx]->fifo, fifo);
    }

    return 0;
}

int sp_map_pad_to_pad(FilterContext *dst, const char *dst_pad,
                      FilterContext *src, const char *src_pad)
{
    int src_pad_idx = 0;
    if (src->num_out_pads > 1) {
        for (; src_pad_idx < src->num_out_pads; src_pad_idx++)
            if (!strcmp(src->out_pads[src_pad_idx]->name, src_pad))
                break;
        if (src_pad_idx == src->num_out_pads) {
            sp_log(src, SP_LOG_ERROR, "Output pad with name \"%s\" not found!\n", src_pad);
            return AVERROR(EINVAL);
        }
    }

    return sp_map_fifo_to_pad(dst, src->out_pads[src_pad_idx]->fifo, dst_pad, 0);
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
    if (!name) {
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
            sp_log(ctx, SP_LOG_ERROR, "More than one pad with label \"%s\"!\n", name);
            ctx->err = AVERROR(EINVAL);
            return;
        }
        if (pad->is_out != is_out || pad->type != type) {
            /* libavfilter graph parser behavior not deterministic. */
            sp_log(ctx, SP_LOG_ERROR, "Pad \"%s\" changed type or direction!\n", name);
            ctx->err = AVERROR(EINVAL);
            return;
        }
    } else {
        if (!first_init) {
            sp_log(ctx, SP_LOG_ERROR, "Pad \"%s\" added after filter init!\n", name);
            ctx->err = AVERROR(EINVAL);
            return;
        }

        pad = is_out ? create_out_pad(ctx) : create_in_pad(ctx);
        if (!pad) {
            ctx->err = AVERROR(ENOMEM);
            return;
        }

        pad->fifo = is_out ? sp_frame_fifo_create(ctx, 0, 0) : sp_frame_fifo_create(ctx, 8, FRAME_FIFO_BLOCK_NO_INPUT);
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
    int cust_pad_name_end = 0;
    char **cust_pad_name = is_out ? ctx->out_pad_names : ctx->in_pad_names;

    int index = 0;
    for (; fio; fio = fio->next) {
        const char *pad_name = fio->name;
        if (cust_pad_name && !cust_pad_name_end) {
            if (cust_pad_name[index])
                pad_name = cust_pad_name[index];
            else
                cust_pad_name_end = 1;
        }
        add_pad(ctx, is_out, index++, fio->filter_ctx, fio->pad_idx, pad_name, first_init);
    }
}

static void add_pads_direct(FilterContext *ctx, int is_out, AVFilterContext *fctx,
                            AVFilterPad *pads, int num_pads, int first_init)
{
    int cust_pad_name_end = 0;
    char **cust_pad_name = is_out ? ctx->out_pad_names : ctx->in_pad_names;

    for (int idx = 0; idx < num_pads; idx++) {
        const char *pad_name = avfilter_pad_get_name(pads, idx);
        if (cust_pad_name && !cust_pad_name_end) {
            if (cust_pad_name[idx])
                pad_name = cust_pad_name[idx];
            else
                cust_pad_name_end = 1;
        }
        add_pad(ctx, is_out, idx, fctx, idx, pad_name, first_init);
    }
}

static int re_create_filtering(FilterContext *ctx, int first_init)
{
    int err = 0;

    ctx->graph = avfilter_graph_alloc();
    if (!ctx->graph) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to allocate, no memory!\n");
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
            sp_log(ctx, SP_LOG_ERROR, "Filter \"%s\" not found!\n", ctx->graph_str);
            err = AVERROR(EINVAL);
            goto end;
        }

        sp_set_avopts_pos(ctx, filt, filt->priv, ctx->direct_filter_opts);

        err = avfilter_init_str(filt, NULL);
        if (err) {
            sp_log(ctx, SP_LOG_ERROR, "Unable to init filter: %s!\n", av_err2str(err));
            goto end;
        }

        add_pads_direct(ctx, 0, filt, filt->input_pads, filt->nb_inputs, first_init);
        add_pads_direct(ctx, 1, filt, filt->output_pads, filt->nb_outputs, first_init);
    } else {
        AVFilterInOut *in = NULL, *out = NULL;

        err = avfilter_graph_parse2(ctx->graph, ctx->graph_str, &in, &out);
        if (err) {
            sp_log(ctx, SP_LOG_ERROR, "Unable to parse filter graph: %s!\n", av_err2str(err));
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

static int rename_ctx(FilterContext *ctx, const char *name, const char *filt)
{
    char *new_name = NULL;
    if (name) {
        new_name = av_strdup(name);
        if (!new_name)
            return AVERROR(ENOMEM);
    } else {
        int len = strlen(sp_class_get_name(ctx)) + 1 + strlen(filt) + 1;
        new_name = av_mallocz(len);
        if (!new_name)
            return AVERROR(ENOMEM);
        av_strlcpy(new_name, sp_class_get_name(ctx), len);
        av_strlcat(new_name, ":", len);
        av_strlcat(new_name, filt, len);
    }

    sp_class_set_name(ctx, new_name);
    av_free(new_name);

    return 0;
}

int sp_init_filter_single(AVBufferRef *ctx_ref, const char *name, const char *filt,
                          char **in_pad_names, char **out_pad_names, enum AVPixelFormat req_fmt,
                          AVDictionary *opts, AVDictionary *graph_opts,
                          enum AVHWDeviceType derive_device)
{
    FilterContext *ctx = (FilterContext *)ctx_ref->data;
    ctx->direct_filter = 1;
    ctx->graph_str = filt;
    ctx->graph_opts = graph_opts;
    ctx->direct_filter_fmt = req_fmt;
    ctx->direct_filter_opts = opts;
    ctx->device_type = derive_device;
    ctx->in_pad_names = in_pad_names;
    ctx->out_pad_names = out_pad_names;
    int ret = rename_ctx(ctx, name, filt);
    if (ret < 0)
        return ret;
    return re_create_filtering(ctx, 1);
}

int sp_init_filter_graph(AVBufferRef *ctx_ref, const char *name, const char *graph,
                         char **in_pad_names, char **out_pad_names,
                         AVDictionary *graph_opts, enum AVHWDeviceType derive_device)
{
    FilterContext *ctx = (FilterContext *)ctx_ref->data;
    ctx->direct_filter = 0;
    ctx->graph_str = graph;
    ctx->graph_opts = graph_opts;
    ctx->device_type = derive_device;
    ctx->in_pad_names = in_pad_names;
    ctx->out_pad_names = out_pad_names;
    int ret = rename_ctx(ctx, name, "graph");
    if (ret < 0)
        return ret;
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
        sp_log(ctx, SP_LOG_ERROR, "Could not derive hardware device: %s!\n",
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
        snprintf(name, sizeof(name), "%s", pad->name);

        if ((err = avfilter_graph_create_filter(&pad->buffer, dst_filter,
                                                name, NULL, NULL, ctx->graph))) {
            sp_log(ctx, SP_LOG_ERROR, "Unable to create pad graph: %s!\n", av_err2str(err));
            goto error;
        }

        if ((err = avfilter_link(pad->filter, pad->filter_pad, pad->buffer, 0))) {
            sp_log(ctx, SP_LOG_ERROR, "Unable to link graph: %s!\n", av_err2str(err));
            goto error;
        }
    }

    for (int i = 0; i < ctx->num_in_pads; i++) {
        FilterPad *pad = ctx->in_pads[i];
        if (pad->buffer)
            continue;

        av_frame_free(&pad->in_fmt);

        sp_log(ctx, SP_LOG_VERBOSE, "Getting a frame to configure pad \"%s\"...\n",
               pad->name);
        AVFrame *in_fmt = sp_frame_fifo_peek(pad->fifo);
        pad->in_fmt = av_frame_alloc();
        av_frame_copy_props(pad->in_fmt, in_fmt);

        /* Set the filter buffer source paramters */
        AVBufferSrcParameters *params = av_buffersrc_parameters_alloc();
        if (!params) {
            err = AVERROR(ENOMEM);
            av_frame_free(&in_fmt);
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

        av_frame_free(&in_fmt);

        const AVFilter *filter = avfilter_get_by_name(filter_name);
        if (filter) {
            char name[256];
            snprintf(name, sizeof(name), "%s", pad->name);
            pad->buffer = avfilter_graph_alloc_filter(ctx->graph, filter, name);
        }

        if (!pad->buffer) {
            sp_log(ctx, SP_LOG_ERROR, "Unable to create buffer for input pad %s!\n", pad->name);
            av_free(params);
            err = AVERROR(ENOMEM);
            goto error;
        }

        err = av_buffersrc_parameters_set(pad->buffer, params);
        av_free(params);
        if (err < 0) {
            sp_log(ctx, SP_LOG_ERROR, "Unable to set buffer params: %s!\n", av_err2str(err));
            goto error;
        }

        if ((err = avfilter_init_str(pad->buffer, NULL)) < 0) {
            sp_log(ctx, SP_LOG_ERROR, "Unable to initialize filter: %s!\n", av_err2str(err));
            goto error;
        }

        if ((err = avfilter_link(pad->buffer, 0, pad->filter, pad->filter_pad)) < 0) {
            sp_log(ctx, SP_LOG_ERROR, "Unable to link graph: %s!\n", av_err2str(err));
            goto error;
        }
    }

    return 0;

error:
    ctx->err = err;

    return err;
}

static int push_input_pads(FilterContext *ctx, int *flush, int opportunistically)
{
    int err = 0, ret, push_flags;
    enum SPFrameFIFOFlags pull_flags;

    pull_flags = opportunistically ? FRAME_FIFO_PULL_NO_BLOCK : 0x0;

    int pads_err = 0;
    int pads_satisfied = 0;

    for (int i = 0; i < ctx->num_in_pads; i++) {
        FilterPad *in_pad = ctx->in_pads[i];

        unsigned nb_req;
        if (!in_pad->eos && !opportunistically) {
            nb_req = sp_frame_fifo_get_size(in_pad->fifo);
            /* If we're doing this non-opportunistically, we'll want at least
             * one frame in */
            if (!nb_req)
                nb_req = 1;
        } else {
            nb_req = av_buffersrc_get_nb_failed_requests(in_pad->buffer);
            if (nb_req && in_pad->eos) {
                sp_log(ctx, SP_LOG_ERROR, "Filter asking for more frames on a "
                       "flushed input pad \"%s\"!\n", in_pad->name);
                return AVERROR(EINVAL);
            }
        }

        /* If we're doing it opportunistically, don't do any processing now,
         * since we'll likely pull right away after this and do the processing
         * then. Otherwise, do the processing now, unless there are many frames,
         * in which case, get them off our books as fast as possible. */
        push_flags = (opportunistically || nb_req > 1) ? 0x0 : AV_BUFFERSRC_FLAG_PUSH;

        int j;
        for (j = 0; j < nb_req; j++) {
            AVFrame *in_frame;
            ret = sp_frame_fifo_pop_flags(in_pad->fifo, &in_frame, pull_flags);
            if (opportunistically && (ret == AVERROR(EAGAIN))) {
                break;
            } else if (ret < 0) {
                sp_log(ctx, SP_LOG_ERROR, "Error pulling frame from FIFO at input pad \"%s\": %s!\n",
                       in_pad->name, av_err2str(ret));
                return ret;
            }

            if (!in_frame) {
                *flush = 1;
                push_flags = AV_BUFFERSRC_FLAG_PUSH;
                in_pad->eos = 1;
                pads_satisfied++;
                j = nb_req;
            } else {
                FormatExtraData *fe = (FormatExtraData *)in_frame->opaque_ref->data;
                sp_log(ctx, SP_LOG_TRACE, "Giving frame to input pad \"%s\", pts = %f\n",
                       in_pad->name, av_q2d(fe->time_base) * in_frame->pts);
            }

            /* Takes ownership of in_frame */
            ret = av_buffersrc_add_frame_flags(in_pad->buffer, in_frame, push_flags);

            /* It did take ownership but it just moved the ref, it doesn't free
             * the frame as well */
            av_frame_free(&in_frame);

            if (ret == AVERROR(ENOMEM)) {
                return ret;
            } else if (ret < 0) {
                sp_log(ctx, SP_LOG_ERROR, "Error pushing frame to input pad \"%s\": %s!\n",
                       in_pad->name, av_err2str(ret));
                pads_err++;
                err = ret;
            }
        }

        /* The pad is not satisfied (our own vague term) until it has
         * received at least 1 frame, or all requested (could be none) */
        if (j == nb_req || j)
            pads_satisfied++;
    }

    return (err < 0 && !pads_satisfied) ? err : (pads_satisfied == ctx->num_in_pads);
}

static int drain_output_pad(FilterContext *ctx, FilterPad *out_pad,
                            AVFrame **tmp_frame, int *flush)
{
    int ret, input_pushed = 0;
    AVFrame *filt_frame = *tmp_frame;
    int pull_flags = *flush ? AV_BUFFERSINK_FLAG_NO_REQUEST : 0;

    do {
        /* Pull frame */
        ret = av_buffersink_get_frame_flags(out_pad->buffer, filt_frame, pull_flags);

        /* Check for EAGAIN or errors */
        if (!input_pushed && ret == AVERROR(EAGAIN)) {
            ret = push_input_pads(ctx, flush, 1);

            /* The function above returns 0 if not all pads have been satisfied.
             * If so, no point in trying again. */
            if (ret <= 0)
                return ret;

            /* The flush status may have changed. */
            pull_flags = *flush ? AV_BUFFERSINK_FLAG_NO_REQUEST : 0;
            input_pushed++;
            continue;
        } else if (ret < 0) {
            break;
        }

        input_pushed = 0;

        av_buffer_unref(&filt_frame->opaque_ref);
        filt_frame->opaque_ref = av_buffer_allocz(sizeof(FormatExtraData));
        if (!filt_frame->opaque_ref)
            return AVERROR(ENOMEM);

        FormatExtraData *fe = (FormatExtraData *)filt_frame->opaque_ref->data;
        fe->time_base = out_pad->buffer->inputs[0]->time_base;
        if (out_pad->buffer->inputs[0]->type == AVMEDIA_TYPE_VIDEO)
            fe->avg_frame_rate  = out_pad->buffer->inputs[0]->frame_rate;
        else if (out_pad->buffer->inputs[0]->type == AVMEDIA_TYPE_AUDIO)
            fe->bits_per_sample = av_get_bytes_per_sample(out_pad->buffer->inputs[0]->format) * 8;

        sp_log(ctx, SP_LOG_TRACE, "Pushing frame to FIFO from output pad \"%s\", pts = %f\n",
               out_pad->name, av_q2d(fe->time_base) * filt_frame->pts);

        ret = sp_frame_fifo_push(out_pad->fifo, filt_frame);
        av_frame_free(tmp_frame);
        if (ret == AVERROR(ENOMEM))
            return ret;

        *tmp_frame = filt_frame = av_frame_alloc();
        if (!filt_frame)
            return AVERROR(ENOMEM);

        if (ret == AVERROR(ENOBUFS)) {
            out_pad->dropped_frames++;
            sp_log(ctx, SP_LOG_WARN,
                   "Dropping filtered frame from output pad \"%s\" (%i dropped)!\n",
                   out_pad->name, out_pad->dropped_frames);
        } else if (ret < 0) {
            sp_log(ctx, SP_LOG_ERROR, "Error pushing frame to FIFO on output pad \"%s\": %s\n",
                   out_pad->name, av_err2str(ret));
            return ret;
        }
    } while (1);

    /* Probably overkill, but why take a chance? */
    if ((ret == AVERROR_EOF) || ((ret == AVERROR(EAGAIN)) && *flush)) {
        sp_log(ctx, SP_LOG_VERBOSE, "Output pad \"%s\" flushed!\n", out_pad->name);
        ret = AVERROR_EOF;
        out_pad->eos = 1;
    } else if (ret != AVERROR(EAGAIN)) {
        sp_log(ctx, SP_LOG_ERROR, "Error pulling filtered frame from output pad \"%s\": %s!\n",
               out_pad->name, av_err2str(ret));
    }

    return ret;
}

static void *filtering_thread(void *data)
{
    int err = 0, flushing = 0;
    FilterContext *ctx = data;

    sp_set_thread_name_self(sp_class_get_name(ctx));

    sp_log(ctx, SP_LOG_VERBOSE, "Filter initialized!\n");
    sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_INIT, NULL);

    AVFrame *filt_frame = av_frame_alloc();

    while (1) {
        pthread_mutex_lock(&ctx->lock);

        err = push_input_pads(ctx, &flushing, 0);
        if (err < 0)
            goto fail;

        pthread_mutex_unlock(&ctx->lock);
        pthread_mutex_lock(&ctx->lock);

        int pads_flushed = 0;
        int pads_errors = 0;
        for (int i = 0; i < ctx->num_out_pads; i++) {
            FilterPad *out_pad = ctx->out_pads[i];

            if (out_pad->eos) {
                pads_flushed++;
                continue;
            }

            err = drain_output_pad(ctx, out_pad, &filt_frame, &flushing);
            if (err == AVERROR(ENOMEM))
                goto fail;
            else if (err == AVERROR_EOF)
                pads_flushed++;
            else if (err == AVERROR(EAGAIN))
                continue;
            else if (err < 0)
                pads_errors++;
        }

        pthread_mutex_unlock(&ctx->lock);

        /* Yes, I'm being clever. */
        if ((pads_flushed + pads_errors) == ctx->num_out_pads)
            break;
    }

    if (err >= 0 || (err == AVERROR_EOF))
        sp_log(ctx, SP_LOG_DEBUG, "Filter flushed!\n");
    else
        sp_log(ctx, SP_LOG_ERROR, "Filter errors: %s!\n", av_err2str(err));

    av_frame_free(&filt_frame);
    return NULL;

fail:
    av_frame_free(&filt_frame);
    pthread_mutex_unlock(&ctx->lock);
    return NULL;
}

static int sp_filter_init_graph(FilterContext *ctx)
{
    int err = 0;
    if (!ctx->graph)
        err = re_create_filtering(ctx, 1);

    if (err)
        return err;

    err = sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_CONFIG, NULL);
    if (err < 0)
        return err;

    if ((err = init_pads(ctx)))
        return err;

    if (!ctx->hw_device_ref && ctx->device_type != AV_HWDEVICE_TYPE_NONE) {
        err = av_hwdevice_ctx_create(&ctx->hw_device_ref, ctx->device_type,
                                     NULL, NULL, 0);
        if (err < 0) {
            sp_log(ctx, SP_LOG_ERROR, "Could not init hardware device: %s!\n",
                   av_err2str(err));
            return err;
        }
    }

    if (ctx->hw_device_ref) {
        for (int i = 0; i < ctx->graph->nb_filters; i++) {
            AVFilterContext *filter = ctx->graph->filters[i];
            filter->hw_device_ctx = av_buffer_ref(ctx->hw_device_ref);
        }
        av_buffer_unref(&ctx->hw_device_ref);
    }

    if ((err = avfilter_graph_config(ctx->graph, NULL)) < 0) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to configure graph: %s!\n", av_err2str(err));
        free_graph(ctx);
        ctx->err = err;
        return err;
    }

    if (ctx->dump_graph) {
        char *graph_dump = avfilter_graph_dump(ctx->graph, NULL);
        if (graph_dump)
            sp_log(NULL, SP_LOG_INFO, "\n%s\n", graph_dump);
        av_free(graph_dump);
    }

    sp_log(ctx, SP_LOG_DEBUG, "Filter configured!\n");

    return err;
}

static int filter_ioctx_ctrl_cb(AVBufferRef *event_ref, void *callback_ctx,
                               void *_ctx, void *dep_ctx, void *data)
{
    SPCtrlTemplateCbCtx *event = callback_ctx;
    FilterContext *ctx = _ctx;

    if (event->ctrl & SP_EVENT_CTRL_START) {
        if (!sp_eventlist_has_dispatched(ctx->events, SP_EVENT_ON_CONFIG)) {
            int ret = sp_filter_init_graph(ctx);
            if (ret < 0)
                return ret;
        }
        ctx->epoch = atomic_load(event->epoch);
        if (!ctx->filter_thread)
            pthread_create(&ctx->filter_thread, NULL, filtering_thread, ctx);
    } else if (event->ctrl & SP_EVENT_CTRL_STOP) {
        if (ctx->filter_thread) {
            for (int i = 0; i < ctx->num_in_pads; i++)
                sp_frame_fifo_push(ctx->in_pads[i]->fifo, NULL);
            pthread_join(ctx->filter_thread, NULL);
            ctx->filter_thread = 0;
        }
    } else if (event->ctrl & SP_EVENT_CTRL_OPTS) {
        pthread_mutex_lock(&ctx->lock);
        const char *tmp_val = NULL;
        if ((tmp_val = dict_get(event->opts, "dump_graph")))
            if (!strcmp(tmp_val, "true") || strtol(tmp_val, NULL, 10) != 0)
                ctx->dump_graph = 1;
        if ((tmp_val = dict_get(event->opts, "fifo_size"))) {
            long int len = strtol(tmp_val, NULL, 10);
            if (len < 0) {
                sp_log(ctx, SP_LOG_ERROR, "Invalid fifo size \"%s\"!\n", tmp_val);
            } else {
                for (int i = 0; i < ctx->num_in_pads; i++)
                    sp_frame_fifo_set_max_queued(ctx->in_pads[i]->fifo, len);
            }
        }
        pthread_mutex_unlock(&ctx->lock);
    } else if (event->ctrl & SP_EVENT_CTRL_COMMAND) {
        char result[4096];
        const AVDictionaryEntry *e = NULL;
        const char *target = dict_get(event->cmd, "sp_filter_target");

        target = target ? target : "all";

        while ((e = av_dict_get(event->cmd, "", e, AV_DICT_IGNORE_SUFFIX))) {
            if (!strcmp(e->key, "sp_filter_target"))
                continue;

            int ret = avfilter_graph_send_command(ctx->graph, target, e->key, e->value,
                                                  result, sizeof(result), 0);
            sp_log(ctx, ret == AVERROR(EINVAL) ? SP_LOG_ERROR : SP_LOG_VERBOSE,
                   "Filter (%s) response to command %s=%s: %s%s%s\n",
                   target, e->key, e->value, av_err2str(ret), strlen(result) ? " - " : "", result);
        }

    } else {
        return AVERROR(ENOTSUP);
    }

    return 0;
}

int sp_filter_ctrl(AVBufferRef *ctx_ref, SPEventType ctrl, void *arg)
{
    FilterContext *ctx = (FilterContext *)ctx_ref->data;
    return sp_ctrl_template(ctx, ctx->events, SP_EVENT_CTRL_COMMAND,
                            filter_ioctx_ctrl_cb, ctrl, arg);
}

static void filter_free(void *opaque, uint8_t *data)
{
    FilterContext *ctx = (FilterContext *)data;

    for (int i = 0; i < ctx->num_in_pads; i++)
        sp_frame_fifo_unmirror_all(ctx->in_pads[i]->fifo);

    for (int i = 0; i < ctx->num_out_pads; i++)
        sp_frame_fifo_unmirror_all(ctx->out_pads[i]->fifo);

    if (ctx->filter_thread) {
        for (int i = 0; i < ctx->num_in_pads; i++)
            sp_frame_fifo_push(ctx->in_pads[i]->fifo, NULL);
        pthread_join(ctx->filter_thread, NULL);
    }

    pthread_mutex_lock(&ctx->lock);

    sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_DESTROY, ctx);
    sp_bufferlist_free(&ctx->events);

    avfilter_graph_free(&ctx->graph);

    if (ctx->in_pad_names) {
        for (int i = 0; ctx->in_pad_names[i]; i++)
            av_free(ctx->in_pad_names[i]);
        av_freep(&ctx->in_pad_names);
    }

    if (ctx->out_pad_names) {
        for (int i = 0; ctx->out_pad_names[i]; i++)
            av_free(ctx->out_pad_names[i]);
        av_freep(&ctx->out_pad_names);
    }

    for (int i = 0; i < ctx->num_in_pads; i++) {
        av_buffer_unref(&ctx->in_pads[i]->fifo);
        av_frame_free(&ctx->in_pads[i]->in_fmt);
        av_free(ctx->in_pads[i]->name);
        av_free(ctx->in_pads[i]);
    }
    av_free(ctx->in_pads);

    for (int i = 0; i < ctx->num_out_pads; i++) {
        av_buffer_unref(&ctx->out_pads[i]->fifo);
        av_frame_free(&ctx->out_pads[i]->in_fmt);
        av_free(ctx->out_pads[i]->name);
        av_free(ctx->out_pads[i]);
    }
    av_free(ctx->out_pads);

    pthread_mutex_unlock(&ctx->lock);
    pthread_mutex_destroy(&ctx->lock);

    av_dict_free(&ctx->direct_filter_opts);
    av_dict_free(&ctx->graph_opts);

    sp_class_free(ctx);
    av_free(ctx);
}

AVBufferRef *sp_filter_alloc(void)
{
    FilterContext *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return NULL;

    int err = sp_class_alloc(ctx, "lavfi", SP_TYPE_FILTER, NULL);
    if (err < 0) {
        av_free(ctx);
        return NULL;
    }

    AVBufferRef *ctx_ref = av_buffer_create((uint8_t *)ctx, sizeof(*ctx),
                                            filter_free, NULL, 0);
    if (!ctx_ref) {
        sp_class_free(ctx);
        av_free(ctx);
        return NULL;
    }

    pthread_mutex_init(&ctx->lock, NULL);
    ctx->events = sp_bufferlist_new();

    return ctx_ref;
}
