#include <stdatomic.h>
#include <libavutil/avstring.h>
#include <libavutil/channel_layout.h>
#include <pulse/pulseaudio.h>

#include "frame_fifo.h"
#include "src_common.h"
#include "utils.h"

typedef struct PulseCtx {
    AVClass *class;

    SourceInfo *sources;
    int num_sources;

    struct PulseCaptureCtx **capture_ctx;
    int capture_ctx_num;

    pa_context *pa_context;
    pa_threaded_mainloop *pa_mainloop;
    pa_mainloop_api *pa_mainloop_api;

    pthread_mutex_t sink_seen_last;
    pthread_mutex_t source_seen_last;

    int err;
    void *err_opaque;
    report_error *report_err;
} PulseCtx;

typedef struct PulseCaptureCtx {
    PulseCtx *main;
    AVFrameFIFO *fifo;
    uint64_t identifier;

    FormatReport fmt_report;
    pa_sample_spec pa_spec;

    int64_t start_pts;

    atomic_bool quit;
} PulseCaptureCtx;

static void pulse_stream_read_cb(pa_stream *stream, size_t size,
                                 void *data)
{
    PulseCaptureCtx *ctx = data;

    if (atomic_load(&ctx->quit)) {
        pa_stream_drop(stream);
        pa_stream_disconnect(stream);
        av_free(ctx);
        return;
    }

    const void *buffer;
    pa_stream_peek(stream, &buffer, &size);

    AVFrame *f = av_frame_alloc();
    f->sample_rate    = ctx->fmt_report.sample_rate;
    f->format         = ctx->fmt_report.sample_fmt;
    f->channel_layout = ctx->fmt_report.channel_layout;
    f->nb_samples     = size >> (av_log2(av_get_bytes_per_sample(f->format)) + (ctx->pa_spec.channels > 1));

    /* I can't believe these bastards don't output aligned data. Let's hope the
     * ability to have writeable refcounted frames is worth it elsewhere. */
    av_frame_get_buffer(f, 0);
    if (buffer)
        memcpy(f->data[0], buffer, size);
    else
        memset(f->data[0], 0, size); /* Buffer is actually a hole */

    int delay_neg;
    pa_usec_t pts, delay;
    if (!pa_stream_get_time(stream, &pts) && buffer &&
        !pa_stream_get_latency(stream, &delay, &delay_neg)) {
        f->pts = pts;
        f->pts -= delay_neg ? -(int64_t)delay : (int64_t)delay;
        if (!ctx->start_pts)
            ctx->start_pts = f->pts;
        f->pts -= ctx->start_pts;
    } else {
        f->pts = INT64_MIN;
    }

    pa_stream_drop(stream);

    if (!ctx->fifo) {
        av_frame_free(&f);
    } else if (push_to_fifo(ctx->fifo, f)) {
        av_log(ctx->main, AV_LOG_INFO, "Unable to push frame to FIFO!\n");
        av_frame_free(&f);
    }
}

static void pulse_stream_status_cb(pa_stream *stream, void *data)
{

}

static const pa_sample_format_t format_map[] = {
    [PA_SAMPLE_FLOAT32NE] = AV_SAMPLE_FMT_FLT,
    [PA_SAMPLE_S16NE]     = AV_SAMPLE_FMT_S16,
    [PA_SAMPLE_S32NE]     = AV_SAMPLE_FMT_S32,
};

FN_CREATING(PulseCtx, PulseCaptureCtx, capture_ctx, capture_ctx, capture_ctx_num)

static int start_pulse(void *s, uint64_t identifier, AVDictionary *opts,
                       AVFrameFIFO *dst, report_format *info_cb, void *info_cb_ctx)
{
    PulseCtx *ctx = s;
    SourceInfo *src = NULL;

    for (int i = 0; i < ctx->num_sources; i++)
        if (identifier == ctx->sources[i].identifier)
            src = &ctx->sources[i];
    if (!src)
        return AVERROR(EINVAL);

    av_log(ctx, AV_LOG_INFO, "Starting capturing from \"%s\"\n",
           src->desc);

    PulseCaptureCtx *cap_ctx = create_capture_ctx(ctx);

    cap_ctx->main = ctx;
    cap_ctx->quit = ATOMIC_VAR_INIT(0);
    cap_ctx->identifier = identifier;
    cap_ctx->fifo = dst;

    pa_buffer_attr attr = { 0 };
    pa_channel_map map  = { 0 };

    cap_ctx->pa_spec.format   = PA_SAMPLE_S32NE;
    cap_ctx->pa_spec.rate     = 48000;
    cap_ctx->pa_spec.channels = 2;
    if (cap_ctx->pa_spec.channels < 2)
        pa_channel_map_init_mono(&map);
    else
        pa_channel_map_init_stereo(&map);

    cap_ctx->fmt_report.type           = AVMEDIA_TYPE_AUDIO;
    cap_ctx->fmt_report.sample_fmt     = format_map[cap_ctx->pa_spec.format];
    cap_ctx->fmt_report.sample_rate    = cap_ctx->pa_spec.rate;
    cap_ctx->fmt_report.time_base      = av_make_q(1, 1000000);
    cap_ctx->fmt_report.channel_layout = av_get_default_channel_layout(cap_ctx->pa_spec.channels);

    int err = info_cb(info_cb_ctx, &cap_ctx->fmt_report);
    if (err)
        goto fail;

    uint32_t bsize = 512*4;
    attr.fragsize  = bsize;
    attr.maxlength = bsize;

    pa_stream *stream = pa_stream_new(ctx->pa_context, "wlstream",
                                      &cap_ctx->pa_spec, &map);

    /* Set stream callbacks */
    pa_stream_set_state_callback(stream, pulse_stream_status_cb, cap_ctx);
    pa_stream_set_read_callback(stream, pulse_stream_read_cb, cap_ctx);

    /* Start stream */
    pa_stream_connect_record(stream, src->name, &attr,
                             PA_STREAM_ADJUST_LATENCY     |
                             PA_STREAM_AUTO_TIMING_UPDATE |
                             PA_STREAM_INTERPOLATE_TIMING);

    return 0;

fail:
    /* TODO: Undo everything */

    return err;
}

static void pulse_sink_info_cb(pa_context *context, const pa_sink_info *info,
                               int is_last, void *data)
{
    PulseCtx *ctx = data;

    if (is_last) {
        pthread_mutex_unlock(&ctx->sink_seen_last);
        return;
    }

    ctx->num_sources++;
    ctx->sources = av_realloc(ctx->sources, ctx->num_sources*sizeof(*ctx->sources));

    ctx->sources[ctx->num_sources - 1].name = av_strdup(info->monitor_source_name);
    ctx->sources[ctx->num_sources - 1].identifier = (uint64_t)&ctx->sources[ctx->num_sources - 1].name;
    ctx->sources[ctx->num_sources - 1].desc = av_strdup(info->description);
}

static void pulse_source_info_cb(pa_context *context, const pa_source_info *info,
                                 int is_last, void *data)
{
    PulseCtx *ctx = data;

    if (is_last) {
        pthread_mutex_unlock(&ctx->source_seen_last);
        return;
    }

    ctx->num_sources++;
    ctx->sources = av_realloc(ctx->sources, ctx->num_sources*sizeof(*ctx->sources));

    ctx->sources[ctx->num_sources - 1].name = av_strdup(info->name);
    ctx->sources[ctx->num_sources - 1].identifier = (uint64_t)&ctx->sources[ctx->num_sources - 1].name;
    ctx->sources[ctx->num_sources - 1].desc = av_strdup(info->description);
}

static void pulse_server_info_cb(pa_context *context, const pa_server_info *info,
                                 void *data)
{
    pa_operation *op;
    PulseCtx *ctx = data;

    op = pa_context_get_sink_info_by_name(context, info->default_sink_name,
                                          pulse_sink_info_cb, ctx);
    pa_operation_unref(op);

    op = pa_context_get_source_info_by_name(context, info->default_source_name,
                                            pulse_source_info_cb, ctx);
    pa_operation_unref(op);

    pa_threaded_mainloop_signal(ctx->pa_mainloop, 0);
}

static int stop_pulse(void  *s, uint64_t identifier)
{
    PulseCtx *ctx = s;

    for (int i = 0; i < ctx->capture_ctx_num; i++) {
        PulseCaptureCtx *src = ctx->capture_ctx[i];
        if (identifier == ctx->capture_ctx[i]->identifier) {
            av_log(ctx, AV_LOG_INFO, "Stopping pulse capture from id %lu\n", identifier);
            atomic_store(&src->quit, 1);

            /* The PulseCaptureCtx gets freed by the capture function */

            /* Remove from list */
            ctx->capture_ctx_num = FFMAX(ctx->capture_ctx_num - 1, 0);
            memcpy(&ctx->capture_ctx[i], &ctx->capture_ctx[i + 1],
                   (ctx->capture_ctx_num - i)*sizeof(PulseCaptureCtx *));
        }
    }

    return 0;
}

static void free_pulse(void **s)
{
    PulseCtx *ctx = *s;

    if (ctx->pa_mainloop)
        pa_threaded_mainloop_stop(ctx->pa_mainloop);

    for (int i = 0; i < ctx->num_sources; i++) {
        SourceInfo *src = &ctx->sources[i];
        av_freep(&src->name);
        av_freep(&src->desc);
    }
    av_freep(&ctx->sources);

    av_freep(&ctx->capture_ctx);

    if (ctx->pa_context) {
        pa_context_disconnect(ctx->pa_context);
        pa_context_unref(ctx->pa_context);
    }

    if (ctx->pa_mainloop)
        pa_threaded_mainloop_free(ctx->pa_mainloop);

    av_freep(&ctx->class);
    av_freep(s);
}

static void sources_pulse(void *s, SourceInfo **sources, int *num)
{
    PulseCtx *ctx = s;

    pthread_mutex_lock(&ctx->sink_seen_last);
    pthread_mutex_lock(&ctx->source_seen_last);

    *sources = ctx->sources;
    *num = ctx->num_sources;

    pthread_mutex_unlock(&ctx->sink_seen_last);
    pthread_mutex_unlock(&ctx->source_seen_last);
}

static void pulse_state_cb(pa_context *context, void *data)
{
    PulseCtx *ctx = data;

    switch (pa_context_get_state(context)) {
    case PA_CONTEXT_UNCONNECTED:
        av_log(ctx, AV_LOG_INFO, "PulseAudio reports it is unconnected!\n");
        break;
    case PA_CONTEXT_CONNECTING:
        av_log(ctx, AV_LOG_INFO, "Connecting to PulseAudio!\n");
        break;
    case PA_CONTEXT_AUTHORIZING:
        av_log(ctx, AV_LOG_INFO, "Authorizing PulseAudio connection!\n");
        break;
    case PA_CONTEXT_SETTING_NAME:
        av_log(ctx, AV_LOG_INFO, "Sending client name!\n");
        break;
    case PA_CONTEXT_FAILED:
        av_log(ctx, AV_LOG_INFO, "PulseAudio connection failed!\n");
        break;
    case PA_CONTEXT_TERMINATED:
        av_log(ctx, AV_LOG_INFO, "PulseAudio connection terminated!\n");
        break;
    case PA_CONTEXT_READY:
        av_log(ctx, AV_LOG_INFO, "PulseAudio connection ready!\n");
        pa_operation_unref(pa_context_get_server_info(context,
                           pulse_server_info_cb, ctx));
        break;
    }
}

static int init_pulse(void **s, report_error *err_cb, void *err_opaque)
{
    PulseCtx *ctx = av_mallocz(sizeof(*ctx));
    ctx->class = av_mallocz(sizeof(*ctx->class));
    *ctx->class = (AVClass) {
        .class_name = "wlstream_pulse",
        .item_name  = av_default_item_name,
        .version    = LIBAVUTIL_VERSION_INT,
    };

    pthread_mutex_init(&ctx->sink_seen_last, NULL);
    pthread_mutex_lock(&ctx->sink_seen_last);
    pthread_mutex_init(&ctx->source_seen_last, NULL);
    pthread_mutex_lock(&ctx->source_seen_last);

    ctx->err_opaque      = err_opaque;
    ctx->report_err      = err_cb;
    ctx->pa_mainloop     = pa_threaded_mainloop_new();
    ctx->pa_mainloop_api = pa_threaded_mainloop_get_api(ctx->pa_mainloop);

    ctx->pa_context = pa_context_new(ctx->pa_mainloop_api, "wlstream");
    pa_context_set_state_callback(ctx->pa_context, pulse_state_cb, ctx);
    pa_context_connect(ctx->pa_context, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL);

    pa_threaded_mainloop_start(ctx->pa_mainloop);

    pa_threaded_mainloop_wait(ctx->pa_mainloop);

    *s = ctx;

    return 0;
}

const CaptureSource src_pulse = {
    .name    = "pulseaudio",
    .init    = init_pulse,
    .start   = start_pulse,
    .sources = sources_pulse,
    .stop    = stop_pulse,
    .free    = free_pulse,
};
