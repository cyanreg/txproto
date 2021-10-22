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

#include <signal.h>
#include <stdatomic.h>

#include "logging.h"
#include "utils.h"

#include "version.h"
#include "../config.h"

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
    int fn_ref;
} EpochEventCtx;

typedef struct TXMainContext {
    SPClass *class;

    struct TXCLIContext *cli;
    struct TXLuaContext *lua;

    int lua_exit_code;

    int source_update_cb_ref;

    atomic_int_fast64_t epoch_value;

    AVBufferRef **io_api_ctx;

    SPBufferList *events;
    SPBufferList *ext_buf_refs;
} TXMainContext;

#include "cli.h"
#include "lua_common.h"
