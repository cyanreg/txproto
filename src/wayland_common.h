#define _GNU_SOURCE
#include <pthread.h>

#include "wlr-export-dmabuf-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include "idle-inhibit-unstable-v1-client-protocol.h"

#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>

typedef struct WaylandOutput {
    struct wl_list link;
    uint32_t id;
    struct wl_output *output;
    char *make;
    char *model;

    int scale;
    int width;
    int height;
    AVRational framerate;

    void (*remove_callback)(void *remove_ctx, uint32_t identifier);
    void *remove_ctx;
} WaylandOutput;

typedef struct WaylandCtx {
    AVClass *class;

    /* Event thread */
    pthread_t event_thread;

    /* Main interfaces */
    struct wl_seat *seat;
    struct wl_display *display;
    struct xdg_wm_base *wm_base;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct zxdg_decoration_manager_v1 *xdg_decoration_manager;
    struct zwp_idle_inhibit_manager_v1 *idle_inhibit_manager;
    struct wl_data_device_manager *dnd_device_manager;
    struct zwlr_export_dmabuf_manager_v1 *dmabuf_export_manager;
    struct zwlr_screencopy_manager_v1 *screencopy_export_manager;
    struct wl_shm *shm_interface;

    /* Device reference to where the DMABUF frames live */
    AVBufferRef *drm_device_ref;

    /* List containing all outputs, dynamically updated */
    struct wl_list output_list;

    /* Wakeup pipe to reliably run/stop the event thread */
    int wakeup_pipe[2];
} WaylandCtx;

int  sp_wayland_init(WaylandCtx **s);
void sp_waylad_uninit(WaylandCtx **s);
WaylandOutput *sp_find_wayland_output(WaylandCtx *ctx,
                                      struct wl_output *out, uint32_t id);

