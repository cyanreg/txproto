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

#include <stdatomic.h>

#include <xcb/xcb.h>
#include <xcb/shm.h>
#include <xcb/randr.h>
#include <sys/shm.h>

#include <libavutil/time.h>
#include <libavutil/pixdesc.h>

#include "iosys_common.h"
#include "utils.h"
#include "ctrl_template.h"
#include "../config.h"

const IOSysAPI src_xcb;

typedef struct XCBCtx {
    SPClass *class;

    xcb_connection_t *con;

    atomic_int quit;
    pthread_t source_update;
    pthread_mutex_t lock;

    /* Sinks list */
    SPBufferList *entries;
    SPBufferList *events;
} XCBCtx;

typedef struct XCBPriv {
    XCBCtx *main_ctx;

    xcb_screen_t *root;
} XCBPriv;

typedef struct XCBCapture {
    atomic_int quit;
    pthread_t pull_thread;
    int err;

    xcb_window_t win;
    xcb_drawable_t drawable;

    AVBufferPool *pool;
    size_t fsize;

    int dropped_frames;

    int64_t epoch;
    int64_t next_frame_ts;
    int64_t frame_delay;
} XCBCapture;

static void free_shm_buffer(void *opaque, uint8_t *data)
{
    shmdt(data);
}

static AVBufferRef *allocate_shm_buffer(void *opaque, size_t size)
{
    xcb_connection_t *con = opaque;
    xcb_shm_seg_t segment;
    AVBufferRef *ref;
    uint8_t *data;
    int id;

    id = shmget(IPC_PRIVATE, size, IPC_CREAT | 0777);
    if (id == -1)
        return NULL;

    segment = xcb_generate_id(con);
    xcb_shm_attach(con, segment, id, 0);
    data = shmat(id, NULL, 0);
    shmctl(id, IPC_RMID, 0);
    if ((intptr_t)data == -1 || !data)
        return NULL;

    ref = av_buffer_create(data, size, free_shm_buffer, (void *)(ptrdiff_t)segment, 0);
    if (!ref)
        shmdt(data);

    return ref;
}

static int alloc_buffer_pool(XCBCtx *ctx, XCBCapture *priv, size_t fsize,
                             AVBufferRef **buf_out)
{
    if (priv->fsize != fsize) {
        av_buffer_pool_uninit(&priv->pool);
        priv->pool = av_buffer_pool_init2(fsize, ctx->con, allocate_shm_buffer, NULL);
        if (!priv->pool) {
            sp_log(ctx, SP_LOG_ERROR, "Unable to allocate SHM buffer pool!\n");
            return AVERROR(ENOMEM);
        }

        priv->fsize = fsize;
    }

    AVBufferRef *buf = av_buffer_pool_get(priv->pool);
    if (!buf) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to allocate SHM buffer!\n");
        return AVERROR(ENOMEM);
    }

    *buf_out = buf;

    return 0;
}

static int pixfmt_from_pixmap_format(XCBCtx *ctx, int depth,
                                     int *pix_fmt, int *bpp)
{
    const xcb_setup_t *setup = xcb_get_setup(ctx->con);
    const xcb_format_t *fmt  = xcb_setup_pixmap_formats(setup);
    int length               = xcb_setup_pixmap_formats_length(setup);

    *pix_fmt = 0;

    while (length--) {
        if (fmt->depth == depth) {
            switch (depth) {
            case 32:
                if (fmt->bits_per_pixel == 32)
                    *pix_fmt = setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST ?
                               AV_PIX_FMT_BGR0 : AV_PIX_FMT_0RGB;
                break;
            case 24:
                if (fmt->bits_per_pixel == 32)
                    *pix_fmt = setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST ?
                               AV_PIX_FMT_BGR0 : AV_PIX_FMT_0RGB;
                else if (fmt->bits_per_pixel == 24)
                    *pix_fmt = setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST ?
                               AV_PIX_FMT_BGR24 : AV_PIX_FMT_RGB24;
                break;
            case 16:
                if (fmt->bits_per_pixel == 16)
                    *pix_fmt = setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST ?
                               AV_PIX_FMT_RGB565LE : AV_PIX_FMT_RGB565BE;
                break;
            case 15:
                if (fmt->bits_per_pixel == 16)
                    *pix_fmt = setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST ?
                               AV_PIX_FMT_RGB555LE : AV_PIX_FMT_RGB555BE;
                break;
            case 8:
                if (fmt->bits_per_pixel == 8)
                    *pix_fmt = AV_PIX_FMT_RGB8;
                break;
            }
        }

        if (*pix_fmt) {
            *bpp        = fmt->bits_per_pixel;
            return 0;
        }

        fmt++;
    }

    return AVERROR(EINVAL);
}

static void *xcb_thread(void *s)
{
    int err = 0;
    IOSysEntry *entry = s;
    XCBPriv *api_priv = entry->api_priv;
    XCBCapture *priv = entry->io_priv;
    XCBCtx *ctx = api_priv->main_ctx;

    sp_set_thread_name_self(sp_class_get_name(entry));

    xcb_get_geometry_cookie_t geo_c;
    xcb_get_geometry_reply_t *geo_r;
    geo_c = xcb_get_geometry(ctx->con, priv->win);

    sp_eventlist_dispatch(entry, entry->events, SP_EVENT_ON_CONFIG | SP_EVENT_ON_INIT, NULL);

    while (!atomic_load(&ctx->quit)) {
        geo_r = xcb_get_geometry_reply(ctx->con, geo_c, NULL);

        AVFrame *frame = av_frame_alloc();

        int bpp;
        size_t fsize;
        enum AVPixelFormat pixfmt;
        AVBufferRef *out_buf = NULL;

        err = pixfmt_from_pixmap_format(ctx, geo_r->depth, &pixfmt, &bpp);
        if (err < 0) {
            sp_log(entry, SP_LOG_ERROR, "Unable to map bit depth to pixel format!\n");
            goto end;
        }

        fsize = (geo_r->width * geo_r->height * bpp) / 8;

        err = alloc_buffer_pool(ctx, priv, fsize, &out_buf);
        if (err < 0)
            goto end;

        xcb_shm_seg_t seg;
        xcb_shm_get_image_cookie_t img_c;
        xcb_shm_get_image_reply_t *img_r;
        xcb_generic_error_t *xerr = NULL;

        seg = (xcb_shm_seg_t)(uintptr_t)av_buffer_pool_buffer_get_opaque(out_buf);

        img_c = xcb_shm_get_image(ctx->con, priv->drawable,
                                  0, 0, geo_r->width, geo_r->height, ~0,
                                  XCB_IMAGE_FORMAT_Z_PIXMAP, seg, 0);
        img_r = xcb_shm_get_image_reply(ctx->con, img_c, &xerr);

        xcb_flush(ctx->con);

        if (xerr) {
            sp_log(entry, SP_LOG_ERROR,
                   "Cannot get the image data "
                   "event_error: response_type:%u error_code:%u "
                   "sequence:%u resource_id:%u minor_code:%u major_code:%u.\n",
                   xerr->response_type, xerr->error_code,
                   xerr->sequence, xerr->resource_id,
                   xerr->minor_code, xerr->major_code);

            free(xerr);
            av_frame_free(&frame);
            err = AVERROR(EACCES);
            goto end;
        }

        free(img_r);

        frame->width       = geo_r->width;
        frame->height      = geo_r->height;
        frame->format      = pixfmt;
        frame->pts         = av_gettime_relative() - priv->epoch;
        frame->time_base   = AV_TIME_BASE_Q;
        frame->data[0]     = (uint8_t *)out_buf->data;
        frame->linesize[0] = geo_r->width * bpp / 8;
        frame->buf[0]      = out_buf;

        frame->opaque_ref = av_buffer_allocz(sizeof(FormatExtraData));

        FormatExtraData *fe = (FormatExtraData *)frame->opaque_ref->data;
        fe->time_base       = AV_TIME_BASE_Q;
        fe->avg_frame_rate  = entry->framerate;

        sp_log(entry, SP_LOG_TRACE, "Pushing frame to FIFO, pts = %f\n",
               av_q2d(fe->time_base) * frame->pts);

        /* We don't do this check at the start on since there's still some chance
         * whatever's consuming the FIFO will be done by now. */
        err = sp_frame_fifo_push(entry->frames, frame);
        av_frame_free(&frame);
        if (err == AVERROR(ENOBUFS)) {
            priv->dropped_frames++;
            sp_log(entry, SP_LOG_WARN, "Dropping frame (%i dropped so far)!\n",
                   priv->dropped_frames);

            SPGenericData entries[] = {
                D_TYPE("dropped_frames", NULL, priv->dropped_frames),
                { 0 },
            };
            sp_eventlist_dispatch(entry, entry->events, SP_EVENT_ON_STATS, entries);
        } else if (err) {
            sp_log(entry, SP_LOG_ERROR, "Unable to push frame to FIFO: %s!\n",
                   av_err2str(err));
            goto end;
        }

        free(geo_r);

        geo_c = xcb_get_geometry(ctx->con, priv->win);

        /* Framerate limiting */
        int64_t now = av_gettime_relative();
        if (priv->next_frame_ts && (now < priv->next_frame_ts)) {
            int64_t wait_time;
            while (1) {
                wait_time = priv->next_frame_ts - now;
                if (wait_time <= 0)
                    break;
                av_usleep(wait_time);
                now = av_gettime_relative();
            }
        }
        priv->next_frame_ts = now + priv->frame_delay;
    }

end:
    priv->err = err;
    return NULL;
}

static int xcb_ioctx_ctrl_cb(AVBufferRef *event_ref, void *callback_ctx, void *ctx,
                             void *dep_ctx, void *data)
{
    SPCtrlTemplateCbCtx *event = callback_ctx;

    IOSysEntry *entry = ctx;
    XCBCapture *io_priv = entry->io_priv;

    if (event->ctrl & SP_EVENT_CTRL_START) {
        io_priv->epoch = atomic_load(event->epoch);
        pthread_create(&io_priv->pull_thread, NULL, xcb_thread, entry);
        sp_log(entry, SP_LOG_VERBOSE, "Started capture thread\n");
        return 0;
    } else if (event->ctrl & SP_EVENT_CTRL_STOP) {
        atomic_store(&io_priv->quit, 1);
        pthread_join(io_priv->pull_thread, NULL);
        sp_log(entry, SP_LOG_VERBOSE, "Stopped capture thread\n");
        return 0;
    } else {
        return AVERROR(ENOTSUP);
    }
}

static int xcb_ioctx_ctrl(AVBufferRef *entry, SPEventType ctrl, void *arg)
{
    IOSysEntry *iosys_entry = (IOSysEntry *)entry->data;
    return sp_ctrl_template(iosys_entry, iosys_entry->events, 0x0,
                            xcb_ioctx_ctrl_cb, ctrl, arg);
}

static int xcb_init_io(AVBufferRef *ctx_ref, AVBufferRef *entry,
                       AVDictionary *opts)
{
    IOSysEntry *iosys_entry = (IOSysEntry *)entry->data;
    XCBPriv *priv = iosys_entry->api_priv;

    XCBCapture *cap_priv = av_mallocz(sizeof(*cap_priv));
    if (!cap_priv)
        return AVERROR(ENOMEM);

    cap_priv->quit = ATOMIC_VAR_INIT(0);

    iosys_entry->ctrl = xcb_ioctx_ctrl;
    iosys_entry->events = sp_bufferlist_new();

    cap_priv->win = priv->root->root;
    cap_priv->drawable = priv->root->root;

    cap_priv->frame_delay = av_rescale_q(1, av_inv_q(iosys_entry->framerate),
                                         AV_TIME_BASE_Q);

    iosys_entry->io_priv = cap_priv;
    return 0;
}

static void destroy_entry(void *opaque, uint8_t *data)
{
    IOSysEntry *entry = (IOSysEntry *)data;
    XCBPriv *priv = entry->api_priv;

    if (entry->io_priv) {
        XCBCapture *io_priv = entry->io_priv;

        atomic_store(&io_priv->quit, 1);
        pthread_join(io_priv->pull_thread, NULL);

        /* EOS */
        sp_frame_fifo_push(entry->frames, NULL);

        av_buffer_pool_uninit(&io_priv->pool);
    }

    av_free(priv);
    sp_class_free(entry);
    av_free(entry);
}

static void iter_monitors(XCBCtx *ctx, xcb_screen_t *xscreen)
{
    xcb_connection_t *con = ctx->con;

    xcb_randr_get_monitors_cookie_t mon_c;
    xcb_randr_get_monitors_reply_t *mon_r;

    mon_c = xcb_randr_get_monitors(con, xscreen->root, true);
    mon_r = xcb_randr_get_monitors_reply(con, mon_c, 0);
    if (!mon_r) {
        sp_bufferlist_free(&ctx->entries);
        return;
    }

    int monitors = xcb_randr_get_monitors_monitors_length(mon_r);
    if (!monitors) {
        sp_bufferlist_free(&ctx->entries);
        free(mon_r);
        return;
    }

    xcb_randr_monitor_info_iterator_t mon_i;
    mon_i = xcb_randr_get_monitors_monitors_iterator(mon_r);

    for (int i = 0; i < monitors; i++) {
        xcb_randr_monitor_info_t *mon = mon_i.data;

        int change = 0;
        int new_entry = 0;
        IOSysEntry *entry = NULL;
        XCBPriv *priv = NULL;
        AVBufferRef *entry_ref = sp_bufferlist_ref(ctx->entries,
                                                   sp_bufferlist_iosysentry_by_id,
                                                   &mon->name);

        if (!entry_ref) {
            entry = av_mallocz(sizeof(*entry));
            priv = av_mallocz(sizeof(*priv));
            sp_class_alloc(entry, NULL, SP_TYPE_VIDEO_BIDIR, ctx);
            new_entry = 1;
        } else {
            entry = (IOSysEntry *)entry_ref->data;
            priv = entry->api_priv;
        }

        if (mon->name) {
            xcb_get_atom_name_cookie_t atom_c;
            xcb_get_atom_name_reply_t *atom_r;

            atom_c = xcb_get_atom_name(con, mon->name);
			atom_r = xcb_get_atom_name_reply(con, atom_c, 0);

            if (atom_r) {
                // xcb strings are not null terminated
                const char *atom_name = xcb_get_atom_name_name(atom_r);
                int name_len = xcb_get_atom_name_name_length(atom_r);
                char *name = strndup(atom_name, name_len);

                const char *old_name = sp_class_get_name(entry);
                if (old_name)
                    change |= strcmp(name, old_name);
                sp_class_set_name(entry, name);
                av_free(name);
                av_free(atom_r);
            } else {
                sp_class_set_name(entry, "unknown");
            }
        } else {
            sp_class_set_name(entry, "unknown");
        }

        xcb_generic_error_t *err;
        xcb_randr_get_screen_info_cookie_t info_c;
        xcb_randr_get_screen_info_reply_t *info_r;

        info_c = xcb_randr_get_screen_info(con, xscreen->root);
        info_r = xcb_randr_get_screen_info_reply(con, info_c, &err);
        if (!info_r) {
            if (new_entry) {
                av_free(entry);
                av_free(priv);
            }
            goto cont;
        }

        if (new_entry) {
            priv->main_ctx = ctx;

            entry->identifier = mon->name;
            entry->api_id = mon->name;
            entry->api_priv = priv;
            entry->type = SP_IO_TYPE_VIDEO_DISPLAY;
            entry->frames = sp_frame_fifo_create(entry, 0, 0);
        }

        change |= (entry->width != mon->width) ||
                  (entry->height != mon->height) ||
                  (entry->is_default != !!mon->primary) ||
                  (priv->root != xscreen) ||
                  (entry->framerate.num != info_r->rate);

        priv->root = xscreen;
        entry->framerate = av_make_q(info_r->rate, 1);
        entry->scale = 1;
        entry->width = mon->width;
        entry->height = mon->height;
        entry->is_default = !!mon->primary;

        free(info_r);

        if (new_entry) {
            AVBufferRef *buf = av_buffer_create((uint8_t *)entry, sizeof(*entry),
                                                destroy_entry, NULL, 0);

            sp_bufferlist_append_noref(ctx->entries, buf);
        } else {
            if (change) {
                sp_eventlist_dispatch(entry, ctx->events,
                                      SP_EVENT_ON_CHANGE |
                                      SP_EVENT_TYPE_SOURCE |
                                      SP_EVENT_TYPE_SINK,
                                      entry);
            }
            av_buffer_unref(&entry_ref);
        }

cont:
        xcb_randr_monitor_info_next(&mon_i);
    }

    free(mon_r);
}

static void update_entries(XCBCtx *ctx)
{
    pthread_mutex_lock(&ctx->lock);

    const xcb_setup_t *setup = xcb_get_setup(ctx->con);
    xcb_screen_iterator_t roots_it = xcb_setup_roots_iterator(setup);

    while (roots_it.rem > 0) {
        xcb_screen_t *screen = roots_it.data;

        iter_monitors(ctx, screen);

        xcb_screen_next(&roots_it);
    }

    pthread_mutex_unlock(&ctx->lock);
}

static void *source_update_thread(void *s)
{
    XCBCtx *ctx = s;

    while (!atomic_load(&ctx->quit)) {
        update_entries(ctx);
        av_usleep(1000000);
    }

    return 0;
}

static int xcb_ctrl(AVBufferRef *ctx_ref, SPEventType ctrl, void *arg)
{
    int err = 0;
    XCBCtx *ctx = (XCBCtx *)ctx_ref->data;

    if (ctrl & SP_EVENT_CTRL_NEW_EVENT) {
        AVBufferRef *event = arg;
        char *fstr = sp_event_flags_to_str_buf(event);
        sp_log(ctx, SP_LOG_DEBUG, "Registering new event (%s)!\n", fstr);
        av_free(fstr);

        if (ctrl & SP_EVENT_FLAG_IMMEDIATE) {
            /* Bring up the new event to speed with current affairs */
            SPBufferList *tmp_event = sp_bufferlist_new();
            sp_eventlist_add(ctx, tmp_event, event, 1);

            update_entries(ctx);

            AVBufferRef *obj = NULL;
            while ((obj = sp_bufferlist_iter_ref(ctx->entries))) {
                sp_eventlist_dispatch(obj->data, tmp_event,
                                      SP_EVENT_ON_CHANGE | SP_EVENT_TYPE_SOURCE, obj->data);
                av_buffer_unref(&obj);
            }

            sp_bufferlist_free(&tmp_event);
        }

        /* Add it to the list now to receive events dynamically */
        err = sp_eventlist_add(ctx, ctx->events, event, 1);
        if (err < 0)
            return err;
    }

    return 0;
}

static AVBufferRef *xcb_ref_entry(AVBufferRef *ctx_ref, uint32_t identifier)
{
    XCBCtx *ctx = (XCBCtx *)ctx_ref->data;
    return sp_bufferlist_pop(ctx->entries, sp_bufferlist_iosysentry_by_id, &identifier);
}

static void xcb_uninit(void *opaque, uint8_t *data)
{
    XCBCtx *ctx = (XCBCtx *)data;

    /* Stop updating */
    atomic_store(&ctx->quit, 1);
    pthread_join(ctx->source_update, NULL);

    sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_DESTROY, ctx);
    sp_bufferlist_free(&ctx->entries);

    xcb_disconnect(ctx->con);

    sp_class_free(ctx);
    av_free(ctx);
}

static int xcb_init(AVBufferRef **s)
{
    int err = 0;
    XCBCtx *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return AVERROR(ENOMEM);

    AVBufferRef *ctx_ref = av_buffer_create((uint8_t *)ctx, sizeof(*ctx),
                                            xcb_uninit, NULL, 0);
    if (!ctx_ref) {
        av_free(ctx);
        return AVERROR(ENOMEM);
    }

    ctx->entries = sp_bufferlist_new();
    if (!ctx->entries) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    ctx->events = sp_bufferlist_new();
    if (!ctx->events) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    err = sp_class_alloc(ctx, src_xcb.name, SP_TYPE_CONTEXT, NULL);
    if (err < 0)
        goto fail;

    int screens;
    ctx->con = xcb_connect(NULL, &screens);

    if ((err = xcb_connection_has_error(ctx->con))) {
        sp_log(ctx, SP_LOG_ERROR, "Cannot open display, error %d!\n", err);
        err = AVERROR(EIO);
        goto fail;
    }

    xcb_shm_query_version_cookie_t shm_c;
    xcb_shm_query_version_reply_t *shm_r;
    shm_c = xcb_shm_query_version(ctx->con);
    shm_r = xcb_shm_query_version_reply(ctx->con, shm_c, NULL);
    if (!shm_r) {
        sp_log(ctx, SP_LOG_ERROR, "SHM is not supported by the X server!\n");
        err = AVERROR_EXTERNAL;
        goto fail;
    } else {
        free(shm_r);
    }

    xcb_randr_query_version_cookie_t ver_c;
    xcb_randr_query_version_reply_t *ver_r;
    ver_c = xcb_randr_query_version(ctx->con, XCB_RANDR_MAJOR_VERSION, XCB_RANDR_MINOR_VERSION);
    ver_r = xcb_randr_query_version_reply(ctx->con, ver_c, 0);
	if (!ver_r) {
        sp_log(ctx, SP_LOG_ERROR, "RANDR is not supported by the X server!\n");
        err = AVERROR_EXTERNAL;
        goto fail;
    } else {
        free(ver_r);
    }

    /* Init context lock */
    pthread_mutex_init(&ctx->lock, NULL);

    ctx->quit = ATOMIC_VAR_INIT(0);
    pthread_create(&ctx->source_update, NULL, source_update_thread, ctx);

    *s = ctx_ref;

    return 0;

fail:
    av_buffer_unref(&ctx_ref);

    return err;
}

const IOSysAPI src_xcb = {
    .name      = "xcb",
    .ctrl      = xcb_ctrl,
    .init_sys  = xcb_init,
    .ref_entry = xcb_ref_entry,
    .init_io   = xcb_init_io,
};
