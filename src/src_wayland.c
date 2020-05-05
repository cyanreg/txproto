#define _GNU_SOURCE
#include <unistd.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <poll.h>
#include <fcntl.h>

#include <libdrm/drm_fourcc.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/bprint.h>
#include <libavutil/buffer.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/time.h>

#include "wlr-export-dmabuf-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"

#include "src_common.h"
#include "utils.h"
#include "../config.h"

struct wayland_output {
    struct wl_list link;
    uint32_t id;
    struct wl_output *output;
    char *make;
    char *model;
    int width;
    int height;
    AVRational framerate;
};

typedef struct WaylandCtx {
    AVClass *class;

    int64_t epoch;

    pthread_t event_thread;

    struct wl_display *display;
    struct wl_registry *registry;

    struct zwlr_export_dmabuf_manager_v1 *dmabuf_export_manager;
    struct zwlr_screencopy_manager_v1 *screencopy_export_manager;
    struct wl_shm *shm_interface;

    AVBufferRef *drm_device_ref;

    struct wl_list output_list;
    SourceInfo *sources;
    int num_sources;

    struct WaylandCaptureCtx **capture_ctx;
    int capture_ctx_num;

    int wakeup_pipe[2];
} WaylandCtx;

typedef struct WaylandCaptureCtx {
    WaylandCtx *main;
    uint64_t identifier;
    struct wl_output *target;
    SPFrameFIFO *fifo;

    /* Stats */
    int dropped_frames;
    int dropped_frames_msg_state;

    /* Framerate limiting */
    AVRational frame_rate;
    int64_t next_frame_ts;
    int64_t frame_delay;

    /* Error handling */
    error_handler *error_handler;
    void *error_handler_ctx;

    /* Capture options */
    int capture_cursor;
    int use_screencopy;

    /* Frame being signalled */
    AVFrame *frame;

    /* To shut down cleanly */
    pthread_mutex_t frame_obj_lock;

    /* DMABUF stuff */
    struct {
        struct zwlr_export_dmabuf_frame_v1 *frame_obj;
        AVBufferRef *frames_ref;
    } dmabuf;

    /* Screencopy stuff */
    struct {
        struct zwlr_screencopy_frame_v1 *frame_obj;
        AVBufferPool *pool;
        uint32_t stride;
        uint32_t format;
        int width;
        int height;
    } scrcpy;
} WaylandCaptureCtx;

FN_CREATING(WaylandCtx, WaylandCaptureCtx, capture_ctx, capture_ctx, capture_ctx_num)

static void output_handle_geometry(void *data, struct wl_output *wl_output,
                                   int32_t x, int32_t y, int32_t phys_width,
                                   int32_t phys_height, int32_t subpixel,
                                   const char *make, const char *model,
                                   int32_t transform)
{
    struct wayland_output *output = data;
    output->make = av_strdup(make);
    output->model = av_strdup(model);
}

static void output_handle_mode(void *data, struct wl_output *wl_output,
                               uint32_t flags, int32_t width, int32_t height,
                               int32_t refresh)
{
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        struct wayland_output *output = data;
        output->width = width;
        output->height = height;
        output->framerate = (AVRational){ refresh, 1000 };
    }
}

static void output_handle_done(void *data, struct wl_output *wl_output)
{
    /* Nothing to do */
}

static void output_handle_scale(void *data, struct wl_output *wl_output,
                                int32_t factor)
{
    /* Nothing to do */
}

static const struct wl_output_listener output_listener = {
    .geometry = output_handle_geometry,
    .mode = output_handle_mode,
    .done = output_handle_done,
    .scale = output_handle_scale,
};

static void registry_handle_add(void *data, struct wl_registry *reg,
                                uint32_t id, const char *interface,
                                uint32_t ver)
{
    WaylandCtx *ctx = data;

    if (!strcmp(interface, wl_output_interface.name)) {
        struct wayland_output *output = av_mallocz(sizeof(*output));

        output->id = id;
        output->output = wl_registry_bind(reg, id, &wl_output_interface, 1);

        wl_output_add_listener(output->output, &output_listener, output);
        wl_list_insert(&ctx->output_list, &output->link);
    }

    if (!strcmp(interface, wl_shm_interface.name)) {
        const struct wl_interface *i = &wl_shm_interface;
        ctx->shm_interface = wl_registry_bind(reg, id, i, 1);
	}

    if (!strcmp(interface, zwlr_export_dmabuf_manager_v1_interface.name)) {
        const struct wl_interface *i = &zwlr_export_dmabuf_manager_v1_interface;
        ctx->dmabuf_export_manager = wl_registry_bind(reg, id, i, 1);
	}

    if (!strcmp(interface, zwlr_screencopy_manager_v1_interface.name)) {
        const struct wl_interface *i = &zwlr_screencopy_manager_v1_interface;
        ctx->screencopy_export_manager = wl_registry_bind(reg, id, i, 1);
    }
}

static void remove_output(struct wayland_output *out)
{
    wl_list_remove(&out->link);
    av_free(out->make);
    av_free(out->model);
    av_free(out);
}

static struct wayland_output *find_output(WaylandCtx *ctx,
                                          struct wl_output *out, uint32_t id)
{
    struct wayland_output *output, *tmp;
    wl_list_for_each_safe(output, tmp, &ctx->output_list, link)
        if ((output->output == out) || (output->id == id))
            return output;
    return NULL;
}

static void registry_handle_remove(void *data, struct wl_registry *reg,
                                   uint32_t id)
{
    WaylandCtx *ctx = data;
    struct wayland_output *output = find_output(ctx, NULL, id);
    src_wayland.stop(ctx, id);
    remove_output(output);
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_add,
    .global_remove = registry_handle_remove,
};

static void dmabuf_frame_free(void *opaque, uint8_t *data)
{
    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)data;

    for (int i = 0; i < desc->nb_objects; ++i)
        close(desc->objects[i].fd);

    if (opaque)
        zwlr_export_dmabuf_frame_v1_destroy(opaque);

    av_free(data);
}

static void dmabuf_register_cb(WaylandCaptureCtx *ctx);

static void dmabuf_frame_start(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
                               uint32_t width, uint32_t height, uint32_t offset_x,
                               uint32_t offset_y, uint32_t buffer_flags, uint32_t flags,
                               uint32_t format, uint32_t mod_high, uint32_t mod_low,
                               uint32_t num_objects)
{
    int err = 0;
    WaylandCaptureCtx *ctx = data;
    pthread_mutex_lock(&ctx->frame_obj_lock);

    /* Allocate DRM specific struct */
    AVDRMFrameDescriptor *desc = av_mallocz(sizeof(*desc));
    if (!desc) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    desc->nb_objects = num_objects;
    desc->objects[0].format_modifier = ((uint64_t)mod_high << 32) | mod_low;

    desc->nb_layers = 1;
    desc->layers[0].format = format;
    desc->layers[0].nb_planes = 0;

    /* Allocate a frame */
    ctx->frame = av_frame_alloc();
    if (!ctx->frame) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    /* Set base frame properties */
    ctx->frame->width      = width;
    ctx->frame->height     = height;
    ctx->frame->format     = AV_PIX_FMT_DRM_PRIME;
    ctx->frame->sample_aspect_ratio = av_make_q(1, 1);
    ctx->frame->opaque_ref = av_buffer_allocz(sizeof(FormatExtraData));

    /* Set the frame data to the DRM specific struct */
    ctx->frame->buf[0] = av_buffer_create((uint8_t*)desc, sizeof(*desc),
                                          &dmabuf_frame_free, frame, 0);
    if (!ctx->frame->buf[0]) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    ctx->frame->data[0] = (uint8_t*)desc;

    pthread_mutex_unlock(&ctx->frame_obj_lock);

    return;

fail:
    dmabuf_frame_free(frame, (uint8_t *)desc);
    ctx->dmabuf.frame_obj = NULL;

    if (ctx->error_handler)
        ctx->error_handler(ctx->error_handler_ctx, err);

    pthread_mutex_unlock(&ctx->frame_obj_lock);
}

static void dmabuf_frame_object(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
                                uint32_t index, int32_t fd, uint32_t size,
                                uint32_t offset, uint32_t stride, uint32_t plane_index)
{
    WaylandCaptureCtx *ctx = data;
    pthread_mutex_lock(&ctx->frame_obj_lock);

    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)ctx->frame->data[0];

    desc->objects[index].fd = fd;
    desc->objects[index].size = size;

    desc->layers[0].planes[plane_index].object_index = index;
    desc->layers[0].planes[plane_index].offset = offset;
    desc->layers[0].planes[plane_index].pitch = stride;

    desc->layers[0].nb_planes = FFMAX(desc->layers[0].nb_planes, plane_index + 1);

    pthread_mutex_unlock(&ctx->frame_obj_lock);
}

static void frame_set_colorspace(AVFrame *f, enum AVPixelFormat format)
{
    /* sRGB uses the 709 primaries with the AVCOL_TRC_IEC61966_2_1 transfer
     * function.
     * Only valid for standard displays, but that's what most are */
    f->color_primaries = AVCOL_PRI_BT709;
    f->colorspace      = AVCOL_SPC_BT709;
    f->color_range     = AVCOL_RANGE_JPEG;
    f->color_trc       = AVCOL_TRC_IEC61966_2_1;
    f->color_trc       = AVCOL_TRC_IEC61966_2_1;
}

static enum AVPixelFormat drm_wl_fmt_to_pixfmt(enum wl_shm_format drm_format,
                                               enum wl_shm_format wl_format)
{
    #define FMTDBL(fmt) DRM_FORMAT_##fmt, WL_SHM_FORMAT_##fmt
    static const struct {
        enum wl_shm_format src_drm;
        enum wl_shm_format src_wl;
        enum AVPixelFormat dst;
    } format_map[] = {
        { FMTDBL(XRGB8888), AV_PIX_FMT_BGR0 },
        { FMTDBL(ARGB8888), AV_PIX_FMT_BGR0 }, /* We lie here */
        { FMTDBL(XBGR8888), AV_PIX_FMT_RGB0 },
        { FMTDBL(ABGR8888), AV_PIX_FMT_RGBA },
    };
    #undef FMTDBL

    for (int i = 0; i < FF_ARRAY_ELEMS(format_map); i++) {
        if ((drm_format != UINT32_MAX) && format_map[i].src_drm == drm_format)
            return format_map[i].dst;
        if ((wl_format != UINT32_MAX) && format_map[i].src_wl == wl_format)
            return format_map[i].dst;
    }

    return AV_PIX_FMT_NONE;
}

static int attach_drm_frames_ref(WaylandCaptureCtx *ctx, AVFrame *f,
                                 enum AVPixelFormat sw_format)
{
    int err = 0;
    AVHWFramesContext *hwfc;

    if (ctx->dmabuf.frames_ref) {
        hwfc = (AVHWFramesContext*)ctx->dmabuf.frames_ref->data;
        if (hwfc->width == f->width && hwfc->height == f->height &&
            hwfc->sw_format == sw_format) {
            goto attach;
        }
        av_buffer_unref(&ctx->dmabuf.frames_ref);
    }

    ctx->dmabuf.frames_ref = av_hwframe_ctx_alloc(ctx->main->drm_device_ref);
    if (!ctx->dmabuf.frames_ref) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    hwfc = (AVHWFramesContext*)ctx->dmabuf.frames_ref->data;

    hwfc->format = f->format;
    hwfc->sw_format = sw_format;
    hwfc->width = f->width;
    hwfc->height = f->height;

    err = av_hwframe_ctx_init(ctx->dmabuf.frames_ref);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "AVHWFramesContext init failed: %s!\n",
               av_err2str(err));
        goto fail;
    }

attach:
    /* Set frame hardware context referencce */
    f->hw_frames_ctx = av_buffer_ref(ctx->dmabuf.frames_ref);
    if (!f->hw_frames_ctx) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    return 0;

fail:
    av_buffer_unref(&ctx->dmabuf.frames_ref);

    return err;
}

static void dmabuf_frame_ready(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
                               uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec)
{
    int64_t err = 0;
    WaylandCaptureCtx *ctx = data;
    pthread_mutex_lock(&ctx->frame_obj_lock);

    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)ctx->frame->data[0];
    enum AVPixelFormat sw_fmt = drm_wl_fmt_to_pixfmt(desc->layers[0].format, UINT32_MAX);

    if (sw_fmt == AV_PIX_FMT_NONE) {
        av_log(ctx->main, AV_LOG_ERROR, "Unsupported DMABUF format!\n");
        err = AVERROR(ENOTSUP);
        goto fail;
    }

    /* Set the colorspace */
    frame_set_colorspace(ctx->frame, sw_fmt);

    /* Opaque ref */
    FormatExtraData *fe = (FormatExtraData *)ctx->frame->opaque_ref->data;
    fe->time_base       = av_make_q(1, 1000000000);
    fe->avg_frame_rate  = ctx->frame_rate;

    /* Timestamp of when the frame will be presented */
    int64_t presented = ((((uint64_t)tv_sec_hi) << 32) | tv_sec_lo) * 1000000000 + tv_nsec;

    /* Current time */
    struct timespec tsp = { 0 };
    clock_gettime(CLOCK_MONOTONIC, &tsp);

    /* Delay */
    int64_t delay = presented - ((tsp.tv_sec * 1000000000) + tsp.tv_nsec);

    ctx->frame->pts = av_add_stable(fe->time_base, delay, av_make_q(1, 1000000),
                                    av_gettime_relative() - ctx->main->epoch);

    /* The timestamp we're given is when the scanout began at, not when it was
     * presented, so offset that */
    ctx->frame->pts = av_add_stable(fe->time_base, ctx->frame->pts,
                                    AV_TIME_BASE_Q, ctx->frame_delay);

	/* Attach the hardware frame context to the frame */
    if ((err = attach_drm_frames_ref(ctx, ctx->frame, sw_fmt)))
        goto fail;

    /* We don't do this check at the start on since there's still some chance
     * whatever's consuming the FIFO will be done by now. */
    if (ctx->fifo && sp_frame_fifo_is_full(ctx->fifo)) {
        ctx->dropped_frames++;
        av_log_once(ctx->main, AV_LOG_WARNING, AV_LOG_DEBUG, &ctx->dropped_frames_msg_state,
                    "Dropping a frame, queue is full (%i dropped)!\n", ctx->dropped_frames);
        av_frame_free(&ctx->frame);
        ctx->dmabuf.frame_obj = NULL;
    } else if (ctx->fifo) {
        if (sp_frame_fifo_push(ctx->fifo, ctx->frame)) {
            av_log(ctx->main, AV_LOG_ERROR, "Unable to push frame to FIFO!\n");
            err = AVERROR(ENOMEM);
            goto fail;
        }
        ctx->dropped_frames_msg_state = 0;
        ctx->frame = NULL;
    } else {
        av_frame_free(&ctx->frame);
        ctx->dmabuf.frame_obj = NULL;
    }

    /* Framerate limiting */
    if (ctx->frame_delay) {
        int64_t now = av_gettime_relative();
        if (ctx->next_frame_ts && (now < ctx->next_frame_ts)) {
            int64_t delay;
            while (1) {
                delay = ctx->next_frame_ts - now;
                if (delay <= 0)
                    break;
                av_usleep(delay);
                now = av_gettime_relative();
            }
        }
        ctx->next_frame_ts = now + ctx->frame_delay;
    }

    dmabuf_register_cb(ctx);

    pthread_mutex_unlock(&ctx->frame_obj_lock);

    return;

fail:
    av_frame_free(&ctx->frame);
    ctx->dmabuf.frame_obj = NULL;

    if (ctx->error_handler)
        ctx->error_handler(ctx->error_handler_ctx, err);

    pthread_mutex_unlock(&ctx->frame_obj_lock);
}

static void dmabuf_frame_cancel(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
                                uint32_t reason)
{
    WaylandCaptureCtx *ctx = data;
    pthread_mutex_lock(&ctx->frame_obj_lock);

    av_log(ctx->main, AV_LOG_WARNING, "Frame cancelled!\n");
    av_frame_free(&ctx->frame);
    ctx->dmabuf.frame_obj = NULL;
    if (reason == ZWLR_EXPORT_DMABUF_FRAME_V1_CANCEL_REASON_PERMANENT) {
        av_log(ctx->main, AV_LOG_ERROR, "Permanent failure, stopping capture!\n");
        av_buffer_unref(&ctx->dmabuf.frames_ref);
        if (ctx->error_handler)
            ctx->error_handler(ctx->error_handler_ctx, AVERROR_EXTERNAL);
    } else {
        dmabuf_register_cb(ctx);
    }

    pthread_mutex_unlock(&ctx->frame_obj_lock);
}

static const struct zwlr_export_dmabuf_frame_v1_listener dmabuf_frame_listener = {
	.frame  = dmabuf_frame_start,
	.object = dmabuf_frame_object,
	.ready  = dmabuf_frame_ready,
	.cancel = dmabuf_frame_cancel,
};

static void dmabuf_register_cb(WaylandCaptureCtx *ctx)
{
    struct zwlr_export_dmabuf_frame_v1 *f;
    f = zwlr_export_dmabuf_manager_v1_capture_output(ctx->main->dmabuf_export_manager,
                                                     ctx->capture_cursor, ctx->target);
    zwlr_export_dmabuf_frame_v1_add_listener(f, &dmabuf_frame_listener, ctx);
    ctx->dmabuf.frame_obj = f;
}

typedef struct WaylandCopyFrame {
    struct wl_buffer *buffer;
    void *data;
    size_t size;
} WaylandCopyFrame;

static void scrcpy_frame_free(void *opaque, uint8_t *data)
{
    WaylandCopyFrame *f = (WaylandCopyFrame *)data;
    wl_buffer_destroy(f->buffer);
	munmap(f->data, f->size);
	av_free(f);
}

static AVBufferRef *shm_pool_alloc(void *opaque, int size)
{
    WaylandCaptureCtx *ctx = opaque;
    WaylandCopyFrame *f = av_malloc(sizeof(WaylandCopyFrame));

    f->size = ctx->scrcpy.stride * ctx->scrcpy.height;

    char name[255];
    snprintf(name, sizeof(name), PROJECT_NAME "_%ix%i_s%i_f0x%x", ctx->scrcpy.width,
             ctx->scrcpy.height, ctx->scrcpy.stride, ctx->scrcpy.format);

    int fd = memfd_create(name, 0);

    int ret;
    while ((ret = ftruncate(fd, f->size)) == EINTR) {
        // No-op
    }
    if (ret < 0) {
        close(fd);
        av_log(ctx->main, AV_LOG_ERROR, "ftruncate failed!\n");
        return NULL;
    }

    f->data = mmap(NULL, f->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (f->data == MAP_FAILED) {
        av_log(ctx->main, AV_LOG_ERROR, "mmap failed!\n");
        close(fd);
        return NULL;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(ctx->main->shm_interface, fd, f->size);
    close(fd);
    f->buffer = wl_shm_pool_create_buffer(pool, 0, ctx->scrcpy.width,
                                          ctx->scrcpy.height,
                                          ctx->scrcpy.stride,
                                          ctx->scrcpy.format);
    wl_shm_pool_destroy(pool);

    return av_buffer_create((uint8_t *)f, sizeof(WaylandCopyFrame),
                            scrcpy_frame_free, NULL, 0);
}

static void scrcpy_register_cb(WaylandCaptureCtx *ctx);

static void scrcpy_give_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
                               enum wl_shm_format format, uint32_t width, uint32_t height,
                               uint32_t stride)
{
    int err;
    WaylandCaptureCtx *ctx = data;
    pthread_mutex_lock(&ctx->frame_obj_lock);
    enum AVPixelFormat pix_fmt = drm_wl_fmt_to_pixfmt(UINT32_MAX, format);

    if ((stride != ctx->scrcpy.stride) || (height != ctx->scrcpy.height) ||
        (format != ctx->scrcpy.format) || (width  != ctx->scrcpy.width)) {
        av_buffer_pool_uninit(&ctx->scrcpy.pool);

        ctx->scrcpy.width  = width;
        ctx->scrcpy.height = height;
        ctx->scrcpy.stride = stride;
        ctx->scrcpy.format = format;

        ctx->scrcpy.pool = av_buffer_pool_init2(sizeof(WaylandCopyFrame), ctx,
                                                shm_pool_alloc, NULL);

        if (!ctx->scrcpy.pool) {
            err = AVERROR(ENOMEM);
            goto fail;
        }
    }

    ctx->frame = av_frame_alloc();
    if (!ctx->frame) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    ctx->frame->width = width;
    ctx->frame->height = height;
    ctx->frame->linesize[0] = stride;
    ctx->frame->opaque_ref = av_buffer_allocz(sizeof(FormatExtraData));
    ctx->frame->buf[0] = av_buffer_pool_get(ctx->scrcpy.pool);
    if (!ctx->frame->buf[0]) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    ctx->frame->format = pix_fmt;
    ctx->frame->sample_aspect_ratio = av_make_q(1, 1);

    frame_set_colorspace(ctx->frame, pix_fmt);

    WaylandCopyFrame *cpf = (WaylandCopyFrame *)ctx->frame->buf[0]->data;
    ctx->frame->data[0] = cpf->data;

    zwlr_screencopy_frame_v1_copy(frame, cpf->buffer);

    pthread_mutex_unlock(&ctx->frame_obj_lock);

    return;

fail:
    av_frame_free(&ctx->frame);

    if (ctx->error_handler)
        ctx->error_handler(ctx->error_handler_ctx, err);
    zwlr_screencopy_frame_v1_destroy(frame);
    ctx->scrcpy.frame_obj = NULL;

    pthread_mutex_unlock(&ctx->frame_obj_lock);
}

static void scrcpy_flags(void *data, struct zwlr_screencopy_frame_v1 *frame,
                         uint32_t flags)
{
    WaylandCaptureCtx *ctx = data;
    pthread_mutex_lock(&ctx->frame_obj_lock);

    /* Horizontal flipping - we can do it */
    if (flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) {
        /* Increment the data pointer to the last line */
        ctx->frame->data[0] += ctx->frame->linesize[0] * (ctx->frame->height - 1);
        /* Invert the stride */
        ctx->frame->linesize[0] *= -1;
    }

    pthread_mutex_unlock(&ctx->frame_obj_lock);
}

static void scrcpy_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
                         uint32_t tv_sec_hi, uint32_t tv_sec_lo,
                         uint32_t tv_nsec)
{
    int err;
    WaylandCaptureCtx *ctx = data;
    pthread_mutex_lock(&ctx->frame_obj_lock);

    /* Opaque ref */
    FormatExtraData *fe = (FormatExtraData *)ctx->frame->opaque_ref->data;
    fe->time_base       = av_make_q(1, 1000000000);
    fe->avg_frame_rate  = ctx->frame_rate;

    /* Timestamp of when the frame was presented */
    int64_t presented = ((((uint64_t)tv_sec_hi) << 32) | tv_sec_lo) * 1000000000 + tv_nsec;

    /* Current time */
    struct timespec tsp = { 0 };
    clock_gettime(CLOCK_MONOTONIC, &tsp);

    /* Delay */
    int64_t delay = presented - ((tsp.tv_sec * 1000000000) + tsp.tv_nsec);

    ctx->frame->pts = av_add_stable(fe->time_base, delay, av_make_q(1, 1000000),
                                    av_gettime_relative() - ctx->main->epoch);

    /* We don't do this check at the start on since there's still some chance
     * whatever's consuming the FIFO will be done by now. */
    if (ctx->fifo && sp_frame_fifo_is_full(ctx->fifo)) {
        ctx->dropped_frames++;
        av_log_once(ctx->main, AV_LOG_WARNING, AV_LOG_DEBUG, &ctx->dropped_frames_msg_state,
                    "Dropping a frame, queue is full (%i dropped)!\n", ctx->dropped_frames);
        av_frame_free(&ctx->frame);
    } else if (ctx->fifo) {
        if (sp_frame_fifo_push(ctx->fifo, ctx->frame)) {
            av_log(ctx->main, AV_LOG_ERROR, "Unable to push frame to FIFO!\n");
            err = AVERROR(ENOMEM);
            goto fail;
        }
        ctx->dropped_frames_msg_state = 0;
        ctx->frame = NULL;
    } else {
        av_frame_free(&ctx->frame);
    }

    zwlr_screencopy_frame_v1_destroy(frame);

    /* Framerate limiting */
    if (ctx->frame_delay) {
        int64_t now = av_gettime_relative();
        if (ctx->next_frame_ts && (now < ctx->next_frame_ts)) {
            int64_t delay;
            while (1) {
                delay = ctx->next_frame_ts - now;
                if (delay <= 0)
                    break;
                av_usleep(delay);
                now = av_gettime_relative();
            }
        }
        ctx->next_frame_ts = now + ctx->frame_delay;
    }

    scrcpy_register_cb(ctx);

    pthread_mutex_unlock(&ctx->frame_obj_lock);

    return;

fail:
    av_frame_free(&ctx->frame);

    if (ctx->error_handler)
        ctx->error_handler(ctx->error_handler_ctx, err);
    zwlr_screencopy_frame_v1_destroy(frame);
    ctx->scrcpy.frame_obj = NULL;

    pthread_mutex_unlock(&ctx->frame_obj_lock);
}

static void scrcpy_fail(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
    WaylandCaptureCtx *ctx = data;
    pthread_mutex_lock(&ctx->frame_obj_lock);

    av_log(ctx->main, AV_LOG_ERROR, "Copy failed!\n");

    if (ctx->error_handler)
        ctx->error_handler(ctx->error_handler_ctx, AVERROR_EXTERNAL);

    av_buffer_pool_uninit(&ctx->scrcpy.pool);
    zwlr_screencopy_frame_v1_destroy(frame);
    ctx->scrcpy.frame_obj = NULL;

    av_frame_free(&ctx->frame);

    pthread_mutex_unlock(&ctx->frame_obj_lock);
}

static const struct zwlr_screencopy_frame_v1_listener scrcpy_frame_listener = {
	.buffer = scrcpy_give_buffer,
	.flags  = scrcpy_flags,
	.ready  = scrcpy_ready,
	.failed = scrcpy_fail,
};

static void scrcpy_register_cb(WaylandCaptureCtx *ctx)
{
    struct zwlr_screencopy_frame_v1 *f;
    f = zwlr_screencopy_manager_v1_capture_output(ctx->main->screencopy_export_manager,
                                                  ctx->capture_cursor, ctx->target);
    zwlr_screencopy_frame_v1_add_listener(f, &scrcpy_frame_listener, ctx);
    ctx->scrcpy.frame_obj = f;
}

static int start_wlcapture(void *s, uint64_t identifier, AVDictionary *opts, SPFrameFIFO *dst,
                           error_handler *err_cb, void *error_handler_ctx)
{
    int err;
    WaylandCtx *ctx = s;
    struct wayland_output *src = find_output(ctx, NULL, identifier);

    if (!src)
        return AVERROR(EINVAL);

    av_log(ctx, AV_LOG_INFO, "Starting capturing from \"%s\" (id: %li)\n",
           src->model, identifier);

    WaylandCaptureCtx *cap_ctx = create_capture_ctx(ctx);

    AVRational framerate_req = src->framerate;

    cap_ctx->main              = ctx;
    cap_ctx->identifier        = identifier;
    cap_ctx->target            = src->output;
    cap_ctx->fifo              = dst;
    cap_ctx->frame_rate        = framerate_req;
    cap_ctx->error_handler     = err_cb;
    cap_ctx->error_handler_ctx = error_handler_ctx;
    pthread_mutex_init(&cap_ctx->frame_obj_lock, NULL);

    /* Options */
    if (dict_get(opts, "capture_cursor"))
        cap_ctx->capture_cursor = strtol(dict_get(opts, "capture_cursor"), NULL, 10);
    if (dict_get(opts, "use_screencopy"))
        cap_ctx->use_screencopy = strtol(dict_get(opts, "use_screencopy"), NULL, 10);
    if (dict_get(opts, "framerate_num"))
        framerate_req.num = strtol(dict_get(opts, "framerate_num"), NULL, 10);
    if (dict_get(opts, "framerate_den"))
        framerate_req.den = strtol(dict_get(opts, "framerate_den"), NULL, 10);

    if ((framerate_req.num && !framerate_req.den) ||
        (framerate_req.den && !framerate_req.num) ||
        (av_cmp_q(framerate_req, src->framerate) > 0)) {
        av_log(ctx, AV_LOG_ERROR, "Invalid framerate!\n");
        err = AVERROR(EINVAL);
        goto fail;
    } else if ((framerate_req.num && framerate_req.den) &&
               (av_cmp_q(framerate_req, src->framerate) != 0)) {
        cap_ctx->frame_rate = framerate_req;
        cap_ctx->frame_delay = av_rescale_q(1, av_inv_q(framerate_req), AV_TIME_BASE_Q);
    }

    if (cap_ctx->use_screencopy)
        scrcpy_register_cb(cap_ctx);
    else
        dmabuf_register_cb(cap_ctx);

    wl_display_flush(ctx->display);

    av_dict_free(&opts);

    return 0;

fail:
    av_dict_free(&opts);

    return err;
}

static int stop_wlcapture(void  *s, uint64_t identifier)
{
    WaylandCtx *ctx = s;

    for (int i = 0; i < ctx->capture_ctx_num; i++) {
        WaylandCaptureCtx *cap_ctx = ctx->capture_ctx[i];
        if (cap_ctx->identifier == identifier) {
            av_log(ctx, AV_LOG_INFO, "Stopping wayland capture from id %lu\n", identifier);

            pthread_mutex_lock(&cap_ctx->frame_obj_lock);

            /* Destroying the frame objects should destroy everything */
            if (cap_ctx->scrcpy.frame_obj)
                zwlr_screencopy_frame_v1_destroy(cap_ctx->scrcpy.frame_obj);

            /* av_frame_free also destroys the callback */
            if (cap_ctx->dmabuf.frame_obj && !cap_ctx->frame)
                zwlr_export_dmabuf_frame_v1_destroy(cap_ctx->dmabuf.frame_obj);

            av_frame_free(&cap_ctx->frame);

            pthread_mutex_unlock(&cap_ctx->frame_obj_lock);

            /* Free anything allocated */
            av_buffer_pool_uninit(&cap_ctx->scrcpy.pool);
            av_buffer_unref(&cap_ctx->dmabuf.frames_ref);

            /* Send EOF */
            if (cap_ctx->fifo)
                sp_frame_fifo_push(cap_ctx->fifo, NULL);

            /* Remove capture context */
            remove_capture_ctx(ctx, cap_ctx);

            return 0;
        }
    }

    return AVERROR(EINVAL);
}

static void sources_wlcapture(void *s, SourceInfo **sources, int *num)
{
    WaylandCtx *ctx = s;

    /* Free */
    for (int i = 0; i < ctx->num_sources; i++) {
        SourceInfo *src = &ctx->sources[i];
        av_freep(&src->name);
        av_freep(&src->desc);
    }
    av_freep(&ctx->sources);

    /* Realloc */
    ctx->num_sources = wl_list_length(&ctx->output_list);
    ctx->sources = av_mallocz(ctx->num_sources*sizeof(*ctx->sources));

    /* Copy */
    int cnt = 0;
    struct wayland_output *o, *tmp_o;
    wl_list_for_each_reverse_safe(o, tmp_o, &ctx->output_list, link) {
        SourceInfo *src = &ctx->sources[cnt++];
        src->identifier = o->id;
        src->name = av_strdup(o->model);

        AVBPrint tmp;
        av_bprint_init(&tmp, 0, -1);
        av_bprintf(&tmp, "%s %ix%i %.2fHz", o->make, o->width,
                   o->height, av_q2d(o->framerate));
        av_bprint_finalize(&tmp, &src->desc);
    }

    /* Done */
    *sources = ctx->sources;
    *num = ctx->num_sources;
}

static void uninit_wlcapture(void **s)
{
    WaylandCtx *ctx = *s;    

    sp_write_wakeup_pipe(ctx->wakeup_pipe);
    pthread_join(ctx->event_thread, NULL);

    struct wayland_output *output, *tmp_o;
    wl_list_for_each_safe(output, tmp_o, &ctx->output_list, link)
        remove_output(output);

    if (ctx->dmabuf_export_manager)
        zwlr_export_dmabuf_manager_v1_destroy(ctx->dmabuf_export_manager);

    if (ctx->screencopy_export_manager)
        zwlr_screencopy_manager_v1_destroy(ctx->screencopy_export_manager);

    av_buffer_unref(&ctx->drm_device_ref);

    wl_display_disconnect(ctx->display);

    for (int i = 0; i < ctx->num_sources; i++) {
        SourceInfo *src = &ctx->sources[i];
        av_freep(&src->name);
        av_freep(&src->desc);
    }
    av_freep(&ctx->sources);

    av_freep(&ctx->class);
    av_freep(s);
}

void *wayland_events_thread(void *arg)
{
    WaylandCtx *ctx = arg;

    pthread_setname_np(pthread_self(), ctx->class->class_name);

    while (1) {
        while (wl_display_prepare_read(ctx->display))
			wl_display_dispatch_pending(ctx->display);
        wl_display_flush(ctx->display);

        struct pollfd fds[2] = {
            {.fd = wl_display_get_fd(ctx->display), .events = POLLIN, },
            {.fd = ctx->wakeup_pipe[0],             .events = POLLIN, },
        };

        if (poll(fds, FF_ARRAY_ELEMS(fds), -1) < 0) {
            wl_display_cancel_read(ctx->display);
            break;
        }

        if (fds[0].revents & POLLIN) {
            if (wl_display_read_events(ctx->display) < 0) {
                av_log(ctx, AV_LOG_ERROR, "Error reading events!\n");
                break;
            }
            wl_display_dispatch_pending(ctx->display);
        }

        /* Stop the loop */
        if (fds[1].revents & POLLIN) {
            sp_flush_wakeup_pipe(ctx->wakeup_pipe);
            break;
        }
    }

    return NULL;
}

static void drm_device_free(AVHWDeviceContext *hwdev)
{
    close(((AVDRMDeviceContext *)hwdev->hwctx)->fd);
}

static int init_drm_hwcontext(WaylandCtx *ctx)
{
    int err;

    /* DRM hwcontext */
    ctx->drm_device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_DRM);
    if (!ctx->drm_device_ref)
        return AVERROR(ENOMEM);

    AVHWDeviceContext *ref_data = (AVHWDeviceContext*)ctx->drm_device_ref->data;
    AVDRMDeviceContext *hwctx = ref_data->hwctx;

    /* Hope this is the right one */
    hwctx->fd = open("/dev/dri/renderD128", O_RDWR);
    ref_data->free = drm_device_free;

    err = av_hwdevice_ctx_init(ctx->drm_device_ref);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Failed to open DRM device: %s!\n", av_err2str(err));
        return err;
    }

    return 0;
}

static int init_wlcapture(void **s, int64_t epoch)
{
    int err = 0;
    WaylandCtx *ctx = av_mallocz(sizeof(*ctx));
    ctx->class = av_mallocz(sizeof(*ctx->class));
    *ctx->class = (AVClass) {
        .class_name = "wayland",
        .item_name  = av_default_item_name,
        .version    = LIBAVUTIL_VERSION_INT,
    };

    ctx->display = wl_display_connect(NULL);
    if (!ctx->display) {
        av_log(ctx, AV_LOG_ERROR, "Failed to connect to display!\n");
        return AVERROR(EINVAL);
    }

    ctx->wakeup_pipe[0] = ctx->wakeup_pipe[1] = -1;

    sp_make_wakeup_pipe(ctx->wakeup_pipe);

    wl_list_init(&ctx->output_list);

    ctx->registry = wl_display_get_registry(ctx->display);
    wl_registry_add_listener(ctx->registry, &registry_listener, ctx);

    wl_display_roundtrip(ctx->display);
    wl_display_dispatch(ctx->display);

    if (!ctx->dmabuf_export_manager) {
        av_log(ctx, AV_LOG_ERROR, "Compositor doesn't support %s!\n",
               zwlr_export_dmabuf_manager_v1_interface.name);
        return AVERROR(ENOSYS);
    } else {
        if ((err = init_drm_hwcontext(ctx)))
            goto fail;
    }

    if (!ctx->screencopy_export_manager) {
        av_log(ctx, AV_LOG_ERROR, "Compositor doesn't support %s!\n",
               zwlr_screencopy_manager_v1_interface.name);
        return AVERROR(ENOSYS);
    }

    /* Start the event thread */
    pthread_create(&ctx->event_thread, NULL, wayland_events_thread, ctx);

    ctx->epoch = epoch;

    *s = ctx;

    return 0;

fail:
    //TODO
    return err;
}

const CaptureSource src_wayland = {
    .name    = "wayland",
    .init    = init_wlcapture,
    .start   = start_wlcapture,
    .sources = sources_wlcapture,
    .stop    = stop_wlcapture,
    .free    = uninit_wlcapture,
};
