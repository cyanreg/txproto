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

#include <pthread.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext_vulkan.h>

#include <libplacebo/vulkan.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/icc.h>

#include "interface_common.h"
#include "utils.h"
#include "logging.h"
#include "../config.h"

#define DEFAULT_USAGE_FLAGS (VK_IMAGE_USAGE_SAMPLED_BIT      |                 \
                             VK_IMAGE_USAGE_STORAGE_BIT      |                 \
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT |                 \
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT)

/* 1 to 1 map (even indices are the same */
enum pl_chroma_location placebo_chroma_loc_map[AVCHROMA_LOC_NB] = {
    [AVCHROMA_LOC_UNSPECIFIED] = PL_CHROMA_UNKNOWN,
    [AVCHROMA_LOC_LEFT]        = PL_CHROMA_LEFT,
    [AVCHROMA_LOC_CENTER]      = PL_CHROMA_CENTER,
    [AVCHROMA_LOC_TOPLEFT]     = PL_CHROMA_TOP_LEFT,
    [AVCHROMA_LOC_TOP]         = PL_CHROMA_TOP_CENTER,
    [AVCHROMA_LOC_BOTTOMLEFT]  = PL_CHROMA_BOTTOM_LEFT,
    [AVCHROMA_LOC_BOTTOM]      = PL_CHROMA_BOTTOM_CENTER,
};

/* 1 to 1 map */
enum pl_color_levels placebo_color_range_map[AVCOL_RANGE_NB] = {
    [AVCOL_RANGE_UNSPECIFIED] = PL_COLOR_LEVELS_UNKNOWN,
    [AVCOL_RANGE_MPEG]        = PL_COLOR_LEVELS_TV,
    [AVCOL_RANGE_JPEG]        = PL_COLOR_LEVELS_PC,
};

static enum pl_color_primaries avprim_to_placebo(enum AVColorPrimaries pri)
{
    switch (pri) {
    case AVCOL_PRI_UNSPECIFIED: return PL_COLOR_PRIM_UNKNOWN;
    case AVCOL_PRI_SMPTE170M:   return PL_COLOR_PRIM_BT_601_525;
    case AVCOL_PRI_SMPTE240M:   return PL_COLOR_PRIM_BT_601_525;
    case AVCOL_PRI_BT470BG:     return PL_COLOR_PRIM_BT_601_625;
    case AVCOL_PRI_BT709:       return PL_COLOR_PRIM_BT_709;
    case AVCOL_PRI_BT470M:      return PL_COLOR_PRIM_BT_470M;
    case AVCOL_PRI_BT2020:      return PL_COLOR_PRIM_BT_2020;
    case AVCOL_PRI_SMPTE428:    return PL_COLOR_PRIM_CIE_1931;
    case AVCOL_PRI_SMPTE431:    return PL_COLOR_PRIM_DCI_P3;
    case AVCOL_PRI_SMPTE432:    return PL_COLOR_PRIM_DISPLAY_P3;
    default:                    return PL_COLOR_PRIM_UNKNOWN;
    }
}

static enum pl_color_transfer avtrc_to_placebo(enum AVColorTransferCharacteristic trc)
{
    switch (trc) {
    case AVCOL_TRC_UNSPECIFIED:  return PL_COLOR_TRC_UNKNOWN;
    case AVCOL_TRC_BT709:        return PL_COLOR_TRC_BT_1886;
    case AVCOL_TRC_IEC61966_2_1: return PL_COLOR_TRC_SRGB;
    case AVCOL_TRC_LINEAR:       return PL_COLOR_TRC_LINEAR;
    case AVCOL_TRC_GAMMA22:      return PL_COLOR_TRC_GAMMA22;
    case AVCOL_TRC_GAMMA28:      return PL_COLOR_TRC_GAMMA28;
    case AVCOL_TRC_SMPTE2084:    return PL_COLOR_TRC_PQ;
    case AVCOL_TRC_ARIB_STD_B67: return PL_COLOR_TRC_HLG;
    default:                     return PL_COLOR_TRC_UNKNOWN;
    }
}

static enum pl_color_system avspc_to_placebo(enum AVColorSpace csp)
{
    switch (csp) {
    case AVCOL_SPC_UNSPECIFIED: return PL_COLOR_SYSTEM_UNKNOWN;
    case AVCOL_SPC_BT470BG:     return PL_COLOR_SYSTEM_BT_601;
    case AVCOL_SPC_BT709:       return PL_COLOR_SYSTEM_BT_709;
    case AVCOL_SPC_SMPTE170M:   return PL_COLOR_SYSTEM_SMPTE_240M;
    case AVCOL_SPC_SMPTE240M:   return PL_COLOR_SYSTEM_SMPTE_240M;
    case AVCOL_SPC_BT2020_NCL:  return PL_COLOR_SYSTEM_BT_2020_NC;
    case AVCOL_SPC_BT2020_CL:   return PL_COLOR_SYSTEM_BT_2020_C;
    case AVCOL_SPC_ICTCP:       return PL_COLOR_SYSTEM_BT_2100_PQ;
    case AVCOL_SPC_YCGCO:       return PL_COLOR_SYSTEM_YCGCO;
    case AVCOL_SPC_RGB:         return PL_COLOR_SYSTEM_RGB;
    default:                    return PL_COLOR_SYSTEM_UNKNOWN;
    }
}

typedef struct InterfaceWindowCtx {
    SPClass *class;

    struct InterfaceCtx *main;

    struct InterfaceWindowCtx *root;
    pthread_mutex_t lock;

    SPBufferList *events;

    enum SurfaceType type;

    struct {
        int x, y;
    } cursor;

    struct {
        int x0, x1, y0, y1;
        int state_x0, state_x1, state_y0, state_y1;
        uint32_t flags;
        const struct pl_tex *region;
    } highlight;

    struct {
        AVBufferRef *dst;
    } overlay;

    struct {
        AVBufferRef *fifo;
    } main_win;

    /* State */
    int is_dirty;
    int width;
    int height;
    float scale;
    AVFrame *last_frame;

    /* Windowing system context */
    void *wctx;

    /* Hardware device */
    AVBufferRef *device_ref;
    AVBufferRef *frames_ref;

    InterfaceCB cb;

    /* Renderer */
    struct pl_vulkan_swapchain_params pl_swap_params;
    const struct pl_swapchain *pl_swap;
    const struct pl_vulkan *pl_vk_ctx;
    const struct pl_gpu *pl_gpu;
    struct pl_renderer *pl_renderer;
} InterfaceWindowCtx;

typedef struct InterfaceCtx {
    SPClass *class;

    SPBufferList *events;

    /* Main windowing system context */
    void *window_ctx;
    InterfaceSystem *sys;
    AVBufferRef *device_ref;
    enum VkPresentModeKHR present_mode;

    InterfaceWindowCtx main_win;
    InterfaceWindowCtx highlight_win;

    struct {
        SPClass *class;
        struct pl_context *ctx;
    } placebo;
} InterfaceCtx;

#include "interface_main.h"
#include "interface_highlight.h"

/* Do not call from interface, call win->main->sys->surf_destroy instead */
static void destroy_win(void *wctx)
{
    InterfaceWindowCtx *win = wctx;
    pthread_mutex_lock(&win->lock);

    pl_tex_destroy(win->pl_gpu, &win->highlight.region);
    pl_swapchain_destroy(&win->pl_swap);
    pl_renderer_destroy(&win->pl_renderer);

    av_frame_free(&win->last_frame);
    av_buffer_unref(&win->frames_ref);
    av_buffer_unref(&win->device_ref);

    pthread_mutex_unlock(&win->lock);
    pthread_mutex_destroy(&win->lock);
}

static void state_win(void *wctx, int is_fs, float scale)
{
    InterfaceWindowCtx *win = wctx;
    win->scale = scale;
}

static int resize_win(void *wctx, int *w, int *h, int active_resize)
{
    InterfaceWindowCtx *win = wctx;

    if ((*w == win->width) && (*h == win->height) && win->pl_swap)
        return 0;

    if (!active_resize || !win->pl_swap) {
        pl_swapchain_destroy(&win->pl_swap);
        win->pl_swap = pl_vulkan_create_swapchain(win->pl_vk_ctx, &win->pl_swap_params);
        if (!win->pl_swap) {
            sp_log(win->main, SP_LOG_ERROR, "Error creating libplacebo swapchain!\n");
            return AVERROR_EXTERNAL;
        }
    }

    pl_swapchain_resize(win->pl_swap, w, h);

    win->width = *w;
    win->height = *h;
    win->is_dirty = 1;

    return 0;
}

static int common_windows_init(InterfaceCtx *ctx, InterfaceWindowCtx *win,
                               const char *title)
{
    int err = 0;

    win->events = sp_bufferlist_new();
    if (!win->events)
        return AVERROR(ENOMEM);

    pthread_mutex_init(&win->lock, NULL);
    win->cursor.x     = INT32_MIN;
    win->cursor.y     = INT32_MIN;
    win->main         = ctx;
    win->width        = win->width ? win->width :-1;
    win->height       = win->height ? win->height : -1;
    win->scale        = 1.0f;
    win->device_ref   = av_buffer_ref(ctx->device_ref);
    if (!win->device_ref) {
        sp_bufferlist_free(&ctx->events);
        return AVERROR(ENOMEM);
    }

    win->highlight.x0       = INT32_MIN;
    win->highlight.y0       = INT32_MIN;
    win->highlight.x1       = INT32_MIN;
    win->highlight.y1       = INT32_MIN;
    win->highlight.state_x0 = INT32_MIN;
    win->highlight.state_x1 = INT32_MIN;
    win->highlight.state_y0 = INT32_MIN;
    win->highlight.state_y1 = INT32_MIN;

    win->cb.destroy   = destroy_win;
    win->cb.resize    = resize_win;
    win->cb.state     = state_win;

    win->is_dirty = 1;

    win->pl_swap_params = (struct pl_vulkan_swapchain_params) {
        .present_mode = ctx->present_mode,
        .allow_suboptimal = true,
    };

    AVHWDeviceContext *ref_data = (AVHWDeviceContext*)ctx->device_ref->data;
    AVVulkanDeviceContext *hwctx = ref_data->hwctx;

    /* First, create the window with whatever windowing system we're using,
     * and let it give us the VkSurface to render into */
    err = ctx->sys->surf_init(ctx->window_ctx, &win->wctx, win->overlay.dst,
                              &win->cb, win, title, win->width, win->height,
                              win->type, hwctx->inst, &win->pl_swap_params.surface);
    if (err < 0) {
        sp_bufferlist_free(&ctx->events);
        av_buffer_unref(&win->device_ref);
        return err;
    }

    /* Now that we have it, init the libplacebo contexts */
    struct pl_vulkan_import_params vkparams = { 0 };
    vkparams.instance             = hwctx->inst;
    vkparams.phys_device          = hwctx->phys_dev;
    vkparams.device               = hwctx->act_dev;
    vkparams.extensions           = hwctx->enabled_dev_extensions;
    vkparams.num_extensions       = hwctx->nb_enabled_dev_extensions;
    vkparams.queue_graphics.index = hwctx->queue_family_index;
    vkparams.queue_graphics.count = hwctx->nb_graphics_queues;
    vkparams.queue_compute.index  = hwctx->queue_family_comp_index;
    vkparams.queue_compute.count  = hwctx->nb_comp_queues;
    vkparams.queue_transfer.index = hwctx->queue_family_tx_index;
    vkparams.queue_transfer.count = hwctx->nb_tx_queues;
    vkparams.features             = &hwctx->device_features;

    win->pl_vk_ctx = pl_vulkan_import(ctx->placebo.ctx, &vkparams);
    if (!win->pl_vk_ctx) {
        sp_log(ctx, SP_LOG_ERROR, "Error creating libplacebo context!\n");
        sp_bufferlist_free(&ctx->events);
        av_buffer_unref(&win->device_ref);
        return AVERROR_EXTERNAL;
    }

    /* Set the rendering GPU */
    win->pl_gpu = win->pl_vk_ctx->gpu;

    /* Set the renderer */
    win->pl_renderer = pl_renderer_create(ctx->placebo.ctx, win->pl_gpu);

    return 0;
}

/*
static int create_window(InterfaceCtx *ctx, InterfaceWindowCtx *win,
                         InterfaceWindowCtx *main, const char *title,
                         int init_w, int init_h,
                         void *in_frames, enum SurfaceType type)
{
    void *ref_wctx = NULL;
    AVHWDeviceContext *ref_data = (AVHWDeviceContext*)ctx->device_ref->data;
    AVVulkanDeviceContext *hwctx = ref_data->hwctx;

    win->root         = main;


    switch (type) {
    case STYPE_MAIN:
        win->cb.mouse_mov_cb = mouse_move_main;
        win->cb.input        = win_input_main;
        win->cb.render       = render_main;
        break;
    case STYPE_OVERLAY:
        if (!main || main->type != STYPE_MAIN) {
            sp_log(ctx, SP_LOG_ERROR, "Invalid reference window for overlay!\n");
            return AVERROR(EINVAL);
        }
        ref_wctx = main->wctx;
        break;
    case STYPE_HIGHLIGHT:
        break;
    }

    return 0;
}

static int interface_create_main_win(InterfaceCtx *ctx, InterfaceWindowCtx *win,
                                     const char *title, int init_w, int init_h)
{
    return create_window(ctx, win, NULL, title, init_w, init_h, NULL, STYPE_MAIN);
}

static int interface_create_highlight_win(InterfaceCtx *ctx, InterfaceWindowCtx *win,
                                          const char *title)
{
    return create_window(ctx, win, NULL, title, -1, -1, NULL, STYPE_HIGHLIGHT);
}
*/

AVBufferRef *sp_interface_highlight_win(AVBufferRef *ref, const char *title,
                                        AVBufferRef *event)
{
    InterfaceCtx *ctx = (InterfaceCtx *)ref->data;

    AVBufferRef *win_ref = av_buffer_allocz(sizeof(InterfaceWindowCtx));
    InterfaceWindowCtx *win = (InterfaceWindowCtx *)win_ref->data;

    int err = sp_class_alloc(win, title, SP_TYPE_INTERFACE, ctx);
    if (err < 0) {
        av_buffer_unref(&win_ref);
        return NULL;
    }

    win->type             = STYPE_HIGHLIGHT;
    win->cb.mouse_mov_cb  = mouse_move_highlight;
    win->cb.input         = win_input_highlight;
    win->cb.render        = render_highlight;

    common_windows_init(ctx, win, title ? title : NULL);

    sp_eventlist_add(win, win->events, event);

    /* Signal to the window system we're ready to render */
    ctx->sys->surf_done(win->wctx);

    return win_ref;
}

int interface_send_fifo_to_main(void *s, AVBufferRef *fifo)
{
    InterfaceCtx *ctx = s;

    pthread_mutex_lock(&ctx->main_win.lock);

//    ctx->main_win.main_win.fifo = sp_frame_fifo_tap(fifo, 1);

    pthread_mutex_unlock(&ctx->main_win.lock);

    return 0;
}

static void log_cb_pl(void *ctx, enum pl_log_level level, const char *msg)
{
    int sp_level;

    switch (level) {
    case PL_LOG_FATAL:  sp_level = SP_LOG_FATAL;   break;
    case PL_LOG_ERR:    sp_level = SP_LOG_ERROR;   break;
    case PL_LOG_WARN:   sp_level = SP_LOG_WARN;    break;
    case PL_LOG_INFO:   sp_level = SP_LOG_DEBUG;   break; /* Too spammy for info */
    case PL_LOG_DEBUG:  sp_level = SP_LOG_TRACE;   break;
    case PL_LOG_TRACE:
    default: return;
    }

    sp_log(ctx, sp_level, "%s\n", msg);
}

static void interface_uninit(void *opaque, uint8_t *data)
{
    av_free(data);
}

int sp_interface_init(AVBufferRef **s)
{
    int err = 0;
    InterfaceCtx *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return AVERROR(ENOMEM);

    AVBufferRef *ctx_ref = av_buffer_create((uint8_t *)ctx, sizeof(*ctx),
                                            interface_uninit, NULL, 0);
    if (!ctx_ref) {
        av_free(ctx);
        return AVERROR(ENOMEM);
    }

    err = sp_class_alloc(ctx, "interface", SP_TYPE_CONTEXT, NULL);
    if (err < 0)
        goto fail;

    ctx->events = sp_bufferlist_new();
    if (!ctx->events) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    /* Init graphics backend */
    AVBufferRef *render_dev = NULL;
    const char *vulkan_inst_ext = NULL;
    ctx->present_mode = VK_PRESENT_MODE_FIFO_KHR;

    for (int i = 0; i < sp_compiled_interfaces_len; i++) {
        render_dev = NULL;
        vulkan_inst_ext = NULL;
        ctx->present_mode = VK_PRESENT_MODE_FIFO_KHR;

        ctx->sys = sp_compiled_interfaces[i];
        ctx->sys->init(&ctx->window_ctx, &render_dev, &vulkan_inst_ext,
                       &ctx->present_mode);
        if (err >= 0)
            break;

        ctx->sys = NULL;
    }

    if (!ctx->sys) {
        sp_log(ctx, SP_LOG_WARN, "No VOs available for output\n");
        err = 0;
        goto fail;
    }

    /* Graphics device options */
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "debug", "0", 0);
    av_dict_set(&opts, "linear_images", "0", 0);

    /* Instance extensions */
    av_dict_set(&opts, "instance_extensions", "VK_KHR_surface" "+"
                                              "VK_KHR_get_physical_device_properties2", 0);
    if (vulkan_inst_ext) {
        av_dict_set(&opts, "instance_extensions", "+", AV_DICT_APPEND);
        av_dict_set(&opts, "instance_extensions", vulkan_inst_ext, AV_DICT_APPEND);
    }

    /* Device extensions */
    av_dict_set(&opts, "device_extensions", "VK_KHR_swapchain", 0);

    for (int i = 0; i < pl_vulkan_num_recommended_extensions; i++) {
        av_dict_set(&opts, "device_extensions", "+", AV_DICT_APPEND);
        av_dict_set(&opts, "device_extensions",
                    pl_vulkan_recommended_extensions[i], AV_DICT_APPEND);
    }

    /* Derive/create graphics device */
    if (render_dev) {
        err = av_hwdevice_ctx_create_derived_opts(&ctx->device_ref,
                                                  AV_HWDEVICE_TYPE_VULKAN,
                                                  render_dev, opts, 0);
    } else {
        err = av_hwdevice_ctx_create(&ctx->device_ref, AV_HWDEVICE_TYPE_VULKAN,
                                     NULL, opts, 0);
    }

    if (err < 0) {
        sp_log(ctx, SP_LOG_WARN, "Unable to %s device: %s!\n",
               render_dev ? "derive" : "create", av_err2str(err));
        goto fail;
    }

    /* libplacebo logging context */
    err = sp_class_alloc(&ctx->placebo, "placebo", SP_TYPE_EXTERNAL, ctx);
    if (err < 0)
        goto fail;

    ctx->placebo.ctx = pl_context_create(PL_API_VER, &(struct pl_context_params) {
        .log_cb    = log_cb_pl,
        .log_priv  = &ctx->placebo,
        .log_level = PL_LOG_TRACE,
    });

    *s = ctx_ref;

    return 0;

fail:
    av_buffer_unref(&ctx_ref);

    return err; 
}
