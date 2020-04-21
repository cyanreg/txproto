#define _GNU_SOURCE
#include <unistd.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <poll.h>

#include <libdrm/drm_fourcc.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/buffer.h>
#include <libavutil/hwcontext_drm.h>

#include "wlr-export-dmabuf-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"

#include "frame_fifo.h"
#include "src_common.h"
#include "utils.h"

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

typedef struct WaylandCopyFrame {
    struct wl_buffer *buffer;
    void *data;
    size_t size;
} WaylandCopyFrame;

typedef struct WaylandCaptureCtx {
    WaylandCtx *main;
    uint64_t identifier;
    struct wl_output *target;
    report_format *info_cb;
    void *info_cb_ctx;
    atomic_bool stop_capture;
    AVFrameFIFO *fifo;

    /* Capture options */
    int capture_cursor;
    int use_screencopy;

    /* To make sure timestamps start from 0 */
    int64_t vid_start_pts;

    /* Frame being signalled */
    AVFrame *frame;
    FormatReport fmt_report;
    int width;
    int height;

    /* DMABUF stuff */
    AVBufferRef *drm_frames_ref;

    /* Screencopy stuff */
    AVBufferPool *scrcpy_pool;
    uint32_t scrcpy_stride;
    uint32_t scrcpy_format;
} WaylandCaptureCtx;

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
    remove_output(find_output((WaylandCtx *)data, NULL, id));
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

    zwlr_export_dmabuf_frame_v1_destroy(opaque);

    av_free(data);
}

static void dmabuf_frame_start(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
                               uint32_t width, uint32_t height, uint32_t offset_x,
                               uint32_t offset_y, uint32_t buffer_flags, uint32_t flags,
                               uint32_t format, uint32_t mod_high, uint32_t mod_low,
                               uint32_t num_objects)
{
    WaylandCaptureCtx *ctx = data;
    int err = 0;

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

    /* Allocate a frame */
    ctx->frame = av_frame_alloc();
    if (!ctx->frame) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    /* Set base frame properties */
    ctx->frame->width  = width;
    ctx->frame->height = height;
    ctx->frame->format = AV_PIX_FMT_DRM_PRIME;

    /* Set the frame data to the DRM specific struct */
    ctx->frame->buf[0] = av_buffer_create((uint8_t*)desc, sizeof(*desc),
                                          &dmabuf_frame_free, frame, 0);
    if (!ctx->frame->buf[0]) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    ctx->frame->data[0] = (uint8_t*)desc;

    return;

fail:
//    ctx->err = err;
    dmabuf_frame_free(frame, (uint8_t *)desc);
//    ctx->report_err(ctx->opaque_ctx, ctx->err);
}

static void dmabuf_frame_object(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
                                uint32_t index, int32_t fd, uint32_t size,
                                uint32_t offset, uint32_t stride, uint32_t plane_index)
{
    WaylandCaptureCtx *ctx = data;
    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)ctx->frame->data[0];

    desc->objects[index].fd = fd;
    desc->objects[index].size = size;

    desc->layers[0].planes[plane_index].object_index = index;
    desc->layers[0].planes[plane_index].offset = offset;
    desc->layers[0].planes[plane_index].pitch = stride;
}

static enum AVPixelFormat drm_fmt_to_pixfmt(uint32_t fmt) {
    switch (fmt) {
    case DRM_FORMAT_NV12: return AV_PIX_FMT_NV12;
    case DRM_FORMAT_ARGB8888: return AV_PIX_FMT_BGRA;
    case DRM_FORMAT_XRGB8888: return AV_PIX_FMT_BGR0;
    case DRM_FORMAT_ABGR8888: return AV_PIX_FMT_RGBA;
    case DRM_FORMAT_XBGR8888: return AV_PIX_FMT_RGB0;
    case DRM_FORMAT_RGBA8888: return AV_PIX_FMT_ABGR;
    case DRM_FORMAT_RGBX8888: return AV_PIX_FMT_0BGR;
    case DRM_FORMAT_BGRA8888: return AV_PIX_FMT_ARGB;
    case DRM_FORMAT_BGRX8888: return AV_PIX_FMT_0RGB;
    default: return AV_PIX_FMT_NONE;
    };
}

static int attach_drm_frames_ref(WaylandCaptureCtx *ctx, AVFrame *f,
                                 enum AVPixelFormat sw_format)
{
    int err = 0;
    AVHWFramesContext *hwfc;

    if (ctx->drm_frames_ref) {
        hwfc = (AVHWFramesContext*)ctx->drm_frames_ref->data;
        if (hwfc->width == f->width && hwfc->height == f->height &&
            hwfc->sw_format == sw_format) {
            goto attach;
        }
        av_buffer_unref(&ctx->drm_frames_ref);
    }

    ctx->drm_frames_ref = av_hwframe_ctx_alloc(ctx->main->drm_device_ref);
    if (!ctx->drm_frames_ref) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    hwfc = (AVHWFramesContext*)ctx->drm_frames_ref->data;

    hwfc->format = f->format;
    hwfc->sw_format = sw_format;
    hwfc->width = f->width;
    hwfc->height = f->height;

    err = av_hwframe_ctx_init(ctx->drm_frames_ref);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "AVHWFramesContext init failed: %s!\n",
               av_err2str(err));
        goto fail;
    }

attach:
    /* Set frame hardware context referencce */
    f->hw_frames_ctx = av_buffer_ref(ctx->drm_frames_ref);
    if (!f->hw_frames_ctx) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    return 0;

fail:
    av_buffer_unref(&ctx->drm_frames_ref);
    return err;
}

static void dmabuf_register_cb(WaylandCaptureCtx *ctx);

static void dmabuf_frame_ready(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
                               uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec)
{
    WaylandCaptureCtx *ctx = data;
    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)ctx->frame->data[0];
    enum AVPixelFormat pix_fmt = drm_fmt_to_pixfmt(desc->layers[0].format);
    int err = 0;

	/* Timestamp, nanoseconds timebase */
    ctx->frame->pts = ((((uint64_t)tv_sec_hi) << 32) | tv_sec_lo) * 1000000000 + tv_nsec;
    if (!ctx->vid_start_pts)
        ctx->vid_start_pts = ctx->frame->pts;
    ctx->frame->pts -= ctx->vid_start_pts;

    if (ctx->fmt_report.width != ctx->frame->width ||
        ctx->fmt_report.height != ctx->frame->height ||
        ctx->fmt_report.pix_fmt != ctx->frame->format) {
        ctx->fmt_report.width = ctx->frame->width;
        ctx->fmt_report.height = ctx->frame->height;
        ctx->fmt_report.pix_fmt = ctx->frame->format;
        ctx->info_cb(ctx->info_cb_ctx, &ctx->fmt_report);
    }

	/* Attach the hardware frame context to the frame */
    if ((err = attach_drm_frames_ref(ctx, ctx->frame, pix_fmt)))
        goto end;

    /* TODO: support multiplane stuff */
    desc->layers[0].nb_planes = av_pix_fmt_count_planes(pix_fmt);

    if (!ctx->fifo) {
        av_frame_free(&ctx->frame);
        dmabuf_register_cb(ctx);
        av_log(ctx->main, AV_LOG_INFO, "Copy not done!\n");
    } else if (push_to_fifo(ctx->fifo, ctx->frame)) {
        av_log(ctx->main, AV_LOG_ERROR, "Unable to push frame to FIFO!\n");
        av_frame_free(&ctx->frame);
    } else {
        dmabuf_register_cb(ctx);
        ctx->frame = NULL;
    }

end:
//    ctx->err = err;
    av_frame_free(&ctx->frame);
//    ctx->report_err(ctx->opaque_ctx, ctx->err);
}

static void dmabuf_frame_cancel(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
                                uint32_t reason)
{
	WaylandCaptureCtx *ctx = data;
	av_log(ctx->main, AV_LOG_WARNING, "Frame cancelled!\n");
	av_frame_free(&ctx->frame);
	if (reason == ZWLR_EXPORT_DMABUF_FRAME_V1_CANCEL_REASON_PERMANENT) {
		av_log(ctx->main, AV_LOG_ERROR, "Permanent failure, exiting\n");
//		ctx->err = AVERROR(EINVAL);
//		ctx->report_err(ctx->opaque_ctx, ctx->err);
	} else {
		dmabuf_register_cb(ctx);
	}
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
}

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

    f->size = ctx->scrcpy_stride * ctx->height;

    char name[255];
    snprintf(name, sizeof(name), "wlcapture_%ix%i_s%i_f0x%x", ctx->width,
             ctx->height, ctx->scrcpy_stride, ctx->scrcpy_format);
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
    f->buffer = wl_shm_pool_create_buffer(pool, 0, ctx->width,
                                          ctx->height,
                                          ctx->scrcpy_stride,
                                          ctx->scrcpy_format);
    wl_shm_pool_destroy(pool);

    return av_buffer_create((uint8_t *)f, sizeof(WaylandCopyFrame),
                            scrcpy_frame_free, NULL, 0);
}

static enum AVPixelFormat map_shm_to_ffmpeg(enum wl_shm_format format)
{
    static const struct {
        enum wl_shm_format src;
        enum AVPixelFormat dst;
    } format_map[] = {
        { WL_SHM_FORMAT_XRGB8888, AV_PIX_FMT_BGR0 },
        { WL_SHM_FORMAT_ARGB8888, AV_PIX_FMT_BGR0 },
        { WL_SHM_FORMAT_XBGR8888, AV_PIX_FMT_RGB0 },
        { WL_SHM_FORMAT_ABGR8888, AV_PIX_FMT_RGBA },
    };

    for (int i = 0; i < FF_ARRAY_ELEMS(format_map); i++)
        if (format_map[i].src == format)
            return format_map[i].dst;

    return AV_PIX_FMT_NONE;
}

static void scrcpy_give_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
                               enum wl_shm_format format, uint32_t width, uint32_t height,
                               uint32_t stride)
{
    WaylandCaptureCtx *ctx = data;

    if (stride != ctx->scrcpy_stride || height != ctx->height ||
        format != ctx->scrcpy_format || width  != ctx->width) {
        av_buffer_pool_uninit(&ctx->scrcpy_pool);

        ctx->fmt_report.width = ctx->width  = width;
        ctx->fmt_report.height = ctx->height = height;
        ctx->scrcpy_stride = stride;
        ctx->scrcpy_format = format;
        ctx->fmt_report.pix_fmt = map_shm_to_ffmpeg(format);
        ctx->info_cb(ctx->info_cb_ctx, &ctx->fmt_report);

        ctx->scrcpy_pool = av_buffer_pool_init2(sizeof(WaylandCopyFrame), ctx,
                                                shm_pool_alloc, NULL);
    }

    ctx->frame = av_frame_alloc();

    ctx->frame->width = width;
    ctx->frame->height = height;
    ctx->frame->linesize[0] = stride;
    ctx->frame->buf[0] = av_buffer_pool_get(ctx->scrcpy_pool);
    ctx->frame->format = ctx->fmt_report.pix_fmt;

    WaylandCopyFrame *cpf = (WaylandCopyFrame *)ctx->frame->buf[0]->data;
    ctx->frame->data[0] = cpf->data;

    zwlr_screencopy_frame_v1_copy(frame, cpf->buffer);
}

static void scrcpy_flags(void *data, struct zwlr_screencopy_frame_v1 *frame,
                         uint32_t flags)
{
    WaylandCaptureCtx *ctx = data;    

    /* Horizontal flipping - we can do it */
    if (flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) {
        /* Increment the data pointer to the last line */
        ctx->frame->data[0] += ctx->frame->linesize[0] * (ctx->frame->height - 1);
        /* Invert the stride */
        ctx->frame->linesize[0] *= -1;
    }
}

static void scrcpy_register_cb(WaylandCaptureCtx *ctx);

static void scrcpy_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
                         uint32_t tv_sec_hi, uint32_t tv_sec_lo,
                         uint32_t tv_nsec)
{
    WaylandCaptureCtx *ctx = data;
    if (atomic_load(&ctx->stop_capture)) {
        av_frame_free(&ctx->frame);
        av_free(ctx);
        return;
    }

    ctx->frame->pts = ((((uint64_t)tv_sec_hi) << 32) | tv_sec_lo) * 1000000000 + tv_nsec;
    if (!ctx->vid_start_pts)
        ctx->vid_start_pts = ctx->frame->pts;
    ctx->frame->pts -= ctx->vid_start_pts;

    if (!ctx->fifo) {
        av_frame_free(&ctx->frame);
        scrcpy_register_cb(ctx);
        av_log(ctx->main, AV_LOG_INFO, "Copy done!\n");
    } else if (push_to_fifo(ctx->fifo, ctx->frame)) {
        av_log(ctx->main, AV_LOG_ERROR, "Unable to push frame to FIFO!\n");
        av_frame_free(&ctx->frame);
    } else {
        scrcpy_register_cb(ctx);
        ctx->frame = NULL;
    }
}

static void scrcpy_fail(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
    WaylandCaptureCtx *ctx = data;
    av_log(ctx->main, AV_LOG_ERROR, "Copy failed!\n");
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
}

FN_CREATING(WaylandCtx, WaylandCaptureCtx, capture_ctx, capture_ctx, capture_ctx_num)

static int start_wlcapture(void *s, uint64_t identifier, AVDictionary *opts,
                           AVFrameFIFO *dst, report_format *info_cb, void *info_cb_ctx)
{
    WaylandCtx *ctx = s;
    struct wayland_output *src = find_output(ctx, NULL, identifier);

    if (!src)
        return AVERROR(EINVAL);

    av_log(ctx, AV_LOG_INFO, "Starting capturing from \"%s\" (id: %li)\n",
           src->model, identifier);

    WaylandCaptureCtx *cap_ctx = create_capture_ctx(ctx);

    cap_ctx->main = ctx;
    cap_ctx->stop_capture = ATOMIC_VAR_INIT(0);
    cap_ctx->identifier = identifier;
    cap_ctx->target = src->output;
    cap_ctx->fifo = dst;
    cap_ctx->info_cb = info_cb;
    cap_ctx->info_cb_ctx = info_cb_ctx;
    cap_ctx->fmt_report.type = AVMEDIA_TYPE_VIDEO;
    cap_ctx->fmt_report.time_base = av_make_q(1, 1000000000);

    /* Options */
    cap_ctx->capture_cursor = strtol(dict_get(opts, "capture_cursor"), NULL, 10);
    cap_ctx->use_screencopy = strtol(dict_get(opts, "use_screencopy"), NULL, 10);

    if (cap_ctx->use_screencopy)
        scrcpy_register_cb(cap_ctx);
    else
        dmabuf_register_cb(cap_ctx);

    wl_display_dispatch(ctx->display);

    return 0;
}

static int stop_wlcapture(void  *s, uint64_t identifier)
{
    WaylandCtx *ctx = s;

    for (int i = 0; i < ctx->capture_ctx_num; i++) {
        WaylandCaptureCtx *cap_ctx = ctx->capture_ctx[i];
        if (cap_ctx->identifier == identifier) {
            av_log(ctx, AV_LOG_INFO, "Stopping wayland capture from id %lu\n", identifier);
            av_buffer_pool_uninit(&cap_ctx->scrcpy_pool);
            atomic_store(&cap_ctx->stop_capture, 1);

            /* The WaylandCaptureCtx gets freed by the capture function */

            /* Remove from the list */
            ctx->capture_ctx_num = FFMAX(ctx->capture_ctx_num - 1, 0);
            memcpy(&ctx->capture_ctx[i], &ctx->capture_ctx[i + 1],
                   (ctx->capture_ctx_num - i)*sizeof(WaylandCaptureCtx *));
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
        src->desc = av_strdup(o->make);
    }

    /* Done */
    *sources = ctx->sources;
    *num = ctx->num_sources;
}

static void uninit_wlcapture(void **s)
{
    WaylandCtx *ctx = *s;

    wls_write_wakeup_pipe(ctx->wakeup_pipe);
    pthread_join(ctx->event_thread, NULL);

    struct wayland_output *output, *tmp_o;
    wl_list_for_each_safe(output, tmp_o, &ctx->output_list, link)
        remove_output(output);

    if (ctx->dmabuf_export_manager)
        zwlr_export_dmabuf_manager_v1_destroy(ctx->dmabuf_export_manager);

    if (ctx->screencopy_export_manager)
        zwlr_screencopy_manager_v1_destroy(ctx->screencopy_export_manager);

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
            wls_flush_wakeup_pipe(ctx->wakeup_pipe);
            break;
        }
    }

    return NULL;
}

static int init_dmabuf_hwcontext(WaylandCtx *ctx)
{
    /* DRM hwcontext */
    ctx->drm_device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_DRM);
    if (!ctx->drm_device_ref)
        return AVERROR(ENOMEM);

    AVHWDeviceContext *ref_data = (AVHWDeviceContext*)ctx->drm_device_ref->data;
    AVDRMDeviceContext *hwctx = ref_data->hwctx;

    /* We don't need a device (we don't even know it and can't open it) */
    hwctx->fd = -1;

    av_hwdevice_ctx_init(ctx->drm_device_ref);

    return 0;
}

static int init_wlcapture(void **s, report_error *err_cb, void *err_opaque)
{
    int err = 0;
    WaylandCtx *ctx = av_mallocz(sizeof(*ctx));
    ctx->class = av_mallocz(sizeof(*ctx->class));
    *ctx->class = (AVClass) {
        .class_name = "wlstream_wlcapture",
        .item_name  = av_default_item_name,
        .version    = LIBAVUTIL_VERSION_INT,
    };

    ctx->display = wl_display_connect(NULL);
    if (!ctx->display) {
        av_log(ctx, AV_LOG_ERROR, "Failed to connect to display!\n");
        return AVERROR(EINVAL);
    }

    ctx->wakeup_pipe[0] = ctx->wakeup_pipe[1] = -1;

    wls_make_wakeup_pipe(ctx->wakeup_pipe);

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
        err = init_dmabuf_hwcontext(ctx);
        if (err)
            goto fail;
    }

    if (!ctx->screencopy_export_manager) {
        av_log(ctx, AV_LOG_ERROR, "Compositor doesn't support %s!\n",
               zwlr_screencopy_manager_v1_interface.name);
        return AVERROR(ENOSYS);
    }

    /* Start the event thread */
    pthread_create(&ctx->event_thread, NULL, wayland_events_thread, ctx);
    pthread_setname_np(ctx->event_thread, "wayland events thread");

    *s = ctx;

    return 0;

fail:
    //TODO
    return err;
}

const CaptureSource src_wayland = {
    .name    = "wayland_capture",
    .init    = init_wlcapture,
    .start   = start_wlcapture,
    .sources = sources_wlcapture,
    .stop    = stop_wlcapture,
    .free    = uninit_wlcapture,
};
