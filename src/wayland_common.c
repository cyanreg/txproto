#include "wayland_common.h"
#include "utils.h"

#include <unistd.h>
#include <poll.h>
#include <fcntl.h>

static pthread_mutex_t init_ctx_mutex = PTHREAD_MUTEX_INITIALIZER;
static WaylandCtx *init_ctx = NULL;
static int init_ctx_refcount = 0;

static void output_handle_geometry(void *data, struct wl_output *wl_output,
                                   int32_t x, int32_t y, int32_t phys_width,
                                   int32_t phys_height, int32_t subpixel,
                                   const char *make, const char *model,
                                   int32_t transform)
{
    WaylandOutput *output = data;
    output->make = av_strdup(make);
    output->model = av_strdup(model);
}

static void output_handle_mode(void *data, struct wl_output *wl_output,
                               uint32_t flags, int32_t width, int32_t height,
                               int32_t refresh)
{
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        WaylandOutput *output = data;
        output->width = width;
        output->height = height;
        output->framerate = (AVRational){ refresh, 1000 };
    }
}

static void output_handle_done(void *data, struct wl_output *wl_output)
{
    /* Nothing to do */
}

static void output_handle_scale(void *data, struct wl_output *wl_output,
                                int32_t factor)
{
    /* Nothing to do */
}

static const struct wl_output_listener output_listener = {
    .geometry = output_handle_geometry,
    .mode = output_handle_mode,
    .done = output_handle_done,
    .scale = output_handle_scale,
};

static void registry_handle_add(void *data, struct wl_registry *reg,
                                uint32_t id, const char *interface,
                                uint32_t ver)
{
    int found = 1;
    WaylandCtx *ctx = data;

    if (!strcmp(interface, wl_output_interface.name) && (ver >= 2) && found++) {
        WaylandOutput *output = av_mallocz(sizeof(*output));

        output->id     = id;
        output->scale  = 1;
        output->output = wl_registry_bind(reg, id, &wl_output_interface, 2);

        wl_output_add_listener(output->output, &output_listener, output);
        wl_list_insert(&ctx->output_list, &output->link);
    }

    if (!strcmp(interface, wl_compositor_interface.name) && (ver >= 3) && found++)
        ctx->compositor = wl_registry_bind(reg, id, &wl_compositor_interface, 3);

    if (!strcmp(interface, xdg_wm_base_interface.name) && found++)
        ctx->wm_base = wl_registry_bind(reg, id, &xdg_wm_base_interface, 2);

    if (!strcmp(interface, wl_seat_interface.name) && found++)
        ctx->seat = wl_registry_bind(reg, id, &wl_seat_interface, 1);

    if (!strcmp(interface, wl_shm_interface.name) && found++)
        ctx->shm_interface = wl_registry_bind(reg, id, &wl_shm_interface, 1);

    if (!strcmp(interface, wl_data_device_manager_interface.name) && (ver >= 3) && found++)
        ctx->dnd_device_manager = wl_registry_bind(reg, id, &wl_data_device_manager_interface, 3);

    if (!strcmp(interface, zxdg_decoration_manager_v1_interface.name) && found++)
        ctx->xdg_decoration_manager = wl_registry_bind(reg, id, &zxdg_decoration_manager_v1_interface, 1);

    if (!strcmp(interface, zwp_idle_inhibit_manager_v1_interface.name) && found++)
        ctx->idle_inhibit_manager = wl_registry_bind(reg, id, &zwp_idle_inhibit_manager_v1_interface, 1);

    if (!strcmp(interface, zwlr_export_dmabuf_manager_v1_interface.name) && found++)
        ctx->dmabuf_export_manager = wl_registry_bind(reg, id, &zwlr_export_dmabuf_manager_v1_interface, 1);

    if (!strcmp(interface, zwlr_screencopy_manager_v1_interface.name) && found++)
        ctx->screencopy_export_manager = wl_registry_bind(reg, id, &zwlr_screencopy_manager_v1_interface, 1);

    if (found > 1)
        av_log(ctx, AV_LOG_DEBUG, "Registered for interface %s\n", interface);
}

static void remove_output(WaylandOutput *out)
{
    wl_list_remove(&out->link);
    av_free(out->make);
    av_free(out->model);
    av_free(out);
}

WaylandOutput *sp_find_wayland_output(WaylandCtx *ctx,
                                      struct wl_output *out, uint32_t id)
{
    WaylandOutput *output, *tmp;
    wl_list_for_each_safe(output, tmp, &ctx->output_list, link)
        if ((output->output == out) || (output->id == id))
            return output;
    return NULL;
}

static void registry_handle_remove(void *data, struct wl_registry *reg,
                                   uint32_t id)
{
    WaylandCtx *ctx = data;
    WaylandOutput *output = sp_find_wayland_output(ctx, NULL, id);
    output->remove_callback(output->remove_ctx, id);
    remove_output(output);
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_add,
    .global_remove = registry_handle_remove,
};

static void drm_device_free(AVHWDeviceContext *hwdev)
{
    close(((AVDRMDeviceContext *)hwdev->hwctx)->fd);
}

static int init_drm_hwcontext(WaylandCtx *ctx)
{
    int err;

    /* DRM hwcontext */
    ctx->drm_device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_DRM);
    if (!ctx->drm_device_ref)
        return AVERROR(ENOMEM);

    AVHWDeviceContext *ref_data = (AVHWDeviceContext*)ctx->drm_device_ref->data;
    AVDRMDeviceContext *hwctx = ref_data->hwctx;

    /* Hope this is the right one */
    hwctx->fd = open("/dev/dri/renderD128", O_RDWR);
    ref_data->free = drm_device_free;

    err = av_hwdevice_ctx_init(ctx->drm_device_ref);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Failed to open DRM device: %s!\n", av_err2str(err));
        return err;
    }

    return 0;
}

void *wayland_events_thread(void *arg)
{
    WaylandCtx *ctx = arg;

    pthread_setname_np(pthread_self(), ctx->class->class_name);

    while (1) {
        while (wl_display_prepare_read(ctx->display))
			wl_display_dispatch_pending(ctx->display);
        wl_display_flush(ctx->display);

        struct pollfd fds[2] = {
            {.fd = wl_display_get_fd(ctx->display), .events = POLLIN, },
            {.fd = ctx->wakeup_pipe[0],             .events = POLLIN, },
        };

        if (poll(fds, FF_ARRAY_ELEMS(fds), -1) < 0) {
            wl_display_cancel_read(ctx->display);
            break;
        }

        if (fds[0].revents & POLLIN) {
            if (wl_display_read_events(ctx->display) < 0) {
                av_log(ctx, AV_LOG_ERROR, "Error reading events!\n");
                break;
            }
            wl_display_dispatch_pending(ctx->display);
        }

        /* Stop the loop */
        if (fds[1].revents & POLLIN) {
            sp_flush_wakeup_pipe(ctx->wakeup_pipe);
            break;
        }
    }

    return NULL;
}

void sp_waylad_uninit(WaylandCtx **s)
{
    pthread_mutex_lock(&init_ctx_mutex);

    WaylandCtx *ctx = init_ctx;

    if (--init_ctx_refcount > 0)
        goto end;

    sp_write_wakeup_pipe(ctx->wakeup_pipe);
    pthread_join(ctx->event_thread, NULL);

    WaylandOutput *output, *tmp_o;
    wl_list_for_each_safe(output, tmp_o, &ctx->output_list, link)
        remove_output(output);

    if (ctx->dmabuf_export_manager)
        zwlr_export_dmabuf_manager_v1_destroy(ctx->dmabuf_export_manager);

    if (ctx->screencopy_export_manager)
        zwlr_screencopy_manager_v1_destroy(ctx->screencopy_export_manager);

    av_buffer_unref(&ctx->drm_device_ref);

    wl_display_disconnect(ctx->display);

    av_freep(&ctx->class);
    av_freep(&init_ctx);

end:
    *s = NULL;
    pthread_mutex_unlock(&init_ctx_mutex);
}

int sp_wayland_init(WaylandCtx **s)
{
    int err = 0;

    pthread_mutex_lock(&init_ctx_mutex);

    WaylandCtx *ctx = init_ctx;

    if (init_ctx)
        goto end;

    ctx = av_mallocz(sizeof(*ctx));
    ctx->class = av_mallocz(sizeof(*ctx->class));
    *ctx->class = (AVClass) {
        .class_name = "wayland",
        .item_name  = av_default_item_name,
        .version    = LIBAVUTIL_VERSION_INT,
    };

    ctx->display = wl_display_connect(NULL);
    if (!ctx->display) {
        av_log(ctx, AV_LOG_ERROR, "Failed to connect to display!\n");
        return AVERROR(EINVAL);
    }

    ctx->wakeup_pipe[0] = ctx->wakeup_pipe[1] = -1;

    sp_make_wakeup_pipe(ctx->wakeup_pipe);

    wl_list_init(&ctx->output_list);

    ctx->registry = wl_display_get_registry(ctx->display);
    wl_registry_add_listener(ctx->registry, &registry_listener, ctx);

    wl_display_roundtrip(ctx->display);
    wl_display_dispatch(ctx->display);

    if (!ctx->dmabuf_export_manager) {
        av_log(ctx, AV_LOG_ERROR, "Compositor doesn't support %s, capture unavailable!\n",
               zwlr_export_dmabuf_manager_v1_interface.name);
    } else {
        if ((err = init_drm_hwcontext(ctx)))
            goto fail;
    }

    if (!ctx->screencopy_export_manager) {
        av_log(ctx, AV_LOG_ERROR, "Compositor doesn't support %s, capture unavailable!\n",
               zwlr_screencopy_manager_v1_interface.name);
    }

    init_ctx = ctx;

    /* Start the event thread */
    pthread_create(&ctx->event_thread, NULL, wayland_events_thread, ctx);

end:
    init_ctx_refcount++;
    *s = ctx;

    pthread_mutex_unlock(&init_ctx_mutex);

    return 0;

fail:
    //TODO
    pthread_mutex_unlock(&init_ctx_mutex);
    return err;
}
