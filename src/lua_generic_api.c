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

#include "../config.h"

#ifdef HAVE_INTERFACE
#include "interface_common.h"
#endif

#include "iosys_common.h"

#include <libtxproto/commit.h>
#include <libtxproto/control.h>
#include <libtxproto/encode.h>
#include <libtxproto/decode.h>
#include <libtxproto/link.h>
#include <libtxproto/mux.h>
#include <libtxproto/filter.h>

#include "lua_generic_api.h"
#include "lua_api_utils.h"

int sp_lua_generic_link(lua_State *L)
{
    int err = 0;
    TXMainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));
    AVBufferRef *obj1 = lua_touserdata(L, lua_upvalueindex(2));

    LUA_CLEANUP_FN_DEFS(sp_class_get_name(obj1->data), "link")

    int nargs = lua_gettop(L);
    if (nargs != 1 && nargs != 2)
        LUA_ERROR("Invalid number of arguments, expected 1 or 2 got %i!", lua_gettop(L));

    int autostart = 1;
    const char *src_pad_name = NULL;
    const char *dst_pad_name = NULL;
    int src_stream_id = -1;
    const char *src_stream_desc = NULL;
    if (nargs == 2) {
        if (lua_istable(L, -1)) {
            GET_OPT_STR(src_pad_name, "src_pad");
            GET_OPT_STR(dst_pad_name, "dst_pad");
            GET_OPT_BOOL(autostart, "autostart");
        } else if (lua_isinteger(L, -1)) {
            src_stream_id = lua_tointeger(L, -1);
        } else if (lua_isstring(L, -1)) {
            src_pad_name = dst_pad_name = src_stream_desc = lua_tostring(L, -1);
        } else {
            LUA_ERROR("Invalid argument, expected \"table\" (options) or \"string\" (stream/pad name), got \"%s\"!",
                      lua_typename(L, lua_type(L, -1)));
        }
        lua_pop(L, 1);
    }

    if (!lua_istable(L, -1))
        LUA_ERROR("Invalid argument, expected \"table\" (source context), got \"%s\"!",
                  lua_typename(L, lua_type(L, -1)));

    lua_pushnil(L);
    lua_next(L, -2);
    if (!lua_isfunction(L, -1))
        LUA_ERROR("Invalid argument, expected \"table\"[0].\"function\", got \"%s\"!",
                  lua_typename(L, lua_type(L, -1)));

    lua_getupvalue(L, 3, 2);
    if (!lua_isuserdata(L, -1))
        LUA_ERROR("Invalid argument, expected \"table\"[0].\"function\"[upvalue].\"userdata\", got \"%s\"!",
                  lua_typename(L, lua_type(L, -1)));

    AVBufferRef *obj2 = lua_touserdata(L, -1);

    err = sp_generic_link(ctx, obj1, obj2, autostart, src_pad_name, dst_pad_name,
                          src_stream_id, src_stream_desc);
    if (err < 0) {
        if (cleanup_ref && *cleanup_ref)
            av_buffer_unref(cleanup_ref);
        return lua_error(L);
    }

    return 0;
}

int sp_lua_generic_ctrl(lua_State *L)
{
    int err = 0;
    TXMainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));
    AVBufferRef *obj_ref = lua_touserdata(L, lua_upvalueindex(2));

    LUA_CLEANUP_FN_DEFS(sp_class_get_name(obj_ref->data), "ctrl")

    int num_args = lua_gettop(L);
    if (num_args != 1 && num_args != 2)
        LUA_ERROR("Invalid number of arguments, expected 1 or 2, got %i!", num_args);
    if (num_args == 2 && !lua_istable(L, -1))
        LUA_ERROR("Invalid argument, expected \"table\" (options), got \"%s\"!",
                  lua_typename(L, lua_type(L, -1)));
    if (!lua_isstring(L, -num_args) && !lua_istable(L, -num_args))
        LUA_ERROR("Invalid argument, expected \"string\" or \"table\" (flags), got \"%s\"!",
                  lua_typename(L, lua_type(L, -num_args)));

    AVDictionary *opts = NULL;
    if (num_args == 2) {
        err = sp_lua_parse_table_to_avdict(L, &opts);
        if (err < 0)
            LUA_ERROR("Unable to parse given options table: %s!", av_err2str(err));
        lua_pop(L, 1);
    }

    uint64_t flags;
    if (lua_isstring(L, -1))
        err = sp_event_string_to_flags(ctx, &flags, lua_tostring(L, -1));
    else
        err = sp_lua_table_to_event_flags(ctx, L, &flags);

    if (err < 0)
        LUA_ERROR("Unable to parse given flags: %s", av_err2str(err));

    err = sp_generic_ctrl(ctx, obj_ref, flags, opts);
    if (err < 0) {
        if (cleanup_ref && *cleanup_ref)
            av_buffer_unref(cleanup_ref);
        return lua_error(L);
    }

    av_dict_free(&opts);

    return 0;
}
