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

#define USE_LAVU 0

static enum MouseStyle mouse_move_main(void *wctx, int x, int y)
{
    InterfaceWindowCtx *win = wctx;
    pthread_mutex_lock(&win->lock);

    win->cursor.x = x;
    win->cursor.y = y;

    pthread_mutex_unlock(&win->lock);
    return MSTYLE_DEFAULT;
}

static int win_input_main(void *wctx, uint64_t val)
{
    InterfaceWindowCtx *win = wctx;
    pthread_mutex_lock(&win->lock);

    uint64_t flags = val & SP_KEY_FLAG_MASK;
    val &= ~SP_KEY_FLAG_MASK;

    switch (val) {
    case 'c':
        if (val == 'c' && !(flags & SP_KEY_FLAG_CTRL)) {
            pthread_mutex_unlock(&win->lock);
            win->main->sys->surf_destroy(&win->wctx);
            return 0;
        }
        break;
    };

    pthread_mutex_unlock(&win->lock);
    return 0;
}

#if USE_LAVU
static int get_frames_ref(InterfaceWindowCtx *win, AVFrame *in_f)
{
    int err = 0;
    AVBufferRef *input_frames_ref = NULL;
    AVHWFramesContext *hwfc = NULL, *in_hwfc = NULL;
    AVVulkanFramesContext *vkfc;

    if (win->frames_ref)
        hwfc = (AVHWFramesContext *)win->frames_ref->data;

    if (in_f->hw_frames_ctx) {
        input_frames_ref = in_f->hw_frames_ctx;
        in_hwfc = (AVHWFramesContext *)input_frames_ref->data;
    }

    if (hwfc && in_hwfc) {
        if ((in_hwfc->width == hwfc->width) && (in_hwfc->height == hwfc->height) &&
            (in_hwfc->sw_format == hwfc->sw_format))
            return 0;
    } else if (hwfc) {
        if ((in_f->width == hwfc->width) && (in_f->height == hwfc->height) &&
            (in_f->format == hwfc->sw_format))
            return 0;
    }

    if (win->frames_ref)
        av_buffer_unref(&win->frames_ref);

    if (input_frames_ref) {
        err = av_hwframe_ctx_create_derived(&win->frames_ref, AV_PIX_FMT_VULKAN,
                                            win->device_ref, input_frames_ref, 0);
        if (err < 0)
            sp_log(win->main, SP_LOG_WARN, "Could not derive hardware frames context: %s!\n",
                   av_err2str(err));
        else
            return 0;
    }

    win->frames_ref = av_hwframe_ctx_alloc(win->device_ref);
    if (!win->frames_ref)
        return AVERROR(ENOMEM);

    hwfc = (AVHWFramesContext *)win->frames_ref->data;
    hwfc->format = AV_PIX_FMT_VULKAN;

    if (in_hwfc) {
        hwfc->sw_format = in_hwfc->sw_format;
        hwfc->width     = in_hwfc->width;
        hwfc->height    = in_hwfc->height;
    } else {
        hwfc->sw_format = in_f->format;
        hwfc->width     = in_f->width;
        hwfc->height    = in_f->height;
    }

    vkfc = (AVVulkanFramesContext *)hwfc->hwctx;

    vkfc->flags = AV_VK_FRAME_FLAG_NONE;
    vkfc->tiling = VK_IMAGE_TILING_LINEAR;

    err = av_hwframe_ctx_init(win->frames_ref);
    if (err < 0) {
        sp_log(win->main, SP_LOG_ERROR, "Could not init hardware frames context: %s!\n",
               av_err2str(err));
        av_buffer_unref(&win->frames_ref);
        return err;
    }

    return 0;
}
#endif

static int get_render_source(InterfaceWindowCtx *win, struct pl_frame *dst,
                             AVFrame **src, double *dar)
{
    int ret;

    if (!*src) {
        if (win->main_win.have_last) {
            memcpy(dst, &win->main_win.last, sizeof(*dst));
            *dar = win->main_win.last_dar;
            return 0;
        } else {
            return AVERROR(EAGAIN);
        }
    }

    if ((*src)->sample_aspect_ratio.num)
        *dar = av_q2d((*src)->sample_aspect_ratio);
    else
        *dar = 1.0;

#if USE_LAVU
    /* Generate new frames reference */
    ret = get_frames_ref(win, *src);
    if (ret < 0) {
        av_frame_free(src);
        return ret;
    }

    /* Map frame */
    if ((*src)->hw_frames_ctx && ((*src)->format != AV_PIX_FMT_VULKAN)) {
        AVFrame *mapped_frame = av_frame_alloc();
        if (!mapped_frame) {
            av_frame_free(src);
            return AVERROR(ENOMEM);
        }

        AVHWFramesContext *mapped_hwfc;
        mapped_hwfc = (AVHWFramesContext *)win->frames_ref->data;
        mapped_frame->format = mapped_hwfc->format;

        /* Set frame hardware context referencce */
        mapped_frame->hw_frames_ctx = av_buffer_ref(win->frames_ref);
        if (!mapped_frame->hw_frames_ctx) {
            av_frame_free(&mapped_frame);
            av_frame_free(src);
            return AVERROR(ENOMEM);
        }

        ret = av_hwframe_map(mapped_frame, *src, AV_HWFRAME_MAP_READ);
        if (ret < 0) {
            sp_log(win->main, SP_LOG_ERROR, "Error mapping: %s!\n",
                   av_err2str(ret));
            av_frame_free(&mapped_frame);
            av_frame_free(src);
            return ret;
        }

        /* Copy properties */
        av_frame_copy_props(mapped_frame, *src);

        /* Replace original frame with mapped */
        av_frame_free(src);
        *src = mapped_frame;
    } else if (!(*src)->hw_frames_ctx) { /* Upload */
        AVFrame *tx_frame = av_frame_alloc();
        if (!tx_frame) {
            av_frame_free(src);
            return AVERROR(ENOMEM);
        }

        AVHWFramesContext *enc_hwfc;
        enc_hwfc = (AVHWFramesContext *)win->frames_ref->data;

        tx_frame->format = enc_hwfc->format;
        tx_frame->width  = enc_hwfc->width;
        tx_frame->height = enc_hwfc->height;

        /* Set frame hardware context referencce */
        tx_frame->hw_frames_ctx = av_buffer_ref(win->frames_ref);
        if (!tx_frame->hw_frames_ctx) {
            av_frame_free(&tx_frame);
            av_frame_free(src);
            return AVERROR(ENOMEM);
        }

        /* Get hardware frame buffer */
        av_hwframe_get_buffer(win->frames_ref, tx_frame, 0);

        /* Transfer data */
        ret = av_hwframe_transfer_data(tx_frame, *src, 0);
        if (ret < 0) {
            sp_log(win->main, SP_LOG_ERROR, "Error transferring: %s!\n",
                   av_err2str(ret));
            av_frame_free(&tx_frame);
            av_frame_free(src);
            return ret;
        }

        av_frame_copy_props(tx_frame, *src);
        av_frame_free(src);
        *src = tx_frame;
    }
#endif

    struct pl_avframe_params map_params = {
        .frame    = *src,
        .tex      = win->main_win.tex,
        .map_dovi = true,
    };

    ret = pl_map_avframe_ex(win->pl_gpu, dst, &map_params);
    av_frame_free(src);
    if (!ret) {
        sp_log(win->main, SP_LOG_ERROR, "Could not map AVFrame to libplacebo!\n");
        return AVERROR(EINVAL);
    }

    if (win->main_win.have_last)
        pl_unmap_avframe(win->pl_gpu, &win->main_win.last);
    win->main_win.have_last = 1;
    win->main_win.last_dar = *dar;
    memcpy(&win->main_win.last, dst, sizeof(*dst));

    return 0;
}

static int render_main(void *wctx)
{
    int ret = 0;
    AVFrame *in_f = NULL;
    InterfaceWindowCtx *win = wctx;

    pthread_mutex_lock(&win->lock);

    /* We don't need to render */
    if (!win->is_dirty)
        goto end;

    /* Begin rendering */
    struct pl_swapchain_frame frame;
    pl_swapchain_start_frame(win->pl_swap, &frame);

    /* Clear screen */
    float rgba[4] = { 0.0, 0.0, 0.0, 1.0 };
    pl_tex_clear(win->pl_gpu, frame.fbo, rgba);

    /* Check if we're connected at all to render */
    if (!win->main_win.fifo)
        goto finish;

    double dar;
    struct pl_frame src;

    /* Get source frame */
    in_f = sp_frame_fifo_pop(win->main_win.fifo);
    if ((ret = get_render_source(win, &src, &in_f, &dar)) < 0)
        goto finish;

    /* Set rendering parameters */
    struct pl_render_params render_params = pl_render_default_params;
    render_params.upscaler = &pl_filter_triangle;
    render_params.downscaler = NULL;
    render_params.sigmoid_params = NULL;
    render_params.color_adjustment = &pl_color_adjustment_neutral;
    render_params.peak_detect_params = NULL;
    render_params.color_map_params = &pl_color_map_default_params;
    render_params.dither_params = NULL;
    render_params.icc_params = &pl_icc_default_params;
    render_params.skip_anti_aliasing = 1;
    render_params.allow_delayed_peak_detect = 1;

    /* Set rendering target params */
    struct pl_frame target = { 0 };
    pl_frame_from_swapchain(&target, &frame);

    target.crop.x0 = 0;
    target.crop.y0 = 0;
    target.crop.x1 = win->width;
    target.crop.y1 = win->height;

    pl_rect2df_aspect_set_rot(&target.crop, dar * pl_rect2df_aspect(&src.crop),
                              src.rotation - target.rotation,
                              0.0);

    pl_render_image(win->pl_renderer, &src, &target, &render_params);

finish:
    if (!(ret = pl_swapchain_submit_frame(win->pl_swap)))
        sp_log(win->main, SP_LOG_ERROR, "Could not submit swapchain frame!\n");

    if (ret) {
        pl_swapchain_swap_buffers(win->pl_swap);
        win->is_dirty = 1;
    }

    ret = !ret ? AVERROR(EINVAL) : 0;

end:
    pthread_mutex_unlock(&win->lock);

    return ret;
}
