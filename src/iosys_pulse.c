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
#include <libavutil/channel_layout.h>
#include <libavutil/crc.h>

#include <pulse/pulseaudio.h>

#include "iosys_common.h"
#include "utils.h"
#include "../config.h"

const IOSysAPI src_pulse;

enum PulseType {
    PULSE_SINK = 0,
    PULSE_SOURCE = 1,
    PULSE_SINK_INPUT = 2,
};

typedef struct PulseCtx {
    AVClass *class;
    int log_lvl_offset;

    pa_context *pa_context;
    pa_threaded_mainloop *pa_mainloop;
    pa_mainloop_api *pa_mainloop_api;

    /* Defaults */
    char *default_sink_name;
    char *default_source_name;

    /* Sinks list */
    SPBufferList *entries;
    SPBufferList *events;
} PulseCtx;

typedef struct PulsePriv {
    PulseCtx *main;
    pa_stream *stream;

    /* Pulse info */
    enum PulseType type;
    uint32_t index;
    char *desc;
    pa_sample_spec ss;
    pa_channel_map map;
    uint32_t monitor_source;    /* Sinks only       */
    uint32_t master_sink_index; /* Sink inputs only */

    int64_t epoch;

    AVBufferPool *pool;
    int pool_entry_size;

    /* Stats */
    int dropped_samples;

    /* First frame delivered minus epoch */
    int64_t delay;

    /* Info */
    int sample_rate;
    enum AVSampleFormat sample_fmt;
    uint64_t channel_layout;
    int bits_per_sample;
    AVRational time_base;
} PulsePriv;

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
    return state != PA_OPERATION_DONE ? AVERROR_EXTERNAL : 0;
}

static void stream_success_cb(pa_stream *stream, int success, void *data)
{
    PulsePriv *priv = data;
    pa_threaded_mainloop_signal(priv->main->pa_mainloop, 0);
}

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

    if (!map)
        map = av_get_default_channel_layout(ch_map->channels);

    return map;
}

static int frame_get_pool_buffer(PulsePriv *priv, AVFrame *f)
{
    int ret = av_samples_get_buffer_size(&f->linesize[0], f->channels,
                                         f->nb_samples, f->format, 0);
    if (ret < 0)
        return ret;

    if (!priv->pool || ret > priv->pool_entry_size) {
        av_buffer_pool_uninit(&priv->pool);
        priv->pool = av_buffer_pool_init2(ret, NULL, NULL, NULL);
    }

    AVBufferRef *buf = av_buffer_pool_get(priv->pool);
    if (!buf)
        return AVERROR(ENOMEM);

    /* This assumes data is not planar */
    f->buf[0] = buf;
    f->data[0] = f->buf[0]->data;
    f->extended_data = f->data;

    return 0;
}

static void stream_read_cb(pa_stream *stream, size_t size, void *data)
{
    const void *buffer;
    IOSysEntry *iosys_entry = (IOSysEntry *)data;
    PulsePriv *priv = iosys_entry->api_priv;
    const pa_sample_spec *ss = pa_stream_get_sample_spec(stream);
    const pa_channel_map *ch_map = pa_stream_get_channel_map(stream);

    /* NOTE: this will NEVER return data more than once per call, regardless
     * of what some other pulse API users do or think. Stop testing for it. */
    if (pa_stream_peek(stream, &buffer, &size) < 0) {
        sp_log(iosys_entry, SP_LOG_ERROR, "Unable to get samples from PA: %s\n",
               pa_strerror(pa_context_errno(priv->main->pa_context)));
        return;
    }

    /* There's no data */
    if (!size)
        return;

    AVFrame *f          = av_frame_alloc();
    f->sample_rate      = ss->rate;
    f->format           = format_map[ss->format].av_format;
    f->channel_layout   = pa_to_lavu_ch_map(ch_map);
    f->channels         = ss->channels;
    f->nb_samples       = (size / av_get_bytes_per_sample(f->format)) / f->channels;
    f->opaque_ref       = av_buffer_allocz(sizeof(FormatExtraData));

    FormatExtraData *fe = (FormatExtraData *)f->opaque_ref->data;
    fe->time_base       = av_make_q(1, 1000000);
    fe->bits_per_sample = format_map[ss->format].bits_per_sample;

    /* Allocate the frame. */
    frame_get_pool_buffer(priv, f);

    /* Copy samples */
    if (buffer) {
        if (fe->bits_per_sample == 24) {
            /* *sigh*
             * Pulseaudio's definition of PA_SAMPLE_S24_32 is to have the padding
             * in the MSB's. This is completely stupid and against what everyone
             * sane does. So we have an easy-to-SIMD copy that hopefully compilers
             * get right and actually SIMD. */
            size >>= 2;
            const uint32_t *src = buffer;
            int32_t *dst = (int32_t *)f->data[0];
            for (int i = 0; i < size; i++)
                dst[i] = src[i] << 8;
        } else {
            memcpy(f->data[0], buffer, size);
        }
    } else { /* There's a hole */
        av_samples_set_silence(f->data, 0, f->nb_samples, f->channels, f->format);
    }

    if (!priv->delay)
        priv->delay = av_gettime_relative() - priv->epoch;

    /* Get PTS*/
    pa_usec_t pts;
    if (pa_stream_get_time(stream, &pts) == PA_OK) {
        f->pts = pts + priv->delay;
        int delay_neg;
        pa_usec_t delay;
        if (pa_stream_get_latency(stream, &delay, &delay_neg) == PA_OK)
            f->pts += delay_neg ? +((int64_t)delay) : -((int64_t)delay);
    } else {
        f->pts = AV_NOPTS_VALUE;
    }

    /* Copied, and pts calculated, we can drop the buffer now */
    pa_stream_drop(stream);

    int nb_samples = f->nb_samples;
    sp_log(iosys_entry, SP_LOG_TRACE, "Pushing frame to FIFO, pts = %f, len = %.2f ms\n",
           av_q2d(fe->time_base) * f->pts, (1000.0f * nb_samples) / f->sample_rate);
    int err = sp_frame_fifo_push(iosys_entry->frames, f);
    av_frame_free(&f);
    if (err == AVERROR(ENOBUFS)) {
        sp_log(iosys_entry, SP_LOG_WARN, "Dropping %i samples!\n", nb_samples);
    } else if (err) {
        sp_log(iosys_entry, SP_LOG_ERROR, "Unable to push frame to FIFO: %s!\n",
               av_err2str(err));
        /* Fatal error happens here */
    }
}

static void stream_status_cb(pa_stream *stream, void *data)
{
    IOSysEntry *iosys_entry = (IOSysEntry *)data;
    PulsePriv *priv = iosys_entry->api_priv;
    const pa_sample_spec *ss;
    const pa_channel_map *ch_map;
    char map_str[256];

    pa_stream_state_t state = pa_stream_get_state(stream);
    switch (state) {
    case PA_STREAM_CREATING:
        sp_log(iosys_entry, SP_LOG_TRACE, "Creating stream...\n");
        return;
    case PA_STREAM_READY:
        ss = pa_stream_get_sample_spec(stream);
        ch_map = pa_stream_get_channel_map(stream);
        av_get_channel_layout_string(map_str, sizeof(map_str), ss->channels, pa_to_lavu_ch_map(ch_map));
        sp_log(iosys_entry, SP_LOG_VERBOSE, "Stream ready, format: %iHz %s %ich %s\n",
               ss->rate, map_str, ss->channels, av_get_sample_fmt_name(format_map[ss->format].av_format));
        pa_threaded_mainloop_signal(priv->main->pa_mainloop, 0);
        sp_eventlist_dispatch(iosys_entry, iosys_entry->events, SP_EVENT_ON_CONFIG, NULL);
        return;
    case PA_STREAM_TERMINATED: /* Clean termination */
        return;
    case PA_STREAM_UNCONNECTED:
    case PA_STREAM_FAILED: /* Unclean termination */
        sp_log(iosys_entry, SP_LOG_ERROR, "Capture stream failed: %s!\n",
               pa_strerror(pa_context_errno(priv->main->pa_context)));
        return;
    default:
        sp_log(iosys_entry, SP_LOG_DEBUG, "Unhandled stream state %i\n", state);
        return;
    }
}

/* ffmpeg can only operate on native endian sample formats */
static enum pa_sample_format pulse_remap_to_useful[] = {
    [PA_SAMPLE_U8]        = PA_SAMPLE_U8,
    [PA_SAMPLE_ALAW]      = PA_SAMPLE_S16NE,
    [PA_SAMPLE_ULAW]      = PA_SAMPLE_S16NE,
    [PA_SAMPLE_S16LE]     = PA_SAMPLE_S16NE,
    [PA_SAMPLE_S16BE]     = PA_SAMPLE_S16NE,
    [PA_SAMPLE_FLOAT32LE] = PA_SAMPLE_FLOAT32NE,
    [PA_SAMPLE_FLOAT32BE] = PA_SAMPLE_FLOAT32NE,
    [PA_SAMPLE_S32LE]     = PA_SAMPLE_S32NE,
    [PA_SAMPLE_S32BE]     = PA_SAMPLE_S32NE,
    [PA_SAMPLE_S24LE]     = PA_SAMPLE_S24_32NE,
    [PA_SAMPLE_S24BE]     = PA_SAMPLE_S24_32NE,
    [PA_SAMPLE_S24_32LE]  = PA_SAMPLE_S24_32NE,
    [PA_SAMPLE_S24_32BE]  = PA_SAMPLE_S24_32NE,
};

static AVBufferRef *find_entry_by_sink_idx(AVBufferRef *test, void *opaque)
{
    PulsePriv *priv = (PulsePriv *)((IOSysEntry *)test->data)->api_priv;
    uint32_t idx = *((uint32_t *)opaque);
    if (priv->index == idx && priv->type == PULSE_SINK)
        return test;
    return NULL;
}

static AVBufferRef *find_entry_by_monitor_idx(AVBufferRef *test, void *opaque)
{
    PulsePriv *priv = (PulsePriv *)((IOSysEntry *)test->data)->api_priv;
    uint32_t idx = *((uint32_t *)opaque);
    if (priv->index == idx && priv->type == PULSE_SOURCE)
        return test;
    return NULL;
}

static int pulse_init_io(AVBufferRef *ctx_ref, AVBufferRef *entry, AVDictionary *opts)
{
    int err = 0;
    PulseCtx *ctx = (PulseCtx *)ctx_ref->data;

    pa_threaded_mainloop_lock(ctx->pa_mainloop);

    IOSysEntry *iosys_entry = (IOSysEntry *)entry->data;
    PulsePriv *priv = iosys_entry->api_priv;
    const char *target_name = sp_class_get_name(iosys_entry);

    int is_sink = sp_class_get_type(iosys_entry) & SP_TYPE_AUDIO_SINK;
    if (!is_sink)
        iosys_entry->frames = sp_frame_fifo_create(iosys_entry, 0, 0);
    else
        iosys_entry->frames = sp_frame_fifo_create(iosys_entry, 16, FRAME_FIFO_BLOCK_NO_INPUT);

    priv->main = (PulseCtx *)ctx_ref->data;

    pa_sample_spec req_ss = priv->ss;
    pa_channel_map req_map = priv->map;

    /* Filter out useless formats */
    req_ss.format = pulse_remap_to_useful[req_ss.format];

    /* We don't care about the rate as we'll have to resample ourselves anyway */
    if (req_ss.rate <= 0) {
        sp_log(ctx, SP_LOG_ERROR, "Source \"%s\" (id: %u) has invalid samplerate!\n",
               sp_class_get_name(iosys_entry), priv->index);
        err = AVERROR(EINVAL);
        goto fail;
    }

    /* Check for crazy layouts */
    uint64_t lavu_ch_map = pa_to_lavu_ch_map(&req_map);
    if (av_get_default_channel_layout(req_map.channels) != lavu_ch_map) {
        if (req_map.channels == 1)
            pa_channel_map_init_mono(&req_map);
        else
            pa_channel_map_init_stereo(&req_map);
    }

    priv->stream = pa_stream_new(ctx->pa_context, PROJECT_NAME, &req_ss, &req_map);
    if (!priv->stream) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to init stream: %s!\n",
               pa_strerror(pa_context_errno(ctx->pa_context)));
        err = AVERROR(EINVAL);
        goto fail;
    }

    /* Set the buffer size */
    pa_buffer_attr attr = { -1, -1, -1, -1, -1 };

    int64_t buffer_size = -1;
    const char *buffer_ms = dict_get(opts, "buffer_ms");
    if (buffer_ms && sp_is_number(buffer_ms))
        buffer_size = SPMAX(lrintf(strtof(buffer_ms, NULL) * 1000), UINT32_MAX);
    else if (!(buffer_ms && strcmp(buffer_ms, "default")))
        buffer_size = 320 * 1000; /* Divisible by frame sizes of both 1024 and 960 */

    if (buffer_size > 0)
        attr.fragsize = pa_usec_to_bytes((uint32_t)buffer_size, &req_ss);
    else
        attr.fragsize = -1;

    /* Set stream callbacks */
    pa_stream_set_state_callback(priv->stream, stream_status_cb, iosys_entry);
    pa_stream_set_read_callback(priv->stream, stream_read_cb, iosys_entry);

    if (priv->type == PULSE_SINK_INPUT) {
        /* First, find the sink to which the sink input is connected to */
        AVBufferRef *sink = sp_bufferlist_ref(ctx->entries, find_entry_by_sink_idx,
                                              &priv->master_sink_index);
        if (!sink) {
            sp_log(ctx, SP_LOG_ERROR, "Zombie sink input %s (0x%x, %i) is streaming to "
                   "a non-existent master sink %i!\n", sp_class_get_name(iosys_entry),
                   iosys_entry->identifier, iosys_entry->api_id, priv->master_sink_index);
            err = AVERROR(EINVAL);
            goto fail;
        }

        PulsePriv *priv_sink_ctx = (PulsePriv *)((IOSysEntry *)sink->data)->api_priv;
        uint32_t monitor_source = priv_sink_ctx->monitor_source;

        /* Find the monitor of the sink */
        AVBufferRef *monitor = sp_bufferlist_ref(ctx->entries, find_entry_by_monitor_idx,
                                                 &monitor_source);
        if (!monitor) {
            sp_log(ctx, SP_LOG_ERROR, "Sink has a non-existent monitor source!\n");
            err = AVERROR(EINVAL);
            av_buffer_unref(&sink);
            goto fail;
        }

        if (pa_stream_set_monitor_stream(priv->stream, priv->index) < 0) {
            sp_log(ctx, SP_LOG_ERROR, "pa_stream_set_monitor_stream() failed: %s!\n",
                   pa_strerror(pa_context_errno(ctx->pa_context)));
            err = AVERROR(EINVAL);
            av_buffer_unref(&sink);
            av_buffer_unref(&monitor);
            goto fail;
        }

        target_name = sp_class_get_name(monitor->data);
        av_buffer_unref(&sink);
        av_buffer_unref(&monitor);
    }

    /* Start stream */
    err = pa_stream_connect_record(priv->stream, target_name, &attr,
                                   PA_STREAM_ADJUST_LATENCY     |
                                   PA_STREAM_NOT_MONOTONIC      |
                                   PA_STREAM_AUTO_TIMING_UPDATE |
                                   PA_STREAM_INTERPOLATE_TIMING |
                                   PA_STREAM_DONT_MOVE          |
                                   PA_STREAM_START_CORKED       |
                                   PA_STREAM_NOFLAGS);
    if (err) {
        sp_log(ctx, SP_LOG_ERROR, "pa_stream_connect_record() failed: %s!\n",
               pa_strerror(pa_context_errno(ctx->pa_context)));
        err = AVERROR(EINVAL);
        goto fail;
    }

    /* Wait until the stream is ready */
    while (1) {
        int state = pa_stream_get_state(priv->stream);
        if (state == PA_STREAM_READY)
            break;
        if (!PA_STREAM_IS_GOOD(state)) {
            sp_log(ctx, SP_LOG_ERROR, "Stream configure failed: %s!\n",
                   pa_strerror(state));
            goto fail;
        }
        pa_threaded_mainloop_wait(ctx->pa_mainloop);
    }

    pa_threaded_mainloop_unlock(ctx->pa_mainloop);

    return 0;

fail:
    pa_threaded_mainloop_unlock(ctx->pa_mainloop);

    return err;
}

typedef struct PulseIOCtrlCtx {
    enum SPEventType ctrl;
    AVDictionary *opts;
    atomic_int_fast64_t *epoch;
} PulseIOCtrlCtx;

static void pulse_ioctx_ctrl_free(void *opaque, uint8_t *data)
{
    PulseIOCtrlCtx *event = (PulseIOCtrlCtx *)data;
    av_dict_free(&event->opts);
    av_free(data);
}

static int pulse_ioctx_ctrl_cb(AVBufferRef *opaque, void *src_ctx, void *data)
{
    PulseIOCtrlCtx *event = (PulseIOCtrlCtx *)opaque->data;

    IOSysEntry *iosys_entry = src_ctx;
    PulsePriv *priv = iosys_entry->api_priv;

    if (event->ctrl & SP_EVENT_CTRL_START) {
        pa_threaded_mainloop_lock(priv->main->pa_mainloop);
        priv->epoch = atomic_load(event->epoch);
        int ret = waitop(priv->main, pa_stream_cork(priv->stream, 0, stream_success_cb, priv));
        if (ret >= 0)
            sp_eventlist_dispatch(iosys_entry, iosys_entry->events, SP_EVENT_ON_INIT, NULL);
        return ret;
    } else if (event->ctrl & SP_EVENT_CTRL_STOP) {
        pa_threaded_mainloop_lock(priv->main->pa_mainloop);
        return waitop(priv->main, pa_stream_cork(priv->stream, 1, stream_success_cb, priv));
    } else {
        return AVERROR(ENOTSUP);
    }
}

static int pulse_ioctx_ctrl(AVBufferRef *entry, enum SPEventType ctrl, void *arg)
{
    IOSysEntry *iosys_entry = (IOSysEntry *)entry->data;

    if (ctrl & SP_EVENT_CTRL_COMMIT) {
        sp_log(iosys_entry, SP_LOG_DEBUG, "Comitting!\n");
        return sp_eventlist_dispatch(iosys_entry, iosys_entry->events, SP_EVENT_ON_COMMIT, NULL);
    } else if (ctrl & SP_EVENT_CTRL_DISCARD) {
        sp_log(iosys_entry, SP_LOG_DEBUG, "Discarding!\n");
        sp_eventlist_discard(iosys_entry->events);
    } else if (ctrl & SP_EVENT_CTRL_NEW_EVENT) {
        char *fstr = sp_event_flags_to_str_buf(arg);
        sp_log(iosys_entry, SP_LOG_DEBUG, "Registering new event (%s)!\n", fstr);
        av_free(fstr);
        return sp_eventlist_add(iosys_entry, iosys_entry->events, arg);
    } else if (ctrl & SP_EVENT_CTRL_DEP) {
        char *fstr = sp_event_flags_to_str(ctrl & ~SP_EVENT_CTRL_MASK);
        sp_log(iosys_entry, SP_LOG_DEBUG, "Registering new dependency (%s)!\n", fstr);
        av_free(fstr);
        return sp_eventlist_add_with_dep(iosys_entry, iosys_entry->events, arg, ctrl);
    } else if (ctrl & SP_EVENT_CTRL_OPTS) {
        AVDictionary *dict = arg;
        AVDictionaryEntry *dict_entry = NULL;
        while ((dict_entry = av_dict_get(dict, "", dict_entry, AV_DICT_IGNORE_SUFFIX))) {
            if (strcmp(dict_entry->key, "buffer_ms")) {
                sp_log(iosys_entry, SP_LOG_ERROR, "Option \"%s\" not found!\n", dict_entry->key);
                return AVERROR(EINVAL);
            }
        }
    } else if ((ctrl & SP_EVENT_CTRL_MASK) & ~(SP_EVENT_CTRL_START | SP_EVENT_CTRL_STOP)) {
        return AVERROR(ENOTSUP);
    }

    SP_EVENT_BUFFER_CTX_ALLOC(PulseIOCtrlCtx, ctrl_ctx, pulse_ioctx_ctrl_free, NULL)

    ctrl_ctx->ctrl = ctrl;
    if (ctrl & SP_EVENT_CTRL_OPTS)
        av_dict_copy(&ctrl_ctx->opts, arg, 0);
    if (ctrl & SP_EVENT_CTRL_START)
        ctrl_ctx->epoch = arg;

    if (ctrl & SP_EVENT_FLAG_IMMEDIATE) {
        int ret = pulse_ioctx_ctrl_cb(ctrl_ctx_ref, iosys_entry, NULL);
        av_buffer_unref(&ctrl_ctx_ref);
        return ret;
    }

    enum SPEventType flags = SP_EVENT_FLAG_ONESHOT | SP_EVENT_ON_COMMIT | ctrl;
    AVBufferRef *ctrl_event = sp_event_create(pulse_ioctx_ctrl_cb, NULL,
                                              flags, ctrl_ctx_ref,
                                              sp_event_gen_identifier(iosys_entry, NULL, flags));

    char *fstr = sp_event_flags_to_str_buf(ctrl_event);
    sp_log(iosys_entry, SP_LOG_DEBUG, "Registering new event (%s)!\n", fstr);
    av_free(fstr);

    int err = sp_eventlist_add(iosys_entry, iosys_entry->events, ctrl_event);
    av_buffer_unref(&ctrl_event);
    if (err < 0)
        return err;

    return 0;
}

static void destroy_entry(void *opaque, uint8_t *data)
{
    IOSysEntry *entry = (IOSysEntry *)data;
    PulsePriv *priv = entry->api_priv;
    PulseCtx *ctx = priv->main;

    if (priv->stream) {
        /* Drain it */
        pa_threaded_mainloop_lock(ctx->pa_mainloop);
        waitop(ctx, pa_stream_drain(priv->stream, stream_success_cb, priv));

        /* Disconnect */
        pa_threaded_mainloop_lock(ctx->pa_mainloop);
        pa_stream_disconnect(priv->stream);
        pa_threaded_mainloop_unlock(ctx->pa_mainloop);

        sp_log(entry, SP_LOG_VERBOSE, "Stream flushed and stopped!\n");

        priv->stream = NULL;
    }

    sp_frame_fifo_push(entry->frames, NULL);
    sp_frame_fifo_unmirror_all(entry->frames);
    av_buffer_unref(&entry->frames);

    sp_eventlist_dispatch(entry, entry->events, SP_EVENT_ON_DESTROY, entry);

    sp_bufferlist_free(&entry->events);
    av_buffer_pool_uninit(&priv->pool);

    av_free(priv);
    av_free(entry->desc);
    sp_class_free(entry);
    av_free(entry);
}

#define CALLBACK_BOILERPLATE(stype, entry_type)                                         \
    PulseCtx *ctx = data;                                                               \
    SPBufferList *list = ctx->entries;                                                  \
    if (eol)                                                                            \
        return;                                                                         \
                                                                                        \
    uint32_t idx = sp_iosys_gen_identifier(ctx, info->index, entry_type);               \
    IOSysEntry *entry;                                                                  \
    AVBufferRef *buf = sp_bufferlist_ref(list, sp_bufferlist_iosysentry_by_id, &idx);   \
    const int new_entry = !buf;                                                         \
    if (!buf) {                                                                         \
        entry = av_mallocz(sizeof(*entry));                                             \
        entry->api_priv = av_mallocz(sizeof(PulsePriv));                                \
        buf = av_buffer_create((uint8_t *)entry, sizeof(*entry),                        \
                               destroy_entry, ctx, 0);                                  \
    }                                                                                   \
                                                                                        \
    entry = (IOSysEntry *)buf->data;                                                    \
    PulsePriv *priv = (PulsePriv *)entry->api_priv;                                     \
                                                                                        \
    if (!new_entry) {                                                                   \
        int nam = strcmp(sp_class_get_name(entry), info->name); /* Name changes */      \
        int ss = memcmp(&priv->ss, &info->sample_spec,                                  \
                        sizeof(pa_sample_spec)); /* Sample fmt */                       \
        int map = memcmp(&priv->map, &info->channel_map,                                \
                         sizeof(pa_channel_map)); /* Channel map */                     \
        int lvl = (nam | ss | map) ? SP_LOG_DEBUG : SP_LOG_TRACE;                       \
        int other = !nam && !ss && !map;                                                \
        sp_log(ctx, lvl, "Updating " stype " %s (id: 0x%x) %s%s%s%s%s%s%s%s%s%s%s%s\n", \
               info->name, idx,                                                         \
               nam ? "(" : "", nam ?      "name" : "", nam ? ") " : "",                 \
               ss ?  "(" : "",  ss ? "samplefmt" : "",  ss ? ") " : "",                 \
               map ? "(" : "", map ?       "map" : "", map ? ") " : "",                 \
               other ? "(" : "", other ? "misc" : "", other ? ") " : "");               \
        av_free(entry->desc);                                                           \
    } else {                                                                            \
        sp_log(ctx, SP_LOG_DEBUG, "Adding new " stype " %s (id: 0x%x)\n",               \
               info->name, idx);                                                        \
                                                                                        \
        entry->events = sp_bufferlist_new();                                            \
        entry->ctrl = pulse_ioctx_ctrl;                                                 \
                                                                                        \
        sp_class_alloc(entry, NULL,                                                     \
                       entry_type == PULSE_SINK ?                                       \
                       SP_TYPE_AUDIO_BIDIR : SP_TYPE_AUDIO_SOURCE, ctx);                \
    }                                                                                   \
                                                                                        \
    priv->type = entry_type;                                                            \
    priv->ss = info->sample_spec;                                                       \
    priv->map = info->channel_map;                                                      \
                                                                                        \
    entry->identifier = idx;                                                            \
    entry->api_id = info->index;                                                        \
    entry->api = &src_pulse;                                                            \
    sp_class_set_name(entry, info->name);                                               \
    entry->sample_rate = priv->ss.rate;                                                 \
    entry->channels = priv->ss.channels;                                                \
    entry->channel_layout = pa_to_lavu_ch_map(&priv->map);                              \
    entry->sample_fmt = format_map[pulse_remap_to_useful[priv->ss.format]].av_format;   \
    entry->volume = pa_sw_volume_to_linear(info->volume.values[0]);

#define CALLBACK_FOOTER(entry_type)                                                     \
    sp_eventlist_dispatch(entry, ctx->events,                                           \
                          SP_EVENT_ON_CHANGE | SP_EVENT_TYPE_SOURCE |                   \
                          (entry_type == PULSE_SINK ? SP_EVENT_TYPE_SINK : 0),          \
                          entry);                                                       \
    if (new_entry)                                                                      \
        sp_bufferlist_append_noref(list, buf);                                          \
    else                                                                                \
        av_buffer_unref(&buf);

static void sink_cb(pa_context *context, const pa_sink_info *info,
                    int eol, void *data)
{
    CALLBACK_BOILERPLATE("sink", PULSE_SINK)
    entry->desc = av_strdup(info->description);
    priv->monitor_source = info->monitor_source;
    CALLBACK_FOOTER(PULSE_SINK)
}

static void source_cb(pa_context *context, const pa_source_info *info,
                      int eol, void *data)
{
    CALLBACK_BOILERPLATE("source", PULSE_SOURCE)
    entry->desc = av_strdup(info->description);
    CALLBACK_FOOTER(PULSE_SOURCE)
}

static void sink_input_cb(pa_context *context, const pa_sink_input_info *info,
                          int eol, void *data)
{
    CALLBACK_BOILERPLATE("sink input", PULSE_SINK_INPUT)
    entry->desc = av_strdup("sink input");
    priv->master_sink_index = info->sink;
    CALLBACK_FOOTER(PULSE_SINK_INPUT)
}

static void subscribe_cb(pa_context *context, pa_subscription_event_type_t type,
                         uint32_t index, void *data)
{
    pa_operation *o;
    PulseCtx *ctx = data;
    pa_subscription_event_type_t facility = type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    pa_subscription_event_type_t event = type & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

#define MONITOR_TEMPL(event_type, hook_fn, callback, stype, type)                           \
    case PA_SUBSCRIPTION_EVENT_ ## event_type:                                              \
        if (event & PA_SUBSCRIPTION_EVENT_REMOVE) {                                         \
            uint32_t nidx = sp_iosys_gen_identifier(ctx, index, type);                      \
            AVBufferRef *ref = sp_bufferlist_pop(ctx->entries,                              \
                                                 sp_bufferlist_iosysentry_by_id,            \
                                                 &nidx);                                    \
            if (!ref) {                                                                     \
                sp_log(ctx, SP_LOG_ERROR, stype " id 0x%x not found!\n", nidx);             \
                return;                                                                     \
            }                                                                               \
            av_buffer_unref(&ref);                                                          \
        } else {                                                                            \
            if (!(o = pa_context_get_ ## hook_fn(context, index, callback, ctx))) {         \
                sp_log(ctx, SP_LOG_ERROR, "pa_context_get_" #hook_fn "() failed "           \
                       "for id %u\n", index);                                               \
                return;                                                                     \
            }                                                                               \
            pa_operation_unref(o);                                                          \
        }                                                                                   \
        break;

    switch (facility) {
    MONITOR_TEMPL(SINK, sink_info_by_index, sink_cb, "sink", PULSE_SINK)
    MONITOR_TEMPL(SOURCE, source_info_by_index, source_cb, "source", PULSE_SOURCE)
    MONITOR_TEMPL(SINK_INPUT, sink_input_info, sink_input_cb, "sink input", PULSE_SINK_INPUT)
    default:
        break;
    };

#undef MONITOR_TEMPL
}

static void pulse_state_cb(pa_context *context, void *data)
{
    PulseCtx *ctx = data;

    switch (pa_context_get_state(context)) {
    case PA_CONTEXT_UNCONNECTED:
        sp_log(ctx, SP_LOG_ERROR, "PulseAudio reports it is unconnected!\n");
        break;
    case PA_CONTEXT_CONNECTING:
        sp_log(ctx, SP_LOG_DEBUG, "Connecting to PulseAudio!\n");
        break;
    case PA_CONTEXT_AUTHORIZING:
        sp_log(ctx, SP_LOG_TRACE, "Authorizing PulseAudio connection!\n");
        break;
    case PA_CONTEXT_SETTING_NAME:
        sp_log(ctx, SP_LOG_TRACE, "Sending client name!\n");
        break;
    case PA_CONTEXT_FAILED:
        sp_log(ctx, SP_LOG_ERROR, "PulseAudio connection failed: %s!\n",
               pa_strerror(pa_context_errno(context)));
        pa_context_unref(context);
        break;
    case PA_CONTEXT_TERMINATED:
        sp_log(ctx, SP_LOG_VERBOSE, "PulseAudio connection terminated!\n");
        break;
    case PA_CONTEXT_READY:
        sp_log(ctx, SP_LOG_VERBOSE, "PulseAudio connection ready, arming callbacks!\n");
        pa_operation *o;

        /* Subscribe for new updates */
        pa_context_set_subscribe_callback(context, subscribe_cb, ctx);
        if (!(o = pa_context_subscribe(context, (pa_subscription_mask_t)
                                                (PA_SUBSCRIPTION_MASK_SINK       |
                                                 PA_SUBSCRIPTION_MASK_SOURCE     |
                                                 PA_SUBSCRIPTION_MASK_SINK_INPUT), NULL, NULL))) {
            sp_log(ctx, SP_LOG_ERROR, "pa_context_subscribe() failed: %s\n",
                   pa_strerror(pa_context_errno(context)));
            return;
        }
        pa_operation_unref(o);

#define LOAD_INITIAL(hook_fn, callback)                        \
    if (!(o = hook_fn(context, callback, ctx))) {              \
        sp_log(ctx, SP_LOG_ERROR, #hook_fn "() failed: %s!\n", \
               pa_strerror(pa_context_errno(context)));        \
        return;                                                \
    }                                                          \
    pa_operation_unref(o);

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

static int pulse_ctrl(AVBufferRef *ctx_ref, enum SPEventType ctrl, void *arg)
{
    int err = 0;
    PulseCtx *ctx = (PulseCtx *)ctx_ref->data;

    if (ctrl & SP_EVENT_CTRL_NEW_EVENT) {
        AVBufferRef *event = arg;
        char *fstr = sp_event_flags_to_str_buf(event);
        sp_log(ctx, SP_LOG_DEBUG, "Registering new event (%s)!\n", fstr);
        av_free(fstr);

        if (ctrl & SP_EVENT_FLAG_IMMEDIATE) {
            /* Somewhat of a hack to get the thread going first time to get the list */
            pa_threaded_mainloop_lock(ctx->pa_mainloop);
            waitop(ctx, pa_context_get_server_info(ctx->pa_context, server_info_cb, ctx));

            /* Bring up the new event to speed with current affairs */
            SPBufferList *tmp_event = sp_bufferlist_new();
            sp_eventlist_add(ctx, tmp_event, event);

            AVBufferRef *obj = NULL;
            while ((obj = sp_bufferlist_iter_ref(ctx->entries))) {
                IOSysEntry *entry = (IOSysEntry *)obj->data;
                int is_sink = sp_class_get_type(entry) & SP_TYPE_AUDIO_SINK;
                const char *def = is_sink ? ctx->default_sink_name : ctx->default_source_name;

                entry->is_default = !strcmp(sp_class_get_name(entry), def);

                sp_eventlist_dispatch(entry, tmp_event,
                                      SP_EVENT_ON_CHANGE | SP_EVENT_TYPE_SOURCE |
                                      (is_sink ? SP_EVENT_TYPE_SINK : 0),
                                      entry);
                av_buffer_unref(&obj);
            }

            sp_bufferlist_free(&tmp_event);
        }

        /* Add it to the list now to receive events dynamically */
        err = sp_eventlist_add(ctx, ctx->events, event);
        if (err < 0)
            return err;
    }

    return 0;
}

static AVBufferRef *pulse_ref_entry(AVBufferRef *ctx_ref, uint32_t identifier)
{
    PulseCtx *ctx = (PulseCtx *)ctx_ref->data;
    return sp_bufferlist_ref(ctx->entries, sp_bufferlist_iosysentry_by_id, &identifier);
}

static void pulse_uninit(void *opaque, uint8_t *data)
{
    PulseCtx *ctx = (PulseCtx *)data;

    if (ctx->pa_mainloop)
        pa_threaded_mainloop_stop(ctx->pa_mainloop);

    sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_DESTROY, ctx);
    sp_bufferlist_free(&ctx->events);

    if (ctx->pa_context) {
        pa_context_disconnect(ctx->pa_context);
        pa_context_unref(ctx->pa_context);
        ctx->pa_context = NULL;
    }

    sp_bufferlist_free(&ctx->entries);

    if (ctx->pa_mainloop) {
        pa_threaded_mainloop_free(ctx->pa_mainloop);
        ctx->pa_mainloop = NULL;
    }

    av_freep(&ctx->default_sink_name);
    av_freep(&ctx->default_source_name);
    sp_class_free(ctx);
    av_free(ctx);
}

static int pulse_init(AVBufferRef **s)
{
    int err = 0, locked = 0;
    PulseCtx *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return AVERROR(ENOMEM);

    AVBufferRef *ctx_ref = av_buffer_create((uint8_t *)ctx, sizeof(*ctx),
                                            pulse_uninit, NULL, 0);
    if (!ctx_ref) {
        av_free(ctx);
        return AVERROR(ENOMEM);
    }

    ctx->entries = sp_bufferlist_new();
    if (!ctx->entries) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    ctx->events = sp_bufferlist_new();
    if (!ctx->events) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    err = sp_class_alloc(ctx, src_pulse.name, SP_TYPE_CONTEXT, NULL);
    if (err < 0)
        goto fail;

    ctx->pa_mainloop = pa_threaded_mainloop_new();
    pa_threaded_mainloop_start(ctx->pa_mainloop);
    pa_threaded_mainloop_set_name(ctx->pa_mainloop, sp_class_get_name(ctx));

    pa_threaded_mainloop_lock(ctx->pa_mainloop);
    locked = 1;

    ctx->pa_mainloop_api = pa_threaded_mainloop_get_api(ctx->pa_mainloop);

    pa_proplist *proplist = pa_proplist_new();
    if (!proplist) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    ctx->pa_context = pa_context_new_with_proplist(ctx->pa_mainloop_api,
                                                   PROJECT_NAME, proplist);
    pa_proplist_free(proplist);
    if (!ctx->pa_context) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to create pulseaudio context!\n");
        err = AVERROR_EXTERNAL;
        goto fail;
    }

    pa_context_set_state_callback(ctx->pa_context, pulse_state_cb, ctx);
    pa_context_connect(ctx->pa_context, NULL, PA_CONTEXT_NOFLAGS, NULL);

    /* Wait until the context is ready */
    while (1) {
        int state = pa_context_get_state(ctx->pa_context);
        if (state == PA_CONTEXT_READY)
            break;
        if (!PA_CONTEXT_IS_GOOD(state)) {
            err = AVERROR_EXTERNAL;
            goto fail;
        }

        pa_threaded_mainloop_wait(ctx->pa_mainloop);
    }

    pa_threaded_mainloop_unlock(ctx->pa_mainloop);
    locked = 0;

    *s = ctx_ref;

    return 0;

fail:
    if (locked)
        pa_threaded_mainloop_unlock(ctx->pa_mainloop);

    av_buffer_unref(&ctx_ref);

    return err;
}

const IOSysAPI src_pulse = {
    .name      = "pulse",
    .ctrl      = pulse_ctrl,
    .init_sys  = pulse_init,
    .ref_entry = pulse_ref_entry,
    .init_io   = pulse_init_io,
};
