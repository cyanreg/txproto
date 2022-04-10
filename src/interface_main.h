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

static int get_render_source(InterfaceWindowCtx *win, struct pl_frame *dst,
                             AVFrame **src, double *dar)
{
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

    struct pl_avframe_params map_params = {
        .frame    = *src,
        .tex      = win->main_win.tex,
        .map_dovi = true,
    };

    bool ret = pl_map_avframe_ex(win->pl_gpu, dst, &map_params);
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
