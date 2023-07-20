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

#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#include <libdrm/drm_fourcc.h>
#include <libavutil/pixdesc.h>
#include <libavutil/bprint.h>
#include <libavutil/buffer.h>
#include <libavutil/time.h>

#include "wayland_common.h"

#include <libtxproto/utils.h>
#include <libtxproto/log.h>
#include "iosys_common.h"
#include "ctrl_template.h"
#include "../config.h"

#ifdef HAVE_GBM
#include <gbm.h>
#endif

const IOSysAPI src_wayland;

typedef struct WaylandCaptureCtx {
    SPClass *class;
    WaylandCtx *wl;

    AVBufferRef *wl_ref;
    SPBufferList *events;
} WaylandCaptureCtx;

enum WaylandCaptureMode {
    CAP_MODE_DMABUF,
    CAP_MODE_SCRCPY,
    CAP_MODE_SCRCPY_DMABUF,
};

typedef struct WaylandCapturePriv {
    WaylandCaptureCtx *main;
    AVBufferRef *main_ref;

    int64_t epoch;
    int oneshot;

    /* Stats */
    int dropped_frames;

    /* Framerate limiting */
    AVRational frame_rate;
    int64_t next_frame_ts;
    int64_t frame_delay;

    /* Capture options */
    enum WaylandCaptureMode capture_mode;
    int capture_cursor;

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
        struct {
            int got_info;
            AVBufferPool *pool;
            uint32_t stride;
            uint32_t format;
            int width;
            int height;
        } shm;
        struct {
            int got_info;
            AVBufferRef *frames_ref;
#ifdef HAVE_GBM
            struct gbm_device *gbm_dev;
#endif
            uint32_t format;
            int width;
            int height;
        } dmabuf;
    } scrcpy;
} WaylandCapturePriv;

static void frame_set_colorspace(AVFrame *f, enum AVPixelFormat format)
{
    /* sRGB uses the 709 primaries with the AVCOL_TRC_IEC61966_2_1 transfer
     * function.
     * Only valid for standard displays, but that's what most are */
    f->color_primaries = AVCOL_PRI_BT709;
    f->colorspace      = AVCOL_SPC_RGB;
    f->color_range     = AVCOL_RANGE_JPEG;
    f->color_trc       = AVCOL_TRC_IEC61966_2_1;
}

/* Maps and removes alpha channel */
static enum AVPixelFormat drm_wl_fmt_to_pixfmt(enum wl_shm_format *drm_format,
                                               enum wl_shm_format *wl_format)
{
    static const struct {
        enum wl_shm_format src_drm;
        enum wl_shm_format src_wl;
        enum wl_shm_format stripped_drm;
        enum wl_shm_format stripped_wl;
        enum AVPixelFormat dst;
    } format_map[] = {
#define FMTDBL(fmt) DRM_FORMAT_##fmt, WL_SHM_FORMAT_##fmt
        { FMTDBL(XRGB8888),    FMTDBL(XRGB8888),    AV_PIX_FMT_BGR0    },
        { FMTDBL(ARGB8888),    FMTDBL(XRGB8888),    AV_PIX_FMT_BGR0    },
        { FMTDBL(XBGR8888),    FMTDBL(XBGR8888),    AV_PIX_FMT_RGB0    },
        { FMTDBL(ABGR8888),    FMTDBL(XBGR8888),    AV_PIX_FMT_RGB0    },
        { FMTDBL(XRGB2101010), FMTDBL(ARGB2101010), AV_PIX_FMT_X2BGR10 },
        { FMTDBL(XBGR2101010), FMTDBL(ABGR2101010), AV_PIX_FMT_X2RGB10 },
        { FMTDBL(NV12),        FMTDBL(NV12),        AV_PIX_FMT_NV12    },
        { FMTDBL(P010),        FMTDBL(P010),        AV_PIX_FMT_P010    },
#undef FMTDBL
    };

    for (int i = 0; i < SP_ARRAY_ELEMS(format_map); i++) {
        if (drm_format && format_map[i].src_drm == *drm_format) {
            *drm_format = format_map[i].stripped_drm;
            return format_map[i].dst;
        }
        if (wl_format && format_map[i].src_wl == *wl_format) {
            *wl_format = format_map[i].stripped_wl;
            return format_map[i].dst;
        }
    }

    return AV_PIX_FMT_NONE;
}

static void cleanup_state(IOSysEntry *entry, struct zwlr_export_dmabuf_frame_v1 *df,
                          struct zwlr_screencopy_frame_v1 *sf, int err)
{
    WaylandCapturePriv *priv = entry->io_priv;
    av_frame_free(&priv->frame);

    if (err < 0)
        sp_eventlist_dispatch(entry, entry->events, SP_EVENT_ON_ERROR, NULL);

    av_buffer_pool_uninit(&priv->scrcpy.shm.pool);
    av_buffer_unref(&priv->scrcpy.dmabuf.frames_ref);
    av_buffer_unref(&priv->dmabuf.frames_ref);

    if (df) {
        zwlr_export_dmabuf_frame_v1_destroy(df);
        priv->dmabuf.frame_obj = NULL;
    } else if (sf) {
        zwlr_screencopy_frame_v1_destroy(sf);
        priv->scrcpy.frame_obj = NULL;
    }
}

static void schedule_frame(IOSysEntry *entry);

static void dmabuf_frame_free(void *opaque, uint8_t *data)
{
    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)data;

    for (int i = 0; i < desc->nb_objects; ++i)
        close(desc->objects[i].fd);

    if (opaque)
        zwlr_export_dmabuf_frame_v1_destroy(opaque);

    av_free(data);
}

static void dmabuf_frame_start(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
                               uint32_t width, uint32_t height, uint32_t offset_x,
                               uint32_t offset_y, uint32_t buffer_flags, uint32_t flags,
                               uint32_t format, uint32_t mod_high, uint32_t mod_low,
                               uint32_t num_objects)
{
    int err = 0;
    IOSysEntry *entry = (IOSysEntry *)data;
    WaylandCapturePriv *priv = entry->io_priv;

    pthread_mutex_lock(&priv->frame_obj_lock);

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
    priv->frame = av_frame_alloc();
    if (!priv->frame) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    /* Set base frame properties */
    priv->frame->width               = width;
    priv->frame->height              = height;
    priv->frame->format              = AV_PIX_FMT_DRM_PRIME;
    priv->frame->sample_aspect_ratio = av_make_q(1, 1);
    priv->frame->opaque_ref          = av_buffer_allocz(sizeof(FormatExtraData));
    if (!priv->frame->opaque_ref) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    /* Set the frame data to the DRM specific struct */
    priv->frame->buf[0] = av_buffer_create((uint8_t*)desc, sizeof(*desc),
                                           &dmabuf_frame_free, frame, 0);
    if (!priv->frame->buf[0]) {
        av_free(priv->frame->opaque_ref);
        err = AVERROR(ENOMEM);
        goto fail;
    }

    priv->frame->data[0] = (uint8_t*)desc;

    pthread_mutex_unlock(&priv->frame_obj_lock);

    return;

fail:
    cleanup_state(entry, frame, NULL, err);
    pthread_mutex_unlock(&priv->frame_obj_lock);
}

static void dmabuf_frame_object(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
                                uint32_t index, int32_t fd, uint32_t size,
                                uint32_t offset, uint32_t stride, uint32_t plane_index)
{
    IOSysEntry *entry = (IOSysEntry *)data;
    WaylandCapturePriv *priv = entry->io_priv;

    pthread_mutex_lock(&priv->frame_obj_lock);

    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)priv->frame->data[0];

    desc->objects[index].fd = fd;
    desc->objects[index].size = size;

    desc->layers[0].planes[plane_index].object_index = index;
    desc->layers[0].planes[plane_index].offset = offset;
    desc->layers[0].planes[plane_index].pitch = stride;

    desc->layers[0].nb_planes = SPMAX(desc->layers[0].nb_planes, plane_index + 1);

    pthread_mutex_unlock(&priv->frame_obj_lock);
}

static int attach_drm_frames_ref(IOSysEntry *entry, AVFrame *f,
                                 enum AVPixelFormat sw_format)
{
    int err = 0;
    AVHWFramesContext *hwfc;
    WaylandCapturePriv *priv = entry->io_priv;

    if (priv->dmabuf.frames_ref) {
        hwfc = (AVHWFramesContext*)priv->dmabuf.frames_ref->data;
        if (hwfc->width == f->width && hwfc->height == f->height &&
            hwfc->sw_format == sw_format) {
            goto attach;
        }
        if (priv->dmabuf.frames_ref)
            sp_eventlist_dispatch(entry, entry->events, SP_EVENT_ON_CHANGE, NULL);
        av_buffer_unref(&priv->dmabuf.frames_ref);
    }

    priv->dmabuf.frames_ref = av_hwframe_ctx_alloc(priv->main->wl->drm_device_ref);
    if (!priv->dmabuf.frames_ref) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    hwfc = (AVHWFramesContext*)priv->dmabuf.frames_ref->data;

    hwfc->format = f->format;
    hwfc->sw_format = sw_format;
    hwfc->width = f->width;
    hwfc->height = f->height;

    err = av_hwframe_ctx_init(priv->dmabuf.frames_ref);
    if (err) {
        sp_log(entry, SP_LOG_ERROR, "AVHWFramesContext init failed: %s!\n",
               av_err2str(err));
        goto fail;
    }

attach:
    /* Set frame hardware context referencce */
    f->hw_frames_ctx = av_buffer_ref(priv->dmabuf.frames_ref);
    if (!f->hw_frames_ctx) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    return 0;

fail:
    av_buffer_unref(&priv->dmabuf.frames_ref);

    return err;
}

static void dmabuf_frame_ready(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
                               uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec)
{
    int err = 0;
    IOSysEntry *entry = (IOSysEntry *)data;
    WaylandCapturePriv *priv = entry->io_priv;

    pthread_mutex_lock(&priv->frame_obj_lock);

    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)priv->frame->data[0];

    /* This removes alpha from the format as well */
    enum AVPixelFormat sw_fmt = drm_wl_fmt_to_pixfmt(&desc->layers[0].format, NULL);

    if (sw_fmt == AV_PIX_FMT_NONE) {
        sp_log(entry, SP_LOG_ERROR, "Unsupported DMABUF format!\n");
        err = AVERROR(ENOTSUP);
        goto fail;
    }

    /* Set the colorspace */
    frame_set_colorspace(priv->frame, sw_fmt);

    /* Opaque ref */
    FormatExtraData *fe = (FormatExtraData *)priv->frame->opaque_ref->data;
    fe->time_base       = av_make_q(1, 1000000000);
    fe->avg_frame_rate  = priv->frame_rate;

    /* Timestamp of when the frame will be presented */
    int64_t presented = ((((uint64_t)tv_sec_hi) << 32) | tv_sec_lo) * 1000000000 + tv_nsec;

    /* Current time */
    struct timespec tsp = { 0 };
    clock_gettime(CLOCK_MONOTONIC, &tsp);

    /* Delay */
    int64_t delay = presented - ((tsp.tv_sec * 1000000000) + tsp.tv_nsec);

    priv->frame->pts = av_add_stable(fe->time_base, delay, av_make_q(1, 1000000),
                                     av_gettime_relative() - priv->epoch);

	/* Attach the hardware frame context to the frame */
    if ((err = attach_drm_frames_ref(entry, priv->frame, sw_fmt)))
        goto fail;

    sp_log(entry, SP_LOG_TRACE, "Pushing frame to FIFO, pts = %f\n",
           av_q2d(fe->time_base) * priv->frame->pts);

    /* We don't do this check at the start on since there's still some chance
     * whatever's consuming the FIFO will be done by now. */
    err = sp_frame_fifo_push(entry->frames, priv->frame);
    av_frame_free(&priv->frame);
    if (err == AVERROR(ENOBUFS)) {
        priv->dropped_frames++;
        sp_log(entry, SP_LOG_WARN, "Dropping frame (%i dropped so far)!\n",
               priv->dropped_frames);

        SPGenericData entries[] = {
            D_TYPE("dropped_frames", NULL, priv->dropped_frames),
            { 0 },
        };
        sp_eventlist_dispatch(entry, entry->events, SP_EVENT_ON_STATS, entries);
    } else if (err) {
        sp_log(entry, SP_LOG_ERROR, "Unable to push frame to FIFO: %s!\n",
               av_err2str(err));
        goto fail;
    }

    /* Framerate limiting */
    if (priv->frame_delay) {
        int64_t now = av_gettime_relative();
        if (priv->next_frame_ts && (now < priv->next_frame_ts)) {
            int64_t wait_time;
            while (1) {
                wait_time = priv->next_frame_ts - now;
                if (wait_time <= 0)
                    break;
                av_usleep(wait_time);
                now = av_gettime_relative();
            }
        }
        priv->next_frame_ts = now + priv->frame_delay;
    }

    schedule_frame(entry);

    pthread_mutex_unlock(&priv->frame_obj_lock);

    return;

fail:
    cleanup_state(entry, frame, NULL, err);
    pthread_mutex_unlock(&priv->frame_obj_lock);
}

static void dmabuf_frame_cancel(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
                                uint32_t reason)
{
    IOSysEntry *entry = (IOSysEntry *)data;
    WaylandCapturePriv *priv = entry->io_priv;

    pthread_mutex_lock(&priv->frame_obj_lock);

    if (reason == ZWLR_EXPORT_DMABUF_FRAME_V1_CANCEL_REASON_PERMANENT) {
        sp_log(entry, SP_LOG_ERROR, "Capture failed!\n");
        cleanup_state(entry, frame, NULL, AVERROR_EXTERNAL);
    } else {
        sp_log(priv->main, SP_LOG_WARN, "Frame cancelled!\n");
        schedule_frame(entry);
    }

    pthread_mutex_unlock(&priv->frame_obj_lock);
}

typedef struct WaylandSHMCopyFrame {
    struct wl_buffer *buffer;
    void *data;
    void *mapped;
    size_t size;
} WaylandSHMCopyFrame;

static void scrcpy_shm_frame_free(void *opaque, uint8_t *data)
{
    WaylandSHMCopyFrame *f = (WaylandSHMCopyFrame *)data;
	munmap(f->mapped, f->size);
	wl_buffer_destroy(f->buffer);
	av_free(f);
}

static AVBufferRef *shm_pool_alloc(void *opaque, size_t size)
{
    IOSysEntry *entry = (IOSysEntry *)opaque;
    WaylandCapturePriv *priv = entry->io_priv;

    WaylandSHMCopyFrame *f = av_malloc(sizeof(WaylandSHMCopyFrame));

#define FRAME_MEM_ALIGN 4096

    f->size = priv->scrcpy.shm.stride * priv->scrcpy.shm.height + FRAME_MEM_ALIGN;

    char name[255];
    snprintf(name, sizeof(name), PROJECT_NAME "_%ix%i_s%i_f0x%x",
             priv->scrcpy.shm.width, priv->scrcpy.shm.height,
             priv->scrcpy.shm.stride, priv->scrcpy.shm.format);

    int fd = memfd_create(name, MFD_ALLOW_SEALING);

    if (posix_fallocate(fd, 0, f->size)) {
        close(fd);
        sp_log(entry, SP_LOG_ERROR, "posix_fallocate failed: %i!\n", errno);
        return NULL;
    }

    fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW);
    fcntl(fd, F_SET_RW_HINT, RWH_WRITE_LIFE_SHORT);

    f->mapped = mmap(NULL, f->size, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_NORESERVE, fd, 0);
    if (f->mapped == MAP_FAILED) {
        sp_log(entry, SP_LOG_ERROR, "mmap failed!\n");
        close(fd);
        return NULL;
    }

    f->data = (void *)SPALIGN((uintptr_t)f->mapped, FRAME_MEM_ALIGN);

    struct wl_shm_pool *pool;
    pool = wl_shm_create_pool(priv->main->wl->shm_interface, fd, f->size);
    close(fd);
    f->buffer = wl_shm_pool_create_buffer(pool, (uintptr_t)f->data - (uintptr_t)f->mapped,
                                          priv->scrcpy.shm.width,  priv->scrcpy.shm.height,
                                          priv->scrcpy.shm.stride, priv->scrcpy.shm.format);
    wl_shm_pool_destroy(pool);

#undef FRAME_MEM_ALIGN

    return av_buffer_create((uint8_t *)f, sizeof(WaylandSHMCopyFrame),
                            scrcpy_shm_frame_free, NULL, 0);
}

static void scrcpy_frame_info_shm(void *data, struct zwlr_screencopy_frame_v1 *frame,
                                  enum wl_shm_format format, uint32_t width, uint32_t height,
                                  uint32_t stride)
{
    int err;
    IOSysEntry *entry = (IOSysEntry *)data;
    WaylandCapturePriv *priv = entry->io_priv;

    pthread_mutex_lock(&priv->frame_obj_lock);

    if ((stride != priv->scrcpy.shm.stride) || (height != priv->scrcpy.shm.height) ||
        (format != priv->scrcpy.shm.format) || (width  != priv->scrcpy.shm.width)) {
        if (priv->scrcpy.shm.pool)
            sp_eventlist_dispatch(entry, entry->events, SP_EVENT_ON_CHANGE, NULL);
        av_buffer_pool_uninit(&priv->scrcpy.shm.pool);

        priv->scrcpy.shm.width  = width;
        priv->scrcpy.shm.height = height;
        priv->scrcpy.shm.stride = stride;
        priv->scrcpy.shm.format = format;

        priv->scrcpy.shm.pool = av_buffer_pool_init2(sizeof(WaylandSHMCopyFrame), entry,
                                                     shm_pool_alloc, NULL);

        if (!priv->scrcpy.shm.pool) {
            err = AVERROR(ENOMEM);
            goto fail;
        }
    }

    priv->scrcpy.shm.got_info = 1;

    pthread_mutex_unlock(&priv->frame_obj_lock);

    return;

fail:
    cleanup_state(entry, NULL, frame, err);
    pthread_mutex_unlock(&priv->frame_obj_lock);
}

#ifdef HAVE_GBM
typedef struct WaylandDMABUFCopyFrame {
    struct wl_buffer *buffer;
    struct gbm_bo *bo;
} WaylandDMABUFCopyFrame;

static void scrcpy_dmabuf_frame_free(void *opaque, uint8_t *data)
{
    WaylandDMABUFCopyFrame *f = (WaylandDMABUFCopyFrame *)opaque;
    gbm_bo_destroy(f->bo);
	wl_buffer_destroy(f->buffer);
	av_free(data);
	av_free(f);
}

static AVBufferRef *dmabuf_pool_alloc(void *opaque, size_t size)
{
    IOSysEntry *entry = (IOSysEntry *)opaque;
    WaylandCapturePriv *priv = entry->io_priv;

    AVDRMFrameDescriptor *desc = av_malloc(sizeof(AVDRMFrameDescriptor));
    WaylandDMABUFCopyFrame *f = av_malloc(sizeof(WaylandDMABUFCopyFrame));

    f->bo = gbm_bo_create(priv->scrcpy.dmabuf.gbm_dev,
                          priv->scrcpy.dmabuf.width,
                          priv->scrcpy.dmabuf.height,
                          priv->scrcpy.dmabuf.format,
                          GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);

    struct zwp_linux_buffer_params_v1 *params;
    params = zwp_linux_dmabuf_v1_create_params(priv->main->wl->dmabuf);

    int fd = gbm_bo_get_fd(f->bo);
    uint32_t off = gbm_bo_get_offset(f->bo, 0);
    uint32_t bo_stride = gbm_bo_get_stride(f->bo);
    uint64_t mod = gbm_bo_get_modifier(f->bo);

    desc->nb_objects = 1;
    desc->objects[0].fd = fd;
    desc->objects[0].size = lseek(desc->objects[0].fd, 0, SEEK_END);
    desc->objects[0].format_modifier = mod;

    desc->nb_layers = 1;
    desc->layers[0].format = priv->scrcpy.dmabuf.format;
    desc->layers[0].nb_planes = 1;
    desc->layers[0].planes[0].object_index = 0;
    desc->layers[0].planes[0].offset = 0;
    desc->layers[0].planes[0].pitch = bo_stride;

    zwp_linux_buffer_params_v1_add(params, fd, 0, off, bo_stride,
                                   mod >> 32, mod & 0xffffffff);

    f->buffer = zwp_linux_buffer_params_v1_create_immed(params,
                                                        priv->scrcpy.dmabuf.width,
                                                        priv->scrcpy.dmabuf.height,
                                                        priv->scrcpy.dmabuf.format,
                                                        0x0);

    return av_buffer_create((uint8_t *)desc, sizeof(AVDRMFrameDescriptor),
                            scrcpy_dmabuf_frame_free, f, 0);
}

static void scrcpy_frames_ref_free(AVHWFramesContext *hwfc)
{
    av_buffer_pool_uninit(&hwfc->pool);
}

static void scrcpy_frame_info_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *frame,
                                     uint32_t format, uint32_t width, uint32_t height)
{
    int err;
    IOSysEntry *entry = (IOSysEntry *)data;
    WaylandCapturePriv *priv = entry->io_priv;

    pthread_mutex_lock(&priv->frame_obj_lock);

    if ((width  != priv->scrcpy.dmabuf.width) || (height != priv->scrcpy.dmabuf.height) ||
        (format != priv->scrcpy.dmabuf.format)) {
        if (priv->scrcpy.dmabuf.frames_ref)
            sp_eventlist_dispatch(entry, entry->events, SP_EVENT_ON_CHANGE, NULL);

        av_buffer_unref(&priv->scrcpy.dmabuf.frames_ref);

        priv->scrcpy.dmabuf.width  = width;
        priv->scrcpy.dmabuf.height = height;
        priv->scrcpy.dmabuf.format = format;

        priv->scrcpy.dmabuf.frames_ref = av_hwframe_ctx_alloc(priv->main->wl->drm_device_ref);
        if (!priv->scrcpy.dmabuf.frames_ref) {
            err = AVERROR(ENOMEM);
            goto fail;
        }

        AVHWFramesContext *hwfc = (AVHWFramesContext *)priv->scrcpy.dmabuf.frames_ref->data;

        hwfc->free = scrcpy_frames_ref_free;
        hwfc->user_opaque = priv;
        hwfc->format = AV_PIX_FMT_DRM_PRIME;
        hwfc->width = width;
        hwfc->height = height;
        hwfc->sw_format = drm_wl_fmt_to_pixfmt(&format, NULL);

        hwfc->pool = av_buffer_pool_init2(sizeof(AVDRMFrameDescriptor), entry,
                                          dmabuf_pool_alloc, NULL);
        if (!hwfc->pool) {
            err = AVERROR(ENOMEM);
            goto fail;
        }

        err = av_hwframe_ctx_init(priv->scrcpy.dmabuf.frames_ref);
        if (err < 0)
            goto fail;
    }

    priv->scrcpy.dmabuf.got_info = 1;

    pthread_mutex_unlock(&priv->frame_obj_lock);

    return;

fail:
    cleanup_state(entry, NULL, frame, err);
    pthread_mutex_unlock(&priv->frame_obj_lock);
}
#else
static void scrcpy_frame_info_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *frame,
                                     uint32_t format, uint32_t width, uint32_t height)
{

}
#endif

static void scrcpy_frame_info_done(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
    int err;
    struct wl_buffer *dst;
    IOSysEntry *entry = (IOSysEntry *)data;
    WaylandCapturePriv *priv = entry->io_priv;

    pthread_mutex_lock(&priv->frame_obj_lock);

    priv->frame = av_frame_alloc();
    if (!priv->frame) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

#ifdef HAVE_GBM
    if (priv->capture_mode == CAP_MODE_SCRCPY_DMABUF) {
        err = av_hwframe_get_buffer(priv->scrcpy.dmabuf.frames_ref, priv->frame, 0);
        if (err < 0)
            goto fail;

        AVHWFramesContext *hwfc = (AVHWFramesContext *)priv->frame->hw_frames_ctx->data;
        frame_set_colorspace(priv->frame, hwfc->sw_format);

        WaylandDMABUFCopyFrame *cpf = av_buffer_pool_buffer_get_opaque(priv->frame->buf[0]);
        dst = cpf->buffer;
    } else
#endif
    {
        uint32_t wlfmt = priv->scrcpy.shm.format;
        priv->frame->width = priv->scrcpy.shm.width;
        priv->frame->height = priv->scrcpy.shm.height;
        priv->frame->linesize[0] = priv->scrcpy.shm.stride;
        priv->frame->format = drm_wl_fmt_to_pixfmt(NULL, &wlfmt);
        frame_set_colorspace(priv->frame, priv->frame->format);

        priv->frame->buf[0] = av_buffer_pool_get(priv->scrcpy.shm.pool);
        if (!priv->frame->buf[0]) {
            err = AVERROR(ENOMEM);
            goto fail;
        }

        WaylandSHMCopyFrame *cpf = (WaylandSHMCopyFrame *)priv->frame->buf[0]->data;
        priv->frame->data[0] = cpf->data;
        dst = cpf->buffer;
    }

    priv->frame->opaque_ref = av_buffer_allocz(sizeof(FormatExtraData));
    if (!priv->frame->opaque_ref) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    priv->frame->sample_aspect_ratio = av_make_q(1, 1);

    zwlr_screencopy_frame_v1_copy(frame, dst);

    pthread_mutex_unlock(&priv->frame_obj_lock);

    return;

fail:
    cleanup_state(entry, NULL, frame, err);
    pthread_mutex_unlock(&priv->frame_obj_lock);
}

static void scrcpy_frame_flags(void *data, struct zwlr_screencopy_frame_v1 *frame,
                               uint32_t flags)
{
    IOSysEntry *entry = (IOSysEntry *)data;
    WaylandCapturePriv *priv = entry->io_priv;

    pthread_mutex_lock(&priv->frame_obj_lock);

    /* Horizontal flipping - we can do it */
    if (flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT &&
        !priv->frame->hw_frames_ctx) {
        /* Increment the data pointer to the last line */
        priv->frame->data[0] += priv->frame->linesize[0] * (priv->frame->height - 1);
        /* Invert the stride */
        priv->frame->linesize[0] *= -1;
    }

    pthread_mutex_unlock(&priv->frame_obj_lock);
}

static void scrcpy_frame_damages(void *data, struct zwlr_screencopy_frame_v1 *frame,
                                 uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{

}

static void scrcpy_frame_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
                               uint32_t tv_sec_hi, uint32_t tv_sec_lo,
                               uint32_t tv_nsec)
{
    IOSysEntry *entry = (IOSysEntry *)data;
    WaylandCapturePriv *priv = entry->io_priv;

    pthread_mutex_lock(&priv->frame_obj_lock);

    /* Opaque ref */
    FormatExtraData *fe = (FormatExtraData *)priv->frame->opaque_ref->data;
    fe->time_base       = av_make_q(1, 1000000000);
    fe->avg_frame_rate  = priv->frame_rate;

    /* Timestamp of when the frame was presented */
    int64_t presented = ((((uint64_t)tv_sec_hi) << 32) | tv_sec_lo) * 1000000000 + tv_nsec;

    /* Current time */
    struct timespec tsp = { 0 };
    clock_gettime(CLOCK_MONOTONIC, &tsp);

    /* Delay */
    int64_t delay = presented - ((tsp.tv_sec * 1000000000) + tsp.tv_nsec);

    priv->frame->pts = av_add_stable(fe->time_base, delay, av_make_q(1, 1000000),
                                     av_gettime_relative() - priv->epoch);

    sp_log(entry, SP_LOG_TRACE, "Pushing frame to FIFO, pts = %f\n",
           av_q2d(fe->time_base) * priv->frame->pts);

    /* We don't do this check at the start on since there's still some chance
     * whatever's consuming the FIFO will be done by now. */
    int err = sp_frame_fifo_push(entry->frames, priv->frame);
    av_frame_free(&priv->frame);
    if (err == AVERROR(ENOBUFS)) {
        priv->dropped_frames++;
        sp_log(entry, SP_LOG_WARN, "Dropping frame (%i dropped so far)!\n",
               priv->dropped_frames);

        SPGenericData entries[] = {
            D_TYPE("dropped_frames", NULL, priv->dropped_frames),
            { 0 },
        };
        sp_eventlist_dispatch(entry, entry->events, SP_EVENT_ON_STATS, entries);
    } else if (err) {
        sp_log(entry, SP_LOG_ERROR, "Unable to push frame to FIFO: %s!\n",
               av_err2str(err));
        goto fail;
    }

    zwlr_screencopy_frame_v1_destroy(frame);

    /* Framerate limiting */
    if (priv->frame_delay) {
        int64_t now = av_gettime_relative();
        if (priv->next_frame_ts && (now < priv->next_frame_ts)) {
            int64_t wait_time;
            while (1) {
                wait_time = priv->next_frame_ts - now;
                if (wait_time <= 0)
                    break;
                av_usleep(wait_time);
                now = av_gettime_relative();
            }
        }
        priv->next_frame_ts = now + priv->frame_delay;
    }

    schedule_frame(entry);

    pthread_mutex_unlock(&priv->frame_obj_lock);

    return;

fail:
    cleanup_state(entry, NULL, frame, err);
    pthread_mutex_unlock(&priv->frame_obj_lock);
}

static void scrcpy_frame_fail(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
    IOSysEntry *entry = (IOSysEntry *)data;
    WaylandCapturePriv *priv = entry->io_priv;

    pthread_mutex_lock(&priv->frame_obj_lock);

    sp_log(entry, SP_LOG_ERROR, "Copy failed!\n");
    cleanup_state(entry, NULL, frame, AVERROR_EXTERNAL);

    pthread_mutex_unlock(&priv->frame_obj_lock);
}

static const struct zwlr_screencopy_frame_v1_listener scrcpy_frame_listener = {
    .buffer       = scrcpy_frame_info_shm,
    .linux_dmabuf = scrcpy_frame_info_dmabuf,
    .buffer_done  = scrcpy_frame_info_done,

    .flags        = scrcpy_frame_flags,
    .damage       = scrcpy_frame_damages,
    .ready        = scrcpy_frame_ready,
    .failed       = scrcpy_frame_fail,
};

static const struct zwlr_export_dmabuf_frame_v1_listener dmabuf_frame_listener = {
	.frame  = dmabuf_frame_start,
	.object = dmabuf_frame_object,
	.ready  = dmabuf_frame_ready,
	.cancel = dmabuf_frame_cancel,
};

static void dmabuf_register_cb(IOSysEntry *entry)
{
    WaylandCapturePriv *priv = entry->io_priv;
    WaylandIOPriv *api_priv = entry->api_priv;

    struct zwlr_export_dmabuf_frame_v1 *f;
    f = zwlr_export_dmabuf_manager_v1_capture_output(priv->main->wl->dmabuf_export_manager,
                                                     priv->capture_cursor, api_priv->output);
    zwlr_export_dmabuf_frame_v1_add_listener(f, &dmabuf_frame_listener, entry);
    priv->dmabuf.frame_obj = f;
}

static void scrcpy_register_cb(IOSysEntry *entry)
{
    WaylandCapturePriv *priv = entry->io_priv;
    WaylandIOPriv *api_priv = entry->api_priv;

    struct zwlr_screencopy_frame_v1 *f;
    f = zwlr_screencopy_manager_v1_capture_output(priv->main->wl->screencopy_export_manager,
                                                  priv->capture_cursor, api_priv->output);
    zwlr_screencopy_frame_v1_add_listener(f, &scrcpy_frame_listener, entry);
    priv->scrcpy.frame_obj = f;
    priv->scrcpy.shm.got_info = 0;
    priv->scrcpy.dmabuf.got_info = 0;
}

static void schedule_frame(IOSysEntry *entry)
{
    WaylandCapturePriv *priv = entry->io_priv;
    if (priv->oneshot && priv->oneshot++ >= 2) {
        cleanup_state(entry, NULL, NULL, 0);
        sp_frame_fifo_push(entry->frames, NULL);
        return;
    }
    int capture_mode = priv->capture_mode;
    if (capture_mode == CAP_MODE_DMABUF)
        dmabuf_register_cb(entry);
    else
        scrcpy_register_cb(entry);
}

static void destroy_capture(WaylandCapturePriv *priv)
{
    pthread_mutex_lock(&priv->frame_obj_lock);

    /* Destroying the frame objects should destroy everything */
    if (priv->scrcpy.frame_obj)
        zwlr_screencopy_frame_v1_destroy(priv->scrcpy.frame_obj);

    /* av_frame_free also destroys the callback */
    if (priv->dmabuf.frame_obj && !priv->frame)
        zwlr_export_dmabuf_frame_v1_destroy(priv->dmabuf.frame_obj);

    av_frame_free(&priv->frame);

    pthread_mutex_unlock(&priv->frame_obj_lock);

    /* Free anything allocated */
    av_buffer_pool_uninit(&priv->scrcpy.shm.pool);
    av_buffer_unref(&priv->scrcpy.dmabuf.frames_ref);
    av_buffer_unref(&priv->dmabuf.frames_ref);
#ifdef HAVE_GBM
    gbm_device_destroy(priv->scrcpy.dmabuf.gbm_dev);
    priv->scrcpy.dmabuf.gbm_dev = NULL;
#endif
}

static int wlcapture_ioctx_ctrl_cb(AVBufferRef *event_ref, void *callback_ctx, void *ctx,
                                   void *dep_ctx, void *data)
{
    SPCtrlTemplateCbCtx *event = callback_ctx;

    IOSysEntry *entry = ctx;
    WaylandCapturePriv *priv = entry->io_priv;

    if (event->ctrl & SP_EVENT_CTRL_START) {
        priv->epoch = atomic_load(event->epoch);
        schedule_frame(entry);
        wl_display_flush(priv->main->wl->display);
        return sp_eventlist_dispatch(entry, entry->events, SP_EVENT_ON_CONFIG | SP_EVENT_ON_INIT, NULL);
    } else if (event->ctrl & SP_EVENT_CTRL_STOP) {
        pthread_mutex_lock(&priv->frame_obj_lock);
        cleanup_state(entry, priv->dmabuf.frame_obj, priv->scrcpy.frame_obj, 0);
        pthread_mutex_unlock(&priv->frame_obj_lock);
        sp_eventlist_dispatch(entry, entry->events, SP_EVENT_ON_EOS, NULL);
        return 0;
    } else {
        return AVERROR(ENOTSUP);
    }
}

static int wlcapture_ioctx_ctrl(AVBufferRef *entry, SPEventType ctrl, void *arg)
{
    IOSysEntry *iosys_entry = (IOSysEntry *)entry->data;
    return sp_ctrl_template(iosys_entry, iosys_entry->events, 0x0,
                            wlcapture_ioctx_ctrl_cb, ctrl, arg);
}

static int wlcapture_init_io(AVBufferRef *ctx_ref, AVBufferRef *entry,
                             AVDictionary *opts)
{
    int err = 0;
    WaylandCaptureCtx *ctx = (WaylandCaptureCtx *)ctx_ref->data;

    IOSysEntry *iosys_entry = (IOSysEntry *)entry->data;
    if (iosys_entry->io_priv) {
        sp_log(iosys_entry, SP_LOG_ERROR, "Entry is being already captured!\n");
        return AVERROR(EINVAL);
    }

    WaylandCapturePriv *priv = av_mallocz(sizeof(*priv));
    if (!priv)
        return AVERROR(ENOMEM);

    pthread_mutex_init(&priv->frame_obj_lock, NULL);

    const char *oneshot_str = dict_get(opts, "oneshot");
    priv->oneshot = oneshot_str ? !strcmp(oneshot_str, "1") : 0;
    priv->capture_mode = CAP_MODE_SCRCPY;
    priv->capture_cursor = 0;

    /* Options */
    if (dict_get(opts, "capture_cursor"))
        priv->capture_cursor = strtol(dict_get(opts, "capture_cursor"), NULL, 10);
    if (dict_get(opts, "capture_mode")) {
        const char *mode_str = dict_get(opts, "capture_mode");
        if (!strcmp(mode_str, "dmabuf")) {
            priv->capture_mode = CAP_MODE_DMABUF;
        } else if (!strcmp(mode_str, "screencopy")) {
            priv->capture_mode = CAP_MODE_SCRCPY;
#ifdef HAVE_GBM
        } else if (!strcmp(mode_str, "screencopy-dmabuf")) {
            priv->capture_mode = CAP_MODE_SCRCPY_DMABUF;
#endif
        } else {
            sp_log(iosys_entry, SP_LOG_ERROR, "Unsupported capture mode \"%s\"!\n", mode_str);
            av_free(priv);
            err = AVERROR(EINVAL);
            goto end;
        }
    }

    AVRational framerate_req = av_make_q(0, 0);
    if (dict_get(opts, "framerate_num"))
        framerate_req.num = strtol(dict_get(opts, "framerate_num"), NULL, 10);
    if (dict_get(opts, "framerate_den"))
        framerate_req.den = strtol(dict_get(opts, "framerate_den"), NULL, 10);

    if (priv->capture_mode == CAP_MODE_DMABUF && !ctx->wl->dmabuf_export_manager) {
        sp_log(ctx, SP_LOG_ERROR, "DMABUF capture protocol unavailable!\n");
        err = AVERROR(ENOTSUP);
        goto end;
    }

    if (!ctx->wl->screencopy_export_manager && (priv->capture_mode == CAP_MODE_SCRCPY ||
        priv->capture_mode == CAP_MODE_SCRCPY_DMABUF)) {
        sp_log(ctx, SP_LOG_ERROR, "Screencopy protocol unavailable!\n");
        err = AVERROR(ENOTSUP);
        goto end;
    }

    if ((framerate_req.num && !framerate_req.den) ||
        (framerate_req.den && !framerate_req.num) ||
        (av_cmp_q(framerate_req, iosys_entry->framerate) > 0)) {
        sp_log(ctx, SP_LOG_ERROR, "Invalid framerate!\n");
        err = AVERROR(EINVAL);
        goto end;
    } else if ((framerate_req.num && framerate_req.den) &&
               (av_cmp_q(framerate_req, iosys_entry->framerate) != 0)) {
        priv->frame_rate = framerate_req;
        priv->frame_delay = av_rescale_q(1, av_inv_q(framerate_req), AV_TIME_BASE_Q);
    }

    iosys_entry->io_priv = priv;
    iosys_entry->ctrl = wlcapture_ioctx_ctrl;
    iosys_entry->events = sp_bufferlist_new();
    priv->main_ref = av_buffer_ref(ctx_ref);
    priv->main = (WaylandCaptureCtx *)priv->main_ref->data;

#ifdef HAVE_GBM
    AVHWDeviceContext *dev_ctx = (AVHWDeviceContext *)priv->main->wl->drm_device_ref->data;
    AVDRMDeviceContext *pdev_ctx = dev_ctx->hwctx;

    if (!priv->scrcpy.dmabuf.gbm_dev)
        priv->scrcpy.dmabuf.gbm_dev = gbm_create_device(pdev_ctx->fd);
#endif

end:
    return err;
}

static int wlcapture_ctrl(AVBufferRef *ctx_ref, SPEventType ctrl, void *arg)
{
    int err = 0;
    WaylandCaptureCtx *ctx = (WaylandCaptureCtx *)ctx_ref->data;

    if (ctrl & SP_EVENT_CTRL_NEW_EVENT) {
        AVBufferRef *event = arg;
        char *fstr = sp_event_flags_to_str_buf(event);
        sp_log(ctx, SP_LOG_DEBUG, "Registering new event (%s)!\n", fstr);
        av_free(fstr);

        if (ctrl & SP_EVENT_FLAG_IMMEDIATE) {
            /* Bring up the new event to speed with current affairs */
            SPBufferList *tmp_event = sp_bufferlist_new();
            sp_eventlist_add(ctx, tmp_event, event, 1);

            AVBufferRef *obj = NULL;
            while ((obj = sp_bufferlist_iter_ref(ctx->wl->output_list))) {
                sp_eventlist_dispatch(obj->data, tmp_event,
                                      SP_EVENT_ON_CHANGE | SP_EVENT_TYPE_SOURCE,
                                      obj->data);
                av_buffer_unref(&obj);
            }

            sp_bufferlist_free(&tmp_event);
        }

        /* Add it to the list now to receive events dynamically */
        err = sp_eventlist_add(ctx, ctx->events, event, 1);
        if (err < 0)
            return err;
    }

    return 0;
}

static AVBufferRef *wlcapture_ref_entry(AVBufferRef *ctx_ref, uint32_t identifier)
{
    WaylandCaptureCtx *ctx = (WaylandCaptureCtx *)ctx_ref->data;
    return sp_bufferlist_ref(ctx->wl->output_list, sp_bufferlist_iosysentry_by_id, &identifier);
}

static void wlcapture_uninit(void *opaque, uint8_t *data)
{
    WaylandCaptureCtx *ctx = (WaylandCaptureCtx *)data;

    sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_DESTROY, NULL);
    sp_bufferlist_free(&ctx->events);

    av_buffer_unref(&ctx->wl_ref);

    sp_class_free(ctx);
    av_free(ctx);
}

static int wlcapture_init(AVBufferRef **s)
{
    int err = 0;
    WaylandCaptureCtx *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return AVERROR(ENOMEM);

    AVBufferRef *ctx_ref = av_buffer_create((uint8_t *)ctx, sizeof(*ctx),
                                            wlcapture_uninit, NULL, 0);
    if (!ctx_ref) {
        av_free(ctx);
        return AVERROR(ENOMEM);
    }

    err = sp_wayland_create(&ctx->wl_ref);
    if (err < 0)
        goto fail;

    ctx->wl = (WaylandCtx *)ctx->wl_ref->data;

    err = sp_class_alloc(ctx, src_wayland.name, SP_TYPE_VIDEO_SOURCE, ctx->wl);
    if (err < 0)
        goto fail;

    if (!ctx->wl->dmabuf_export_manager && !ctx->wl->screencopy_export_manager) {
        sp_log(ctx, SP_LOG_WARN, "Compositor doesn't support any capture protocol!\n");
        err = AVERROR(ENOTSUP);
        goto fail;
    }

    if (!ctx->wl->dmabuf_export_manager) {
        sp_log(ctx, SP_LOG_INFO, "Compositor doesn't support the %s protocol, "
               "display DMABUF capture unavailable!\n",
               zwlr_export_dmabuf_manager_v1_interface.name);
    } else {
        sp_log(ctx, SP_LOG_DEBUG, "Compositor supports the %s protocol\n",
               zwlr_export_dmabuf_manager_v1_interface.name);
    }

    if (!ctx->wl->screencopy_export_manager) {
        sp_log(ctx, SP_LOG_INFO, "Compositor doesn't support the %s protocol, "
               "display screencopy capture unavailable!\n",
               zwlr_screencopy_manager_v1_interface.name);
    } else {
        sp_log(ctx, SP_LOG_DEBUG, "Compositor supports the %s protocol\n",
               zwlr_screencopy_manager_v1_interface.name);
    }

    ctx->events = sp_bufferlist_new();
    if (!ctx->events) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    *s = ctx_ref;

    return 0;

fail:
    av_buffer_unref(&ctx_ref);

    return err;
}

const IOSysAPI src_wayland = {
    .name      = "wayland",
    .ctrl      = wlcapture_ctrl,
    .init_sys  = wlcapture_init,
    .ref_entry = wlcapture_ref_entry,
    .init_io   = wlcapture_init_io,
};
