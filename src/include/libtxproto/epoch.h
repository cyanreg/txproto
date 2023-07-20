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

#include <libavutil/buffer.h>

#include <libtxproto/txproto_main.h>

enum EpochMode {
    EP_MODE_OFFSET,   /* Timestamps will start at an offset (value = system time + offset) */
    EP_MODE_SYSTEM,   /* Timestamps will start at the system arbitrary time (value = 0) */
    EP_MODE_SOURCE,   /* Timestamps will be set to the first output's PTS */
    EP_MODE_EXTERNAL, /* A Lua function will give us the epoch */
};

typedef struct EpochEventCtx {
    enum EpochMode mode;
    int64_t value;
    AVBufferRef *src_ref;

    int (*external_cb)(int64_t systemtime, AVBufferRef *arg);
    AVBufferRef *external_arg;
} EpochEventCtx;

AVBufferRef *sp_epoch_event_new(TXMainContext *ctx);

int sp_epoch_event_set_source(AVBufferRef *epoch_event,
                              AVBufferRef *obj);

int sp_epoch_event_set_offset(AVBufferRef *epoch_event,
                              int64_t value);

int sp_epoch_event_set_system(AVBufferRef *epoch_event);

int sp_epoch_event_set_external(AVBufferRef *epoch_event,
                                int (*external_cb)(int64_t systemtime, AVBufferRef *arg),
                                AVBufferRef *external_arg);
