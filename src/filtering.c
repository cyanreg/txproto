#include "filtering.h"


#include "utils.h"


static void add_pad(FilterContext *ctx, int is_out, int index,
                    AVFilterContext *filter, int filter_pad, const char *name,
                    int first_init)
{
    enum AVMediaType type;

    if (is_out)
        type = avfilter_pad_get_type(filter->input_pads, filter_pad);
    else
        type = avfilter_pad_get_type(filter->output_pads, filter_pad);

    // For anonymous pads, just make something up. libavfilter allows duplicate
    // pad names (while we don't), so we check for collisions along with normal
    // duplicate pads below.
    char tmp[80];
    const char *dir_string = is_out ? "in" : "out";
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

    FilterPad *pad = NULL;
    for (int n = 0; n < ctx->num_in_pads; n++) {
        if (!strcmp(ctx->in_pads[n]->name, name)) {
            pad = ctx->in_pads[n];
            break;
        }
    }

    for (int n = 0; n < ctx->num_out_pads; n++) {
        if (!strcmp(ctx->out_pads[n]->name, name)) {
            pad = ctx->out_pads[n];
            break;
        }
    }

    if (pad) {
        // Graph recreation case: reassociate an existing pad.
        if (pad->filter) {
            // Collision due to duplicate names.
            av_log(ctx, AV_LOG_ERROR, "More than one pad with label \"%s\"!\n", name);
            ctx->err = AVERROR(EINVAL);
            return;
        }
        if (pad->is_out != is_out || pad->type != type) {
            // libavfilter graph parser behavior not deterministic.
            av_log(ctx, AV_LOG_ERROR, "Pad \"%s\" changed type or direction!\n", name);
            ctx->err = AVERROR(EINVAL);
            return;
        }
    } else {
        if (!first_init) {
            av_log(ctx, AV_LOG_ERROR, "Pad \"%s\" added at invalid time!\n", name);
            ctx->err = AVERROR(EINVAL);
            return;
        }

        pad = av_mallocz(sizeof(*pad));

        pad->main = ctx;
        pad->is_out = is_out;
        pad->name = av_strdup(name);
        pad->type = type;
        //pad->pin_index = -1;

        if (is_out) {
            ctx->in_pads = av_realloc(ctx->in_pads, ctx->num_in_pads * sizeof(*ctx->in_pads));
            
        }
    }

    pad->filter = filter;
    pad->filter_pad = filter_pad;
}

static void add_pads(FilterContext *ctx, int is_out, AVFilterInOut *l, int first_init)
{
    int index = 0;
    for (; l; l = l->next)
        add_pad(ctx, is_out, index++, l->filter_ctx, l->pad_idx, l->name, first_init);
}

static void add_pads_direct(FilterContext *ctx, int is_out, AVFilterContext *f,
                            AVFilterPad *pads, int num_pads, int first_init)
{
    for (int n = 0; n < num_pads; n++)
        add_pad(ctx, is_out, n, f, n, avfilter_pad_get_name(pads, n), first_init);
}

int re_create_filtering(FilterContext *ctx, int first_init)
{
    int err;

    ctx->graph = avfilter_graph_alloc();
    if (!ctx->graph) {
        av_log(ctx, AV_LOG_ERROR, "Unable to allocate, no memory!\n");
        return AVERROR(ENOMEM);
    }

    sp_set_avopts(ctx, ctx->graph, ctx->graph_opts);

    if (ctx->direct_filter) {
        const AVFilter *ftype = avfilter_get_by_name(ctx->graph_str);
        AVFilterContext *filt = avfilter_graph_alloc_filter(ctx->graph,
                                                            ftype, "filter");

        if (!filt) {
            av_log(ctx, AV_LOG_ERROR, "Filter \"%s\" not found!\n", ctx->graph_str);
            return AVERROR(EINVAL);
        }

        sp_set_avopts_pos(ctx, filt, filt->priv, ctx->direct_filter_opts);

        err = avfilter_init_str(filt, NULL);
        if (err) {
            av_log(ctx, AV_LOG_ERROR, "Unable to init filter: %s!\n", av_err2str(err));
            return err;
        }

        add_pads_direct(ctx, 0, filt, filt->input_pads, filt->nb_inputs, first_init);
        add_pads_direct(ctx, 1, filt, filt->output_pads, filt->nb_outputs, first_init);
    } else {
        AVFilterInOut *in = NULL, *out = NULL;

        err = avfilter_graph_parse2(ctx->graph, ctx->graph_str, &in, &out);
        if (err) {
            av_log(ctx, AV_LOG_ERROR, "Unable to parse filter graph: %s!\n", av_err2str(err));
            return err;
        }

        add_pads(ctx, 0, in, first_init);
        add_pads(ctx, 1, out, first_init);
        avfilter_inout_free(&in);
        avfilter_inout_free(&out);
    }

    return 0;
}

static int init_filter_pads(FilterContext *ctx)
{
    int err;

    if (!ctx->graph)
        goto error;

    for (int n = 0; n < ctx->num_out_pads; n++) {
        FilterPad *pad = ctx->out_pads[n];
        if (pad->buffer)
            continue;

        const AVFilter *dst_filter = NULL;
        if (pad->type == AVMEDIA_TYPE_AUDIO)
            dst_filter = avfilter_get_by_name("abuffersink");
        else if (pad->type == AVMEDIA_TYPE_VIDEO)
            dst_filter = avfilter_get_by_name("buffersink");

        char name[256];
        snprintf(name, sizeof(name), "sp_sink_%s", pad->name);

        err = avfilter_graph_create_filter(&pad->buffer, dst_filter,
                                           name, NULL, NULL, ctx->graph);
        if (err) {
            av_log(ctx, AV_LOG_ERROR, "Unable to create filter: %s!\n", av_err2str(err));
            return err;
        }

        err = avfilter_link(pad->filter, pad->filter_pad, pad->buffer, 0);
        if (err) {
            av_log(ctx, AV_LOG_ERROR, "Unable to link filter to graph: %s!\n", av_err2str(err));
            return err;
        }
    }

    for (int n = 0; n < ctx->num_in_pads; n++) {
        FilterPad *pad = ctx->in_pads[n];
        if (pad->buffer)
            continue;

        AVFrame *frame = pop_from_fifo(pad->fifo);
        if (!frame) {
            // libavfilter makes this painful. Init it with a dummy config,
            // just so we can tell it the stream is EOF.
            /*
            if (pad->type == AVMEDIA_TYPE_AUDIO) {
                struct mp_aframe *fmt = mp_aframe_create();
                mp_aframe_set_format(fmt, AF_FORMAT_FLOAT);
                mp_aframe_set_chmap(fmt, &(struct mp_chmap)MP_CHMAP_INIT_STEREO);
                mp_aframe_set_rate(fmt, 48000);
                pad->in_fmt = (struct mp_frame){MP_FRAME_AUDIO, fmt};
            }
            if (pad->type == AVMEDIA_TYPE_VIDEO) {
                struct mp_image *fmt = talloc_zero(NULL, struct mp_image);
                mp_image_setfmt(fmt, IMGFMT_420P);
                mp_image_set_size(fmt, 64, 64);
                pad->in_fmt = (struct mp_frame){MP_FRAME_VIDEO, fmt};
            }
            */
        }


#if 0

        if (pad->in_fmt.type != pad->type)
            goto error;

        AVBufferSrcParameters *params = av_buffersrc_parameters_alloc();
        if (!params)
            goto error;

        pad->timebase = AV_TIME_BASE_Q;

        char *filter_name = NULL;
        if (pad->type == MP_FRAME_AUDIO) {
            struct mp_aframe *fmt = pad->in_fmt.data;
            params->format = af_to_avformat(mp_aframe_get_format(fmt));
            params->sample_rate = mp_aframe_get_rate(fmt);
            struct mp_chmap chmap = {0};
            mp_aframe_get_chmap(fmt, &chmap);
            params->channel_layout = mp_chmap_to_lavc(&chmap);
            pad->timebase = (AVRational){1, mp_aframe_get_rate(fmt)};
            filter_name = "abuffer";
        } else if (pad->type == MP_FRAME_VIDEO) {
            struct mp_image *fmt = pad->in_fmt.data;
            params->format = imgfmt2pixfmt(fmt->imgfmt);
            params->width = fmt->w;
            params->height = fmt->h;
            params->sample_aspect_ratio.num = fmt->params.p_w;
            params->sample_aspect_ratio.den = fmt->params.p_h;
            params->hw_frames_ctx = fmt->hwctx;
            params->frame_rate = av_d2q(fmt->nominal_fps, 1000000);
            filter_name = "buffer";
        } else {
            assert(0);
        }

        params->time_base = pad->timebase;

        const AVFilter *filter = avfilter_get_by_name(filter_name);
        if (filter) {
            char name[256];
            snprintf(name, sizeof(name), "mpv_src_%s", pad->name);

            pad->buffer = avfilter_graph_alloc_filter(c->graph, filter, name);
        }
        if (!pad->buffer) {
            av_free(params);
            goto error;
        }

        int ret = av_buffersrc_parameters_set(pad->buffer, params);
        av_free(params);
        if (ret < 0)
            goto error;

        if (avfilter_init_str(pad->buffer, NULL) < 0)
            goto error;

        if (avfilter_link(pad->buffer, 0, pad->filter, pad->filter_pad) < 0)
            goto error;

#endif
    }

    return 1;

error:
    return 0;
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
        .class_name = "encoding",
        .item_name  = av_default_item_name,
        .version    = LIBAVUTIL_VERSION_INT,
    };

    return ctx;
}
