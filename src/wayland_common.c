#include <unistd.h>
#include <poll.h>
#include <fcntl.h>

#include <libavutil/time.h>
#include <xf86drm.h>

#include "wayland_common.h"
#include "utils.h"

static pthread_mutex_t ctx_ref_lock = PTHREAD_MUTEX_INITIALIZER;
static AVBufferRef *ctx_ref = NULL;

static void xdg_out_position(void *opaque, struct zxdg_output_v1 *zxdg_output_v1,
                             int32_t x, int32_t y)
{

}

static void xdg_out_size(void *opaque, struct zxdg_output_v1 *zxdg_output_v1,
                         int32_t width, int32_t height)
{

}

static void xdg_out_done(void *opaque, struct zxdg_output_v1 *zxdg_output_v1)
{
    IOSysEntry *entry = (IOSysEntry *)(((AVBufferRef *)opaque)->data);
    WaylandIOPriv *priv = (WaylandIOPriv *)entry->api_priv;
    WaylandCtx *main = (WaylandCtx *)priv->main->data;

    pthread_mutex_lock(&main->lock);

    if (!priv->pushed_to_list) {
        sp_bufferlist_append(main->output_list, opaque);
        priv->pushed_to_list = 1;
    }

    pthread_mutex_unlock(&main->lock);
}

static void xdg_out_name(void *opaque, struct zxdg_output_v1 *zxdg_output_v1,
                         const char *name)
{
    IOSysEntry *entry = (IOSysEntry *)(((AVBufferRef *)opaque)->data);
    WaylandIOPriv *priv = (WaylandIOPriv *)entry->api_priv;
    WaylandCtx *main = (WaylandCtx *)priv->main->data;

    pthread_mutex_lock(&main->lock);

    sp_class_set_name(entry, name);

    pthread_mutex_unlock(&main->lock);
}

static void xdg_out_desc(void *opaque, struct zxdg_output_v1 *zxdg_output_v1,
                         const char *desc)
{
    IOSysEntry *entry = (IOSysEntry *)(((AVBufferRef *)opaque)->data);
    WaylandIOPriv *priv = (WaylandIOPriv *)entry->api_priv;
    WaylandCtx *main = (WaylandCtx *)priv->main->data;

    pthread_mutex_lock(&main->lock);

    av_freep(&entry->desc);
    entry->desc = av_strdup(desc);

    pthread_mutex_unlock(&main->lock);
}

struct zxdg_output_v1_listener xdg_output_listener = {
    xdg_out_position,
    xdg_out_size,
    xdg_out_done,
    xdg_out_name,
    xdg_out_desc,
};

static void output_handle_geometry(void *opaque, struct wl_output *wl_output,
                                   int32_t x, int32_t y, int32_t phys_width,
                                   int32_t phys_height, int32_t subpixel,
                                   const char *make, const char *model,
                                   int32_t transform)
{
    IOSysEntry *entry = (IOSysEntry *)(((AVBufferRef *)opaque)->data);
    WaylandIOPriv *priv = (WaylandIOPriv *)entry->api_priv;
    WaylandCtx *main = (WaylandCtx *)priv->main->data;

    pthread_mutex_lock(&main->lock);

    if (!sp_class_get_name(entry))
        sp_class_set_name(entry, model);
    if (!entry->desc)
        entry->desc = av_strdup(make);
    if (!x && !y)
        entry->is_default = 1;

    pthread_mutex_unlock(&main->lock);
}

static void output_handle_mode(void *opaque, struct wl_output *wl_output,
                               uint32_t flags, int32_t width, int32_t height,
                               int32_t refresh)
{
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        IOSysEntry *entry = (IOSysEntry *)(((AVBufferRef *)opaque)->data);
        WaylandIOPriv *priv = (WaylandIOPriv *)entry->api_priv;
        WaylandCtx *main = (WaylandCtx *)priv->main->data;

        pthread_mutex_lock(&main->lock);

        entry->width = width;
        entry->height = height;
        entry->framerate = av_make_q(refresh, 1000);

        pthread_mutex_unlock(&main->lock);
    }
}

static void output_handle_done(void *opaque, struct wl_output *wl_output)
{
    IOSysEntry *entry = (IOSysEntry *)(((AVBufferRef *)opaque)->data);
    WaylandIOPriv *priv = (WaylandIOPriv *)entry->api_priv;
    WaylandCtx *main = (WaylandCtx *)priv->main->data;

    pthread_mutex_lock(&main->lock);

    if (main->xdg_output_manager && !priv->xdg_out) {
        priv->xdg_out = zxdg_output_manager_v1_get_xdg_output(main->xdg_output_manager,
                                                              wl_output);
        zxdg_output_v1_add_listener(priv->xdg_out, &xdg_output_listener, opaque);
    } else if (!priv->pushed_to_list) {
        sp_bufferlist_append(main->output_list, opaque);
        priv->pushed_to_list = 1;
    }

    pthread_mutex_unlock(&main->lock);
}

static void output_handle_scale(void *opaque, struct wl_output *wl_output,
                                int32_t scale)
{
    IOSysEntry *entry = (IOSysEntry *)(((AVBufferRef *)opaque)->data);
    WaylandIOPriv *output = (WaylandIOPriv *)entry->api_priv;
    WaylandCtx *main = (WaylandCtx *)output->main->data;

    pthread_mutex_lock(&main->lock);

    entry->scale = scale;

    pthread_mutex_unlock(&main->lock);
}

static const struct wl_output_listener output_listener = {
    .geometry = output_handle_geometry,
    .mode = output_handle_mode,
    .done = output_handle_done,
    .scale = output_handle_scale,
};

static void destroy_entry(void *opaque, uint8_t *data)
{
    IOSysEntry *entry = (IOSysEntry *)opaque;
    WaylandIOPriv *priv = (WaylandIOPriv *)entry->api_priv;

    zxdg_output_v1_destroy(priv->xdg_out);
    wl_output_release(priv->output);
    av_free(entry->desc);

    av_buffer_unref(&priv->main);

    av_free(priv);
    av_free(entry);
}

static void registry_handle_add(void *opaque, struct wl_registry *reg,
                                uint32_t id, const char *interface,
                                uint32_t ver)
{
    int found = 1;
    WaylandCtx *ctx = (WaylandCtx *)((AVBufferRef *)opaque)->data;
    pthread_mutex_lock(&ctx->lock);

    if (!strcmp(interface, wl_output_interface.name) && (ver >= 2) && found++) {
        IOSysEntry *entry = av_mallocz(sizeof(*entry));
        WaylandIOPriv *output = av_mallocz(sizeof(*output));

        output->main   = av_buffer_ref(opaque);
        output->output = wl_registry_bind(reg, id, &wl_output_interface, 2);

        entry->scale = 1;
        entry->identifier = sp_iosys_gen_identifier(ctx, id, 0);
        entry->api_id = id;
        entry->api_priv = output;

        sp_class_alloc(entry, NULL, SP_TYPE_VIDEO_BIDIR, ctx);

        AVBufferRef *buf = av_buffer_create((uint8_t *)entry, sizeof(*entry),
                                            destroy_entry, NULL, 0);

        wl_output_add_listener(output->output, &output_listener, buf);
        if (ctx->xdg_output_manager) {
            output->xdg_out = zxdg_output_manager_v1_get_xdg_output(ctx->xdg_output_manager,
                                                                    output->output);
            zxdg_output_v1_add_listener(output->xdg_out, &xdg_output_listener, buf);
        }
    }

    if (!strcmp(interface, wl_compositor_interface.name) && (ver >= 3) && found++)
        ctx->compositor = wl_registry_bind(reg, id, &wl_compositor_interface, 3);

    if (!strcmp(interface, wl_subcompositor_interface.name) && found++)
        ctx->subcompositor = wl_registry_bind(reg, id, &wl_subcompositor_interface, 1);

    if (!strcmp(interface, zxdg_output_manager_v1_interface.name) && found++)
        ctx->xdg_output_manager = wl_registry_bind(reg, id, &zxdg_output_manager_v1_interface, FFMIN(ver, 2));

    if (!strcmp(interface, xdg_wm_base_interface.name) && found++)
        ctx->wm_base = wl_registry_bind(reg, id, &xdg_wm_base_interface, FFMIN(ver, 2));

    if (!strcmp(interface, wl_seat_interface.name) && !ctx->seat_id && found++)
        ctx->seat_id = id;

    if (!strcmp(interface, wl_shm_interface.name) && found++)
        ctx->shm_interface = wl_registry_bind(reg, id, &wl_shm_interface, 1);

    if (!strcmp(interface, wl_data_device_manager_interface.name) && (ver >= 3) && found++)
        ctx->dnd_device_manager = wl_registry_bind(reg, id, &wl_data_device_manager_interface, 3);

    if (!strcmp(interface, zxdg_decoration_manager_v1_interface.name) && found++)
        ctx->xdg_decoration_manager = wl_registry_bind(reg, id, &zxdg_decoration_manager_v1_interface, 1);

    if (!strcmp(interface, zwp_idle_inhibit_manager_v1_interface.name) && found++)
        ctx->idle_inhibit_manager = wl_registry_bind(reg, id, &zwp_idle_inhibit_manager_v1_interface, 1);

    if (!strcmp(interface, zwp_linux_dmabuf_v1_interface.name) && found++)
        ctx->dmabuf = wl_registry_bind(reg, id, &zwp_linux_dmabuf_v1_interface, 3);

    if (!strcmp(interface, zwlr_export_dmabuf_manager_v1_interface.name) && found++)
        ctx->dmabuf_export_manager = wl_registry_bind(reg, id, &zwlr_export_dmabuf_manager_v1_interface, 1);

    if (!strcmp(interface, zwlr_screencopy_manager_v1_interface.name) && found++)
        ctx->screencopy_export_manager = wl_registry_bind(reg, id, &zwlr_screencopy_manager_v1_interface, FFMIN(ver, 3));

    if (!strcmp(interface, zwlr_layer_shell_v1_interface.name) && found++)
        ctx->layer_shell = wl_registry_bind(reg, id, &zwlr_layer_shell_v1_interface, 1);

    if (found > 1)
        sp_log(ctx, SP_LOG_TRACE, "Registered for interface %s\n", interface);

    pthread_mutex_unlock(&ctx->lock);
}

static AVBufferRef *find_iosysentry_by_output(AVBufferRef *ref, void *opaque)
{
    IOSysEntry *entry = (IOSysEntry *)ref->data;
    WaylandIOPriv *priv = (WaylandIOPriv *)entry->api_priv;
    if (priv->output == (struct wl_output *)opaque)
        return ref;
    return NULL;
}

AVBufferRef *sp_find_ref_wayland_output(WaylandCtx *ctx,
                                        struct wl_output *out, uint32_t id)
{
    AVBufferRef *ret = NULL;

    pthread_mutex_lock(&ctx->lock);

    if (out)
        ret = sp_bufferlist_ref(ctx->output_list, find_iosysentry_by_output, (void *)out);
    else
        ret = sp_bufferlist_ref(ctx->output_list, sp_bufferlist_iosysentry_by_id, &id);

    pthread_mutex_unlock(&ctx->lock);

    return ret;
}

static void registry_handle_remove(void *opaque, struct wl_registry *reg,
                                   uint32_t id)
{
    WaylandCtx *ctx = (WaylandCtx *)((AVBufferRef *)opaque)->data;

    pthread_mutex_lock(&ctx->lock);

    uint32_t new_id = sp_iosys_gen_identifier(ctx, id, 0);
    AVBufferRef *ref = sp_bufferlist_pop(ctx->output_list, sp_bufferlist_iosysentry_by_id, &new_id);
    av_buffer_unref(&ref);

    pthread_mutex_unlock(&ctx->lock);
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_add,
    .global_remove = registry_handle_remove,
};

static void *wayland_events_thread(void *arg)
{
    WaylandCtx *ctx = arg;

    if (ctx->display_fd == -1)
        return NULL;

    sp_set_thread_name_self(sp_class_get_name(ctx));

    while (1) {
        while (wl_display_prepare_read(ctx->display))
			wl_display_dispatch_pending(ctx->display);

        struct pollfd fds[2] = {
            {.fd = ctx->display_fd,     .events = POLLIN, },
            {.fd = ctx->wakeup_pipe[0], .events = POLLIN, },
        };

        int ret = wl_display_flush(ctx->display);
        if (ret < 0) {
            if (errno == EAGAIN) {
                fds[0].events |= POLLOUT;
            } else {
                sp_log(ctx, SP_LOG_ERROR, "Error flushing display FD!\n");
                goto fail;
            }
        }

        if (poll(fds, 2, -1) < 0) {
            sp_log(ctx, SP_LOG_ERROR, "Error reading display FD!\n");
            goto fail;
        }

        if (fds[0].revents & POLLIN) {
            if (wl_display_read_events(ctx->display) < 0) {
                sp_log(ctx, SP_LOG_ERROR, "Error reading events!\n");
                goto fail;
            }
            wl_display_dispatch_pending(ctx->display);
        } else {
            wl_display_cancel_read(ctx->display);
        }

        if (fds[0].revents & POLLOUT)
            wl_display_flush(ctx->display);

        if (fds[1].revents & POLLIN) {
            int64_t res = sp_flush_wakeup_pipe(ctx->wakeup_pipe);
            if (res == AVERROR_EOF)
                break;
        }
    }

    return NULL;

fail:
    wl_display_cancel_read(ctx->display);
    close(ctx->display_fd);
    ctx->display_fd = -1;
    return NULL;
}

static void wayland_uninit(void *opaque, uint8_t *data)
{
    pthread_mutex_lock(&ctx_ref_lock);

    WaylandCtx *ctx = (WaylandCtx *)data;

    sp_write_wakeup_pipe(ctx->wakeup_pipe, AVERROR_EOF);
    pthread_join(ctx->event_thread, NULL);
    sp_close_wakeup_pipe(ctx->wakeup_pipe);

    sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_DESTROY, NULL);

    sp_bufferlist_free(&ctx->events);
    sp_bufferlist_free(&ctx->output_list);

    if (ctx->xdg_output_manager)
        zxdg_output_manager_v1_destroy(ctx->xdg_output_manager);

    if (ctx->compositor)
        wl_compositor_destroy(ctx->compositor);

    if (ctx->subcompositor)
        wl_subcompositor_destroy(ctx->subcompositor);

    if (ctx->wm_base)
        xdg_wm_base_destroy(ctx->wm_base);

    if (ctx->shm_interface)
        wl_shm_destroy(ctx->shm_interface);

    if (ctx->dnd_device_manager)
        wl_data_device_manager_destroy(ctx->dnd_device_manager);

    if (ctx->xdg_decoration_manager)
        zxdg_decoration_manager_v1_destroy(ctx->xdg_decoration_manager);

    if (ctx->idle_inhibit_manager)
       zwp_idle_inhibit_manager_v1_destroy(ctx->idle_inhibit_manager);

    if (ctx->dmabuf)
        zwp_linux_dmabuf_v1_destroy(ctx->dmabuf);

    if (ctx->dmabuf_export_manager)
        zwlr_export_dmabuf_manager_v1_destroy(ctx->dmabuf_export_manager);

    if (ctx->screencopy_export_manager)
        zwlr_screencopy_manager_v1_destroy(ctx->screencopy_export_manager);

    if (ctx->layer_shell)
        zwlr_layer_shell_v1_destroy(ctx->layer_shell);

    if (ctx->drm_device_ref)
        av_buffer_unref(&ctx->drm_device_ref);

    wl_registry_destroy(ctx->registry);

    wl_display_disconnect(ctx->display);

    pthread_mutex_destroy(&ctx->lock);
    sp_class_free(ctx);
    av_free(ctx);

    pthread_mutex_unlock(&ctx_ref_lock);
    pthread_mutex_destroy(&ctx_ref_lock);
}

static int find_render_node(char **node)
{
    drmDevice *devices[64];

    int n = drmGetDevices2(0, devices, SP_ARRAY_ELEMS(devices));
    for (int i = (n - 1); i >= 0; i--) {
        drmDevice *dev = devices[i];
        if (!(dev->available_nodes & (1 << DRM_NODE_RENDER)))
            continue;
        av_freep(node);
        *node = av_strdup(dev->nodes[DRM_NODE_RENDER]);
        if (!(*node))
            return AVERROR(ENOMEM);
        drmFreeDevices(devices, n);
        return 0;
    }

    drmFreeDevices(devices, n);
    return AVERROR(ENOSYS);
}

static void drm_device_free(AVHWDeviceContext *hwdev)
{
    AVDRMDeviceContext *hwctx = hwdev->hwctx;
    close(hwctx->fd);
}

static int init_drm_hwcontext(WaylandCtx *ctx)
{
    int err;
    ctx->drm_device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_DRM);
    if (!ctx->drm_device_ref)
        return AVERROR(ENOMEM);

    AVHWDeviceContext *ref_data = (AVHWDeviceContext*)ctx->drm_device_ref->data;
    AVDRMDeviceContext *hwctx = ref_data->hwctx;

    char *render_node = NULL;
    if ((err = find_render_node(&render_node))) {
        sp_log(ctx, SP_LOG_ERROR, "Failed to find a DRM device: %s!\n",
               av_err2str(err));
        av_buffer_unref(&ctx->drm_device_ref);
        return err;
    }

    hwctx->fd = open(render_node, O_RDWR);
    if (hwctx->fd < 0) {
        sp_log(ctx, SP_LOG_ERROR, "Failed to open DRM device %s: %s!\n",
               render_node, av_err2str(err));
        av_buffer_unref(&ctx->drm_device_ref);
        av_free(render_node);
        return AVERROR(errno);
    }

    ref_data->free = drm_device_free;

    err = av_hwdevice_ctx_init(ctx->drm_device_ref);
    if (err < 0) {
        sp_log(ctx, SP_LOG_ERROR, "Failed to init DRM device %s: %s!\n",
               render_node, av_err2str(err));
        av_buffer_unref(&ctx->drm_device_ref);
        av_free(render_node);
        return err;
    }

    av_free(render_node);

    return 0;
}

int sp_wayland_create(AVBufferRef **ref)
{
    int err = 0;

    pthread_mutex_lock(&ctx_ref_lock);

    AVBufferRef *new_ctx_ref;
    if (ctx_ref) {
        new_ctx_ref = av_buffer_ref(ctx_ref);
        if (new_ctx_ref)
            goto end;
    }

    WaylandCtx *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return AVERROR(ENOMEM);

    new_ctx_ref = ctx_ref = av_buffer_create((uint8_t *)ctx, sizeof(*ctx),
                                             wayland_uninit, NULL, 0);
    if (!ctx_ref) {
        av_free(ctx);
        return AVERROR(ENOMEM);
    }

    err = sp_class_alloc(ctx, "wayland_io", SP_TYPE_CONTEXT, NULL);
    if (err < 0)
        goto fail;

    /* Init context lock */
    pthread_mutex_init(&ctx->lock, NULL);

    /* Wakeup pipe */
    ctx->wakeup_pipe[0] = ctx->wakeup_pipe[1] = ctx->display_fd = -1;
    sp_make_wakeup_pipe(ctx->wakeup_pipe);

    /* Output list */
    ctx->output_list = sp_bufferlist_new();
    ctx->events = sp_bufferlist_new();

    /* Connect to display */
    ctx->display = wl_display_connect(NULL);
    if (!ctx->display) {
        sp_log(ctx, SP_LOG_ERROR, "Failed to connect to display!\n");
        err = AVERROR(EINVAL);
        goto fail;
    }

    /* Register */
    ctx->registry = wl_display_get_registry(ctx->display);
    wl_registry_add_listener(ctx->registry, &registry_listener, new_ctx_ref);

    /* Force the server to process events */
    wl_display_dispatch(ctx->display);
    wl_display_roundtrip(ctx->display);

    /* Get display FD */
    ctx->display_fd = wl_display_get_fd(ctx->display);

    /* Init the central hw device reference */
    if ((err = init_drm_hwcontext(ctx)))
        goto fail;

    /* Start the event thread */
    pthread_create(&ctx->event_thread, NULL, wayland_events_thread, ctx);

end:
    pthread_mutex_unlock(&ctx_ref_lock);
    *ref = new_ctx_ref;

    return 0;

fail:
    av_buffer_unref(&new_ctx_ref);

    *ref = NULL;

    pthread_mutex_unlock(&ctx_ref_lock);
    return err;
}
