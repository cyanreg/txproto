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

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include "logging.h"
#include "utils.h"

#include "version.h"
#include "../config.h"

typedef struct TXMainContext {
    SPClass *class;

    lua_State *lua;
    pthread_mutex_t lock;
    int lua_exit_code;
    AVDictionary *lua_namespace;

    int source_update_cb_ref;

    atomic_int_fast64_t epoch_value;

    AVBufferRef **io_api_ctx;

    SPBufferList *commit_list;
    SPBufferList *discard_list;
    SPBufferList *lua_buf_refs;
} TXMainContext;

#define LUA_PRIV_PREFIX "sp"
#define LUA_PUB_PREFIX "tx"
#define LUA_API_VERSION (int []){ 0, 1 } /* major, minor */

/* Unlock the Lua interface */
#define LUA_INTERFACE_END(ret)            \
    do {                                  \
        pthread_mutex_unlock(&ctx->lock); \
        return (ret);                     \
    } while (0)

/* Lock the Lua interface, preventing other threads from changing it */
#define LUA_LOCK_INTERFACE()                                      \
    do {                                                          \
        pthread_mutex_lock(&ctx->lock);                           \
    } while (0)

/* Load file into the Lua context */
int sp_lfn_loadfile(TXMainContext *ctx, const char *script_name);

/* Load a library into the Lua context */
void sp_load_lua_library(TXMainContext *ctx, const char *lib);
