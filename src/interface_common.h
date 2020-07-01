#pragma once

#include <libavutil/hwcontext.h>
#include <vulkan/vulkan.h>

#include "fifo_frame.h"

int interface_init(void **s);
int interface_send_fifo_to_main(void *s, AVBufferRef *fifo);

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
