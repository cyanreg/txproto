#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>

#include "interface_common.h"
#include "wayland_common.h"

#include <unistd.h>
#include <sys/mman.h>

#include <math.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>

#include "utils.h"
#include "../config.h"

enum SurfaceFlags {
    SURFACE_CHANGED_STATE = 1ULL << 0,
};

typedef struct WaylandSurface {
    struct WaylandGraphicsCtx *main;

    char *title;
    enum SurfaceType type;
    uint64_t flags;

    struct wl_surface *surface;
    struct wl_subsurface *subsurface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct xdg_surface *xdg_surface;
    struct zxdg_toplevel_decoration_v1 *xdg_decoration;

    /* State */
    SPBufferList *visible_outs;
    int scaling;
    int maximized;
    int fullscreen;
    int width, height;
    int old_width, old_height;

    struct wl_callback *frame_callback;
    InterfaceCB *cb;
    void *cb_ctx;
} WaylandSurface;

typedef struct WaylandGraphicsCtx {
    AVClass *class;
    WaylandCtx *wl;
    AVBufferRef *wl_ref;

    struct wl_seat *seat;

    WaylandSurface **surfaces;
    int num_surfaces;

    struct {
        struct wl_keyboard *kb;
        struct xkb_context *xkb_ctx;
        struct xkb_keymap  *xkb_map;
        struct xkb_state   *xkb_state;
    } keyboard;

    struct {
        WaylandSurface *cur_surface;
        enum MouseStyle cur_style;
        struct wl_touch *touch;
        int touch_entries;
        struct wl_cursor_theme *theme;
        struct wl_surface *surface;
        struct wl_pointer *pointer;
        uint32_t pointer_id;
        int allocated_scale;
        int xpos;
        int ypos;
    } cursor;
} WaylandGraphicsCtx;

FN_CREATING(WaylandGraphicsCtx, WaylandSurface, surface, surfaces, num_surfaces)

static WaylandSurface *find_surface(WaylandGraphicsCtx *ctx,
                                    struct wl_surface *wl_surf,
                                    struct xdg_toplevel *wl_toplevel)
{
    int idx = 0;
    WaylandSurface *tmp;
    while ((tmp = get_at_index_surface(ctx, idx++)))
        if (tmp->surface == wl_surf || tmp->xdg_toplevel == wl_toplevel)
            return tmp;
    return NULL;
}

static int load_cursor_theme(WaylandGraphicsCtx *ctx, int scaling)
{
    if (ctx->cursor.allocated_scale == scaling) /* Reuse if size is identical */
        return 0;
    else if (ctx->cursor.theme)
        wl_cursor_theme_destroy(ctx->cursor.theme);

    const char *size_str = getenv("XCURSOR_SIZE");
    int size = 32;
    if (size_str != NULL) {
        errno = 0;
        char *end;
        long size_long = strtol(size_str, &end, 10);
        if (!*end && !errno && size_long > 0 && size_long <= INT_MAX)
            size = (int)size_long;
    }

    ctx->cursor.theme = wl_cursor_theme_load(NULL, size * scaling,
                                             ctx->wl->shm_interface);
    if (!ctx->cursor.theme) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to load cursor theme!\n");
        return AVERROR(EINVAL);
    }

    ctx->cursor.allocated_scale = scaling;

    return 0;
}

static int set_cursor(WaylandGraphicsCtx *ctx, enum MouseStyle style)
{
    if (!ctx->cursor.pointer)
        return AVERROR(ENOTSUP);

    const char *style_name = NULL;
    switch (style) {
    case MSTYLE_DEFAULT:      style_name = "left_ptr";            break;
    case MSTYLE_CROSS:        style_name = "cross";               break;
    case MSTYLE_HAND:         style_name = "hand";                break;

    case MSTYLE_LEFT_SIDE:    style_name = "left_side";           break;
    case MSTYLE_RIGHT_SIDE:   style_name = "right_side";          break;
    case MSTYLE_TOP_SIDE:     style_name = "top_side";            break;
    case MSTYLE_BOTTOM_SIDE:  style_name = "bottom_side";         break;

    case MSTYLE_TOP_LEFT:     style_name = "top_left_corner";     break;
    case MSTYLE_TOP_RIGHT:    style_name = "top_right_corner";    break;
    case MSTYLE_BOTTOM_LEFT:  style_name = "bottom_left_corner";  break;
    case MSTYLE_BOTTOM_RIGHT: style_name = "bottom_right_corner"; break;

    default:
        sp_log(ctx, SP_LOG_ERROR, "Invalid cursor style!\n");
    case MSTYLE_NONE:
        wl_pointer_set_cursor(ctx->cursor.pointer, ctx->cursor.pointer_id,
                              NULL, 0, 0);
        return 0;
    }

    WaylandSurface *surf = ctx->cursor.cur_surface;

    /* Load theme */
    int err = load_cursor_theme(ctx, surf->scaling);
    if (err)
        return err;

    /* Load style */
    struct wl_cursor *st = wl_cursor_theme_get_cursor(ctx->cursor.theme,
                                                      style_name);
    if (!st) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to load cursor theme!\n");
        return AVERROR(EINVAL);
    }

    struct wl_cursor_image *img = st->images[0];
    struct wl_buffer *buffer = wl_cursor_image_get_buffer(img);
    if (!buffer)
        return AVERROR(ENOMEM);

    /* Damage cursor */
    wl_pointer_set_cursor(ctx->cursor.pointer, ctx->cursor.pointer_id, ctx->cursor.surface,
                          img->hotspot_x/surf->scaling, img->hotspot_y/surf->scaling);
    wl_surface_set_buffer_scale(ctx->cursor.surface, surf->scaling);
    wl_surface_attach(ctx->cursor.surface, buffer, 0, 0);
    wl_surface_damage(ctx->cursor.surface, 0, 0, img->width, img->height);
    wl_surface_commit(ctx->cursor.surface);

    ctx->cursor.cur_style = style;

    return 0;
}

static void pointer_handle_enter(void *data, struct wl_pointer *pointer,
                                 uint32_t serial, struct wl_surface *surface,
                                 wl_fixed_t sx, wl_fixed_t sy)
{
    WaylandGraphicsCtx *ctx = data;

    ctx->cursor.pointer = pointer;
    ctx->cursor.pointer_id = serial;
    ctx->cursor.cur_surface = find_surface(ctx, surface, NULL);
    ctx->cursor.xpos = wl_fixed_to_int(sx) * ctx->cursor.cur_surface->scaling;
    ctx->cursor.ypos = wl_fixed_to_int(sy) * ctx->cursor.cur_surface->scaling;

    void *cb_ctx = ctx->cursor.cur_surface->cb_ctx;
    mouse_mov_cb cb = ctx->cursor.cur_surface->cb->mouse_mov_cb;
    set_cursor(ctx, cb(cb_ctx, ctx->cursor.xpos, ctx->cursor.ypos));
}

static void pointer_handle_leave(void *data, struct wl_pointer *pointer,
                                 uint32_t serial, struct wl_surface *surface)
{
    WaylandGraphicsCtx *ctx = data;
    ctx->cursor.cur_surface = NULL;
}

static void pointer_handle_motion(void *data, struct wl_pointer *pointer,
                                  uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    WaylandGraphicsCtx *ctx = data;

    ctx->cursor.xpos = wl_fixed_to_int(sx) * ctx->cursor.cur_surface->scaling;
    ctx->cursor.ypos = wl_fixed_to_int(sy) * ctx->cursor.cur_surface->scaling;

    void *cb_ctx = ctx->cursor.cur_surface->cb_ctx;
    mouse_mov_cb cb = ctx->cursor.cur_surface->cb->mouse_mov_cb;

    enum MouseStyle style = cb(cb_ctx, ctx->cursor.xpos, ctx->cursor.ypos);
    if (style != ctx->cursor.cur_style)
        set_cursor(ctx, style);
}

static void window_move(WaylandGraphicsCtx *ctx, uint32_t serial)
{
    if (ctx->cursor.cur_surface->xdg_toplevel)
        xdg_toplevel_move(ctx->cursor.cur_surface->xdg_toplevel, ctx->seat, serial);
}

static void pointer_handle_button(void *data, struct wl_pointer *wl_pointer,
                                  uint32_t serial, uint32_t time, uint32_t button,
                                  uint32_t state)
{
    WaylandGraphicsCtx *ctx = data;

    uint64_t val = state == WL_POINTER_BUTTON_STATE_PRESSED ? SP_KEY_FLAG_DOWN
                                                            : SP_KEY_FLAG_UP;

    switch (button) {
    case BTN_LEFT:   val |= SP_KEY_MOUSE_LEFT;  break;
    case BTN_RIGHT:  val |= SP_KEY_MOUSE_RIGHT; break;
    case BTN_MIDDLE: val |= SP_KEY_MOUSE_MID;   break;
    case BTN_SIDE:   val |= SP_KEY_MOUSE_SIDE;  break;
    case BTN_EXTRA:  val |= SP_KEY_MOUSE_EXTRA; break;
    default: break;
    }

    ctx->cursor.cur_surface->cb->input(ctx->cursor.cur_surface->cb_ctx, val);
}

static void pointer_handle_axis(void *data, struct wl_pointer *wl_pointer,
                                uint32_t time, uint32_t axis, wl_fixed_t value)
{
    if (wl_fixed_to_double(value) == 0)
        return;
#if 0
    WaylandGraphicsCtx *ctx = data;
    double val = wl_fixed_to_double(value)*0.1;
    switch (axis) {
    case WL_POINTER_AXIS_VERTICAL_SCROLL:
        if (value > 0)
            mp_input_put_wheel(wl->vo->input_ctx, MP_WHEEL_DOWN,  +val);
        if (value < 0)
            mp_input_put_wheel(wl->vo->input_ctx, MP_WHEEL_UP,    -val);
        break;
    case WL_POINTER_AXIS_HORIZONTAL_SCROLL:
        if (value > 0)
            mp_input_put_wheel(wl->vo->input_ctx, MP_WHEEL_RIGHT, +val);
        if (value < 0)
            mp_input_put_wheel(wl->vo->input_ctx, MP_WHEEL_LEFT,  -val);
        break;
    }
#endif
}

static const struct wl_pointer_listener pointer_listener = {
    pointer_handle_enter,
    pointer_handle_leave,
    pointer_handle_motion,
    pointer_handle_button,
    pointer_handle_axis,
};

static int check_for_resize(WaylandGraphicsCtx *ctx, wl_fixed_t x_w, wl_fixed_t y_w,
                            enum xdg_toplevel_resize_edge *edge, WaylandSurface *surf)
{
    if (ctx->cursor.touch_entries || surf->fullscreen || surf->maximized)
        return 0;

    const int edge_pixels = 64;
    int pos[2] = { wl_fixed_to_double(x_w), wl_fixed_to_double(y_w) };
    int left_edge   = pos[0] < edge_pixels;
    int top_edge    = pos[1] < edge_pixels;
    int right_edge  = pos[0] > (surf->width - edge_pixels);
    int bottom_edge = pos[1] > (surf->height - edge_pixels);

    if (left_edge) {
        *edge = XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
        if (top_edge)
            *edge = XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
        else if (bottom_edge)
            *edge = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
    } else if (right_edge) {
        *edge = XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
        if (top_edge)
            *edge = XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
        else if (bottom_edge)
            *edge = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
    } else if (top_edge) {
        *edge = XDG_TOPLEVEL_RESIZE_EDGE_TOP;
    } else if (bottom_edge) {
        *edge = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
    } else {
        *edge = 0;
        return 0;
    }

    return 1;
}

static void touch_handle_down(void *data, struct wl_touch *wl_touch,
                              uint32_t serial, uint32_t time, struct wl_surface *surface,
                              int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
    WaylandGraphicsCtx *ctx = data;
    WaylandSurface *surf = find_surface(ctx, surface, NULL);

    enum xdg_toplevel_resize_edge edge;
    if (check_for_resize(ctx, x_w, y_w, &edge, surf)) {
        ctx->cursor.touch_entries = 0;
        xdg_toplevel_resize(surf->xdg_toplevel, ctx->seat, serial, edge);
        return;
    } else if (ctx->cursor.touch_entries) {
        ctx->cursor.touch_entries = 0;
        xdg_toplevel_move(surf->xdg_toplevel, ctx->seat, serial);
        return;
    }

    ctx->cursor.touch_entries = 1;
    if (!ctx->cursor.cur_surface)
        ctx->cursor.cur_surface = find_surface(ctx, surface, NULL);

    ctx->cursor.xpos = wl_fixed_to_int(x_w) * ctx->cursor.cur_surface->scaling;
    ctx->cursor.ypos = wl_fixed_to_int(y_w) * ctx->cursor.cur_surface->scaling;

    void *cb_ctx = ctx->cursor.cur_surface->cb_ctx;
    mouse_mov_cb cb = ctx->cursor.cur_surface->cb->mouse_mov_cb;

    enum MouseStyle style = cb(cb_ctx, ctx->cursor.xpos, ctx->cursor.ypos);
    if (style != ctx->cursor.cur_style)
        set_cursor(ctx, style);
}

static void touch_handle_up(void *data, struct wl_touch *wl_touch,
                            uint32_t serial, uint32_t time, int32_t id)
{
    WaylandGraphicsCtx *ctx = data;
    ctx->cursor.touch_entries = 0;
}

static void touch_handle_motion(void *data, struct wl_touch *wl_touch,
                                uint32_t time, int32_t id, wl_fixed_t x_w,
                                wl_fixed_t y_w)
{
    WaylandGraphicsCtx *ctx = data;

    ctx->cursor.xpos = wl_fixed_to_int(x_w) * ctx->cursor.cur_surface->scaling;
    ctx->cursor.ypos = wl_fixed_to_int(y_w) * ctx->cursor.cur_surface->scaling;

    void *cb_ctx = ctx->cursor.cur_surface->cb_ctx;
    mouse_mov_cb cb = ctx->cursor.cur_surface->cb->mouse_mov_cb;

    enum MouseStyle style = cb(cb_ctx, ctx->cursor.xpos, ctx->cursor.ypos);
    if (style != ctx->cursor.cur_style)
        set_cursor(ctx, style);
}

static void touch_handle_frame(void *data, struct wl_touch *wl_touch)
{

}

static void touch_handle_cancel(void *data, struct wl_touch *wl_touch)
{

}

static const struct wl_touch_listener touch_listener = {
    touch_handle_down,
    touch_handle_up,
    touch_handle_motion,
    touch_handle_frame,
    touch_handle_cancel,
};

static void keyboard_handle_keymap(void *data, struct wl_keyboard *wl_keyboard,
                                   uint32_t format, int32_t fd, uint32_t size)
{
    WaylandGraphicsCtx *ctx = data;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    char *map_shm = mmap(NULL, size - 1, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map_shm == MAP_FAILED) {
        close(fd);
        return;
    }

    ctx->keyboard.xkb_map = xkb_keymap_new_from_buffer(ctx->keyboard.xkb_ctx, map_shm,
                                                       size - 1,
                                                       XKB_KEYMAP_FORMAT_TEXT_V1,
                                                       XKB_KEYMAP_COMPILE_NO_FLAGS);

    munmap(map_shm, size - 1);
    close(fd);

    if (!ctx->keyboard.xkb_map) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to create XKB keymap\n");
        return;
    }

    ctx->keyboard.xkb_state = xkb_state_new(ctx->keyboard.xkb_map);
    if (!ctx->keyboard.xkb_state) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to create XKB state\n");
        xkb_keymap_unref(ctx->keyboard.xkb_map);
        ctx->keyboard.xkb_map = NULL;
        return;
    }
}

static void keyboard_handle_enter(void *data, struct wl_keyboard *wl_keyboard,
                                  uint32_t serial, struct wl_surface *surface,
                                  struct wl_array *keys)
{

}

static void keyboard_handle_leave(void *data, struct wl_keyboard *wl_keyboard,
                                  uint32_t serial, struct wl_surface *surface)
{

}

static const struct {
    int xkb;
    uint64_t sp;
} keymap[] = {
    /* Special keys */
    { XKB_KEY_Return, SP_KEY_ENTER },
    { XKB_KEY_Escape, SP_KEY_ESC   },
};

static uint64_t lookupkey(int key)
{
    const char *passthrough_keys = " -+*/<>`~!@#$%^&()_{}:;\"\',.?\\|=[]";

    uint64_t sp_key = 0;
    if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') ||
        (key >= '0' && key <= '9') ||
        (key >  0   && key <  256 && strchr(passthrough_keys, key)))
        sp_key = key;

    if (!sp_key) {
        for (int i = 0; i < SP_ARRAY_ELEMS(keymap); i++) {
            if (keymap[i].xkb == key) {
                sp_key = keymap[i].sp;
                break;
            }
        }
    }

    return sp_key;
}

static void keyboard_handle_key(void *data, struct wl_keyboard *wl_keyboard,
                                uint32_t serial, uint32_t time, uint32_t key,
                                uint32_t state)
{
    WaylandGraphicsCtx *ctx = data;

    uint32_t code = key + 8;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(ctx->keyboard.xkb_state, code);

    uint64_t mod_val = state == WL_KEYBOARD_KEY_STATE_PRESSED ? SP_KEY_FLAG_DOWN
                                                              : SP_KEY_FLAG_UP;

    static const char *mod_names[] = {
        XKB_MOD_NAME_SHIFT,
        XKB_MOD_NAME_CTRL,
        XKB_MOD_NAME_ALT,
        XKB_MOD_NAME_LOGO,
        0,
    };

    static uint64_t mods[] = {
        SP_KEY_FLAG_SHIFT,
        SP_KEY_FLAG_CTRL,
        SP_KEY_FLAG_ALT,
        SP_KEY_FLAG_META,
        0,
    };

    for (int n = 0; mods[n]; n++) {
        xkb_mod_index_t index = xkb_keymap_mod_get_index(ctx->keyboard.xkb_map, mod_names[n]);
        if (!xkb_state_mod_index_is_consumed(ctx->keyboard.xkb_state, code, index)
            && xkb_state_mod_index_is_active(ctx->keyboard.xkb_state, index,
                                             XKB_STATE_MODS_DEPRESSED))
            mod_val |= mods[n];
    }

    uint64_t sym_val = lookupkey(sym);
    if (sym_val) {
        ctx->cursor.cur_surface->cb->input(ctx->cursor.cur_surface->cb_ctx,
                                           sym_val | mod_val);
    } else {
        // TODO UTF8
    }
}

static void keyboard_handle_modifiers(void *data, struct wl_keyboard *wl_keyboard,
                                      uint32_t serial, uint32_t mods_depressed,
                                      uint32_t mods_latched, uint32_t mods_locked,
                                      uint32_t group)
{
    WaylandGraphicsCtx *ctx = data;
    xkb_state_update_mask(ctx->keyboard.xkb_state, mods_depressed, mods_latched,
                          mods_locked, 0, 0, group);
}

static void keyboard_handle_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
                                        int32_t rate, int32_t delay)
{

}

static const struct wl_keyboard_listener keyboard_listener = {
    keyboard_handle_keymap,
    keyboard_handle_enter,
    keyboard_handle_leave,
    keyboard_handle_key,
    keyboard_handle_modifiers,
    keyboard_handle_repeat_info,
};

static void seat_handle_caps(void *data, struct wl_seat *seat,
                             enum wl_seat_capability caps)
{
    WaylandGraphicsCtx *ctx = data;

    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !ctx->cursor.pointer) {
        ctx->cursor.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(ctx->cursor.pointer, &pointer_listener, ctx);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && ctx->cursor.pointer) {
        wl_pointer_destroy(ctx->cursor.pointer);
        ctx->cursor.pointer = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !ctx->keyboard.kb) {
        ctx->keyboard.kb = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(ctx->keyboard.kb, &keyboard_listener, ctx);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && ctx->keyboard.kb) {
        wl_keyboard_destroy(ctx->keyboard.kb);
        ctx->keyboard.kb = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !ctx->cursor.touch) {
        ctx->cursor.touch = wl_seat_get_touch(seat);
        wl_touch_set_user_data(ctx->cursor.touch, ctx);
        wl_touch_add_listener(ctx->cursor.touch, &touch_listener, ctx);
    } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && ctx->cursor.touch) {
        wl_touch_destroy(ctx->cursor.touch);
        ctx->cursor.touch = NULL;
    }
}

static const struct wl_seat_listener seat_listener = {
    seat_handle_caps,
};

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    xdg_wm_base_ping,
};

static AVBufferRef *buflist_max_scale(AVBufferRef *entry, void *opaque)
{
    int *scale = (int *)opaque;
    IOSysEntry *out = (IOSysEntry *)entry->data;
    *scale = SPMAX(*scale, out->scale);
    return NULL;
}

static void surface_handle_enter(void *data, struct wl_surface *wl_surface,
                                 struct wl_output *output)
{
    WaylandSurface *surf = data;
    AVBufferRef *ref = sp_find_ref_wayland_output(surf->main->wl, output, 0);
    if (!ref)
        return;

    sp_bufferlist_append(surf->visible_outs, ref);

    /* Get new scale */
    int scale = 1;
    sp_bufferlist_pop(surf->visible_outs, buflist_max_scale, &scale);
    surf->scaling = scale;

    IOSysEntry *out = (IOSysEntry *)ref->data;
    sp_log(surf->main, SP_LOG_DEBUG, "Surface \"%s\" entered output \"%s\", scale = %i\n",
           surf->title, sp_class_get_name(out), scale);

    av_buffer_unref(&ref);
}

static void surface_handle_leave(void *data, struct wl_surface *wl_surface,
                                 struct wl_output *output)
{
    WaylandSurface *surf = data;
    AVBufferRef *ref = sp_find_ref_wayland_output(surf->main->wl, output, 0);
    if (!ref)
        return;

    IOSysEntry *out = (IOSysEntry *)ref->data;
    sp_bufferlist_pop(surf->visible_outs, sp_bufferlist_iosysentry_by_id, (void *)(uintptr_t)out->identifier);

    /* Get new scale */
    int scale = 1;
    sp_bufferlist_pop(surf->visible_outs, buflist_max_scale, &scale);
    surf->scaling = scale;

    sp_log(surf->main, SP_LOG_DEBUG, "Surface \"%s\" gone from output \"%s\", new scale = %i\n",
           surf->title, sp_class_get_name(out), scale);

    av_buffer_unref(&ref);
}

static const struct wl_surface_listener surface_listener = {
    surface_handle_enter,
    surface_handle_leave,
};

static void handle_surface_config(void *data, struct xdg_surface *xdg_surface,
                                  uint32_t serial)
{
    WaylandSurface *surf = data;
    xdg_surface_ack_configure(xdg_surface, serial);
    wl_surface_set_buffer_scale(surf->surface, surf->scaling);

    int new_width = surf->width * surf->scaling;
    int new_height = surf->height * surf->scaling;

    surf->cb->resize(surf->cb_ctx, &new_width, &new_height, 0);
    surf->width = new_width;
    surf->height = new_height;
    surf->cb->render(surf->cb_ctx);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    handle_surface_config,
};

static void layer_surface_handle_configure(void *data,
                                           struct zwlr_layer_surface_v1 *surface,
                                           uint32_t serial, uint32_t width, uint32_t height)
{
    WaylandSurface *surf = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    wl_surface_set_buffer_scale(surf->surface, surf->scaling);
    int new_width = width * surf->scaling;
    int new_height = height * surf->scaling;
    surf->cb->resize(surf->cb_ctx, &new_width, &new_height, 0);
    surf->width = new_width;
    surf->height = new_height;
    surf->cb->render(surf->cb_ctx);
}

static void surface_destroy(void **wctx)
{
    WaylandSurface *surf = *wctx;

    if (surf->frame_callback)
        wl_callback_destroy(surf->frame_callback);
    if (surf->layer_surface)
        zwlr_layer_surface_v1_destroy(surf->layer_surface);
    if (surf->subsurface)
        wl_subsurface_destroy(surf->subsurface);
    if (surf->xdg_decoration)
        zxdg_toplevel_decoration_v1_destroy(surf->xdg_decoration);

    wl_surface_destroy(surf->surface);
    surf->cb->destroy(surf->cb_ctx);
    remove_surface(surf->main, surf);
}

static void layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    surface_destroy(&data);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    layer_surface_handle_configure,
    layer_surface_handle_closed,
};

static void set_border_decorations(WaylandSurface *surf, int enable)
{
    if (!surf->xdg_decoration)
        return;

    enum zxdg_toplevel_decoration_v1_mode mode;
    if (enable)
        mode = ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
    else
        mode = ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;

    zxdg_toplevel_decoration_v1_set_mode(surf->xdg_decoration, mode);
}

static void handle_toplevel_config(void *data, struct xdg_toplevel *toplevel,
                                   int32_t conf_width, int32_t conf_height,
                                   struct wl_array *states)
{
    WaylandSurface *surf = data;

    int new_width = surf->width;
    int new_height = surf->height;
    int new_fullscreen = surf->fullscreen;
    int new_maximized = surf->maximized;
    int active_resize = 0;

    enum xdg_toplevel_state *state;
    wl_array_for_each(state, states) {
        switch (*state) {
        case XDG_TOPLEVEL_STATE_FULLSCREEN:
            new_fullscreen = 1;
            break;
        case XDG_TOPLEVEL_STATE_RESIZING:
            active_resize = 1;
            break;
        case XDG_TOPLEVEL_STATE_ACTIVATED:
            break;
        case XDG_TOPLEVEL_STATE_TILED_TOP:
        case XDG_TOPLEVEL_STATE_TILED_LEFT:
        case XDG_TOPLEVEL_STATE_TILED_RIGHT:
        case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
        case XDG_TOPLEVEL_STATE_MAXIMIZED:
            new_maximized = 1;
            break;
        }
    }

    if ((surf->fullscreen != new_fullscreen) ||
        (surf->maximized != new_maximized)) {
        if (new_fullscreen || new_maximized) { /* Backup width and height */
            surf->old_width = surf->width;
            surf->old_height = surf->height;
            new_width = conf_width;
            new_height = conf_height;
        } else {
            new_width = surf->old_width;
            new_height = surf->old_height;
        }
        surf->fullscreen = new_fullscreen;
        surf->maximized = new_maximized;
        surf->flags |= SURFACE_CHANGED_STATE;
    } else if (conf_width && conf_height) {
        new_width = conf_width;
        new_height = conf_height;
    }

    if ((new_width == surf->width) || (new_height == surf->height))
        return;

    new_width *= surf->scaling;
    new_height *= surf->scaling;
    surf->cb->resize(surf->cb_ctx, &new_width, &new_height, active_resize);
    surf->width = new_width / surf->scaling;
    surf->height = new_height / surf->scaling;
}

static void handle_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
    surface_destroy(&data);
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    handle_toplevel_config,
    handle_toplevel_close,
};

static const struct wl_callback_listener frame_listener;

static void frame_callback(void *data, struct wl_callback *callback, uint32_t time)
{
    WaylandSurface *surf = data;

    if (surf->flags & SURFACE_CHANGED_STATE) {
        set_border_decorations(surf, !(surf->fullscreen || surf->maximized));
        surf->flags &= ~SURFACE_CHANGED_STATE;
    }

    wl_callback_destroy(callback);
    surf->frame_callback = wl_surface_frame(surf->surface);
    wl_callback_add_listener(surf->frame_callback, &frame_listener, surf);

    wl_surface_set_buffer_scale(surf->surface, surf->scaling);
    int ret = surf->cb->render(surf->cb_ctx);
    if (!ret)
        wl_surface_commit(surf->surface);
    else if (ret < 0)
        return; /* ERROR */
}

static const struct wl_callback_listener frame_listener = {
    frame_callback,
};

static int surface_init(void *s, void **wctx, void *ref_wctx, InterfaceCB *cb,
                        void *cb_ctx, const char *sname, int w, int h,
                        enum SurfaceType type, VkInstance inst, VkSurfaceKHR *vksurf)
{
    WaylandGraphicsCtx *ctx = s;
    WaylandSurface *surf = create_surface(ctx);

    surf->type = type;
    surf->cb = cb;
    surf->cb_ctx = cb_ctx;
    surf->main = ctx;
    surf->scaling = 1;
    surf->title = av_strdup(sname);
    surf->surface = wl_compositor_create_surface(ctx->wl->compositor);
    surf->visible_outs = sp_bufferlist_new();

    if (surf->type == STYPE_HIGHLIGHT) {
        AVBufferRef *output = sp_bufferlist_ref(ctx->wl->output_list,
                                                sp_bufferlist_find_fn_first,
                                                NULL);

        IOSysEntry *entry = (IOSysEntry *)output->data;
        WaylandIOPriv *priv = (WaylandIOPriv *)entry->api_priv;

#define FN zwlr_layer_shell_v1_get_layer_surface
        surf->layer_surface = FN(ctx->wl->layer_shell, surf->surface, priv->output,
                                 ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, surf->title);
#undef FN

        zwlr_layer_surface_v1_set_anchor(surf->layer_surface,
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP   |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT  |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);

        zwlr_layer_surface_v1_set_keyboard_interactivity(surf->layer_surface, 1);
        zwlr_layer_surface_v1_set_exclusive_zone(surf->layer_surface, -1);

        surf->width = surf->old_width = entry->width;
        surf->height = surf->old_height = entry->height;
        surface_handle_enter(surf, surf->surface, priv->output);

        av_buffer_unref(&output);
    } else if (surf->type == STYPE_MAIN) {
        surf->width = surf->old_width = w;
        surf->height = surf->old_height = h;

        surf->xdg_surface = xdg_wm_base_get_xdg_surface(ctx->wl->wm_base, surf->surface);
        surf->xdg_toplevel = xdg_surface_get_toplevel(surf->xdg_surface);

#define FN zxdg_decoration_manager_v1_get_toplevel_decoration
        if (ctx->wl->xdg_decoration_manager)
            surf->xdg_decoration = FN(ctx->wl->xdg_decoration_manager, surf->xdg_toplevel);
#undef FN
    } else if (surf->type == STYPE_OVERLAY) {
        surf->width = surf->old_width = w;
        surf->height = surf->old_height = h;

        WaylandSurface *root = ref_wctx;
        surf->subsurface = wl_subcompositor_get_subsurface(ctx->wl->subcompositor,
                                                           surf->surface,
                                                           root->surface);
    }

    /* Only one input for it to be at, so we can set the scale */
    if (surf->type != STYPE_HIGHLIGHT &&
        sp_bufferlist_len(ctx->wl->output_list) == 1) {
        AVBufferRef *output = sp_bufferlist_ref(ctx->wl->output_list,
                                                sp_bufferlist_find_fn_first,
                                                NULL);

        IOSysEntry *entry = (IOSysEntry *)output->data;
        WaylandIOPriv *priv = (WaylandIOPriv *)entry->api_priv;
        surface_handle_enter(surf, surf->surface, priv->output);
        av_buffer_unref(&output);
    }

    VkWaylandSurfaceCreateInfoKHR spawn_info = {
        .sType   = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
        .display = ctx->wl->display,
        .surface = surf->surface,
    };

    VkResult ret = vkCreateWaylandSurfaceKHR(inst, &spawn_info, NULL, vksurf);
    if (ret != VK_SUCCESS) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to init a Vulkan Wayland surface!\n");
        return AVERROR_EXTERNAL;
    }

    *wctx = surf;

    return 0;
}

static void surface_done(void *wctx)
{
    WaylandSurface *surf = wctx;

    /* Surface callbacks */
    wl_surface_add_listener(surf->surface, &surface_listener, surf);

    if (surf->type == STYPE_MAIN) {
        /* Toplevel props */
        xdg_toplevel_set_title (surf->xdg_toplevel, surf->title);
        xdg_toplevel_set_app_id(surf->xdg_toplevel, PROJECT_NAME);
        xdg_surface_add_listener(surf->xdg_surface, &xdg_surface_listener, surf);
        xdg_toplevel_add_listener(surf->xdg_toplevel, &xdg_toplevel_listener, surf);
    } else if (surf->type == STYPE_HIGHLIGHT) {
        zwlr_layer_surface_v1_add_listener(surf->layer_surface,
                                           &layer_surface_listener, surf);
    }

    /* Start the frame callback */
    surf->frame_callback = wl_surface_frame(surf->surface);
    wl_callback_add_listener(surf->frame_callback, &frame_listener, surf);
    wl_surface_commit(surf->surface);

    /* Send commands to display */
    wl_display_flush(surf->main->wl->display);
}

static void uninit(void **s)
{
    if (!s || !(*s))
        return;

    WaylandGraphicsCtx *ctx = *s;

    av_buffer_unref(&ctx->wl_ref);
    sp_class_free(ctx);
    av_free(ctx);
}

static int init(void **s, AVBufferRef **device_ref, const char **vk_inst_ext,
                enum VkPresentModeKHR *present_mode)
{
    int err = 0;
    WaylandGraphicsCtx *ctx = av_mallocz(sizeof(*ctx));
    ctx->class = av_mallocz(sizeof(*ctx->class));
    *ctx->class = (AVClass) {
        .class_name = av_strdup("wayland_gui"),
        .item_name  = av_default_item_name,
        .version    = LIBAVUTIL_VERSION_INT,
    };

    err = sp_wayland_create(&ctx->wl_ref);
    if (err < 0)
        goto fail;

    ctx->wl = (WaylandCtx *)ctx->wl_ref->data;

    ctx->keyboard.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx->keyboard.xkb_ctx) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to init XKB!\n");
        err = AVERROR(ENOMEM);
        goto fail;
    }

    /* Create a cursor surface and keep it around */
    ctx->cursor.surface = wl_compositor_create_surface(ctx->wl->compositor);
    ctx->cursor.cur_style = -1;

    /* Init wm_base */
    xdg_wm_base_add_listener(ctx->wl->wm_base, &xdg_wm_base_listener, ctx);

    /* Listen for seats (mouse, keyboard, touch) */
    ctx->seat = wl_registry_bind(ctx->wl->registry, ctx->wl->seat_id,
                                 &wl_seat_interface, 1);
    wl_seat_add_listener(ctx->seat, &seat_listener, ctx);

    /* Do a flush */
    wl_display_flush(ctx->wl->display);

    /* Set */
    *s = ctx;
    *vk_inst_ext  = "VK_KHR_wayland_surface";
    *present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
    *device_ref   = ctx->wl->drm_device_ref;

    return 0;

fail:
    uninit((void **)&ctx);
    return err;
}

const InterfaceSystem win_wayland = {
    .init         = init,
    .surf_init    = surface_init,
    .surf_done    = surface_done,
    .surf_destroy = surface_destroy,
    .uninit       = uninit,
};
