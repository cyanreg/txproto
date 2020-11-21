#include <libavutil/time.h>

#define HBORDER_RESIZE 18
#define HFLAG_TRACK_X0 (1 << 0)
#define HFLAG_TRACK_X1 (1 << 1)
#define HFLAG_TRACK_Y0 (1 << 2)
#define HFLAG_TRACK_Y1 (1 << 3)

static void clip(int *d0, int *state_ptr, int delta, int max)
{
    int s0 = *d0;
    int state = *state_ptr;

    /* Update position or state with delta */
    if (state == INT32_MIN)
        *d0 = s0 = s0 + delta;
    else
        state = *state_ptr = state + delta;

    /* Set OOB state and clip */
    if ((state == INT32_MIN) && (s0 < 0 || s0 >= max)) {
        state = *state_ptr = s0;
        *d0 = av_clip(s0, 0, max - 1);
    }

    /* Restore state if no longer clipping */
    if ((state != INT32_MIN) && (state >= 0) && (state < max)) {
        *d0 = state;
        *state_ptr = INT32_MIN;
    }
}

static enum MouseStyle mouse_move_highlight(void *wctx, int x, int y)
{
    InterfaceWindowCtx *win = wctx;
    pthread_mutex_lock(&win->lock);

    const int tolerance = lrintf(HBORDER_RESIZE * win->scale);
    enum MouseStyle style = MSTYLE_DEFAULT;
    uint32_t flags = win->highlight.flags;
    int count = 1, delta_x = 0, delta_y = 0;

    if (win->cursor.x >= 0)
        delta_x = x - win->cursor.x;

    if (win->cursor.y >= 0)
        delta_y = y - win->cursor.y;

    win->cursor.x = x;
    win->cursor.y = y;

    if (flags & HFLAG_TRACK_X0 && count++)
        clip(&win->highlight.x0, &win->highlight.state_x0, delta_x, win->width);
    if (flags & HFLAG_TRACK_X1 && count++)
        clip(&win->highlight.x1, &win->highlight.state_x1, delta_x, win->width);
    if (flags & HFLAG_TRACK_Y0 && count++)
        clip(&win->highlight.y0, &win->highlight.state_y0, delta_y, win->height);
    if (flags & HFLAG_TRACK_Y1 && count++)
        clip(&win->highlight.y1, &win->highlight.state_y1, delta_y, win->height);

    if ((count - 1) == 4) { style = MSTYLE_HAND; goto end; }

    if (win->highlight.x0 >= 0 && win->highlight.x1 >= 0 &&
        win->highlight.y0 >= 0 && win->highlight.y1 >= 0) {
        int left = 0, right = 0, top = 0, bottom = 0;
        int top_border    = FFMIN(win->highlight.y0, win->highlight.y1);
        int left_border   = FFMIN(win->highlight.x0, win->highlight.x1);
        int bottom_border = FFMAX(win->highlight.y0, win->highlight.y1);
        int right_border  = FFMAX(win->highlight.x0, win->highlight.x1);
        if (abs(win->cursor.x - left_border)   < tolerance &&
            (win->cursor.y > top_border    - tolerance) &&
            (win->cursor.y < bottom_border + tolerance))
            left   = 1;
        if (abs(win->cursor.x - right_border)  < tolerance &&
            (win->cursor.y > top_border    - tolerance) &&
            (win->cursor.y < bottom_border + tolerance))
            right  = 1;
        if (abs(win->cursor.y - top_border)    < tolerance &&
            (win->cursor.x > left_border   - tolerance) &&
            (win->cursor.x < right_border  + tolerance))
            top    = 1;
        if (abs(win->cursor.y - bottom_border) < tolerance &&
            (win->cursor.x > left_border   - tolerance) &&
            (win->cursor.x < right_border  + tolerance))
            bottom = 1;

        /* All */
        if (left && right && top && bottom) { style = MSTYLE_CROSS; goto end; }

        /* Doubles */
        if (top && left)            { style = MSTYLE_BOTTOM_RIGHT; goto end; }
        if (top && right)           { style = MSTYLE_BOTTOM_LEFT; goto end; }
        if (bottom && left)         { style = MSTYLE_TOP_RIGHT; goto end; }
        if (bottom && right)        { style = MSTYLE_TOP_LEFT; goto end; }

        /* Singles */
        if (top)                    { style = MSTYLE_BOTTOM_SIDE; goto end; }
        if (right)                  { style = MSTYLE_LEFT_SIDE; goto end; }
        if (left)                   { style = MSTYLE_RIGHT_SIDE; goto end; }
        if (bottom)                 { style = MSTYLE_TOP_SIDE; goto end; }
    }

    if ((count - 1) == 1 || (count - 1) == 2)
        style = MSTYLE_CROSS;

end:
    pthread_mutex_unlock(&win->lock);
    return style;
}

static int win_input_highlight(void *wctx, uint64_t val)
{
    InterfaceWindowCtx *win = wctx;
    pthread_mutex_lock(&win->lock);

    const int tolerance = lrintf(HBORDER_RESIZE * win->scale);
    uint64_t flags = val & SP_KEY_FLAG_MASK;
    val &= ~SP_KEY_FLAG_MASK;

    int has_sel = 0;
    SPRect res;

    switch (val) {
    case SP_KEY_MOUSE_LEFT:
        if ((flags & SP_KEY_FLAG_DOWN) && !win->highlight.flags &&
            (win->highlight.x0 >= 0) && (win->highlight.x1 >= 0) &&
            (win->highlight.y0 >= 0) && (win->highlight.y1 >= 0)) {
            if (abs(win->cursor.x - win->highlight.x0) < tolerance &&
                (win->cursor.y > win->highlight.y0 - tolerance) &&
                (win->cursor.y < win->highlight.y1 + tolerance)) {
                win->cursor.x = win->highlight.x0;
                win->highlight.flags |= HFLAG_TRACK_X0;
            }
            if (abs(win->cursor.x - win->highlight.x1) < tolerance &&
                (win->cursor.y > win->highlight.y0 - tolerance) &&
                (win->cursor.y < win->highlight.y1 + tolerance)) {
                win->cursor.x = win->highlight.x1;
                win->highlight.flags |= HFLAG_TRACK_X1;
            }
            if (abs(win->cursor.y - win->highlight.y0) < tolerance &&
                (win->cursor.x > win->highlight.x0 - tolerance) &&
                (win->cursor.x < win->highlight.x1 + tolerance)) {
                win->cursor.y = win->highlight.y0;
                win->highlight.flags |= HFLAG_TRACK_Y0;
            }
            if (abs(win->cursor.y - win->highlight.y1) < tolerance &&
                (win->cursor.x > win->highlight.x0 - tolerance) &&
                (win->cursor.x < win->highlight.x1 + tolerance)) {
                win->cursor.y = win->highlight.y1;
                win->highlight.flags |= HFLAG_TRACK_Y1;
            }
            if (win->highlight.flags) {
                win->is_dirty = 1;
                goto end;
            }
        }
        if (flags & SP_KEY_FLAG_DOWN && !win->highlight.flags) {
            win->highlight.flags = HFLAG_TRACK_X1 | HFLAG_TRACK_Y1;
            win->highlight.x0 = win->highlight.x1 = win->cursor.x;
            win->highlight.y0 = win->highlight.y1 = win->cursor.y;
        } else if (flags & SP_KEY_FLAG_UP) {
            if (!abs(win->highlight.x0 - win->highlight.x1) ||
                !abs(win->highlight.x0 - win->highlight.x1)) {
                win->highlight.x0 = INT32_MIN;
                win->highlight.y0 = INT32_MIN;
                win->highlight.x1 = INT32_MIN;
                win->highlight.y1 = INT32_MIN;
            }
            win->highlight.flags = 0x0;
        }
        win->is_dirty = 1;
        break;
    case SP_KEY_MOUSE_RIGHT:
        if (flags & SP_KEY_FLAG_DOWN && !win->highlight.flags) {
            int x0 = FFMIN(win->highlight.x0, win->highlight.x1);
            int y0 = FFMIN(win->highlight.y0, win->highlight.y1);
            int x1 = FFMAX(win->highlight.x0, win->highlight.x1);
            int y1 = FFMAX(win->highlight.y0, win->highlight.y1);
            if ((x0 < win->cursor.x) && (win->cursor.x < x1) &&
                (y0 < win->cursor.y) && (win->cursor.y < y1))
                win->highlight.flags = HFLAG_TRACK_X0 | HFLAG_TRACK_X1 |
                                       HFLAG_TRACK_Y0 | HFLAG_TRACK_Y1;
        } else if (flags & SP_KEY_FLAG_UP) {
            win->highlight.flags = 0x0;
        }
        break;
    case SP_KEY_ENTER:
        if (!(flags & SP_KEY_FLAG_DOWN))
            break;
        int x0 = FFMIN(win->highlight.x0, win->highlight.x1);
        int y0 = FFMIN(win->highlight.y0, win->highlight.y1);
        int x1 = FFMAX(win->highlight.x0, win->highlight.x1) + 1;
        int y1 = FFMAX(win->highlight.y0, win->highlight.y1) + 1;
        if ((x0 != x1) && (y0 != y1)) {
            sp_log(win, SP_LOG_VERBOSE, "Selected area at (%i, %i) %ix%i\n",
                   x0, y0, x1 - x0, y1 - y0);
            res = (SPRect){ x0, y0, x1 - x0, y1 - y0, win->scale };
            has_sel = 1;
        }
        /* Fallthrough */
    case 'c':
    case SP_KEY_ESC:
        if (val == 'c' && !(flags & SP_KEY_FLAG_CTRL))
            break;
        win->highlight.x0 = INT32_MIN;
        win->highlight.y0 = INT32_MIN;
        win->highlight.x1 = INT32_MIN;
        win->highlight.y1 = INT32_MIN;
        win->highlight.flags = 0x0;
        pthread_mutex_unlock(&win->lock);
        win->main->sys->surf_destroy(&win->wctx);
        SPGenericData rect[] = { D_TYPE("region", NULL, res), { 0 } };
        sp_eventlist_dispatch(win, win->events, SP_EVENT_ON_DESTROY,
                              has_sel ? &rect : NULL);
        return 0;
    };

end:
    pthread_mutex_unlock(&win->lock);
    return 0;
}

static int render_highlight(void *wctx)
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
    float rgba[4] = { 0.20, 0.20, 0.20, 0.40 };
    pl_tex_clear(win->pl_gpu, frame.fbo, rgba);

    int x0 = FFMIN(win->highlight.x0, win->highlight.x1);
    int y0 = FFMIN(win->highlight.y0, win->highlight.y1);
    int x1 = FFMAX(win->highlight.x0, win->highlight.x1) + 1;
    int y1 = FFMAX(win->highlight.y0, win->highlight.y1) + 1;
    int w = x1 - x0;
    int h = y1 - y0;

    if (w <= 0 || h <= 0)
        goto finish;

    struct pl_tex_params pltex_params = {
        .w             = w,
        .h             = h,
        .d             = 0,
        .format        = pl_find_fmt(win->pl_gpu, PL_FMT_UNORM, 4, 8, 0,
                                     PL_FMT_CAP_SAMPLEABLE |PL_FMT_CAP_BLITTABLE |
                                     PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_LINEAR),
        .sampleable    = true,
        .renderable    = true,
        .storable      = false,
        .blit_src      = true,
        .blit_dst      = true,
        .host_writable = false,
        .host_readable = false,
        .sample_mode   = PL_TEX_SAMPLE_LINEAR,
        .address_mode  = PL_TEX_ADDRESS_CLAMP,
    };

    pl_tex_recreate(win->pl_gpu, &win->highlight.region, &pltex_params);
    float foreground_rgba[4] = { 0.0, 0.0, 0.0, 0.0 };
    pl_tex_clear(win->pl_gpu, win->highlight.region, foreground_rgba);

    struct pl_image plimg = {
        .signature = av_gettime_relative(),
        .num_planes = 1,
        .planes[0].texture = win->highlight.region,
        .planes[0].components = 4,
        .planes[0].component_mapping = { 0, 1, 2, 3 },
        .repr = { 0 },
        .color = { 0 },
        .src_rect.x0 = 0,
        .src_rect.y0 = 0,
        .src_rect.x1 = pltex_params.w,
        .src_rect.y1 = pltex_params.h,
    };

    /* Set rendering parameters */
    struct pl_render_params render_params = pl_render_default_params;
    render_params.upscaler = &pl_filter_triangle;
    render_params.downscaler = NULL;
    render_params.sigmoid_params = NULL;
    render_params.color_adjustment = &pl_color_adjustment_neutral;
    render_params.peak_detect_params = NULL;
    render_params.color_map_params = &pl_color_map_default_params;
    render_params.dither_params = NULL;
    render_params.lut3d_params = &pl_3dlut_default_params;
    render_params.skip_anti_aliasing = 1;
    render_params.disable_overlay_sampling = 1;
    render_params.allow_delayed_peak_detect = 1;

    /* Set rendering target params */
    struct pl_render_target target = { 0 };
    pl_render_target_from_swapchain(&target, &frame);
    target.dst_rect.x0 = x0;
    target.dst_rect.x1 = x1;
    target.dst_rect.y0 = y0;
    target.dst_rect.y1 = y1;

    pl_render_image(win->pl_renderer, &plimg, &target, &render_params);

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
