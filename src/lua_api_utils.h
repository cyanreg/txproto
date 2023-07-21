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

#include "lua.h"
#include "lua_api.h"

#define LUA_PUSH_CONTEXTED_INTERFACE(L, fn_table, upvals) \
    do {                                                  \
        luaL_newlibtable(L, fn_table);                    \
        int upvals_nb = SP_ARRAY_ELEMS(upvals);           \
        for (int c = 0; c < upvals_nb; c++)               \
            lua_pushlightuserdata(L, upvals[c]);          \
        luaL_setfuncs(L, fn_table, upvals_nb);            \
    } while (0);

#define LUA_CLEANUP_FN_DEFS(fnprefix, fnname)           \
    av_unused AVBufferRef **cleanup_ref = NULL;         \
    const av_unused char *fn_prefix = fnprefix;         \
    const av_unused char *fn_name = fnname;

#define LUA_SET_CLEANUP(ref) \
    do {                     \
        cleanup_ref = &ref;  \
    } while (0)

#define LUA_ERROR(fmt, ...)                                               \
    do {                                                                  \
        sp_log(ctx, SP_LOG_ERROR, "%s->%s: " fmt "\n",                    \
               fn_prefix, fn_name, __VA_ARGS__);                          \
        lua_pushfstring(L, fmt, __VA_ARGS__);                             \
        if (cleanup_ref && *cleanup_ref)                                  \
            av_buffer_unref(cleanup_ref);                                 \
        return lua_error(L);                                              \
    } while (0)

#define LUA_INTERFACE_BOILERPLATE()                                            \
    do {                                                                       \
        if (lua_gettop(L) != 1)                                                \
            LUA_ERROR("Invalid number of arguments, expected 1, got %i!",      \
                      lua_gettop(L));                                          \
        if (!lua_istable(L, -1))                                               \
            LUA_ERROR("Invalid argument type, expected table, got \"%s\"!",    \
                      lua_typename(L, lua_type(L, -1)));                       \
    } while (0)

#define LUA_CHECK_OPT_VAL(key, expected)                                               \
    int type = lua_getfield(L, -1, key);                                               \
    if (type == LUA_TNIL || type == LUA_TNONE) {                                       \
        lua_pop(L, 1);                                                                 \
        break;                                                                         \
    } else if (type != expected)                                                       \
        LUA_ERROR("Invalid value type for entry \"%s\", expected \"%s\", got \"%s\"!", \
                  key, lua_typename(L, expected), lua_typename(L, type));

#define GET_OPTS_CLASS(dst, key)                                              \
    do {                                                                      \
        LUA_CHECK_OPT_VAL(key, LUA_TTABLE)                                    \
        err = lua_parse_table_to_avopt(ctx, L, dst);                          \
        if (err < 0)                                                          \
            LUA_ERROR("Unable to parse %s: %s!", key, av_err2str(err));       \
        lua_pop(L, 1);                                                        \
    } while (0)

#define GET_OPTS_DICT(dst, key)                                               \
    do {                                                                      \
        LUA_CHECK_OPT_VAL(key, LUA_TTABLE)                                    \
        err = sp_lua_parse_table_to_avdict(L, &dst);                          \
        if (err < 0)                                                          \
            LUA_ERROR("Unable to parse %s: %s!", key, av_err2str(err));       \
        lua_pop(L, 1);                                                        \
    } while (0)

#define GET_OPTS_LIST(dst, key)                                               \
    do {                                                                      \
        LUA_CHECK_OPT_VAL(key, LUA_TTABLE)                                    \
        err = lua_parse_table_to_list(L, &dst);                               \
        if (err < 0)                                                          \
            LUA_ERROR("Unable to parse %s: %s!", key, av_err2str(err));       \
        lua_pop(L, 1);                                                        \
    } while (0)

#define GET_OPT_STR(dst, key)               \
    do {                                    \
        LUA_CHECK_OPT_VAL(key, LUA_TSTRING) \
        dst = lua_tostring(L, -1);          \
        lua_pop(L, 1);                      \
    } while (0)

#define GET_OPT_BOOL(dst, key)               \
    do {                                     \
        LUA_CHECK_OPT_VAL(key, LUA_TBOOLEAN) \
        dst = lua_toboolean(L, -1);          \
        lua_pop(L, 1);                       \
    } while (0)

#define SET_OPT_BOOL(src, key) \
    lua_pushboolean(L, src);   \
    lua_setfield(L, -2, key);

#define SET_OPT_STR(src, key)     \
    do {                          \
        lua_pushstring(L, src);   \
        lua_setfield(L, -2, key); \
    } while (0)

#define GET_OPT_NUM(dst, key)                                                    \
    do {                                                                         \
        LUA_CHECK_OPT_VAL(key, LUA_TNUMBER)                                      \
        dst = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : lua_tonumber(L, -1); \
        lua_pop(L, 1);                                                           \
    } while (0)

#define SET_OPT_NUM(src, key)     \
    do {                          \
        lua_pushnumber(L, src);   \
        lua_setfield(L, -2, key); \
    } while (0)

#define SET_OPT_INT(src, key)     \
    do {                          \
        lua_pushinteger(L, src);  \
        lua_setfield(L, -2, key); \
    } while (0)

#define GET_OPT_LIGHTUSERDATA(dst, key)            \
    do {                                           \
        LUA_CHECK_OPT_VAL(key, LUA_TLIGHTUSERDATA) \
        dst = (void *)lua_touserdata(L, -1);       \
        lua_pop(L, 1);                             \
    } while (0)

#define SET_OPT_LIGHTUSERDATA(src, key) \
    lua_pushlightuserdata(L, src);      \
    lua_setfield(L, -2, key);
