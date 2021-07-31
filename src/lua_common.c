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

#include <unistd.h>

#include <libavutil/avstring.h>

#include "lua_common.h"
#include "lua_api.h"

#include "logging.h"
#include "utils.h"

#include <zlib.h>

/* Built-in scripts */
#include "utils.lua.bin.h"

static void *lua_alloc_fn(void *opaque, void *ptr, size_t type, size_t nsize)
{
    if (!nsize) {
        av_free(ptr);
        return NULL;
    }
    return av_realloc(ptr, nsize);
}

static int lua_panic_handler(lua_State *L)
{
    luaL_traceback(L, L, NULL, 1);
    sp_log(NULL, SP_LOG_ERROR, "%s\n", lua_tostring(L, -1));
    return 0;
}

static void lua_warning_handler(void *opaque, const char *msg, int contd)
{
    sp_log(opaque, SP_LOG_WARN, "%s%c", msg, contd ? '\0' : '\n');
}

void sp_lua_close_ctx(TXLuaContext **s)
{
    if (!s || !(*s))
        return;

    TXLuaContext *lctx = *s;

    lua_State *L = sp_lua_lock_interface(lctx);

    if (lctx->master_thread) {
        TXLuaContext *mctx = lctx->master_thread;
        lua_resetthread(L);
        luaL_unref(L, LUA_REGISTRYINDEX, lctx->thread_ref);
        av_freep(s);
        sp_lua_unlock_interface(mctx, 0);
        return;
    }

    lua_close(L);

    sp_lua_unlock_interface(lctx, 0);
    pthread_mutex_destroy(lctx->lock);

    for (int i = 0; i < lctx->loaded_lib_list_len; i++) {
        if (lctx->loaded_lib_list[i].libp)
            dlclose(lctx->loaded_lib_list[i].libp);
        av_free(lctx->loaded_lib_list[i].name);
        av_free(lctx->loaded_lib_list[i].path);
    }
    av_free(lctx->loaded_lib_list);

    av_free(lctx->lock);
    sp_class_free(lctx);
    av_freep(s);
}

int sp_lua_load_compressed(TXLuaContext *s, const uint8_t *in, const size_t len)
{
    int ret;
    z_stream stream = {
        stream.next_in = (uint8_t *)in,
        stream.avail_in = len,
    };

#define CHUNK_SIZE 4096
    if ((ret = inflateInit2(&stream, MAX_WBITS)) != Z_OK) {
        sp_log(s, SP_LOG_ERROR, "Error initializing zlib: %s\n", stream.msg);
        return ret == Z_STREAM_ERROR ? AVERROR(EINVAL) :
               ret == Z_MEM_ERROR ? AVERROR(ENOMEM) : AVERROR_EXTERNAL;
    }

    uint8_t *buf_out = av_malloc(CHUNK_SIZE);
    size_t buf_size = CHUNK_SIZE;

    do {
        stream.avail_out = buf_size - stream.total_out;
        stream.next_out = buf_out + stream.total_out;

        ret = inflate(&stream, Z_FINISH);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
            sp_log(s, SP_LOG_ERROR, "Error deflating at byte %lu: %s\n",
                   stream.total_out, stream.msg);
            inflateEnd(&stream);
            av_free(buf_out);
            return AVERROR(EINVAL);
        }

        if (ret == Z_BUF_ERROR) {
            buf_size += CHUNK_SIZE;
            uint8_t *tmp = av_realloc(buf_out, buf_size);
            if (!tmp) {
                inflateEnd(&stream);
                av_free(buf_out);
                return AVERROR(ENOMEM);
            }
            buf_out = tmp;
        }
    } while (ret != Z_STREAM_END);

    inflateEnd(&stream);

    lua_State *L = sp_lua_lock_interface(s);

    ret = luaL_loadbufferx(L, buf_out, stream.total_out,
                           "compressed_chunk", "b");
    av_free(buf_out);
    if (ret) {
        sp_log(s, SP_LOG_ERROR, "Error loading Lua code: %s\n", lua_tostring(L, -1));
        ret = AVERROR_EXTERNAL;
    }

    return sp_lua_unlock_interface(s, ret);
}

int sp_lua_create_ctx(TXLuaContext **s, void *ctx, const char *lua_libs_list)
{
    int err;

    TXLuaContext *lctx = av_mallocz(sizeof(*lctx));
    if (!lctx)
        return AVERROR(ENOMEM);

    err = sp_class_alloc(lctx, "lua", SP_TYPE_SCRIPT, ctx);
    if (err < 0) {
        av_free(lctx);
        return AVERROR(ENOMEM);
    }

    /* Create Lua context */
    lua_State *L = lctx->L = lua_newstate(lua_alloc_fn, ctx);
    if (!L) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    /* Create mutex */
    lctx->lock = av_mallocz(sizeof(*lctx->lock));
    if (!lctx->lock) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    pthread_mutexattr_t main_lock_attr;
    pthread_mutexattr_init(&main_lock_attr);
    pthread_mutexattr_settype(&main_lock_attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(lctx->lock, &main_lock_attr);

    /* Use the new generational garbage cleaner */
    lua_gc(L, LUA_GCGEN);
    lua_setwarnf(L, lua_warning_handler, lctx);
    lua_atpanic(L, lua_panic_handler);

    /* Load needed Lua libraries */
    char *list = av_strdup(lua_libs_list);
    char *save, *token = av_strtok(list, ",", &save);
    while (token) {
        if ((err = sp_lua_load_library(lctx, token)) < 0) {
            sp_log(ctx, SP_LOG_ERROR, "Error loading library \"%s\": %s!\n",
                   token, av_err2str(err));
            av_free(list);
            goto fail;
        }
        token = av_strtok(NULL, ",", &save);
    }
    av_free(list);

    /* Load Lua utils */
    if ((err = sp_lua_load_compressed(lctx, utils_lua_bin, utils_lua_bin_len)))
        goto fail;

    *s = lctx;

    return 0;

fail:
    pthread_mutex_destroy(lctx->lock);
    av_free(lctx->lock);
    lua_close(L);
    sp_class_free(lctx);
    av_free(lctx);

    return err;
}

TXLuaContext *sp_lua_create_thread(TXLuaContext *lctx)
{
    if (!lctx)
        return NULL;

    lua_State *L = sp_lua_lock_interface(lctx);

    TXLuaContext *new = av_mallocz(sizeof(*new));
    memcpy(new, lctx, sizeof(*new));
    new->master_thread = lctx->master_thread ? lctx->master_thread : lctx;

    new->L = lua_newthread(L);
    new->thread_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    pthread_mutex_unlock(lctx->lock);

    return new;
}

lua_State *sp_lua_lock_interface(TXLuaContext *lctx)
{
    pthread_mutex_lock(lctx->lock);
    return lctx->L;
}

int sp_lua_unlock_interface(TXLuaContext *lctx, int ret)
{
    pthread_mutex_unlock(lctx->lock);
    return ret;
}

int sp_lua_load_file(TXLuaContext *lctx, const char *script_name)
{
    lua_State *L = sp_lua_lock_interface(lctx);

    int err = luaL_loadfilex(L, script_name, "t");
    if (err == LUA_ERRFILE) {
        sp_log(lctx, SP_LOG_ERROR, "File \"%s\" not found!\n", script_name);
        err = AVERROR(EINVAL);
    } else if (err) {
        sp_log(lctx, SP_LOG_ERROR, "%s\n", lua_tostring(L, -1));
        err = AVERROR_EXTERNAL;
    }

    return sp_lua_unlock_interface(lctx, err);
}

static const luaL_Reg lua_internal_lib_fns[] = {
    { LUA_BASELIBNAME, luaopen_base      },
    { LUA_COLIBNAME,   luaopen_coroutine },
    { LUA_TABLIBNAME,  luaopen_table     },
    { LUA_IOLIBNAME,   luaopen_io        },
    { LUA_OSLIBNAME,   luaopen_os        },
    { LUA_STRLIBNAME,  luaopen_string    },
    { LUA_UTF8LIBNAME, luaopen_utf8      },
    { LUA_MATHLIBNAME, luaopen_math      },
    { LUA_DBLIBNAME,   luaopen_debug     },
    { LUA_LOADLIBNAME, luaopen_package   },
    { NULL,            NULL              },
};

int sp_lua_load_library(TXLuaContext *lctx, const char *lib)
{
    int err = 0;
    lua_State *L = sp_lua_lock_interface(lctx);

    luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    int type = lua_getfield(L, -1, lib);
    lua_pop(L, 1);
    /* Already loaded */
    if (type != LUA_TNIL) {
        err = AVERROR(EADDRINUSE);
        goto end;
    }

#define ADD_LIB_TO_LIST(libname, libpath, libptr, symptr)                      \
    lctx->loaded_lib_list = av_realloc(lctx->loaded_lib_list,                  \
                                       sizeof(*lctx->loaded_lib_list) *        \
                                       (lctx->loaded_lib_list_len + 1));       \
    LuaLoadedLibrary *e = &lctx->loaded_lib_list[lctx->loaded_lib_list_len++]; \
    e->name = av_strdup(libname);                                              \
    e->path = libpath;                                                         \
    e->libp = libptr;                                                          \
    e->lib_open_sym = symptr;                                                  \

    int cnt = 0;
    while (lua_internal_lib_fns[cnt].name) {
        if (!strcmp(lua_internal_lib_fns[cnt].name, lib)) {
            luaL_requiref(L, lua_internal_lib_fns[cnt].name,
                          lua_internal_lib_fns[cnt].func, 1);
            lua_pop(L, 1);
            ADD_LIB_TO_LIST(lib, NULL, NULL, NULL)
            goto end;
        }
        cnt++;
    }

    int lib_len = strlen(lib);
    const char *lib_suffix;
    int lib_suffix_len;

    /* External C library */
    static const char *clibs_dirs[] = {
        "./",
        LUA_CDIR,
#ifdef LUA_CDIR2
        LUA_CDIR2,
#endif
#ifdef LUA_CDIR3
        LUA_CDIR3,
#endif
    };

    static const char *lib_suffixes[] = {
        ".so",
        ".dll",
        ".dylib",
    };

    char *sym_prefix = "luaopen_";
    int sym_prefix_len = strlen(sym_prefix);

    for (int i = 0; i < SP_ARRAY_ELEMS(lib_suffixes); i++) {
        lib_suffix = lib_suffixes[i];
        lib_suffix_len = strlen(lib_suffix);

        for (int j = 0; j < SP_ARRAY_ELEMS(clibs_dirs); j++) {
            int cdir_len = strlen(clibs_dirs[j]);
            int path_len = cdir_len + lib_len + lib_suffix_len + 1;
            char *path = malloc(path_len);
            memcpy(path, clibs_dirs[j], cdir_len);
            memcpy(path + cdir_len, lib, lib_len);
            memcpy(path + cdir_len + lib_len, lib_suffix, lib_suffix_len);
            path[path_len - 1] = '\0';

            void *libp = dlopen(path, RTLD_NOW | RTLD_LOCAL);
            if (!libp) {
                free(path);
                continue;
            }

            int sym_name_len = sym_prefix_len + lib_len + 1;
            char *sym_name = malloc(sym_name_len);
            memcpy(sym_name, sym_prefix, sym_prefix_len);
            memcpy(sym_name + sym_prefix_len, lib, lib_len);
            sym_name[sym_name_len - 1] = '\0';

            void *sym = dlsym(libp, sym_name);
            free(sym_name);
            if (sym) {
                luaL_requiref(L, LUA_LOADLIBNAME, luaopen_package, 1);
                luaL_requiref(L, lib, sym, 1);
                lua_pop(L, 1);
                ADD_LIB_TO_LIST(lib, path, libp, sym)
                goto end;
            }
            free(path);
        }
    }

    /* Native Lua library */
    lib_suffix = ".lua";
    lib_suffix_len = strlen(lib_suffix);

    static const char *libs_dirs[] = {
        "./",
        LUA_LDIR,
#ifdef LUA_LDIR2
        LUA_LDIR2,
#endif
    };

    for (int i = 0; i < SP_ARRAY_ELEMS(libs_dirs); i++) {
        int dir_len = strlen(libs_dirs[i]);
        int path_len = dir_len + lib_len + lib_suffix_len + 1;
        char *path = malloc(path_len);
        memcpy(path, libs_dirs[i], dir_len);
        memcpy(path + dir_len, lib, lib_len);
        memcpy(path + dir_len + lib_len, lib_suffix, lib_suffix_len);
        path[path_len - 1] = '\0';

        if (access(path, F_OK))
            continue;

        luaL_requiref(L, LUA_LOADLIBNAME, luaopen_package, 1);

        int ret = luaL_loadfilex(L, path, "t");
        if (ret) {
            free(path);
            err = AVERROR_EXTERNAL;
            goto end;
        }

        lua_pushstring(L, lib);
        lua_pushstring(L, path);
        lua_call(L, 2, 1);
        if (!lua_istable(L, -1)) {
            free(path);
            err = AVERROR(EINVAL);
            goto end;
        }

        lua_setglobal(L, lib);

        /* Add to the loaded table to keep track of */
        luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
        lua_pushvalue(L, 2);
        lua_setfield(L, -2, lib);

        ADD_LIB_TO_LIST(lib, path, NULL, NULL)

        break;
    }

    err = AVERROR(ENOENT);

end:
    return sp_lua_unlock_interface(lctx, err);
}
