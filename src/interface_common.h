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

#pragma once

#include <libavutil/hwcontext.h>
#include <vulkan/vulkan.h>

#include <libtxproto/fifo_frame.h>
#include <libtxproto/utils.h>

int sp_interface_init(AVBufferRef **s);

int sp_interface_ctrl(AVBufferRef *ctx_ref, SPEventType ctrl, void *arg);

AVBufferRef *sp_interface_main_win(AVBufferRef *ref, const char *title);
AVBufferRef *sp_interface_get_fifo(void *s);

AVBufferRef *sp_interface_highlight_win(AVBufferRef *ref, const char *title,
                                        AVBufferRef *event);

enum MouseStyle {
    MSTYLE_NONE = 0,
    MSTYLE_DEFAULT,
    MSTYLE_CROSS,
    MSTYLE_HAND,

    MSTYLE_LEFT_SIDE,
    MSTYLE_RIGHT_SIDE,
    MSTYLE_TOP_SIDE,
    MSTYLE_BOTTOM_SIDE,

    MSTYLE_TOP_LEFT,
    MSTYLE_TOP_RIGHT,
    MSTYLE_BOTTOM_LEFT,
    MSTYLE_BOTTOM_RIGHT,
};

enum SurfaceType {
    STYPE_MAIN, /* Normal window */
    STYPE_OVERLAY, /* Overlay window */
    STYPE_HIGHLIGHT, /* Highlight surface */
};

enum KeyCodes {
    /* Key values less than this are standard Unicode */
    SP_KEY_BASE       = 1ULL << 31,

    SP_KEY_ENTER,
    SP_KEY_ESC,

    SP_KEY_MOUSE_LEFT,
    SP_KEY_MOUSE_RIGHT,
    SP_KEY_MOUSE_MID,
    SP_KEY_MOUSE_SIDE,
    SP_KEY_MOUSE_EXTRA,
};

enum KeyFlags {
    SP_KEY_FLAG_SHIFT = 1ULL << 58,
    SP_KEY_FLAG_CTRL  = 1ULL << 59,
    SP_KEY_FLAG_ALT   = 1ULL << 60,
    SP_KEY_FLAG_META  = 1ULL << 61,
    SP_KEY_FLAG_DOWN  = 1ULL << 62,
    SP_KEY_FLAG_UP    = 1ULL << 63,

    SP_KEY_FLAG_MASK = (SP_KEY_FLAG_SHIFT | SP_KEY_FLAG_CTRL | SP_KEY_FLAG_ALT |
                        SP_KEY_FLAG_META | SP_KEY_FLAG_DOWN | SP_KEY_FLAG_UP),
};

typedef enum MouseStyle (*mouse_mov_cb)(void *iface_ctx, int x, int y);

typedef struct InterfaceCB {
    /* Mouse movement */
    mouse_mov_cb mouse_mov_cb;
    int (*input)(void *iface_ctx, uint64_t val);

    /* Rendering */
    int (*render)(void *iface_ctx);
    int (*resize)(void *iface_ctx, int *w, int *h, int active_resize);

    /* State */
    void (*state)(void *iface_ctx, int is_fs, float scale);

    /* Destroy */
    void (*destroy)(void *iface_ctx);
} InterfaceCB;

typedef const struct InterfaceSystem {
    /**
     * Output name
     */
    const char *name;

    /**
     * Initialize the windowing system
     * device_ref should be a new reference to the render device or NULL
     */
    int  (*init)(void **s, AVBufferRef **device_ref, const char **vk_inst_ext,
                 enum VkPresentModeKHR *present_mode);

    /**
     * Create a surface
     */
    int (*surf_init)(void *s, void **wctx, void *ref_wctx, InterfaceCB *cb,
                     void *cb_ctx, const char *wname, int w, int h,
                     enum SurfaceType type, VkInstance vkinst, VkSurfaceKHR *vksurf);

    /**
     * Surface init is done
     */
    void (*surf_done)(void *wctx);

    /**
     * Destroy a surface
     */
    void (*surf_destroy)(void **wctx);

    /**
     * Uninitialize the window system
     */
    void (*uninit)(void **s);
} InterfaceSystem;

extern const InterfaceSystem *sp_compiled_interfaces[];
extern const int sp_compiled_interfaces_len;
