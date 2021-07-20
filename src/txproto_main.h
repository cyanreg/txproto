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
#include <luaconf.h>

#include "logging.h"
#include "utils.h"

#include "version.h"
#include "../config.h"

typedef struct LuaLoadedLibrary {
    char *name;
    char *path;
    void *libp;
    void *lib_open_sym;
} LuaLoadedLibrary;

typedef struct TXMainContext {
    SPClass *class;

    LuaLoadedLibrary *loaded_lib_list;
    int loaded_lib_list_len;

    lua_State *lua;
    pthread_mutex_t lua_lock;
    int lua_exit_code;

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

/* Load file into the Lua context */
int sp_lfn_loadfile(TXMainContext *ctx, const char *script_name);

/* Load a library into the given Lua context */
int sp_load_lua_library(TXMainContext *ctx, lua_State *L, const char *lib);

/* Create a Lua context and fill it with our API */
int sp_create_lua_ctx(TXMainContext *ctx, lua_State **dst, const char *lua_libs_list);
