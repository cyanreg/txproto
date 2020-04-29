#include <stdatomic.h>
#include <libavutil/time.h>
#include <libavutil/avstring.h>
#include <libavutil/channel_layout.h>
#include <pulse/pulseaudio.h>

#include "frame_fifo.h"
#include "src_common.h"
#include "utils.h"

typedef struct PulseSource {
    uint32_t index;
    char *name;
    char *desc;
    pa_sample_spec ss;
    pa_channel_map map;

    /* Its actually a sink input - just a client giving a sink audio */
    int is_sink_input;
    uint32_t master_sink_index;
} PulseSource;

typedef struct PulseSink {
    uint32_t index;
    char *name;
    char *desc;
    pa_sample_spec ss;
    pa_channel_map map;
    uint32_t monitor_source;
} PulseSink;

typedef struct PulseCtx {
    AVClass *class;

    pthread_mutex_t capture_ctx_lock;
    struct PulseCaptureCtx **capture_ctx;
    int capture_ctx_num;

    pa_context *pa_context;
    pa_threaded_mainloop *pa_mainloop;
    pa_mainloop_api *pa_mainloop_api;

    /* Defaults */
    char *default_sink_name;
    char *default_source_name;

    /* Sinks list */
    pthread_mutex_t sinks_lock;
    PulseSink **p_sinks;
    int p_sinks_num;

    /* Sources list */
    pthread_mutex_t sources_lock;
    PulseSource **p_sources;
    int p_sources_num;

    /* This is just temporary data, update it in the sources function */
    SourceInfo *tmp_sources;
    int num_tmp_sources;
} PulseCtx;

typedef struct PulseCaptureCtx {
    PulseCtx *main;
    AVFrameFIFO *fifo;
    PulseSource *src;
    pa_stream *stream;

    /* Stats */
    int dropped_samples;

    /* Error handling */
    error_handler *error_handler;
    void *error_handler_ctx;

    /* Info */
    int sample_rate;
    enum AVSampleFormat sample_fmt;
    uint64_t channel_layout;
    int bits_per_sample;
    AVRational time_base;

    int64_t start_pts;
} PulseCaptureCtx;

/**
 * \brief waits for a pulseaudio operation to finish, frees it and
 *        unlocks the mainloop
 * \param op operation to wait for
 * \return 1 if operation has finished normally (DONE state), 0 otherwise
 */
static int waitop(PulseCtx *ctx, pa_operation *op)
{
    if (!op) {
        pa_threaded_mainloop_unlock(ctx->pa_mainloop);
        return 0;
    }
    pa_operation_state_t state = pa_operation_get_state(op);
    while (state == PA_OPERATION_RUNNING) {
        pa_threaded_mainloop_wait(ctx->pa_mainloop);
        state = pa_operation_get_state(op);
    }
    pa_operation_unref(op);
    pa_threaded_mainloop_unlock(ctx->pa_mainloop);
    return state == PA_OPERATION_DONE;
}

FN_CREATING(PulseCtx, PulseSource, source, p_sources, p_sources_num)
FN_CREATING(PulseCtx, PulseSink, sink, p_sinks, p_sinks_num)
FN_CREATING(PulseCtx, PulseCaptureCtx, capture_ctx, capture_ctx, capture_ctx_num)

static const struct {
    enum AVSampleFormat av_format;
    int bits_per_sample;
} format_map[PA_SAMPLE_MAX] = {
    [PA_SAMPLE_U8]        = { AV_SAMPLE_FMT_U8,   8 },
    [PA_SAMPLE_S16NE]     = { AV_SAMPLE_FMT_S16, 16 },
    [PA_SAMPLE_S24_32NE]  = { AV_SAMPLE_FMT_S32, 24 },
    [PA_SAMPLE_S32NE]     = { AV_SAMPLE_FMT_S32, 32 },
    [PA_SAMPLE_FLOAT32NE] = { AV_SAMPLE_FMT_FLT, 32 },
};

static const uint64_t pa_to_lavu_ch_map(const pa_channel_map *ch_map)
{
    static const uint64_t channel_map[PA_CHANNEL_POSITION_MAX] = {
        [PA_CHANNEL_POSITION_FRONT_LEFT]            = AV_CH_FRONT_LEFT,
        [PA_CHANNEL_POSITION_FRONT_RIGHT]           = AV_CH_FRONT_RIGHT,
        [PA_CHANNEL_POSITION_FRONT_CENTER]          = AV_CH_FRONT_CENTER,
        [PA_CHANNEL_POSITION_REAR_CENTER]           = AV_CH_BACK_CENTER,
        [PA_CHANNEL_POSITION_REAR_LEFT]             = AV_CH_BACK_LEFT,
        [PA_CHANNEL_POSITION_REAR_RIGHT]            = AV_CH_BACK_RIGHT,
        [PA_CHANNEL_POSITION_LFE]                   = AV_CH_LOW_FREQUENCY,
        [PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER]  = AV_CH_FRONT_LEFT_OF_CENTER,
        [PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER] = AV_CH_FRONT_RIGHT_OF_CENTER,
        [PA_CHANNEL_POSITION_SIDE_LEFT]             = AV_CH_SIDE_LEFT,
        [PA_CHANNEL_POSITION_SIDE_RIGHT]            = AV_CH_SIDE_RIGHT,
        [PA_CHANNEL_POSITION_TOP_CENTER]            = AV_CH_TOP_CENTER,
        [PA_CHANNEL_POSITION_TOP_FRONT_LEFT]        = AV_CH_TOP_FRONT_LEFT,
        [PA_CHANNEL_POSITION_TOP_FRONT_RIGHT]       = AV_CH_TOP_FRONT_RIGHT,
        [PA_CHANNEL_POSITION_TOP_FRONT_CENTER]      = AV_CH_TOP_FRONT_CENTER,
        [PA_CHANNEL_POSITION_TOP_REAR_LEFT]         = AV_CH_TOP_BACK_LEFT,
        [PA_CHANNEL_POSITION_TOP_REAR_RIGHT]        = AV_CH_TOP_BACK_RIGHT,
        [PA_CHANNEL_POSITION_TOP_REAR_CENTER]       = AV_CH_TOP_BACK_CENTER,
    };

    uint64_t map = 0;
    for (int i = 0; i < ch_map->channels; i++)
        map |= channel_map[ch_map->map[i]];

    return map;
}

static void stream_read_cb(pa_stream *stream, size_t size, void *data)
{
    const void *buffer;
    PulseCaptureCtx *ctx = data;
    const pa_sample_spec *ss = pa_stream_get_sample_spec(stream);
    const pa_channel_map *ch_map = pa_stream_get_channel_map(stream);

    /* Read the samples */
    pa_stream_peek(stream, &buffer, &size);

    /* There's no data */
    if (!buffer && !size)
        return;

    AVFrame *f               = av_frame_alloc();
    f->sample_rate           = ss->rate;
    f->format                = format_map[ss->format].av_format;
    f->channel_layout        = pa_to_lavu_ch_map(ch_map);
    f->channels              = ss->channels;
    f->nb_samples            = (size / av_get_bytes_per_sample(f->format)) / f->channels;
    f->best_effort_timestamp = av_gettime_relative();
    f->opaque_ref            = av_buffer_allocz(sizeof(FormatExtraData));

    FormatExtraData *fe      = (FormatExtraData *)f->opaque_ref->data;
    fe->time_base            = av_make_q(1, 1000000);
    fe->bits_per_sample      = format_map[ss->format].bits_per_sample;

    /* Allocate the frame */
    av_frame_get_buffer(f, 0);

    if (buffer)
        memcpy(f->data[0], buffer, size);
    else /* There's a hole */
        av_samples_set_silence(f->data, 0, f->nb_samples, f->channels, f->format);

    /* Get PTS*/
    pa_usec_t pts;
    if (pa_stream_get_time(stream, &pts) == PA_OK) {
        f->pts = pts;
        int delay_neg;
        pa_usec_t delay;
        if (pa_stream_get_latency(stream, &delay, &delay_neg) == PA_OK) {
            f->pts += delay_neg ? +((int64_t)delay) : -((int64_t)delay);
//            f->best_effort_timestamp += delay_neg ? +((int64_t)delay) : -((int64_t)delay);
        }
    } else {
        f->pts = AV_NOPTS_VALUE;
    }

    /* Copied, and pts calculated, we can drop the buffer now */
    pa_stream_drop(stream);

    if (ctx->fifo && queue_is_full(ctx->fifo)) {
        av_log(ctx->main, AV_LOG_WARNING, "Dropping a frame, queue is full!\n");
        ctx->dropped_samples += f->nb_samples;
        av_frame_free(&f);
    } else if (ctx->fifo) {
        if (push_to_fifo(ctx->fifo, f)) {
            av_log(ctx->main, AV_LOG_ERROR, "Unable to push frame to FIFO!\n");
//            err = AVERROR(ENOMEM);
//            goto fail;
        }
    } else {
        av_frame_free(&f);
    }
}

static void stream_status_cb(pa_stream *stream, void *data)
{
    PulseCtx *main_ctx;
    PulseCaptureCtx *ctx = data;
    const pa_sample_spec *ss;
    const pa_channel_map *ch_map;
    char map_str[256];

    switch (pa_stream_get_state(stream)) {
    case PA_STREAM_READY:
        ss = pa_stream_get_sample_spec(stream);
        ch_map = pa_stream_get_channel_map(stream);
        av_get_channel_layout_string(map_str, sizeof(map_str), ss->channels, pa_to_lavu_ch_map(ch_map));
        av_log(ctx->main, AV_LOG_INFO, "Capture stream ready, format: %iHz %s %ich %s\n",
               ss->rate, map_str, ss->channels, av_get_sample_fmt_name(format_map[ss->format].av_format));
        break;
    case PA_STREAM_UNCONNECTED:
        return;
    case PA_STREAM_CREATING:
        return;
    case PA_STREAM_FAILED:
        av_log(ctx->main, AV_LOG_ERROR, "Capture stream failed!\n");
        /* Fallthrough */
    case PA_STREAM_TERMINATED:
        main_ctx = ctx->main;
        pthread_mutex_t *lock = &main_ctx->capture_ctx_lock;

        av_log(main_ctx, AV_LOG_INFO, "Terminating capture stream!\n");

        /* Remove the context */
        pthread_mutex_lock(lock);
        remove_capture_ctx(main_ctx, ctx);
        pthread_mutex_unlock(lock);

        return;
    }

    return;
}

static int start_pulse(void *s, uint64_t identifier, AVDictionary *opts, AVFrameFIFO *dst,
                       error_handler *err_cb, void *error_handler_ctx)
{
    int err = 0;
    PulseCtx *ctx = s;

    pa_threaded_mainloop_lock(ctx->pa_mainloop);
    pthread_mutex_lock(&ctx->sources_lock);
    pthread_mutex_lock(&ctx->capture_ctx_lock);

    int idx = 0;
    PulseSource *tmp, *src;
    while ((tmp = get_at_index_source(ctx, idx))) {
        if (tmp->index == identifier) {
            src = tmp;
            break;
        }
        idx++;
    }

    if (!tmp) {
        av_log(ctx, AV_LOG_ERROR, "Identifier %lu not found!\n", identifier);
        err = AVERROR(EINVAL);
        goto fail;
    }

    av_log(ctx, AV_LOG_INFO, "Capturing from \"%s\" (id: %u)\n",
           src->name, src->index);

    PulseCaptureCtx *cap_ctx = create_capture_ctx(ctx);
    cap_ctx->main = ctx;
    cap_ctx->src = src;
    cap_ctx->fifo = dst;
    cap_ctx->error_handler = err_cb;
    cap_ctx->error_handler_ctx = error_handler_ctx;

    pa_sample_spec req_ss = src->ss;
    pa_channel_map req_map = src->map;

    /* Filter out useless formats */
    if (req_ss.format != PA_SAMPLE_U8    &&
        req_ss.format != PA_SAMPLE_S16NE && req_ss.format != PA_SAMPLE_S24_32NE &&
        req_ss.format != PA_SAMPLE_S32NE && req_ss.format != PA_SAMPLE_FLOAT32NE)
        req_ss.format = PA_SAMPLE_FLOAT32NE;

    /* We don't care about the rate as we'll have to resample ourselves anyway */
    if ((req_ss.rate < 8000) || (req_ss.rate > 48000))
        req_ss.rate = 48000;

    /* Check for crazy layouts */
    uint64_t lavu_ch_map = pa_to_lavu_ch_map(&req_map);
    if (av_get_default_channel_layout(req_map.channels) != lavu_ch_map)
        pa_channel_map_init_stereo(&req_map);

    cap_ctx->stream = pa_stream_new(ctx->pa_context, PROGRAM_NAME, &req_ss, &req_map);
    if (!cap_ctx->stream) {
        av_log(ctx, AV_LOG_ERROR, "Unable to init stream!\n");
        err = AVERROR(EINVAL);
        goto fail;
    }

    /* Set the buffer size */
    pa_buffer_attr attr = { -1, -1, -1, -1, -1 };
    if (dict_get(opts, "buffer_ms"))
        attr.fragsize = strtol(dict_get(opts, "buffer_ms"), NULL, 10) * 1000;

    if (attr.fragsize > 0)
        attr.fragsize = pa_usec_to_bytes(attr.fragsize, &req_ss);
    else /* 1024 frame size, because codecs */
        attr.fragsize = 1024 * pa_sample_size(&req_ss) * req_ss.channels;

    /* Set stream callbacks */
    pa_stream_set_state_callback(cap_ctx->stream, stream_status_cb, cap_ctx);
    pa_stream_set_read_callback(cap_ctx->stream, stream_read_cb, cap_ctx);

    const char *target_name = src->name;
    if (src->is_sink_input) {
        idx = 0;
        PulseSink *sink_tmp, *sink = NULL;
        while ((sink_tmp = get_at_index_sink(ctx, idx))) {
            if (sink_tmp->index == src->master_sink_index) {
                sink = sink_tmp;
                break;
            }
            idx++;
        }

        if (!sink) {
            av_log(ctx, AV_LOG_ERROR, "Zombie sink input %s (%i) is streaming to "
                   "a non-existent master sink %i!\n",
                   src->name, src->index, src->master_sink_index);
            err = AVERROR(EINVAL);
            goto fail;
        }

        idx = 0;
        PulseSource *monitor = NULL;
        while ((tmp = get_at_index_source(ctx, idx))) {
            if (tmp->index == sink->monitor_source) {
                monitor = tmp;
                break;
            }
            idx++;
        }

        if (!monitor) {
            av_log(ctx, AV_LOG_ERROR, "Sink has a non-existent monitor source!\n");
            err = AVERROR(EINVAL);
            goto fail;
        }

        if (pa_stream_set_monitor_stream(cap_ctx->stream, src->index) < 0) {
            av_log(ctx, AV_LOG_ERROR, "pa_stream_set_monitor_stream() failed!\n");
            err = AVERROR(EINVAL);
            goto fail;
        }

        target_name = monitor->name;
    }

    /* Start stream */
    if (pa_stream_connect_record(cap_ctx->stream, target_name, &attr,
                                 PA_STREAM_ADJUST_LATENCY     |
                                 PA_STREAM_NOT_MONOTONIC      |
                                 PA_STREAM_AUTO_TIMING_UPDATE |
                                 PA_STREAM_INTERPOLATE_TIMING |
                                 PA_STREAM_NOFLAGS)) {
        av_log(ctx, AV_LOG_ERROR, "pa_stream_connect_record() failed!\n");
        err = AVERROR(EINVAL);
        goto fail;
    }

    av_dict_free(&opts);
    pthread_mutex_unlock(&ctx->capture_ctx_lock);
    pthread_mutex_unlock(&ctx->sources_lock);
    pa_threaded_mainloop_unlock(ctx->pa_mainloop);

    return 0;

fail:
    av_dict_free(&opts);
    pthread_mutex_unlock(&ctx->capture_ctx_lock);
    pthread_mutex_unlock(&ctx->sources_lock);
    pa_threaded_mainloop_unlock(ctx->pa_mainloop);

    return err;
}

static void stream_success_cb(pa_stream *stream, int success, void *data)
{
    PulseCaptureCtx *ctx = data;
    pa_threaded_mainloop_signal(ctx->main->pa_mainloop, 0);
}

static int stop_pulse(void *s, uint64_t identifier)
{
    PulseCtx *ctx = s;

    pthread_mutex_lock(&ctx->sources_lock);
    pthread_mutex_lock(&ctx->capture_ctx_lock);

    for (int i = 0; i < ctx->capture_ctx_num; i++) {
        PulseCaptureCtx *cap_ctx = ctx->capture_ctx[i];
        if (identifier == ctx->capture_ctx[i]->src->index) {
            /* Drain it */
            pa_threaded_mainloop_lock(ctx->pa_mainloop);
            waitop(ctx, pa_stream_drain(cap_ctx->stream, stream_success_cb, cap_ctx));

            /* Disconnect */
            pa_threaded_mainloop_lock(ctx->pa_mainloop);
            pa_stream_disconnect(cap_ctx->stream);
            pa_threaded_mainloop_unlock(ctx->pa_mainloop);
        }
    }

    pthread_mutex_unlock(&ctx->capture_ctx_lock);
    pthread_mutex_unlock(&ctx->sources_lock);

    return 0;
}

static void free_tmp_sources(PulseCtx *ctx)
{
    for (int i = 0; i < ctx->num_tmp_sources; i++) {
        av_free(ctx->tmp_sources[i].name);
        av_free(ctx->tmp_sources[i].desc);
    }
    av_freep(&ctx->tmp_sources);
}

#define FREE_STRING_VALUES(src) \
    av_freep(&src->name);       \
    av_freep(&src->desc);       \

#define CALLBACK_BOILERPLATE(type, ftype, stype)                         \
    PulseCtx *ctx = data;                                                \
    if (eol)                                                             \
        return;                                                          \
    pthread_mutex_lock(&ctx->ftype ## s_lock);                           \
    int idx = 0;                                                         \
    type *tmp, *src = NULL;                                              \
    while ((tmp = get_at_index_ ##ftype (ctx, idx++))) {                 \
        if (tmp->index == info->index) {                                 \
            src = tmp;                                                   \
            av_log(ctx, AV_LOG_INFO, "Updating " stype " %s (id: %i)\n", \
                   info->name, info->index);                             \
            FREE_STRING_VALUES(src)                                      \
            break;                                                       \
        }                                                                \
    }                                                                    \
                                                                         \
    if (!src) {                                                          \
        src = create_ ##ftype (ctx);                                     \
        av_log(ctx, AV_LOG_INFO, "Adding new " stype " %s (id: %i)\n",   \
               info->name, info->index);                                 \
    }

static void sink_cb(pa_context *context, const pa_sink_info *info, int eol, void *data)
{
    CALLBACK_BOILERPLATE(PulseSink, sink, "sink")
    src->index          = info->index;
    src->name           = av_strdup(info->name);
    src->desc           = av_strdup(info->description);
    src->ss             = info->sample_spec;
    src->map            = info->channel_map;
    src->monitor_source = info->monitor_source;
    pthread_mutex_unlock(&ctx->sinks_lock);
}

static void source_cb(pa_context *context, const pa_source_info *info, int eol, void *data)
{
    CALLBACK_BOILERPLATE(PulseSource, source, "source")
    src->index = info->index;
    src->name  = av_strdup(info->name);
    src->desc  = av_strdup(info->description);
    src->ss    = info->sample_spec;
    src->map   = info->channel_map;
    pthread_mutex_unlock(&ctx->sources_lock);
}

static void sink_input_cb(pa_context *context, const pa_sink_input_info *info, int eol, void *data)
{
    CALLBACK_BOILERPLATE(PulseSource, source, "sink input")
    src->index             = info->index;
    src->name              = av_strdup(info->name);
    src->desc              = av_strdup("sink input");
    src->is_sink_input     = 1;
    src->master_sink_index = info->sink;
    src->ss                = info->sample_spec;
    src->map               = info->channel_map;
    pthread_mutex_unlock(&ctx->sources_lock);
}

/* Not implemented - implement for playback */
static void stop_stream_sink(PulseCtx *ctx, PulseSink *sink)
{

}

static void stop_stream_source(PulseCtx *ctx, PulseSource *sink)
{
    pthread_mutex_lock(&ctx->capture_ctx_lock);
    for (int i = 0; i < ctx->capture_ctx_num; i++) {
        PulseCaptureCtx *cap_ctx = ctx->capture_ctx[i];
        if (cap_ctx->src == sink) {
            av_log(ctx, AV_LOG_INFO, "Soure %i must be stopped!\n", sink->index);

            /* Kill it immediately, purging all the data */
            pa_stream_disconnect(cap_ctx->stream);

            break;
        }
    }
    pthread_mutex_unlock(&ctx->capture_ctx_lock);
}

static void subscribe_cb(pa_context *context, pa_subscription_event_type_t type,
                         uint32_t index, void *data)
{
    pa_operation *o;
    PulseCtx *ctx = data;
    pa_subscription_event_type_t facility = type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    pa_subscription_event_type_t event = type & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

#define MONITOR_TEMPL(event_type, hook_fn, callback, ftype, stype)                     \
    case PA_SUBSCRIPTION_EVENT_ ## event_type:                                         \
        if (event & PA_SUBSCRIPTION_EVENT_REMOVE) {                                    \
            pthread_mutex_lock(&ctx->ftype ## s_lock);                                 \
            int idx = 0;                                                               \
            while (get_at_index_ ##ftype (ctx, idx))  {                                \
                if (get_at_index_ ##ftype (ctx, idx)->index == index) {                \
                    av_log(ctx, AV_LOG_INFO, "Removing " stype " %s (id: %i)\n",       \
                           get_at_index_ ##ftype (ctx, idx)->name, index);             \
                    stop_stream_ ##ftype(ctx, get_at_index_ ##ftype (ctx, idx));       \
                    FREE_STRING_VALUES(get_at_index_ ##ftype (ctx, idx));              \
                    remove_ ##ftype(ctx, get_at_index_ ##ftype (ctx, idx));            \
                    pthread_mutex_unlock(&ctx->ftype ## s_lock);                       \
                    return;                                                            \
                }                                                                      \
                idx++;                                                                 \
            }                                                                          \
            av_log(ctx, AV_LOG_INFO, stype " id %i not found for removal!\n", index);  \
            pthread_mutex_unlock(&ctx->ftype ## s_lock);                               \
            return;                                                                    \
        } else {                                                                       \
            if (!(o = hook_fn(context, index, callback, ctx))) {                       \
                av_log(ctx, AV_LOG_ERROR, #hook_fn "() failed for id %u\n", index);    \
                return;                                                                \
            }                                                                          \
            pa_operation_unref(o);                                                     \
        }                                                                              \
        break;                                                                         \

    switch (facility) {
    MONITOR_TEMPL(SINK, pa_context_get_sink_info_by_index, sink_cb, sink, "sink")
    MONITOR_TEMPL(SOURCE, pa_context_get_source_info_by_index, source_cb, source, "source")
    MONITOR_TEMPL(SINK_INPUT, pa_context_get_sink_input_info, sink_input_cb, source, "sink input")
    default:
        break;
    };
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
        av_log(ctx, AV_LOG_ERROR, "PulseAudio connection failed!\n");
        pa_context_unref(context);
        break;
    case PA_CONTEXT_TERMINATED:
        av_log(ctx, AV_LOG_INFO, "PulseAudio connection terminated!\n");
        break;
    case PA_CONTEXT_READY:
        av_log(ctx, AV_LOG_INFO, "PulseAudio connection ready, arming callbacks!\n");
        pa_operation *o;

        /* Subscribe for new updates */
        pa_context_set_subscribe_callback(context, subscribe_cb, ctx);
        if (!(o = pa_context_subscribe(context, (pa_subscription_mask_t)
                                                (PA_SUBSCRIPTION_MASK_SINK       |
                                                 PA_SUBSCRIPTION_MASK_SOURCE     |
                                                 PA_SUBSCRIPTION_MASK_SINK_INPUT), NULL, NULL))) {
            av_log(ctx, AV_LOG_INFO, "pa_context_subscribe() failed\n");
            return;
        }
        pa_operation_unref(o);

#define LOAD_INITIAL(hook_fn, callback)                   \
    if (!(o = hook_fn(context, callback, ctx))) {         \
        av_log(ctx, AV_LOG_INFO, #hook_fn "() failed\n"); \
        return;                                           \
    }                                                     \
    pa_operation_unref(o);                                \

        LOAD_INITIAL(pa_context_get_sink_info_list, sink_cb)
        LOAD_INITIAL(pa_context_get_source_info_list, source_cb)
        LOAD_INITIAL(pa_context_get_sink_input_info_list, sink_input_cb)

        /* Signal we're ready to the init function */
        pa_threaded_mainloop_signal(ctx->pa_mainloop, 0);
        break;
    }
}

static void server_info_cb(pa_context *context, const pa_server_info *info, void *data)
{
    PulseCtx *ctx = data;

    av_freep(&ctx->default_sink_name);
    av_freep(&ctx->default_source_name);
    ctx->default_sink_name = av_strdup(info->default_sink_name);
    ctx->default_source_name = av_strdup(info->default_source_name);

    pa_threaded_mainloop_signal(ctx->pa_mainloop, 0);
}

static void sources_pulse(void *s, SourceInfo **sources, int *num)
{
    PulseCtx *ctx = s;

    free_tmp_sources(ctx);

    /* Its a hack to get the thread going first time to get the list */
    pa_threaded_mainloop_lock(ctx->pa_mainloop);
    waitop(ctx, pa_context_get_server_info(ctx->pa_context, server_info_cb, ctx));

    pthread_mutex_lock(&ctx->sources_lock);

    ctx->tmp_sources = av_mallocz(ctx->p_sources_num * sizeof(SourceInfo));
    ctx->num_tmp_sources = ctx->p_sources_num;

    int idx = 0;
    PulseSource *src;
    while ((src = get_at_index_source(ctx, idx))) {
        ctx->tmp_sources[idx].name = av_strdup(src->name);
        ctx->tmp_sources[idx].desc = av_strdup(src->desc);
        ctx->tmp_sources[idx].identifier = src->index;
        idx++;
    }

    pthread_mutex_unlock(&ctx->sources_lock);

    *sources = ctx->tmp_sources;
    *num = ctx->num_tmp_sources;
}

static void free_pulse(void **s)
{
    PulseCtx *ctx = *s;

    if (ctx->pa_mainloop)
        pa_threaded_mainloop_stop(ctx->pa_mainloop);

    int idx = 0;
    PulseSink *sink = NULL;
    while ((sink = get_at_index_sink(ctx, idx++))) {
        FREE_STRING_VALUES(sink)
        av_free(sink);
    }
    av_freep(&ctx->p_sinks);

    PulseSource *source = NULL;
    while ((source = get_at_index_source(ctx, idx++))) {
        FREE_STRING_VALUES(source)
        av_free(source);
    }
    av_freep(&ctx->p_sources);

    free_tmp_sources(ctx);
    av_freep(&ctx->default_sink_name);
    av_freep(&ctx->default_source_name);

    if (ctx->pa_context) {
        pa_context_disconnect(ctx->pa_context);
        pa_context_unref(ctx->pa_context);
    }

    if (ctx->pa_mainloop)
        pa_threaded_mainloop_free(ctx->pa_mainloop);

    av_freep(&ctx->class);
    av_freep(s);
}

static int init_pulse(void **s)
{
    int locked = 0;
    PulseCtx *ctx = av_mallocz(sizeof(*ctx));
    ctx->class = av_mallocz(sizeof(*ctx->class));
    *ctx->class = (AVClass) {
        .class_name = "pulse",
        .item_name  = av_default_item_name,
        .version    = LIBAVUTIL_VERSION_INT,
    };

    pthread_mutex_init(&ctx->sinks_lock, NULL);
    pthread_mutex_init(&ctx->sources_lock, NULL);
    pthread_mutex_init(&ctx->capture_ctx_lock, NULL);

    ctx->pa_mainloop = pa_threaded_mainloop_new();
    pa_threaded_mainloop_start(ctx->pa_mainloop);
    pa_threaded_mainloop_set_name(ctx->pa_mainloop, PROGRAM_NAME "_pulse_thread");

    pa_threaded_mainloop_lock(ctx->pa_mainloop);
    locked = 1;

    ctx->pa_mainloop_api = pa_threaded_mainloop_get_api(ctx->pa_mainloop);

    ctx->pa_context = pa_context_new(ctx->pa_mainloop_api, PROGRAM_NAME);
    pa_context_set_state_callback(ctx->pa_context, pulse_state_cb, ctx);
    pa_context_connect(ctx->pa_context, NULL, PA_CONTEXT_NOFLAGS, NULL);

    /* Wait until the context is ready */
    while (1) {
        int state = pa_context_get_state(ctx->pa_context);
        if (state == PA_CONTEXT_READY)
            break;
        if (!PA_CONTEXT_IS_GOOD(state))
            goto fail;
        pa_threaded_mainloop_wait(ctx->pa_mainloop);
    }

    pa_threaded_mainloop_unlock(ctx->pa_mainloop);
    locked = 0;

    *s = ctx;

    return 0;

fail:
    if (locked)
        pa_threaded_mainloop_unlock(ctx->pa_mainloop);

    return AVERROR(EINVAL);
}

const CaptureSource src_pulse = {
    .name    = "pulse",
    .init    = init_pulse,
    .start   = start_pulse,
    .sources = sources_pulse,
    .stop    = stop_pulse,
    .free    = free_pulse,
};
