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

#include <libtxproto/log.h>
#include <libtxproto/utils.h>

#include "version.h"
#include "../config.h"

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
