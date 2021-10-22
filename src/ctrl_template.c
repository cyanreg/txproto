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

#include "logging.h"
#include "ctrl_template.h"

static void ctrl_template_ctx_free(void *callback_ctx, void *ctx, void *dep_ctx)
{
    SPCtrlTemplateCbCtx *event = callback_ctx;
    av_dict_free(&event->opts);
    av_dict_free(&event->cmd);
}

int sp_ctrl_template(void *ctx, SPBufferList *events, SPEventType extra_ctrl,
                     event_fn callback, SPEventType ctrl, void *arg)
{
    if (ctrl & SP_EVENT_CTRL_COMMIT) {
        sp_log(ctx, SP_LOG_DEBUG, "Comitting!\n");
        return sp_eventlist_dispatch(ctx, events, SP_EVENT_ON_COMMIT, NULL);
    } else if (ctrl & SP_EVENT_CTRL_DISCARD) {
        sp_log(ctx, SP_LOG_DEBUG, "Discarding!\n");
        sp_eventlist_discard(events);
        return 0;
    } else if (ctrl & SP_EVENT_CTRL_NEW_EVENT) {
        char *fstr = sp_event_flags_to_str_buf(arg);
        sp_log(ctx, SP_LOG_DEBUG, "Registering new event (%s)!\n", fstr);
        av_free(fstr);
        return sp_eventlist_add(ctx, events, arg, 1);
    } else if (ctrl & SP_EVENT_CTRL_SIGNAL) {
        char *fstr = sp_event_flags_to_str(ctrl & ~SP_EVENT_CTRL_MASK);
        sp_log(ctx, SP_LOG_DEBUG, "Registering new dependency signal (%s)!\n", fstr);
        av_free(fstr);
        return sp_eventlist_add_signal(ctx, events, arg, ctrl, 1);
    } else if (ctrl & SP_EVENT_CTRL_OPTS) {
        /* TODO: If arg == NULL, send back the options somehow... */
        if (!arg)
            return AVERROR(EINVAL);

        /* TODO: change options to use generic data instead of dictionaries */
    } else if (ctrl & ~(SP_EVENT_CTRL_START  | /* Always supported */
                        SP_EVENT_CTRL_STOP   |
                        SP_EVENT_CTRL_FLUSH  |
                        extra_ctrl)) {
        return AVERROR(ENOTSUP);
    }

    SPEventType type = ctrl | SP_EVENT_FLAG_ONESHOT | SP_EVENT_ON_COMMIT;

    if (ctrl & SP_EVENT_CTRL_COMMAND)
        type |= SP_EVENT_ON_INIT;

    AVBufferRef *ctrl_event = sp_event_create(callback,
                                              ctrl_template_ctx_free,
                                              sizeof(SPCtrlTemplateCbCtx),
                                              NULL,
                                              type,
                                              ctx,
                                              NULL);

    SPCtrlTemplateCbCtx *ctrl_ctx = av_buffer_get_opaque(ctrl_event);

    ctrl_ctx->ctrl = ctrl;
    if (ctrl & SP_EVENT_CTRL_OPTS) {
        av_dict_copy(&ctrl_ctx->opts, arg, 0);
    } else if (ctrl & SP_EVENT_CTRL_COMMAND) {
        av_dict_copy(&ctrl_ctx->cmd, arg, 0);
    } else if (ctrl & SP_EVENT_CTRL_START) {
        ctrl_ctx->epoch = arg;
    }

    if (ctrl & SP_EVENT_FLAG_IMMEDIATE) {
        int ret = callback(ctrl_event, ctrl_ctx, ctx, NULL, arg);
        av_buffer_unref(&ctrl_event);
        return ret;
    }

    char *fstr = sp_event_flags_to_str_buf(ctrl_event);
    sp_log(ctx, SP_LOG_DEBUG, "Registering new event (%s)!\n", fstr);
    av_free(fstr);

    int err = sp_eventlist_add(ctx, events, ctrl_event, 0);
    if (err < 0)
        av_buffer_unref(&ctrl_event);

    return err;
}
