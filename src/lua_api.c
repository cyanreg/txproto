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

#include <libavutil/bprint.h>
#include <libavutil/time.h>
#include <libavutil/pixdesc.h>

#include "lua_api.h"

#include "cli.h"
#include "iosys_common.h"
#include "encoding.h"
#include "muxing.h"
#include "filtering.h"
#include "interface_common.h"

#include "lua_event_templates.h"

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
                  key, lua_typename(L, expected), lua_typename(L, type));              \

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
        err = lua_parse_table_to_avdict(L, &dst);                             \
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

static int lua_parse_table_to_avopt(TXMainContext *ctx, lua_State *L, void *dst)
{
    lua_pushnil(L);

    while (lua_next(L, -2)) {
        int err;
        switch (lua_type(L, -1)) {
        case LUA_TBOOLEAN:
            err = av_opt_set_int(dst,
                                 lua_tostring(L, -2),
                                 lua_toboolean(L, -1),
                                 AV_OPT_SEARCH_CHILDREN);
            break;
        case LUA_TNUMBER:
            if (lua_isinteger(L, -1))
                err = av_opt_set_int(dst,
                                     lua_tostring(L, -2),
                                     lua_tointeger(L, -1),
                                     AV_OPT_SEARCH_CHILDREN);
            else
                err = av_opt_set_double(dst,
                                        lua_tostring(L, -2),
                                        lua_tonumber(L, -1),
                                        AV_OPT_SEARCH_CHILDREN);
            break;
        case LUA_TSTRING:
        default:
            err = av_opt_set(dst,
                             lua_tostring(L, -2),
                             lua_tostring(L, -1),
                             AV_OPT_SEARCH_CHILDREN);
            break;
        }

        if (err < 0) {
            sp_log(ctx, SP_LOG_ERROR, "Error setting option \"%s\": %s!\n",
                   lua_tostring(L, -2), av_err2str(err));
            return err;
        }

        lua_pop(L, 1);
    }

    return 0;
}

static int lua_parse_table_to_list(lua_State *L, char ***dst)
{
    lua_pushnil(L);

    char **str = NULL;
    int num = 0;

    while (lua_next(L, -2)) {
        if (!lua_isstring(L, -1))
            return AVERROR(EINVAL);
        str = av_realloc(str, sizeof(*str) * (num + 1));
        str[num++] = av_strdup(lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    str = av_realloc(str, sizeof(*str) * (num + 1));
    str[num++] = NULL;
    *dst = str;

    return 0;
}

#define FREE_STR_LIST(list)     \
    do {                        \
        char **tlist = list;    \
        while (list && *list) { \
            av_free(*list);     \
            list++;             \
        }                       \
        av_free(tlist);         \
    } while (0)

static int lua_parse_table_to_avdict(lua_State *L, AVDictionary **dict)
{
    lua_pushnil(L);

    while (lua_next(L, -2)) {
        int err;
        const char *key = lua_tostring(L, -2);
        const char *val;
        if (lua_isboolean(L, -1))
            val = lua_toboolean(L, -1) ? "1" : "0";
        else
            val = lua_tostring(L, -1);

        err = av_dict_set(dict, key, val, 0);
        if (err < 0)
            return err;

        lua_pop(L, 1);
    }

    return 0;
}

static int lua_generic_destroy(lua_State *L)
{
    TXMainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));
    AVBufferRef *ref = lua_touserdata(L, lua_upvalueindex(2));
    (void)sp_bufferlist_pop(ctx->ext_buf_refs, sp_bufferlist_find_fn_data, ref);
    av_buffer_unref(&ref);
    return 0;
}

static int lua_event_destroy(lua_State *L)
{
    TXMainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));
    AVBufferRef *ref = lua_touserdata(L, lua_upvalueindex(2));
    (void)sp_bufferlist_pop(ctx->ext_buf_refs, sp_bufferlist_find_fn_data, ref);
    sp_event_unref_expire(&ref);
    return 0;
}

static int lua_event_await(lua_State *L)
{
    TXMainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));
    AVBufferRef *ref = lua_touserdata(L, lua_upvalueindex(2));

    (void)sp_bufferlist_pop(ctx->ext_buf_refs, sp_bufferlist_find_fn_data, ref);
    lua_pushlightuserdata(L, ref);

    return lua_yieldk(L, 1, 0, NULL);
}

static int string_to_event_flags(TXMainContext *ctx, uint64_t *dst,
                                 const char *in_str)
{
    uint64_t flags = 0x0;
    int on_prefix   = 0;
    int type_prefix = 0;
    int ctrl_prefix = 0;
    int flag_prefix = 0;

    /* We have to modify it */
    char *str = av_strdup(in_str);

    *dst = 0x0;

#define FLAG(flag, prf, name)                                    \
    if (!strcmp(tok, #prf ":" name) || !strcmp(tok, name)) {     \
        flags |= flag;                                           \
        on_prefix = type_prefix = ctrl_prefix = flag_prefix = 0; \
        prf## _prefix = 1;                                       \
    } else if (!strcmp(tok, ":" name)) {                         \
        if (prf## _prefix) {                                     \
            flags |= flag;                                       \
        } else {                                                 \
            sp_log(ctx, SP_LOG_ERROR, "Error parsing flags, no " \
                   "%s prefix specified for %s!\n", #prf, name); \
            av_free(str);                                        \
            return AVERROR(EINVAL);                              \
        }                                                        \
    } else

    char *save, *tok = av_strtok(str, " ,+", &save);
    while (tok) {
        FLAG(SP_EVENT_ON_COMMIT,       on,   "commit")
        FLAG(SP_EVENT_ON_CONFIG,       on,   "config")
        FLAG(SP_EVENT_ON_INIT,         on,   "init")
        FLAG(SP_EVENT_ON_CHANGE,       on,   "change")
        FLAG(SP_EVENT_ON_STATS,        on,   "stats")
        FLAG(SP_EVENT_ON_EOS,          on,   "eos")
        FLAG(SP_EVENT_ON_ERROR,        on,   "error")
        FLAG(SP_EVENT_ON_DESTROY,      on,   "destroy")
        FLAG(SP_EVENT_ON_OUTPUT,       on,   "output")
        FLAG(SP_EVENT_ON_MASK,         on,   "all")

        FLAG(SP_EVENT_TYPE_SOURCE,     type, "source")
        FLAG(SP_EVENT_TYPE_SINK,       type, "sink")
        FLAG(SP_EVENT_TYPE_LINK,       type, "link")
        FLAG(SP_EVENT_TYPE_MASK,       type, "all")

        FLAG(SP_EVENT_CTRL_START,      ctrl, "start")
        FLAG(SP_EVENT_CTRL_STOP,       ctrl, "stop")
        FLAG(SP_EVENT_CTRL_OPTS,       ctrl, "opts")
        FLAG(SP_EVENT_CTRL_COMMIT,     ctrl, "commit")
        FLAG(SP_EVENT_CTRL_DISCARD,    ctrl, "discard")
        FLAG(SP_EVENT_CTRL_FLUSH,      ctrl, "flush")

        FLAG(SP_EVENT_FLAG_NO_REORDER, flag, "no_reorder")
        FLAG(SP_EVENT_FLAG_NO_DEDUP,   flag, "no_dedup")
        FLAG(SP_EVENT_FLAG_HIGH_PRIO,  flag, "high_prio")
        FLAG(SP_EVENT_FLAG_LOW_PRIO,   flag, "low_prio")
        FLAG(SP_EVENT_FLAG_IMMEDIATE,  flag, "immediate")
        FLAG(SP_EVENT_FLAG_ONESHOT,    flag, "oneshot")

        /* else */ if (!strcmp(tok, "on:")) {
            type_prefix = ctrl_prefix = flag_prefix = 0;
            on_prefix = 1;
        } else if (!strcmp(tok, "type:")) {
            on_prefix = ctrl_prefix = flag_prefix = 0;
            type_prefix = 1;
        } else if (!strcmp(tok, "ctrl:")) {
            on_prefix = type_prefix = flag_prefix = 0;
            ctrl_prefix = 1;
        } else if (!strcmp(tok, "type:")) {
            on_prefix = type_prefix = ctrl_prefix = 0;
            flag_prefix = 1;
        } else {
            sp_log(ctx, SP_LOG_ERROR, "Error parsing flags, \"%s\" not found!\n", tok);
            av_free(str);
            return AVERROR(EINVAL);
        }

        tok = av_strtok(NULL, " ,+", &save);
    }

    av_free(str);
    *dst = flags;

#undef FLAG
    return 0;
}

static int table_to_event_flags(lua_State *L, TXMainContext *ctx, enum SPEventType *dst)
{
    lua_pushnil(L);

    *dst = 0x0;
    enum SPEventType flags = 0x0;
    while (lua_next(L, -2)) {
        if (!lua_isstring(L, -1))
            return AVERROR(EINVAL);
        uint64_t tmp_flags;
        int err = string_to_event_flags(ctx, &tmp_flags, lua_tostring(L, -1));
        if (err < 0)
            return err;
        flags |= tmp_flags;
        lua_pop(L, 1);
    }

    *dst = flags;

    return 0;
}

typedef struct HookLuaEventCtx {
    enum SPEventType flags;
    int fn_ref;
    AVBufferRef *ctx_ref;
    TXLuaContext *lctx;
} HookLuaEventCtx;

static int hook_lua_event_cb(AVBufferRef *opaque, void *src_ctx, void *data)
{
    int err = 0;
    HookLuaEventCtx *event_ctx = (HookLuaEventCtx *)opaque->data;

    lua_State *L = sp_lua_lock_interface(event_ctx->lctx);

    lua_rawgeti(L, LUA_REGISTRYINDEX, event_ctx->fn_ref);

    int num_args = 0;
    if (event_ctx->flags & SP_EVENT_ON_OUTPUT) {
        SPRationalValue *val = data;
        lua_pushnumber(L, val->value * av_q2d(val->base));
        num_args = 1;
    } else if (event_ctx->flags & (SP_EVENT_ON_STATS | SP_EVENT_ON_DESTROY)) {
        SPGenericData *entry = data;
        if (!data) {
            lua_pushnil(L);
            num_args = 1;
            goto call;
        }
        lua_newtable(L);
        while (entry && (entry->type != SP_DATA_TYPE_NONE)) {

            int pop = 0;
            if (entry->sub) {
                int type = lua_getfield(L, -1, entry->sub);
                if (type == LUA_TNIL) {
                    lua_pop(L, 1);
                    lua_newtable(L);
                } else if (type == LUA_TTABLE) {
                    pop = 1;
                } else {
                    lua_pop(L, 1);
                    continue; /* Do not overwrite */
                }
            }

            if (entry->type == SP_DATA_TYPE_FLOAT || entry->type == SP_DATA_TYPE_DOUBLE) {
                SET_OPT_NUM(LOAD_GEN_DATA_NUM(entry), entry->name);
            } else if (entry->type & SP_DATA_TYPE_NUM) {
                SET_OPT_INT(LOAD_GEN_DATA_NUM(entry), entry->name);
            } else if (entry->type == SP_DATA_TYPE_STRING) {
                SET_OPT_STR(entry->ptr, entry->name);
            } else if (entry->type == SP_DATA_TYPE_RECTANGLE) {
                SPRect *rect = entry->ptr;
                lua_newtable(L);
                SET_OPT_INT(rect->x, "x");
                SET_OPT_INT(rect->y, "y");
                SET_OPT_INT(rect->w, "w");
                SET_OPT_INT(rect->h, "h");
                SET_OPT_NUM(rect->scale, "scale");
                lua_setfield(L, -2, entry->name);
            } else {
                sp_assert(0);
            }

            if (pop)
                lua_pop(L, 1);
            else if (entry->sub)
                lua_setfield(L, -2, entry->sub);

            entry++;
        }
        num_args = 1;
    } else if (event_ctx->flags & SP_EVENT_ON_ERROR) {
        err = data ? *((int *)data) : 0;
        lua_pushstring(L, av_err2str(err));
        num_args = 1;
    }

call:
    err = sp_lua_run_generic_yieldable(event_ctx->lctx, num_args, 1, 1);

    return sp_lua_unlock_interface(event_ctx->lctx, err);
}

static void hook_lua_event_free(void *opaque, uint8_t *data)
{
    HookLuaEventCtx *hook_lua_ctx = (HookLuaEventCtx *)data;

    lua_State *L = sp_lua_lock_interface(hook_lua_ctx->lctx);
    luaL_unref(L, LUA_REGISTRYINDEX, hook_lua_ctx->fn_ref);
    sp_lua_unlock_interface(hook_lua_ctx->lctx, 0);
    sp_lua_close_ctx(&hook_lua_ctx->lctx);

    av_buffer_unref(&hook_lua_ctx->ctx_ref);

    av_free(data);
}

static int lua_generic_hook(lua_State *L)
{
    int err = 0;
    TXMainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));
    AVBufferRef *obj_ref = lua_touserdata(L, lua_upvalueindex(2));

    LUA_CLEANUP_FN_DEFS(sp_class_get_name(obj_ref->data), "hook")

    if (lua_gettop(L) != 2)
        LUA_ERROR("Invalid number of arguments, expected 2, got %i!", lua_gettop(L));
    if (!lua_isfunction(L, -1))
        LUA_ERROR("Invalid argument, expected \"function\" (callback), got \"%s\"!", lua_typename(L, lua_type(L, -1)));
    if (!lua_isstring(L, -2) && !lua_istable(L, -2))
        LUA_ERROR("Invalid argument, expected \"string\" or \"table\" (flags), got \"%s\"!", lua_typename(L, lua_type(L, -2)));

    /* ref pops the item off the stack! */
    int fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (fn_ref == LUA_NOREF || fn_ref == LUA_REFNIL)
        LUA_ERROR("Invalid function specified, got: %s!", "nil");

    uint64_t flags;
    if (lua_isstring(L, -1)) {
        err = string_to_event_flags(ctx, &flags, lua_tostring(L, -1));
    } else {
        err = table_to_event_flags(L, ctx, &flags);
    }

    if (err < 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, fn_ref);
        LUA_ERROR("Unable to parse given flags: %s", av_err2str(err));
    }

    ctrl_fn fn = NULL;
    enum SPType type = sp_class_get_type(obj_ref->data);
    switch (type) {
    case SP_TYPE_ENCODER:
        fn = sp_encoder_ctrl;
        break;
    case SP_TYPE_MUXER:
        fn = sp_muxer_ctrl;
        break;
    case SP_TYPE_FILTER:
        fn = sp_filter_ctrl;
        break;
    case SP_TYPE_AUDIO_SOURCE:
    case SP_TYPE_AUDIO_SINK:
    case SP_TYPE_AUDIO_BIDIR:
    case SP_TYPE_VIDEO_SOURCE:
    case SP_TYPE_VIDEO_SINK:
    case SP_TYPE_VIDEO_BIDIR:
    case SP_TYPE_SUB_SOURCE:
    case SP_TYPE_SUB_SINK:
    case SP_TYPE_SUB_BIDIR:
        fn = ((IOSysEntry *)obj_ref->data)->ctrl;
        break;
    default:
        luaL_unref(L, LUA_REGISTRYINDEX, fn_ref);
        LUA_ERROR("Unsupported CTRL type: %s!", sp_class_type_string(obj_ref->data));
    }

    if (flags & SP_EVENT_CTRL_MASK) {
        luaL_unref(L, LUA_REGISTRYINDEX, fn_ref);
        LUA_ERROR("ctrl: specified on a hook function: %s!", av_err2str(AVERROR(EINVAL)));
    } else if (!(flags & SP_EVENT_ON_MASK)) {
        luaL_unref(L, LUA_REGISTRYINDEX, fn_ref);
        LUA_ERROR("No event specified to hook function on: %s!", av_err2str(AVERROR(EINVAL)));
    }

    SP_EVENT_BUFFER_CTX_ALLOC(HookLuaEventCtx, hook_lua_ctx, hook_lua_event_free, ctx);

    /* Its a user event, we do not want to deduplicate */
    flags |= SP_EVENT_FLAG_NO_DEDUP;

    hook_lua_ctx->lctx = sp_lua_create_thread(ctx->lua);
    hook_lua_ctx->flags = flags;
    hook_lua_ctx->fn_ref = fn_ref;

    AVBufferRef *hook_event = sp_event_create(hook_lua_event_cb, NULL, flags,
                                              hook_lua_ctx_ref, 0x0);
    err = fn(obj_ref, flags | SP_EVENT_CTRL_NEW_EVENT, hook_event);
    if (err < 0)
         LUA_ERROR("Unable to add event: %s", av_err2str(err));

    sp_bufferlist_append_noref(ctx->ext_buf_refs, hook_event);

    void *contexts[] = { ctx, hook_event };
    static const struct luaL_Reg lua_fns[] = {
        { "destroy", lua_event_destroy },
        { "await", lua_event_await },
        { NULL, NULL },
    };

    LUA_PUSH_CONTEXTED_INTERFACE(L, lua_fns, contexts);

    if (!(flags & SP_EVENT_FLAG_IMMEDIATE))
        add_commit_fn_to_list(ctx, fn, obj_ref);

    return 1;
}

static int lua_generic_ctrl(lua_State *L)
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
        err = lua_parse_table_to_avdict(L, &opts);
        if (err < 0)
            LUA_ERROR("Unable to parse given options table: %s!", av_err2str(err));
        lua_pop(L, 1);
    }

    uint64_t flags;
    if (lua_isstring(L, -1)) {
        err = string_to_event_flags(ctx, &flags, lua_tostring(L, -1));
    } else {
        err = table_to_event_flags(L, ctx, &flags);
    }

    if (err < 0)
        LUA_ERROR("Unable to parse given flags: %s", av_err2str(err));

    GENERIC_CTRL(obj_ref, flags, opts);

    av_dict_free(&opts);

    return 0;
}

typedef struct SPLinkSourceEncoderCtx {
    AVBufferRef *src_ref;
    AVBufferRef *enc_ref;
} SPLinkSourceEncoderCtx;

static int api_link_src_enc_cb(AVBufferRef *opaque, void *src_ctx, void *data)
{
    SPLinkSourceEncoderCtx *ctx = (SPLinkSourceEncoderCtx *)opaque->data;
    IOSysEntry *sctx = (IOSysEntry *)ctx->src_ref->data;
    EncodingContext *ectx = (EncodingContext *)ctx->enc_ref->data;

    sp_log(src_ctx, SP_LOG_VERBOSE, "Linking %s \"%s\" and %s \"%s\"\n",
           sp_class_type_string(sctx), sp_class_get_name(sctx),
           sp_class_type_string(ectx), sp_class_get_name(ectx));

    return sp_frame_fifo_mirror(ectx->src_frames, sctx->frames);
}

static void api_link_src_enc_free(void *opaque, uint8_t *data)
{
    SPLinkSourceEncoderCtx *ctx = (SPLinkSourceEncoderCtx *)data;
    av_buffer_unref(&ctx->src_ref);
    av_buffer_unref(&ctx->enc_ref);
    av_free(data);
}

typedef struct SPLinkSourceFilterCtx {
    AVBufferRef *src_ref;
    AVBufferRef *filt_ref;
    const char *filt_pad;
} SPLinkSourceFilterCtx;

static int api_link_src_filt_cb(AVBufferRef *opaque, void *src_ctx, void *data)
{
    SPLinkSourceFilterCtx *ctx = (SPLinkSourceFilterCtx *)opaque->data;
    IOSysEntry *sctx = (IOSysEntry *)ctx->src_ref->data;

    sp_log(src_ctx, SP_LOG_VERBOSE, "Linking %s \"%s\" and %s \"%s\"\n",
           sp_class_type_string(sctx), sp_class_get_name(sctx),
           sp_class_type_string(ctx->filt_ref->data), sp_class_get_name(ctx->filt_ref->data));

    return sp_map_fifo_to_pad(ctx->filt_ref, sctx->frames, ctx->filt_pad, 0);
}

static void api_link_src_filt_free(void *opaque, uint8_t *data)
{
    SPLinkSourceFilterCtx *ctx = (SPLinkSourceFilterCtx *)data;
    av_buffer_unref(&ctx->src_ref);
    av_buffer_unref(&ctx->filt_ref);
    av_free(data);
}

typedef struct SPLinkFilterFilterCtx {
    AVBufferRef *src_ref;
    AVBufferRef *dst_ref;
    const char *src_filt_pad;
    const char *dst_filt_pad;
} SPLinkFilterFilterCtx;

static int api_link_filt_filt_cb(AVBufferRef *opaque, void *src_ctx, void *data)
{
    SPLinkFilterFilterCtx *ctx = (SPLinkFilterFilterCtx *)opaque->data;

    sp_log(src_ctx, SP_LOG_VERBOSE, "Linking %s \"%s\" and %s \"%s\"\n",
           sp_class_type_string(ctx->src_ref->data), sp_class_get_name(ctx->src_ref->data),
           sp_class_type_string(ctx->dst_ref->data), sp_class_get_name(ctx->dst_ref->data));

    return sp_map_pad_to_pad(ctx->dst_ref, ctx->dst_filt_pad,
                             ctx->src_ref, ctx->src_filt_pad);
}

static void api_link_filt_filt_free(void *opaque, uint8_t *data)
{
    SPLinkFilterFilterCtx *ctx = (SPLinkFilterFilterCtx *)data;
    av_buffer_unref(&ctx->src_ref);
    av_buffer_unref(&ctx->dst_ref);
    av_free(data);
}

typedef struct SPLinkFilterEncoderCtx {
    AVBufferRef *filt_ref;
    AVBufferRef *enc_ref;
    const char *filt_pad;
} SPLinkFilterEncoderCtx;

static int api_link_filt_enc_cb(AVBufferRef *opaque, void *src_ctx, void *data)
{
    SPLinkFilterEncoderCtx *ctx = (SPLinkFilterEncoderCtx *)opaque->data;
    EncodingContext *ectx = (EncodingContext *)ctx->enc_ref->data;

    sp_log(src_ctx, SP_LOG_VERBOSE, "Linking %s \"%s\" and %s \"%s\"%s%s\n",
           sp_class_type_string(ectx), sp_class_get_name(ectx),
           sp_class_type_string(ctx->filt_ref->data), sp_class_get_name(ctx->filt_ref->data),
           ctx->filt_pad ? ", pad: " : "",
           ctx->filt_pad ? ctx->filt_pad : "");

    return sp_map_fifo_to_pad(ctx->filt_ref, ectx->src_frames, ctx->filt_pad, 1);
}

static void api_link_filt_enc_free(void *opaque, uint8_t *data)
{
    SPLinkFilterEncoderCtx *ctx = (SPLinkFilterEncoderCtx *)data;
    av_buffer_unref(&ctx->filt_ref);
    av_buffer_unref(&ctx->enc_ref);
    av_free(data);
}

typedef struct SPLinkEncoderMuxerCtx {
    AVBufferRef *enc_ref;
    AVBufferRef *mux_ref;
} SPLinkEncoderMuxerCtx;

static int api_link_enc_mux_cb(AVBufferRef *opaque, void *src_ctx, void *data)
{
    SPLinkEncoderMuxerCtx *ctx = (SPLinkEncoderMuxerCtx *)opaque->data;
    EncodingContext *ectx = (EncodingContext *)ctx->enc_ref->data;
    MuxingContext *mctx = (MuxingContext *)ctx->mux_ref->data;

    sp_log(src_ctx, SP_LOG_VERBOSE, "Linking %s \"%s\" and %s \"%s\"\n",
           sp_class_type_string(mctx), sp_class_get_name(mctx),
           sp_class_type_string(ectx), sp_class_get_name(ectx));

    int err = sp_muxer_add_stream(ctx->mux_ref, ctx->enc_ref);
    if (err < 0)
        return err;

    return sp_packet_fifo_mirror(mctx->src_packets, ectx->dst_packets);
}

static void api_link_enc_mux_free(void *opaque, uint8_t *data)
{
    SPLinkEncoderMuxerCtx *ctx = (SPLinkEncoderMuxerCtx *)data;
    av_buffer_unref(&ctx->enc_ref);
    av_buffer_unref(&ctx->mux_ref);
    av_free(data);
}

static int lua_generic_link(lua_State *L)
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
    if (nargs == 2) {
        if (lua_istable(L, -1)) {
            GET_OPT_STR(src_pad_name, "src_pad");
            GET_OPT_STR(dst_pad_name, "dst_pad");
            GET_OPT_BOOL(autostart, "autostart");
        } else if (lua_isstring(L, -1)) {
            src_pad_name = dst_pad_name = lua_tostring(L, -1);
        } else {
            LUA_ERROR("Invalid argument, expected \"table\" (options) or \"string\" (pad name), got \"%s\"!",
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

#define EITHER(o1, o2, t1, t2)                                                 \
    ((sp_class_get_type(o1->data) & t1 || sp_class_get_type(o1->data) & t2) && \
     (sp_class_get_type(o2->data) & t1 || sp_class_get_type(o2->data) & t2))   \

#define PICK_REF(o1, o2, type)                                   \
    av_buffer_ref(sp_class_get_type(o1->data) == type ? o1 : o2)

#define PICK_REF_INV(o1, o2, type)                               \
    av_buffer_ref(sp_class_get_type(o1->data) != type ? o1 : o2)

    if (EITHER(obj1, obj2, SP_TYPE_ENCODER, SP_TYPE_MUXER)) {
        SP_EVENT_BUFFER_CTX_ALLOC(SPLinkEncoderMuxerCtx, link_enc_mux, api_link_enc_mux_free, NULL)
        link_enc_mux->enc_ref = PICK_REF(obj1, obj2, SP_TYPE_ENCODER);
        link_enc_mux->mux_ref = PICK_REF(obj1, obj2, SP_TYPE_MUXER);

        GENERIC_LINK(EncodingContext, link_enc_mux->enc_ref, sp_encoder_ctrl,
                     MuxingContext, link_enc_mux->mux_ref, sp_muxer_ctrl,
                     link_enc_mux);
    } else if (EITHER(obj1, obj2, SP_TYPE_ENCODER, SP_TYPE_VIDEO_SOURCE) ||
               EITHER(obj1, obj2, SP_TYPE_ENCODER, SP_TYPE_AUDIO_SOURCE)) {
        SP_EVENT_BUFFER_CTX_ALLOC(SPLinkSourceEncoderCtx, link_src_enc, api_link_src_enc_free, NULL)
        link_src_enc->src_ref = PICK_REF_INV(obj1, obj2, SP_TYPE_ENCODER);
        link_src_enc->enc_ref = PICK_REF(obj1, obj2, SP_TYPE_ENCODER);

        ctrl_fn source_ctrl = ((IOSysEntry *)link_src_enc->src_ref->data)->ctrl;

        GENERIC_LINK(IOSysEntry, link_src_enc->src_ref, source_ctrl,
                     EncodingContext, link_src_enc->enc_ref, sp_encoder_ctrl,
                     link_src_enc);
    } else if (EITHER(obj1, obj2, SP_TYPE_ENCODER, SP_TYPE_FILTER)) {
        SP_EVENT_BUFFER_CTX_ALLOC(SPLinkFilterEncoderCtx, link_filt_enc, api_link_filt_enc_free, NULL)
        link_filt_enc->filt_ref = PICK_REF(obj1, obj2, SP_TYPE_FILTER);
        link_filt_enc->enc_ref  = PICK_REF(obj1, obj2, SP_TYPE_ENCODER);
        link_filt_enc->filt_pad = av_strdup(src_pad_name);

        GENERIC_LINK(FilterContext, link_filt_enc->filt_ref, sp_filter_ctrl,
                     EncodingContext, link_filt_enc->enc_ref, sp_encoder_ctrl,
                     link_filt_enc);
    } else if (EITHER(obj1, obj2, SP_TYPE_FILTER, SP_TYPE_VIDEO_SOURCE) ||
               EITHER(obj1, obj2, SP_TYPE_FILTER, SP_TYPE_AUDIO_SOURCE)) {
        SP_EVENT_BUFFER_CTX_ALLOC(SPLinkSourceFilterCtx, link_src_filt, api_link_src_filt_free, NULL)
        link_src_filt->src_ref  = PICK_REF_INV(obj1, obj2, SP_TYPE_FILTER);
        link_src_filt->filt_ref = PICK_REF(obj1, obj2, SP_TYPE_FILTER);
        link_src_filt->filt_pad = av_strdup(dst_pad_name);

        ctrl_fn source_ctrl = ((IOSysEntry *)link_src_filt->src_ref->data)->ctrl;

        GENERIC_LINK(IOSysEntry, link_src_filt->src_ref, source_ctrl,
                     FilterContext, link_src_filt->filt_ref, sp_filter_ctrl,
                     link_src_filt);
    } else if ((sp_class_get_type(obj1->data) == SP_TYPE_FILTER) &&
               (sp_class_get_type(obj1->data) == sp_class_get_type(obj2->data))) {
        SP_EVENT_BUFFER_CTX_ALLOC(SPLinkFilterFilterCtx, link_filt_filt, api_link_filt_filt_free, NULL)
        link_filt_filt->src_ref = av_buffer_ref(obj2);
        link_filt_filt->dst_ref = av_buffer_ref(obj1);

        link_filt_filt->src_filt_pad = av_strdup(src_pad_name);
        link_filt_filt->dst_filt_pad = av_strdup(dst_pad_name);

        GENERIC_LINK(FilterContext, link_filt_filt->src_ref, sp_filter_ctrl,
                     FilterContext, link_filt_filt->dst_ref, sp_filter_ctrl,
                     link_filt_filt);
    }

    LUA_ERROR("Unable to link \"%s\" (%s) to \"%s\" (%s)!",
              sp_class_get_name(obj1->data), sp_class_type_string(obj1->data),
              sp_class_get_name(obj2->data), sp_class_type_string(obj2->data));

    return 0;
}

static int lua_interface_create_selection(lua_State *L)
{
    TXMainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));
    AVBufferRef *iface_ref = lua_touserdata(L, lua_upvalueindex(2));

    LUA_CLEANUP_FN_DEFS(sp_class_get_name(iface_ref->data), "create_selection")

    if (lua_gettop(L) != 1)
        LUA_ERROR("Invalid number of arguments, expected 1, got %i!", lua_gettop(L));
    if (!lua_isfunction(L, -1))
        LUA_ERROR("Invalid argument, expected \"function\" (callback), got \"%s\"!",
                  lua_typename(L, lua_type(L, -1)));

    /* ref pops the item off the stack! */
    int fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (fn_ref == LUA_NOREF || fn_ref == LUA_REFNIL)
        LUA_ERROR("Invalid function specified, got: %s!", "nil");

    SP_EVENT_BUFFER_CTX_ALLOC(HookLuaEventCtx, hook_lua_ctx, hook_lua_event_free, ctx);

    uint64_t flags = SP_EVENT_ON_DESTROY | SP_EVENT_FLAG_IMMEDIATE | SP_EVENT_FLAG_NO_DEDUP;

    hook_lua_ctx->lctx = sp_lua_create_thread(ctx->lua);
    hook_lua_ctx->flags = flags;
    hook_lua_ctx->fn_ref = fn_ref;

    AVBufferRef *hook_event = sp_event_create(hook_lua_event_cb, NULL, flags,
                                              hook_lua_ctx_ref, 0x0);

    hook_lua_ctx->ctx_ref = sp_interface_highlight_win(iface_ref,
                                                       PROJECT_NAME " region selection",
                                                       hook_event);

    sp_bufferlist_append_noref(ctx->ext_buf_refs, hook_event);

    void *contexts[] = { ctx, hook_event };
    static const struct luaL_Reg lua_fns[] = {
        { "destroy", lua_event_destroy },
        { "await", lua_event_await },
        { NULL, NULL },
    };

    LUA_PUSH_CONTEXTED_INTERFACE(L, lua_fns, contexts);

    return 1;
}

static int lua_create_interface(lua_State *L)
{
    TXMainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));

    LUA_CLEANUP_FN_DEFS(sp_class_get_name(ctx), "create_interface")

    AVBufferRef *interface_ref = NULL;
    int err = sp_interface_init(&interface_ref);
    if (err < 0)
        LUA_ERROR("Unable to create interface context: %s!", av_err2str(err));

    sp_bufferlist_append_noref(ctx->ext_buf_refs, interface_ref);

    void *contexts[] = { ctx, interface_ref };
    static const struct luaL_Reg lua_fns[] = {
        { "create_selection", lua_interface_create_selection },
        { "destroy", lua_generic_destroy },
        { NULL, NULL },
    };

    LUA_PUSH_CONTEXTED_INTERFACE(L, lua_fns, contexts);

    return 1;
}

static int lua_create_muxer(lua_State *L)
{
    int err;
    TXMainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));

    LUA_CLEANUP_FN_DEFS(sp_class_get_name(ctx), "create_muxer")
    LUA_INTERFACE_BOILERPLATE();

    AVBufferRef *mctx_ref = sp_muxer_alloc();
    MuxingContext *mctx = (MuxingContext *)mctx_ref->data;

    LUA_SET_CLEANUP(mctx_ref);

    GET_OPT_STR(mctx->name, "name");
    GET_OPT_STR(mctx->out_url, "out_url");
    GET_OPT_STR(mctx->out_format, "out_format");

    err = sp_muxer_init(mctx_ref);
    if (err < 0)
        LUA_ERROR("Unable to init muxer: %s!", av_err2str(err));

    GET_OPTS_CLASS(mctx->avf, "options");

    AVDictionary *init_opts = NULL;
    GET_OPTS_DICT(init_opts, "priv_options");
    if (init_opts) {
        err = sp_muxer_ctrl(mctx_ref, SP_EVENT_CTRL_OPTS | SP_EVENT_FLAG_IMMEDIATE, init_opts);
        if (err < 0)
            LUA_ERROR("Unable to set options: %s!", av_err2str(err));
    }
    av_dict_free(&init_opts);

    sp_bufferlist_append_noref(ctx->ext_buf_refs, mctx_ref);

    void *contexts[] = { ctx, mctx_ref };
    static const struct luaL_Reg lua_fns[] = {
        { "ctrl", lua_generic_ctrl },
        { "hook", lua_generic_hook },
        { "link", lua_generic_link },
        { "destroy", lua_generic_destroy },
        { NULL, NULL },
    };

    LUA_PUSH_CONTEXTED_INTERFACE(L, lua_fns, contexts);

    return 1;
}

static int lua_create_encoder(lua_State *L)
{
    int err;
    const char *temp_str;
    TXMainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));

    LUA_CLEANUP_FN_DEFS(sp_class_get_name(ctx), "create_encoder")
    LUA_INTERFACE_BOILERPLATE();

    AVBufferRef *ectx_ref = sp_encoder_alloc();
    EncodingContext *ectx = (EncodingContext *)ectx_ref->data;

    LUA_SET_CLEANUP(ectx_ref);

    const char *enc_name = NULL;
    GET_OPT_STR(enc_name, "encoder");
    ectx->codec = avcodec_find_encoder_by_name(enc_name);
    if (!ectx->codec)
        LUA_ERROR("Encoder \"%s\" not found!", enc_name);

    GET_OPT_STR(ectx->name, "name");
    err = sp_encoder_init(ectx_ref);
    if (err < 0)
        LUA_ERROR("Unable to init encoder: %s!", av_err2str(err));

    SET_OPT_STR(sp_class_get_name(ectx), "name");

    SET_OPT_LIGHTUSERDATA(ectx_ref, LUA_PRIV_PREFIX "_priv");
    GET_OPTS_CLASS(ectx->avctx, "options");
    GET_OPTS_CLASS(ectx->swr, "resampler_options");

    SET_OPT_INT(ectx->width, "width");
    SET_OPT_INT(ectx->height, "height");
    SET_OPT_INT(ectx->width, "sample_rate");

    temp_str = NULL;
    GET_OPT_STR(temp_str, "pix_fmt");
    if (temp_str) {
        ectx->pix_fmt = av_get_pix_fmt(temp_str);
        if (ectx->pix_fmt == AV_PIX_FMT_NONE && strcmp(temp_str, "none"))
            LUA_ERROR("Invalid pixel format \"%s\"!", temp_str);
    }

    temp_str = NULL;
    GET_OPT_STR(temp_str, "sample_fmt");
    if (temp_str) {
        ectx->sample_fmt = av_get_sample_fmt(temp_str);
        if (ectx->sample_fmt == AV_SAMPLE_FMT_NONE && strcmp(temp_str, "none"))
            LUA_ERROR("Invalid sample format \"%s\"!", temp_str);
    }

    temp_str = NULL;
    GET_OPT_STR(temp_str, "channel_layout");
    if (temp_str) {
        ectx->channel_layout = av_get_channel_layout(temp_str);
        if (!ectx->sample_fmt)
            LUA_ERROR("Invalid channel layout \"%s\"!", temp_str);
    }

    AVDictionary *init_opts = NULL;
    GET_OPTS_DICT(init_opts, "priv_options");
    if (init_opts) {
        err = sp_encoder_ctrl(ectx_ref, SP_EVENT_CTRL_OPTS | SP_EVENT_FLAG_IMMEDIATE, init_opts);
        if (err < 0)
            LUA_ERROR("Unable to set options: %s!", av_err2str(err));
    }
    av_dict_free(&init_opts);

    sp_bufferlist_append_noref(ctx->ext_buf_refs, ectx_ref);

    void *contexts[] = { ctx, ectx_ref };
    static const struct luaL_Reg lua_fns[] = {
        { "ctrl", lua_generic_ctrl },
        { "hook", lua_generic_hook },
        { "link", lua_generic_link },
        { "destroy", lua_generic_destroy },
        { NULL, NULL },
    };

    LUA_PUSH_CONTEXTED_INTERFACE(L, lua_fns, contexts);

    return 1;
}

static int lua_create_io(lua_State *L)
{
    TXMainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));

    LUA_CLEANUP_FN_DEFS(sp_class_get_name(ctx), "create_io")

    int num_args = lua_gettop(L);
    if (num_args != 1 && num_args != 2)
        LUA_ERROR("Invalid number of arguments, expected 1 or 2, got %i!", num_args);
    if (num_args == 2 && !lua_istable(L, -1))
        LUA_ERROR("Invalid argument, expected \"table\" (options), got \"%s\"!", lua_typename(L, lua_type(L, -1)));
    if (!lua_islightuserdata(L, -num_args))
        LUA_ERROR("Invalid argument, expected \"lightuserdata\" (identifier), got \"%s\"!",
                  lua_typename(L, lua_type(L, -num_args)));

    AVDictionary *opts = NULL;
    if (num_args == 2) {
        lua_parse_table_to_avdict(L, &opts);
        lua_pop(L, 1);
    }

    uint32_t identifier = (uintptr_t)lua_touserdata(L, -1);

    int i = 0;
    AVBufferRef *entry = NULL;
    for (; i < sp_compiled_apis_len; i++)
        if (ctx->io_api_ctx[i] &&
            (entry = sp_compiled_apis[i]->ref_entry(ctx->io_api_ctx[i], identifier)))
            break;

    if (!entry) {
        lua_pushnil(L);
        av_dict_free(&opts);
        return 0;
    }

    int err = sp_compiled_apis[i]->init_io(ctx->io_api_ctx[i], entry, opts);
    av_dict_free(&opts);
    if (err < 0)
        LUA_ERROR("Unable to init IO: %s!", av_err2str(err));

    sp_bufferlist_append_noref(ctx->ext_buf_refs, entry);

    void *contexts[] = { ctx, entry };
    static const struct luaL_Reg lua_fns[] = {
        { "ctrl", lua_generic_ctrl },
        { "hook", lua_generic_hook },
        { "link", lua_generic_link },
        { "destroy", lua_generic_destroy },
        { NULL, NULL },
    };

    LUA_PUSH_CONTEXTED_INTERFACE(L, lua_fns, contexts);

    return 1;
}

static int lua_filter_command_template(lua_State *L, int is_graph)
{
    int err = 0;
    TXMainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));
    AVBufferRef *obj_ref = lua_touserdata(L, lua_upvalueindex(2));

    LUA_CLEANUP_FN_DEFS(sp_class_get_name(obj_ref->data), "command")

    /* (graph only) target, commands[] = values[], (optional) flags */
    int args = lua_gettop(L);
    if (args != (is_graph + 1) && args != (is_graph + 2))
        LUA_ERROR("Invalid number of arguments, expected %i or %i, got %i!",
                  is_graph + 1, is_graph + 2, args);
    if (args == (is_graph + 2) && !lua_isstring(L, -1) && !lua_istable(L, -1))
        LUA_ERROR("Invalid argument, expected \"string\" or \"table\" (flags), got \"%s\"!",
                  lua_typename(L, lua_type(L, -1)));
    if (!lua_istable(L, is_graph - args))
        LUA_ERROR("Invalid argument, expected \"table\" (commands), got \"%s\"!",
                  lua_typename(L, lua_type(L, is_graph - args)));
    if (is_graph && !lua_isstring(L, -args))
        LUA_ERROR("Invalid argument, expected \"string\" (target), got \"%s\"!",
                  lua_typename(L, lua_type(L, -args)));

    uint64_t flags = 0x0;
    if (args == (is_graph + 2)) {
        if (lua_isstring(L, -1))
            err = string_to_event_flags(ctx, &flags, lua_tostring(L, -1));
        else
            err = table_to_event_flags(L, ctx, &flags);
        if (err < 0)
            LUA_ERROR("Unable to parse given flags: %s", av_err2str(err));
        lua_pop(L, 1);
    }

    flags |= SP_EVENT_CTRL_COMMAND;

    AVDictionary *cmdlist = NULL;
    lua_parse_table_to_avdict(L, &cmdlist);

    if (is_graph)
        av_dict_set(&cmdlist, "sp_filter_target", lua_tostring(L, -2), 0);

    err = sp_filter_ctrl(obj_ref, flags, cmdlist);
    if (err < 0)
         LUA_ERROR("Unable to process command: %s", av_err2str(err));

    if (!(flags & SP_EVENT_FLAG_IMMEDIATE))
        add_commit_fn_to_list(ctx, sp_filter_ctrl, obj_ref);

    av_dict_free(&cmdlist);

    return 0;
}

static int lua_filter_command(lua_State *L)
{
    return lua_filter_command_template(L, 0);
}

static int lua_create_filter(lua_State *L)
{
    int err;
    TXMainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));

    LUA_CLEANUP_FN_DEFS(sp_class_get_name(ctx), "create_filter")
    LUA_INTERFACE_BOILERPLATE();

    AVBufferRef *fctx_ref = sp_filter_alloc();
    LUA_SET_CLEANUP(fctx_ref);

    const char *name = NULL;
    GET_OPT_STR(name, "name");

    const char *filter = NULL;
    GET_OPT_STR(filter, "filter");
    if (!filter)
        LUA_ERROR("Missing parameter: %s!", "filter");

    AVDictionary *opts = NULL;
    GET_OPTS_DICT(opts, "options");

    enum AVPixelFormat req_fmt = AV_PIX_FMT_NONE;
    const char *temp_str = NULL;
    GET_OPT_STR(temp_str, "pix_fmt");
    if (temp_str) {
        req_fmt = av_get_pix_fmt(temp_str);
        if (req_fmt == AV_PIX_FMT_NONE && strcmp(temp_str, "none"))
            LUA_ERROR("Invalid pixel format \"%s\"!", temp_str);
    }

    enum AVHWDeviceType hwctx_type = AV_HWDEVICE_TYPE_NONE;
    GET_OPT_STR(temp_str, "hwctx");
    if (temp_str) {
        hwctx_type = av_hwdevice_find_type_by_name(temp_str);
        if (hwctx_type == AV_HWDEVICE_TYPE_NONE && strcmp(temp_str, "none"))
            LUA_ERROR("Invalid hardware context \"%s\"!", temp_str);
    }

    char **in_pads = NULL;
    GET_OPTS_LIST(in_pads, "input_pads");

    char **out_pads = NULL;
    GET_OPTS_LIST(out_pads, "output_pads");

    err = sp_init_filter_single(fctx_ref, name, filter, in_pads, out_pads,
                                req_fmt, opts, NULL, hwctx_type);
    if (err < 0)
        LUA_ERROR("Unable to init filter: %s!", av_err2str(err));

    AVDictionary *init_opts = NULL;
    GET_OPTS_DICT(init_opts, "priv_options");
    if (init_opts) {
        err = sp_filter_ctrl(fctx_ref, SP_EVENT_CTRL_OPTS | SP_EVENT_FLAG_IMMEDIATE, init_opts);
        if (err < 0)
            LUA_ERROR("Unable to set options: %s!", av_err2str(err));
    }
    av_dict_free(&init_opts);

    sp_bufferlist_append_noref(ctx->ext_buf_refs, fctx_ref);

    void *contexts[] = { ctx, fctx_ref };
    static const struct luaL_Reg lua_fns[] = {
        { "ctrl", lua_generic_ctrl },
        { "hook", lua_generic_hook },
        { "link", lua_generic_link },
        { "command", lua_filter_command },
        { "destroy", lua_generic_destroy },
        { NULL, NULL },
    };

    LUA_PUSH_CONTEXTED_INTERFACE(L, lua_fns, contexts);

    return 1;
}

static int lua_filtergraph_command(lua_State *L)
{
    return lua_filter_command_template(L, 1);
}

static int lua_create_filtergraph(lua_State *L)
{
    int err;
    TXMainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));

    LUA_CLEANUP_FN_DEFS(sp_class_get_name(ctx), "create_filtergraph")
    LUA_INTERFACE_BOILERPLATE();

    AVBufferRef *fctx_ref = sp_filter_alloc();

    LUA_SET_CLEANUP(fctx_ref);

    const char *name = NULL;
    GET_OPT_STR(name, "name");

    const char *graph = NULL;
    GET_OPT_STR(graph, "graph");
    if (!graph)
        LUA_ERROR("Missing parameter: %s!", "graph");

    AVDictionary *opts = NULL;
    GET_OPTS_DICT(opts, "options");

    enum AVHWDeviceType hwctx_type = AV_HWDEVICE_TYPE_NONE;
    const char *temp_str = NULL;
    GET_OPT_STR(temp_str, "hwctx");
    if (temp_str) {
        hwctx_type = av_hwdevice_find_type_by_name(temp_str);
        if (hwctx_type == AV_HWDEVICE_TYPE_NONE && strcmp(temp_str, "none"))
            LUA_ERROR("Invalid hardware context \"%s\"!", temp_str);
    }

    char **in_pads = NULL;
    GET_OPTS_LIST(in_pads, "input_pads");

    char **out_pads = NULL;
    GET_OPTS_LIST(out_pads, "output_pads");

    err = sp_init_filter_graph(fctx_ref, name, graph, in_pads, out_pads, opts, hwctx_type);
    if (err < 0)
        LUA_ERROR("Unable to init filter: %s!", av_err2str(err));

    AVDictionary *init_opts = NULL;
    GET_OPTS_DICT(init_opts, "priv_options");
    if (init_opts) {
        err = sp_filter_ctrl(fctx_ref, SP_EVENT_CTRL_OPTS | SP_EVENT_FLAG_IMMEDIATE, init_opts);
        if (err < 0)
            LUA_ERROR("Unable to set options: %s!\n", av_err2str(err));
    }
    av_dict_free(&init_opts);

    sp_bufferlist_append_noref(ctx->ext_buf_refs, fctx_ref);

    void *contexts[] = { ctx, fctx_ref };
    static const struct luaL_Reg lua_fns[] = {
        { "ctrl", lua_generic_ctrl },
        { "hook", lua_generic_hook },
        { "link", lua_generic_link },
        { "command", lua_filtergraph_command },
        { "destroy", lua_generic_destroy },
        { NULL, NULL },
    };

    LUA_PUSH_CONTEXTED_INTERFACE(L, lua_fns, contexts);

    return 1;
}

static int epoch_event_cb(AVBufferRef *opaque, void *src_ctx, void *data)
{
    int err = 0;
    TXMainContext *ctx = av_buffer_get_opaque(opaque);
    EpochEventCtx *epoch_ctx = (EpochEventCtx *)opaque->data;

    int64_t val;
    switch(epoch_ctx->mode) {
    case EP_MODE_OFFSET:
        val = av_gettime_relative() + epoch_ctx->value;
        break;
    case EP_MODE_SYSTEM:
        val = 0;
        break;
    case EP_MODE_SOURCE:
        sp_log(ctx, SP_LOG_ERROR, "Warning epoch mode source, using epoch of system!\n");
        val = 0;
        break;
    case EP_MODE_EXTERNAL: {
        lua_State *L = sp_lua_lock_interface(ctx->lua);

        if (epoch_ctx->fn_ref == LUA_NOREF || epoch_ctx->fn_ref == LUA_REFNIL) {
            sp_log(ctx, SP_LOG_ERROR, "Invalid Lua epoch callback \"nil\"!\n");
            return sp_lua_unlock_interface(ctx->lua, AVERROR_EXTERNAL);
        }

        lua_rawgeti(L, LUA_REGISTRYINDEX, epoch_ctx->fn_ref);

        /* 1 argument: system time */
        lua_pushinteger(L, av_gettime_relative());

        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            sp_log(ctx, SP_LOG_ERROR, "Error calling external epoch callback: %s!\n",
                   lua_tostring(L, -1));
            return sp_lua_unlock_interface(ctx->lua, AVERROR_EXTERNAL);
        }

        if (!lua_isinteger(L, -1) && !lua_isnumber(L, -1)) {
            sp_log(ctx, SP_LOG_ERROR, "Invalid return value for epoch function, "
                   "expected \"integer\" or \"number\", got \"%s\"!",
                   lua_typename(L, lua_type(L, -1)));
            return sp_lua_unlock_interface(ctx->lua, AVERROR_EXTERNAL);
        }

        val = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : lua_tonumber(L, -1);

        sp_lua_unlock_interface(ctx->lua, 0);

        break; }
    }

    atomic_store(&ctx->epoch_value, val);

    return err;
}

static void epoch_event_free(void *opaque, uint8_t *data)
{
    TXMainContext *ctx = opaque;
    EpochEventCtx *epoch_ctx = (EpochEventCtx *)data;

    av_buffer_unref(&epoch_ctx->src_ref);

    lua_State *L = sp_lua_lock_interface(ctx->lua);
    luaL_unref(L, LUA_REGISTRYINDEX, epoch_ctx->fn_ref);
    sp_lua_unlock_interface(ctx->lua, 0);

    av_free(data);
}

static int lua_set_epoch(lua_State *L)
{
    TXMainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));

    LUA_CLEANUP_FN_DEFS(sp_class_get_name(ctx), "set_epoch")

    if (lua_gettop(L) != 1)
        LUA_ERROR("Invalid number of arguments, expected 1, got %i!",
                  lua_gettop(L));

    SP_EVENT_BUFFER_CTX_ALLOC(EpochEventCtx, epoch_ctx, epoch_event_free, ctx)
    epoch_ctx->fn_ref = LUA_NOREF;

    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        lua_next(L, -2);
        if (!lua_isfunction(L, -1))
            LUA_ERROR("Invalid argument, expected \"table\"[0].\"function\", got \"%s\"!",
                      lua_typename(L, lua_type(L, -1)));

        lua_getupvalue(L, 3, 2);
        if (!lua_isuserdata(L, -1))
            LUA_ERROR("Invalid argument, expected \"table\"[0].\"function\"[upvalue].\"userdata\", got \"%s\"!",
                      lua_typename(L, lua_type(L, -1)));

        AVBufferRef *obj = lua_touserdata(L, -1);

        enum SPType type = sp_class_get_type(obj->data);
        if (type != SP_TYPE_CLOCK_SOURCE)
            LUA_ERROR("Invalid reference category, expected \"clock source\", got \"%s\"!",
                      sp_class_type_string(obj->data));

        epoch_ctx->mode = EP_MODE_SOURCE;
        epoch_ctx->src_ref = av_buffer_ref(obj);
    } else if (lua_isnumber(L, -1) || lua_isinteger(L, -1)) {
        epoch_ctx->mode = EP_MODE_OFFSET;
        epoch_ctx->value = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : lua_tonumber(L, -1);
    } else if (lua_isstring(L, -1)) {
        const char *str = lua_tostring(L, -1);
        if (!strcmp(str, "zero")) {
            epoch_ctx->mode = EP_MODE_OFFSET;
            epoch_ctx->value = 0;
        } else if (!strcmp(str, "system")) {
            epoch_ctx->mode = EP_MODE_SYSTEM;
        } else {
            av_buffer_unref(&epoch_ctx_ref);
            LUA_ERROR("Invalid epoch mode, expected \"zero\" or \"system\", got \"%s\"!", str);
        }
    } else if (lua_isfunction(L, -1)) {
        epoch_ctx->mode = EP_MODE_EXTERNAL;
        epoch_ctx->fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
        LUA_ERROR("Invalid argument, expected \"string\", \"table\", \"integer\", "
                  "\"number\", or \"function\", " "got \"%s\"!",
                  lua_typename(L, lua_type(L, -1)));
    }

    enum SPEventType flags = SP_EVENT_FLAG_ONESHOT | SP_EVENT_ON_COMMIT | SP_EVENT_FLAG_HIGH_PRIO;
    AVBufferRef *epoch_event = sp_event_create(epoch_event_cb, NULL,
                                               flags, epoch_ctx_ref,
                                               sp_event_gen_identifier(ctx, NULL, flags));

    sp_eventlist_add(ctx, ctx->commit_list, epoch_event);
    av_buffer_unref(&epoch_event);

    return 0;
}

static int lua_commit(lua_State *L)
{
    TXMainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));
    /* No need to lock here, if anything needs locking the commit functions
     * will take care of it */
    sp_eventlist_dispatch(ctx, ctx->commit_list, SP_EVENT_ON_COMMIT, NULL);
    sp_eventlist_discard(ctx->discard_list);

    return 0;
}

static int lua_discard(lua_State *L)
{
    TXMainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));
    sp_eventlist_dispatch(ctx, ctx->discard_list, SP_EVENT_ON_COMMIT, NULL);
    sp_eventlist_discard(ctx->commit_list);

    return 0;
}

typedef struct SPSourceEventCbCtx {
    int fn_ref;
    TXLuaContext *lua; /* Source events happen from a separate Lua thread */
} SPSourceEventCbCtx;

static int source_event_cb(AVBufferRef *opaque, void *src_ctx, void *data)
{
    int err = 0;
    SPSourceEventCbCtx *source_cb_ctx = (SPSourceEventCbCtx *)opaque->data;
    IOSysEntry *entry = src_ctx;

    lua_State *L = sp_lua_lock_interface(source_cb_ctx->lua);

    lua_rawgeti(L, LUA_REGISTRYINDEX, source_cb_ctx->fn_ref);

    lua_pushlightuserdata(L, (void *)(uintptr_t)entry->identifier);

    lua_newtable(L);

    SET_OPT_STR(sp_class_get_name(entry), "name");
    if (sp_class_get_parent_name(entry))
        SET_OPT_STR(sp_class_get_parent_name(entry), "api");
    SET_OPT_STR(sp_class_type_string(entry), "type");
    SET_OPT_STR(entry->desc, "description");
    SET_OPT_LIGHTUSERDATA((void *)(uintptr_t)entry->identifier, "identifier");
    SET_OPT_INT(entry->api_id, "api_id");
    SET_OPT_BOOL(entry->is_default, "default");

    if (entry->type != SP_IO_TYPE_NONE)
        SET_OPT_STR(sp_iosys_entry_type_string(entry->type), "type");

    if (sp_class_get_type(entry) & SP_TYPE_VIDEO_BIDIR) {
        lua_newtable(L);

        SET_OPT_INT(entry->width, "width");
        SET_OPT_INT(entry->height, "height");
        SET_OPT_INT(entry->scale, "scale");
        SET_OPT_NUM(av_q2d(entry->framerate), "framerate");

        lua_setfield(L, -2, "video");
    } else if (sp_class_get_type(entry) & SP_TYPE_AUDIO_BIDIR) {
        lua_newtable(L);

        char map_str[256];
        av_get_channel_layout_string(map_str, sizeof(map_str),
                                     entry->channels, entry->channel_layout);

        SET_OPT_STR(map_str, "channel_layout");
        SET_OPT_INT(entry->channels, "channels");
        SET_OPT_INT(entry->sample_rate, "sample_rate");
        SET_OPT_NUM(entry->volume, "volume");
        SET_OPT_STR(av_get_sample_fmt_name(entry->sample_fmt), "format");

        lua_setfield(L, -2, "audio");
    }

    err = sp_lua_run_generic_yieldable(source_cb_ctx->lua, 2, 1, 1);
    if (err < 0)
        sp_log(entry, SP_LOG_ERROR, "Error calling external source update callback: %s!\n",
               lua_tostring(L, -1));

    return sp_lua_unlock_interface(source_cb_ctx->lua, err);
}

static void source_event_free(void *opaque, uint8_t *data)
{
    SPSourceEventCbCtx *source_cb_ctx = (SPSourceEventCbCtx *)data;

    lua_State *L = sp_lua_lock_interface(source_cb_ctx->lua);
    luaL_unref(L, LUA_REGISTRYINDEX, source_cb_ctx->fn_ref);
    sp_lua_unlock_interface(source_cb_ctx->lua, 0);

    sp_lua_close_ctx(&source_cb_ctx->lua);
    av_free(data);
}

static int lua_register_io_cb(lua_State *L)
{
    int err = 0;
    TXMainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));

    LUA_CLEANUP_FN_DEFS(sp_class_get_name(ctx), "register_io_cb")

    int nb_args = lua_gettop(L);
    if (nb_args != 1 && nb_args != 2)
        LUA_ERROR("Invalid number of arguments, expected 1 or 2, got %i!", nb_args);
    if (nb_args == 2 && !lua_istable(L, -1))
        LUA_ERROR("Invalid argument, expected \"table\" (API names), got \"%s\"!",
                  lua_typename(L, lua_type(L, -1)));
    if (!lua_isfunction(L, -nb_args))
        LUA_ERROR("Invalid argument, expected \"function\" (callback), got \"%s\"!",
                  lua_typename(L, lua_type(L, -nb_args)));

    char **api_list = NULL;
    if (nb_args == 2) {
        err = lua_parse_table_to_list(L, &api_list);
        if (err < 0)
            LUA_ERROR("Unable to load API list: %s!", av_err2str(err));
        lua_pop(L, 1);

        for (int i = 0; (api_list && api_list[i]); i++) {
            int j;
            for (j = 0; j < sp_compiled_apis_len; j++)
                if (!strcmp(api_list[i], sp_compiled_apis[j]->name))
                    break;
            if (j == sp_compiled_apis_len) {
                char temp[99] = { 0 };
                snprintf(temp, sizeof(temp), "%s", api_list[i]);
                FREE_STR_LIST(api_list);
                LUA_ERROR("API \"%s\" not found!", temp);
            }
        }
    }

    int fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (fn_ref == LUA_NOREF || fn_ref == LUA_REFNIL) {
        FREE_STR_LIST(api_list);
        return 0;
    }

    if (!ctx->io_api_ctx)
        ctx->io_api_ctx = av_mallocz(sp_compiled_apis_len * sizeof(*ctx->io_api_ctx));

    /* Initialize I/O APIs */
    int initialized_apis = 0;
    for (int i = 0; i < sp_compiled_apis_len; i++) {
        if (ctx->io_api_ctx[i])
            continue;
        int found = 0;
        for (int j = 0; (api_list && !!api_list[j]); j++) {
            if (!strcmp(api_list[j], sp_compiled_apis[i]->name)) {
                found = 1;
                break;
            }
        }
        if (api_list && !found)
            continue;

        err = sp_compiled_apis[i]->init_sys(&ctx->io_api_ctx[i]);
        if (!api_list && err == AVERROR(ENOSYS)) {
            continue;
        } else if (err < 0) {
            FREE_STR_LIST(api_list);
            LUA_ERROR("Unable to load API \"%s\": %s!", sp_compiled_apis[i]->name,
                      av_err2str(err));
        }

        initialized_apis++;
    }

    if (!initialized_apis) {
        FREE_STR_LIST(api_list);

        if (api_list)
            LUA_ERROR("No requested I/O API(s) available of the %i enabled at build time.\n",
                      sp_compiled_apis_len);
        else
            sp_log(ctx, SP_LOG_WARN, "No I/O APIs available.\n");

        return 0;
    }

    SP_EVENT_BUFFER_CTX_ALLOC(SPSourceEventCbCtx, source_event_ctx, source_event_free, ctx)
    source_event_ctx->fn_ref = fn_ref;
    source_event_ctx->lua = sp_lua_create_thread(ctx->lua);

    enum SPEventType flags = SP_EVENT_TYPE_SOURCE | SP_EVENT_ON_CHANGE |
                             SP_EVENT_FLAG_IMMEDIATE | SP_EVENT_FLAG_NO_DEDUP;
    AVBufferRef *source_event = sp_event_create(source_event_cb, NULL, flags,
                                                source_event_ctx_ref, 0x0);

    LUA_SET_CLEANUP(source_event);

    for (int i = 0; i < sp_compiled_apis_len; i++) {
        int found = 0;
        for (int j = 0; (api_list && !!api_list[j]); j++) {
            if (!strcmp(api_list[j], sp_compiled_apis[i]->name)) {
                found = 1;
                break;
            }
        }
        if ((api_list && !found) || !ctx->io_api_ctx[i])
            continue;

        /* Add and immediately run the event to signal all devices */
        err = sp_compiled_apis[i]->ctrl(ctx->io_api_ctx[i],
                                        SP_EVENT_CTRL_NEW_EVENT | SP_EVENT_FLAG_IMMEDIATE,
                                        source_event);
        if (err < 0) {
            FREE_STR_LIST(api_list);
            LUA_ERROR("Unable to add event to API \"%s\": %s!", sp_compiled_apis[i]->name,
                      av_err2str(err));
        }
    }

    FREE_STR_LIST(api_list);

    sp_bufferlist_append_noref(ctx->ext_buf_refs, source_event);

    void *contexts[] = { ctx, source_event };
    static const struct luaL_Reg lua_fns[] = {
        { "destroy", lua_event_destroy },
        { "await", lua_event_await },
        { NULL, NULL },
    };

    LUA_PUSH_CONTEXTED_INTERFACE(L, lua_fns, contexts);

    return 1;
}

static int lua_log_fn(lua_State *L, enum SPLogLevel lvl)
{
    TXMainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));

    LUA_CLEANUP_FN_DEFS(sp_class_get_name(ctx), "log")

    int nb_args = lua_gettop(L);
    lua_rotate(L, nb_args, nb_args);

    AVBPrint bpc;
    av_bprint_init(&bpc, 256, AV_BPRINT_SIZE_AUTOMATIC);

    for (int i = 0; i < nb_args; i++) {
        size_t len;
        const char *str = lua_tolstring(L, -1, &len);
        av_bprint_append_data(&bpc, str, len);
        av_bprint_chars(&bpc, i != (nb_args - 1) ? ' ' : '\n', 1);
        lua_pop(L, 1);
    }

    av_bprint_chars(&bpc, '\0', 1);

    char *rstr;
    av_bprint_finalize(&bpc, &rstr);

    sp_log(ctx->lua, lvl, "%s", rstr);

    av_free(rstr);

    return 0;
}

static int lua_log(lua_State *L)
{
    return lua_log_fn(L, SP_LOG_INFO);
}

static int lua_log_warn(lua_State *L)
{
    return lua_log_fn(L, SP_LOG_WARN);
}

static int lua_log_err(lua_State *L)
{
    return lua_log_fn(L, SP_LOG_ERROR);
}

static int lua_prompt(lua_State *L)
{
    TXMainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));

    LUA_CLEANUP_FN_DEFS(sp_class_get_name(ctx), "prompt")

    if (lua_gettop(L) != 2)
        LUA_ERROR("Invalid number of arguments, expected 2, got %i!", lua_gettop(L));
    if (!lua_isfunction(L, -1))
        LUA_ERROR("Invalid argument, expected \"function\" (callback), got \"%s\"!",
                  lua_typename(L, lua_type(L, -1)));
    if (!lua_isstring(L, -2))
        LUA_ERROR("Invalid argument, expected \"string\" (prompt message), got \"%s\"!",
                  lua_typename(L, lua_type(L, -2)));

#ifdef HAVE_LIBEDIT
    /* ref pops the item off the stack! */
    int fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (fn_ref == LUA_NOREF || fn_ref == LUA_REFNIL)
        LUA_ERROR("Invalid function specified, got: %s!", "nil");

    SP_EVENT_BUFFER_CTX_ALLOC(HookLuaEventCtx, hook_lua_ctx, hook_lua_event_free, ctx);

    uint64_t flags = SP_EVENT_ON_DESTROY | SP_EVENT_FLAG_IMMEDIATE | SP_EVENT_FLAG_NO_DEDUP;

    hook_lua_ctx->lctx = sp_lua_create_thread(ctx->lua);
    hook_lua_ctx->flags = flags;
    hook_lua_ctx->fn_ref = fn_ref;

    AVBufferRef *hook_event = sp_event_create(hook_lua_event_cb, NULL, flags,
                                              hook_lua_ctx_ref, 0x0);

    int ret = sp_cli_prompt_event(ctx->cli, hook_event, lua_tostring(L, -1));
    if (ret < 0) {
        sp_event_unref_expire(&hook_event);
        LUA_ERROR("Unable to add event: %s!", av_err2str(ret));
        goto end;
    }

    sp_bufferlist_append_noref(ctx->ext_buf_refs, hook_event);

    void *contexts[] = { ctx, hook_event };
    static const struct luaL_Reg lua_fns[] = {
        { "destroy", lua_event_destroy },
        { "await", lua_event_await },
        { NULL, NULL },
    };

    LUA_PUSH_CONTEXTED_INTERFACE(L, lua_fns, contexts);

end:
#else
    LUA_ERROR("Unable to add event: %s!", "txproto was not compiled with libedit enabled");
#endif

    return 1;
}

static int lua_set_status(lua_State *L)
{
    TXMainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));

    LUA_CLEANUP_FN_DEFS(sp_class_get_name(ctx), "set_status")

    if (lua_gettop(L) != 1)
        LUA_ERROR("Invalid number of arguments, expected 1, got %i!", lua_gettop(L));
    if (!lua_isstring(L, -1))
        LUA_ERROR("Invalid argument, expected \"string\" (status), got \"%s\"!",
                  lua_typename(L, lua_type(L, -1)));

    const char *message = lua_tostring(L, -1);
    int err = sp_log_set_status(message, 0);
    if (err < 0)
        LUA_ERROR("Unable to set status: %s!", av_err2str(err));

    return 0;
}

static int lua_api_version(lua_State *L)
{
    TXMainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));

    LUA_CLEANUP_FN_DEFS(sp_class_get_name(ctx), "api_version")

    lua_pushinteger(L, LUA_API_VERSION[0]);
    lua_pushinteger(L, LUA_API_VERSION[1]);

    return 2;
}

static int lua_lang_version(lua_State *L)
{
    TXMainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));

    LUA_CLEANUP_FN_DEFS(sp_class_get_name(ctx), "lua_lang_version")

    lua_pushinteger(L, lua_version(L));

    return 1;
}

int sp_lua_quit(lua_State *L)
{
    TXMainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));

    LUA_CLEANUP_FN_DEFS(sp_class_get_name(ctx), "quit")

    if (lua_gettop(L) != 0 && lua_gettop(L) != 1)
        LUA_ERROR("Invalid number of arguments, expected 0 or 1, got %i!",
                  lua_gettop(L));
    if (lua_gettop(L) && !lua_isinteger(L, -1))
        LUA_ERROR("Invalid argument, expected \"integer\" (return code), got \"%s\"!",
                  lua_typename(L, lua_type(L, -1)));

    if (lua_gettop(L))
        ctx->lua_exit_code = lua_tointeger(L, -1);

    raise(SIGINT);

    return 0;
}

const struct luaL_Reg sp_lua_lib_fns[] = {
    { "register_io_cb", lua_register_io_cb },

    { "create_io", lua_create_io },
    { "create_muxer", lua_create_muxer },
    { "create_encoder", lua_create_encoder },
    { "create_filter", lua_create_filter },
    { "create_filtergraph", lua_create_filtergraph },
    { "create_interface", lua_create_interface },

    { "set_epoch", lua_set_epoch },

    { "commit", lua_commit },
    { "discard", lua_discard },

    { "log", lua_log },
    { "log_warn", lua_log_warn },
    { "log_err", lua_log_err },

    { "set_status", lua_set_status },

    { "prompt", lua_prompt },

    { "api_version", lua_api_version },
    { "lua_version", lua_lang_version },

    { "quit", sp_lua_quit },

    { NULL, NULL },
};

int sp_lua_load_main_api(TXLuaContext *lctx, TXMainContext *ctx)
{
    lua_State *L = sp_lua_lock_interface(lctx);

    LUA_PUSH_CONTEXTED_INTERFACE(L, sp_lua_lib_fns, (void *[]){ ctx });
    lua_setglobal(L, LUA_PUB_PREFIX);

    return sp_lua_unlock_interface(lctx, 0);
}
