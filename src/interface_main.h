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

static int get_frames_ref(InterfaceWindowCtx *win, AVFrame *in_f)
{
    int err = 0;
    AVBufferRef *input_frames_ref = NULL;
    AVHWFramesContext *hwfc = NULL, *in_hwfc = NULL;

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

    hwfc = (AVHWFramesContext*)win->frames_ref->data;
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

    err = av_hwframe_ctx_init(win->frames_ref);
    if (err < 0) {
        sp_log(win->main, SP_LOG_ERROR, "Could not init hardware frames context: %s!\n",
               av_err2str(err));
        av_buffer_unref(&win->frames_ref);
        return err;
    }

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

    /* Get new frame */
    if (!win->main_win.fifo)
        goto finish;

//    in_f = sp_frame_fifo_pop(win->main_win.fifo);
    if (!in_f) { /* Try to render old */
        if (!win->last_frame)
            goto finish;
        in_f = win->last_frame;
    }

    /* Generate new frames reference */
    ret = get_frames_ref(win, in_f);
    if (ret < 0)
        goto finish;

    /* Map frame */
    if (in_f->hw_frames_ctx && (in_f->format != AV_PIX_FMT_VULKAN)) {
        AVFrame *mapped_frame = av_frame_alloc();
        if (!mapped_frame) {
            ret = AVERROR(ENOMEM);
            goto finish;
        }

        AVHWFramesContext *mapped_hwfc;
        mapped_hwfc = (AVHWFramesContext *)win->frames_ref->data;
        mapped_frame->format = mapped_hwfc->format;

        /* Set frame hardware context referencce */
        mapped_frame->hw_frames_ctx = av_buffer_ref(win->frames_ref);
        if (!mapped_frame->hw_frames_ctx) {
            ret = AVERROR(ENOMEM);
            av_frame_free(&mapped_frame);
            goto finish;
        }

        ret = av_hwframe_map(mapped_frame, in_f, AV_HWFRAME_MAP_READ);
        if (ret < 0) {
            sp_log(win->main, SP_LOG_ERROR, "Error mapping: %s!\n",
                   av_err2str(ret));
            av_frame_free(&mapped_frame);
            goto finish;
        }

        /* Copy properties */
        av_frame_copy_props(mapped_frame, in_f);

        /* Replace original frame with mapped */
        av_frame_free(&in_f);
        in_f = mapped_frame;
    } else if (!in_f->hw_frames_ctx) { /* Upload */
        AVFrame *tx_frame = av_frame_alloc();
        if (!tx_frame) {
            ret = AVERROR(ENOMEM);
            goto finish;
        }

        AVHWFramesContext *enc_hwfc;
        enc_hwfc = (AVHWFramesContext *)win->frames_ref->data;

        tx_frame->format = enc_hwfc->format;
        tx_frame->width  = enc_hwfc->width;
        tx_frame->height = enc_hwfc->height;

        /* Set frame hardware context referencce */
        tx_frame->hw_frames_ctx = av_buffer_ref(win->frames_ref);
        if (!tx_frame->hw_frames_ctx) {
            ret = AVERROR(ENOMEM);
            av_frame_free(&tx_frame);
            goto finish;
        }

        /* Get hardware frame buffer */
        av_hwframe_get_buffer(win->frames_ref, tx_frame, 0);

        /* Transfer data */
        ret = av_hwframe_transfer_data(tx_frame, in_f, 0);
        if (ret < 0) {
            sp_log(win->main, SP_LOG_ERROR, "Error transferring: %s!\n",
                   av_err2str(ret));
            av_frame_free(&tx_frame);
            goto finish;
        }

        av_frame_copy_props(tx_frame, in_f);
        av_frame_free(&in_f);
        in_f = tx_frame;
    }

    AVHWFramesContext *hwfc = (AVHWFramesContext *)win->frames_ref->data;
    AVVkFrame *vkf = (AVVkFrame *)in_f->data[0];

    const int nb_planes = av_pix_fmt_count_planes(hwfc->sw_format);
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(hwfc->sw_format);
    const VkFormat *vk_fmt = av_vkfmt_from_pixfmt(hwfc->sw_format);

    struct pl_image plimg = (struct pl_image) {
        .signature = (uint64_t)in_f->pts,

        .color.primaries = avprim_to_placebo(in_f->color_primaries),
        .color.transfer = avtrc_to_placebo(in_f->color_trc),
        .color.light = PL_COLOR_LIGHT_DISPLAY,

        .repr.sys = avspc_to_placebo(in_f->colorspace),
        .repr.levels = placebo_color_range_map[in_f->color_range],
        .repr.alpha = PL_ALPHA_INDEPENDENT,

        .repr.bits.bit_shift = desc->comp[0].shift,
        .repr.bits.color_depth = desc->comp[0].depth,
        .repr.bits.sample_depth = 8,

        .src_rect.x0 = in_f->crop_left,
        .src_rect.y0 = in_f->crop_top,
        .src_rect.x1 = in_f->width - in_f->crop_right,
        .src_rect.y1 = in_f->height - in_f->crop_bottom,

        .num_planes = nb_planes,
    };

    /* Set up component map - libplacebo's in the opposite lavu order */
    for (int i = 0; i < desc->nb_components; i++) {
        const AVComponentDescriptor *c = &desc->comp[i];
        plimg.planes[c->plane].component_mapping[plimg.planes[c->plane].components] = c->plane + c->offset;
        plimg.planes[c->plane].components++;
    }

    /* Map images */
    for (int i = 0; i < nb_planes; i++) {
        int w = in_f->width;
        int h = in_f->height;

        struct pl_vulkan_wrap_params wparams = (struct pl_vulkan_wrap_params) {
            .image = vkf->img[i],
            .format = vk_fmt[i],
            .width = w,
            .height = h,
            .depth = 0,
            .usage = DEFAULT_USAGE_FLAGS,
            .sample_mode = PL_TEX_SAMPLE_LINEAR,
            .address_mode = PL_TEX_ADDRESS_CLAMP,
        };

        plimg.planes[i].texture = pl_vulkan_wrap(win->pl_gpu, &wparams);
        if (!plimg.planes[i].texture) {
            ret = AVERROR(ENOMEM);
            goto finish;
        }

        pl_chroma_location_offset(placebo_chroma_loc_map[in_f->chroma_location],
                                  &plimg.planes[i].shift_x,
                                  &plimg.planes[i].shift_y);

        pl_vulkan_release(win->pl_gpu, plimg.planes[i].texture,
                          vkf->layout[i], vkf->access[i], vkf->sem[i]);
    }

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
    target.dst_rect.x0 = 0;
    target.dst_rect.y0 = 0;
    target.dst_rect.x1 = win->width;
    target.dst_rect.y1 = win->height;

    pl_rect2df_aspect_fit(&target.dst_rect, &plimg.src_rect, 0.0);
    pl_rect2df_offset(&target.dst_rect, 0, 0);
    pl_rect2df_zoom(&target.dst_rect, 1.0);

    pl_render_image(win->pl_renderer, &plimg, &target, &render_params);

    /* Release images from libplacebo */
    for (int i = 0; i < nb_planes; i++)
        pl_vulkan_hold_raw(win->pl_gpu, plimg.planes[i].texture, &vkf->layout[i],
                           &vkf->access[i], vkf->sem[i]);

finish:
    pl_swapchain_submit_frame(win->pl_swap);
    if (!ret)
        pl_swapchain_swap_buffers(win->pl_swap);

    /* Replace last frame if different */
    if (!ret && in_f && (!win->last_frame || (win->last_frame->pts != in_f->pts))) {
        av_frame_free(&win->last_frame);
        win->last_frame = in_f;
    }

    ret = !ret ? 1 : ret;

end:
    win->is_dirty = 1;

    pthread_mutex_unlock(&win->lock);
    return ret;
}
