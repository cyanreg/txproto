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

#include "wlr-export-dmabuf-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"

#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>

#include "iosys_common.h"
#include "logging.h"
#include <libtxproto/utils.h>
#include "os_compat.h"

typedef struct WaylandIOPriv {
    struct WaylandCtx *main_ctx;
    struct wl_output *output;
    struct zxdg_output_v1 *xdg_out;
} WaylandIOPriv;

typedef struct WaylandCtx {
    SPClass *class;

    pthread_mutex_t lock;

    /* Event thread */
    pthread_t event_thread;

    /* Main interfaces */
    uint32_t seat_id;
    struct wl_display *display;
    struct wl_shm *shm_interface;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_subcompositor *subcompositor;
    struct wl_data_device_manager *dnd_device_manager;

    struct zwp_idle_inhibit_manager_v1 *idle_inhibit_manager;
    struct zwp_linux_dmabuf_v1 *dmabuf;

    struct xdg_wm_base *wm_base;
    struct zxdg_output_manager_v1 *xdg_output_manager;
    struct zxdg_decoration_manager_v1 *xdg_decoration_manager;

    struct zwlr_export_dmabuf_manager_v1 *dmabuf_export_manager;
    struct zwlr_screencopy_manager_v1 *screencopy_export_manager;
    struct zwlr_layer_shell_v1 *layer_shell;

    /* Device reference to where the DMABUF frames live */
    AVBufferRef *drm_device_ref;

    /* List containing all outputs */
    SPBufferList *output_list;
    SPBufferList *events;

    /* Display FD */
    int display_fd;

    /* Sliding window capacity monitor for the FD */
    SlidingWinCtx sctx_fd;

    /* Wakeup pipe to reliably run/stop the event thread */
    int wakeup_pipe[2];
} WaylandCtx;

int sp_wayland_create(AVBufferRef **ref);

AVBufferRef *sp_find_ref_wayland_output(WaylandCtx *ctx,
                                        struct wl_output *out, uint32_t id);
