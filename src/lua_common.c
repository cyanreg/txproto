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

#include <zlib.h>

#include <libavutil/avstring.h>
#include <libavutil/base64.h>

#include "lua_common.h"
#include "lua_api.h"
#include "os_compat.h"

#include <libtxproto/log.h>
#include <libtxproto/utils.h>

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
        sp_class_free(lctx);
        av_freep(s);
        *mctx->nb_threads -= 1;
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

    av_free(lctx->nb_threads);
    av_free(lctx->lock);
    sp_class_free(lctx);
    av_freep(s);
}

#define CHUNK_SIZE (1024*64)

struct SPLuaWriterContext {
    TXLuaContext *lctx;

    struct {
        FILE *file;
        const char *path;
        size_t written; /* For files only */

        uint8_t *data;
        unsigned int alloc;
        size_t len;
    } out;

    int is_compressed; /* 0 for no, 1 for yes */

    struct {
        z_stream ctx;
    } zlib;
};

static int lua_writer_end(struct SPLuaWriterContext *lw,
                          uint8_t **buf, size_t *len)
{
    int ret = 0;

    if (lw->is_compressed) {
        lw->zlib.ctx.next_in = NULL;
        lw->zlib.ctx.avail_in = 0;

        do {
            lw->zlib.ctx.next_out = lw->out.data + lw->out.len;
            lw->zlib.ctx.avail_out = lw->out.alloc - lw->out.len;

            ret = deflate(&lw->zlib.ctx, Z_FINISH);

            lw->out.len = lw->out.alloc - lw->zlib.ctx.avail_out;

            if (ret == Z_OK || ret == Z_BUF_ERROR) {
                uint8_t *new_data = av_fast_realloc(lw->out.data, &lw->out.alloc,
                                                    CHUNK_SIZE + lw->out.len);
                if (!new_data) {
                    av_freep(&lw->out.data);
                    lw->out.len = 0;
                    ret = AVERROR(ENOMEM);
                    goto out;
                }

                lw->out.data = new_data;
                continue;
            } else if (ret != Z_STREAM_END) {
                sp_log(lw->lctx, SP_LOG_ERROR, "Unable to flush zlib buffer!\n");
                ret = AVERROR_EXTERNAL;
                goto out;
            }
        } while (ret != Z_STREAM_END);

        ret = 0;

        if (lw->out.file) {
            ret = fwrite(lw->out.data, 1, lw->out.len, lw->out.file);
            if (ret != lw->out.len) {
                ret = AVERROR(errno);
                sp_log(lw->lctx, SP_LOG_ERROR, "Error writing file \"%s\": %s!\n",
                       lw->out.path, av_err2str(ret));
                goto out;
            }

            lw->out.written += ret;
        }
    }

out:
    if (lw->is_compressed)
        deflateEnd(&lw->zlib.ctx);

    if (lw->out.file) {
        fclose(lw->out.file);
        av_freep(&lw->out.data);
        ret = ret < 0 ? ret : lw->out.written;
    } else if (buf && len) {
        *buf = lw->out.data;
        *len = lw->out.len;
        ret = ret < 0 ? ret : lw->out.len;
    } else {
        av_free(lw->out.data);
    }

    memset(lw, 0, sizeof(*lw));

    return ret;
}

static int lua_write_fn(lua_State *L, const void *data, size_t len, void *opaque)
{
    int ret;
    struct SPLuaWriterContext *lw = opaque;

    if (lw->is_compressed) {
        size_t start_len = lw->out.len;
        lw->zlib.ctx.next_in = (void *)data;
        lw->zlib.ctx.avail_in = len;

        while (1) {
            lw->zlib.ctx.next_out = lw->out.data + lw->out.len;
            lw->zlib.ctx.avail_out = lw->out.alloc - lw->out.len;

            ret = deflate(&lw->zlib.ctx, Z_NO_FLUSH);
            if (ret == Z_STREAM_ERROR) {
                sp_log(lw->lctx, SP_LOG_ERROR, "zlib error while compressing chunk!\n");
                return AVERROR_EXTERNAL;
            }

            lw->out.len = lw->out.alloc - lw->zlib.ctx.avail_out;

            if ((ret == Z_OK && !lw->zlib.ctx.avail_out) ||
                (lw->out.len == lw->out.alloc)) {
                uint8_t *new_data = av_fast_realloc(lw->out.data, &lw->out.alloc,
                                                    CHUNK_SIZE + lw->out.len);
                if (!new_data) {
                    av_freep(&lw->out.data);
                    lw->out.len = 0;
                    return AVERROR(ENOMEM);
                }

                lw->out.data = new_data;
                continue;
            }

            if (ret == Z_BUF_ERROR || !lw->zlib.ctx.avail_in) {
                if (lw->out.len > start_len)
                    break; /* Had data, let's output it */
                else
                    return 0; /* More input data needed */
            }
        }

        data = lw->out.data;
        len = lw->out.len;
    }

    if (lw->out.file && len) {
        ret = fwrite(data, 1, len, lw->out.file);
        if (ret != len) {
            sp_log(lw->lctx, SP_LOG_ERROR, "Error writing file \"%s\": %s!\n",
                   lw->out.path, av_err2str(ret));
            return AVERROR(errno);
        }

        lw->out.written += ret;

        if (lw->is_compressed)
            lw->out.len = 0;
    } else if (!lw->is_compressed) {
        uint8_t *new_data = av_fast_realloc(lw->out.data, &lw->out.alloc,
                                            len + lw->out.len);
        if (!new_data) {
            av_freep(&lw->out.data);
            lw->out.len = 0;
            return AVERROR(ENOMEM);
        }

        memcpy(new_data + lw->out.len, data, len);

        lw->out.data = new_data;
        lw->out.len += len;
    }

    return 0;
}

struct SPLuaReaderContext {
    TXLuaContext *lctx;

    struct {
        const char *path;
        const uint8_t *buf;
        ptrdiff_t read;
        size_t len;
        FILE *file;
        int is_compressed; /* 0 for undetermined, -1 for no, 1 for yes */
    } in;

    struct {
        uint8_t *buf;
        size_t len;
        size_t alloc;
    } chunk;

    struct {
        z_stream ctx;
        uint8_t *buf;
        size_t buf_alloc;
    } zlib;

    int err;
};

static void lua_reader_free(struct SPLuaReaderContext *lr)
{
    int ret = lr->err;
    if (lr->in.file)
        fclose(lr->in.file);
    av_free(lr->zlib.buf);
    if (lr->chunk.alloc)
        av_free(lr->chunk.buf);
    inflateEnd(&lr->zlib.ctx);
    memset(lr, 0, sizeof(*lr));
    lr->err = ret;
}

static const char *lua_read_fn(lua_State *L, void *opaque, size_t *size)
{
    int ret;
    struct SPLuaReaderContext *lr = opaque;

more:
    if (lr->in.file) {
        if (!lr->chunk.buf) {
            lr->chunk.alloc = SPMIN(lr->in.len, CHUNK_SIZE);
            lr->chunk.buf = av_malloc(lr->chunk.alloc);
        }
        lr->chunk.len = fread(lr->chunk.buf, 1, lr->chunk.alloc, lr->in.file);
        if (!lr->chunk.len && ferror(lr->in.file)) {
            lr->err = AVERROR(errno);
            sp_log(lr->lctx, SP_LOG_ERROR, "Error reading file \"%s\": %s!\n",
                   lr->in.path, av_err2str(lr->err));
            goto end;
        }
    } else {
        lr->chunk.buf = (uint8_t *)(lr->in.buf + lr->in.read);
        lr->chunk.len = SPMIN(lr->in.len - lr->in.read, CHUNK_SIZE);
    }

    if (!lr->in.is_compressed) {
        int t = lr->chunk.buf[0] == 0x1f && lr->chunk.buf[1] == 0x8b ? 1 : -1;
        lr->in.is_compressed = t;
    }

    if (lr->in.is_compressed > 0) {
        if (!lr->zlib.buf) {
            if ((ret = inflateInit2(&lr->zlib.ctx, MAX_WBITS | 16)) != Z_OK) {
                sp_log(lr->lctx, SP_LOG_ERROR, "Error initializing zlib: %s\n",
                       lr->zlib.ctx.msg);
                lr->err = ret == Z_STREAM_ERROR ? AVERROR(EINVAL) :
                          ret == Z_MEM_ERROR ? AVERROR(ENOMEM) : AVERROR_EXTERNAL;
                goto end;
            }

            lr->zlib.buf_alloc = CHUNK_SIZE;
            lr->zlib.buf = av_malloc(lr->zlib.buf_alloc);
        }

        lr->zlib.ctx.next_in = lr->chunk.buf;
        lr->zlib.ctx.avail_in = lr->chunk.len;

        int zlib_flush = lr->chunk.len ? Z_NO_FLUSH : Z_SYNC_FLUSH;

        do {
            lr->zlib.ctx.next_out = lr->zlib.buf;
            lr->zlib.ctx.avail_out = lr->zlib.buf_alloc;

            ret = inflate(&lr->zlib.ctx, zlib_flush);
            if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
                sp_log(lr->lctx, SP_LOG_ERROR, "Error deflating at bytes %lu to %lu: %s (%i)\n",
                       lr->in.read, lr->in.read + lr->chunk.len, lr->zlib.ctx.msg, ret);
                lr->err = AVERROR(EINVAL);
                goto end;
            }

            if (ret == Z_BUF_ERROR) {
                lr->zlib.buf_alloc += CHUNK_SIZE;
                uint8_t *tmp = av_realloc(lr->zlib.buf, lr->zlib.buf_alloc);
                if (!tmp) {
                    lr->err = AVERROR(ENOMEM);
                    goto end;
                }
                lr->zlib.buf = tmp;

                if (zlib_flush == Z_NO_FLUSH) {
                    if (lr->zlib.buf_alloc > 2*CHUNK_SIZE)
                        zlib_flush = Z_BLOCK; /* Stop. */
                    else if (lr->zlib.buf_alloc > 8*CHUNK_SIZE)
                        zlib_flush = Z_SYNC_FLUSH; /* Pleeeeease */
                }
            }
        } while (ret == Z_BUF_ERROR);

        size_t avail = lr->zlib.ctx.next_out - lr->zlib.buf;
        size_t read = lr->zlib.ctx.next_in - lr->chunk.buf;
        lr->in.read += read;

        if (!avail && read && ret == Z_OK) {
            goto more;
        } else if (avail) {
            *size = avail;
            return lr->zlib.buf;
        } else if (ret != Z_STREAM_END) {
            sp_log(lr->lctx, SP_LOG_ERROR, "zlib decompressing error: %i!\n", ret);
            lr->err = AVERROR(EINVAL);
        }
        /* Fallthrough for EOF and generic errors */
    } else {
        if (!lr->chunk.len)
            goto end; /* EOF */

        lr->in.read += lr->chunk.len;
        *size = lr->chunk.len;
        return lr->chunk.buf;
    }

end:
    lua_reader_free(lr);
    *size = 0;
    return NULL;
}

static int sp_lua_write_internal(TXLuaContext *s, const char *path,
                                 uint8_t **dst, size_t *len,
                                 int compress, int strip, int base64)
{
    int ret = 0, ret_tmp;
    struct SPLuaWriterContext lw = { 0 };
    lua_State *L = sp_lua_lock_interface(s);

    if (!lua_isfunction(L, -1)) {
        sp_log(s, SP_LOG_ERROR, "Error writing Lua code, expected a \"function\", got \"%s\"!\n",
               lua_typename(L, lua_type(L, -1)));
        ret = AVERROR(EINVAL);
        goto end;
    }

    lw.lctx = s;
    lw.is_compressed = compress;

    if (path) {
        size_t path_len = strlen(path);
        if (path_len > 3 && !compress)
            lw.is_compressed = !strcmp(&path[path_len - 3], ".gz");

        lw.out.path = path;
        lw.out.file = av_fopen_utf8(path, "w+");
        if (!lw.out.file) {
            sp_log(s, SP_LOG_ERROR, "File \"%s\" not found!\n", path);
            ret = AVERROR(EINVAL);
            goto end;
        }
    }

    if (lw.is_compressed) {
        lw.out.alloc = CHUNK_SIZE;
        lw.out.data = av_malloc(lw.out.alloc);
        if (!lw.out.data) {
            ret = AVERROR(ENOMEM);
            goto end;
        }

        ret = deflateInit2(&lw.zlib.ctx, Z_BEST_COMPRESSION, Z_DEFLATED,
                           MAX_WBITS | 16, 9, Z_DEFAULT_STRATEGY);
        if (ret == Z_MEM_ERROR) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        if (ret != Z_OK) {
            sp_log(s, SP_LOG_ERROR, "Unable to initialize zlib: %s!\n",
                   lw.zlib.ctx.msg ? lw.zlib.ctx.msg : "generic error");
            ret = AVERROR_EXTERNAL;
            goto end;
        }
    }

    ret = lua_dump(L, lua_write_fn, &lw, strip);
    if (ret < 0)
        sp_log(s, SP_LOG_ERROR, "Error loading Lua code: %s\n", av_err2str(ret));

end:
    ret_tmp = lua_writer_end(&lw, dst, len);
    if (ret >= 0) {
        ret = ret_tmp;
        if (base64) {
            size_t b64_len = AV_BASE64_SIZE(*len);
            uint8_t *b64_buf = av_mallocz(b64_len);
            if (!b64_buf) {
                av_freep(dst);
                *len = 0;
                ret = AVERROR(ENOMEM);
            } else {
                char *b64_ret = av_base64_encode(b64_buf, b64_len, *dst, *len);
                av_freep(dst);
                *len = 0;

                if (!b64_ret) {
                    av_free(b64_buf);
                    ret = AVERROR(EINVAL);
                } else {
                    *dst = b64_buf;
                    *len = ret = b64_len;
                }
            }
        }
    }

    return sp_lua_unlock_interface(s, ret);
}

int sp_lua_write_file(TXLuaContext *s, const char *path)
{
    return sp_lua_write_internal(s, path, NULL, NULL, 0, 0, 0);
}

int sp_lua_write_chunk(TXLuaContext *s, uint8_t **data, size_t *len,
                       int gzip, int strip, int base64)
{
    return sp_lua_write_internal(s, NULL, data, len, gzip, strip, base64);
}

int sp_lua_load_chunk(TXLuaContext *s, const uint8_t *in, size_t len)
{
    if (!len)
        return 0;

    int is_base64 = 0;
    if (in[len - 1] == '\0') {
        is_base64 = av_base64_decode(NULL, (const char *)in, 0) >= 0;

        if (is_base64) {
            len = AV_BASE64_DECODE_SIZE(len);
            uint8_t *dec_in = av_mallocz(len);

            if (!dec_in)
                return AVERROR(ENOMEM);

            av_base64_decode(dec_in, (const char *)in, len);

            in = dec_in;
        }
    }

    struct SPLuaReaderContext lr = { 0 };
    lr.lctx = s;
    lr.in.buf = in;
    lr.in.len = len;

    lua_State *L = sp_lua_lock_interface(s);

    int ret = lua_load(L, lua_read_fn, &lr, "chunk loader", "bt");
    if (is_base64)
        av_free((uint8_t *)in);

    if (lr.err) {
        ret = lr.err;
    } else if (ret) {
        sp_log(s, SP_LOG_ERROR, "Error loading Lua code: %s\n", lua_tostring(L, -1));
        ret = AVERROR_EXTERNAL;
    }
    lua_reader_free(&lr);

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

    lctx->nb_threads = av_mallocz(sizeof(*lctx->nb_threads));
    if (!lctx->nb_threads) {
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
    if ((err = sp_lua_load_chunk(lctx, utils_lua_bin, utils_lua_bin_len)))
        goto fail;

    /* Run the utils script */
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        sp_log(lctx, SP_LOG_ERROR, "Lua script error: %s\n", lua_tostring(L, -1));
        err = AVERROR(EINVAL);
        goto fail;
    }

    *s = lctx;

    return 0;

fail:
    pthread_mutex_destroy(lctx->lock);
    av_free(lctx->lock);
    av_free(lctx->nb_threads);
    lua_close(L);
    sp_class_free(lctx);
    av_free(lctx);

    return err;
}

TXLuaContext *sp_lua_create_thread(TXLuaContext *lctx, void *parent_ctx)
{
    TXLuaContext *new = NULL;

    if (!lctx)
        return NULL;

    lua_State *L = sp_lua_lock_interface(lctx);

    new = av_mallocz(sizeof(*new));
    if (!new)
        goto end;

    memcpy(new, lctx, sizeof(*new));
    new->master_thread = lctx->master_thread ? lctx->master_thread : lctx;

    int err = sp_class_alloc(new, "lua", SP_TYPE_SCRIPT, parent_ctx);
    if (err < 0) {
        av_free(new);
        goto end;
    }

    new->L = lua_newthread(L);
    new->thread_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    *new->nb_threads += 1;

end:
    sp_lua_unlock_interface(lctx, 0);
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
    struct SPLuaReaderContext lr = { 0 };
    lr.lctx = lctx;
    lr.in.path = script_name;

    lr.in.file = av_fopen_utf8(script_name, "r");
    if (!lr.in.file) {
        sp_log(lctx, SP_LOG_ERROR, "File \"%s\" not found!\n", script_name);
        return AVERROR(EINVAL);
    }

    fseek(lr.in.file, 0, SEEK_END);
    lr.in.len = ftell(lr.in.file);
    rewind(lr.in.file);

    lua_State *L = sp_lua_lock_interface(lctx);

    int ret = lua_load(L, lua_read_fn, &lr, "file loader", "bt");
    if (lr.err) {
        ret = lr.err;
    } else if (ret) {
        sp_log(lctx, SP_LOG_ERROR, "Error loading Lua code: %s\n", lua_tostring(L, -1));
        ret = AVERROR_EXTERNAL;
    }

    lua_reader_free(&lr);

    return sp_lua_unlock_interface(lctx, ret);
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

int sp_lua_run_generic_yieldable(TXLuaContext *lctx, int nb_args, int clean_stack,
                                 int ctx_is_locked)
{
    int ret, nresults = 0;

    lua_State *L = ctx_is_locked ? lctx->L : sp_lua_lock_interface(lctx);

    sp_log(lctx, SP_LOG_DEBUG, "Lua thread started.\n");

    do {
        AVBufferRef *event_await = NULL;

        lua_pop(L, nresults);

        ret = lua_resume(L, NULL, nb_args, &nresults);
        if (ret != LUA_YIELD && ret != LUA_OK) {
            sp_log(lctx, SP_LOG_ERROR, "Lua script error: %s\n", lua_tostring(L, -1));
            ret = AVERROR_EXTERNAL;
            goto end;
        }

        sp_log(lctx, SP_LOG_DEBUG, "Lua thread %s!\n",
               ret == LUA_OK ? "exited" : "yielded");

        if (nresults && lua_islightuserdata(L, -1))
            event_await = lua_touserdata(L, -1);

        if (nresults && lua_isfunction(L, -1)) {
            lua_CFunction cfn = lua_tocfunction(L, -1);
            if (cfn == sp_lua_quit) {
                ret = AVERROR_EXIT;
                goto end;
            }
        }

        if (ret == LUA_YIELD || event_await) {
            /* Unlock */
            sp_lua_unlock_interface(lctx, 0);

            char *fstr = sp_event_flags_to_str_buf(event_await);
            sp_log(lctx, SP_LOG_DEBUG, "Thread waiting on an event signal (id:%lu %s)...\n",
                   sp_event_get_id(event_await), fstr);
            av_free(fstr);

            /* Await event, if any */
            sp_event_unref_await(&event_await);

            sp_log(lctx, SP_LOG_DEBUG, "Event signalled, %s\n",
                   ret == LUA_YIELD ? "resuming!" : "exiting.");

            /* Lock again */
            sp_lua_lock_interface(lctx);
        }
    } while (ret == LUA_YIELD);

    ret = 0;

end:
    if (ret < 0)
        sp_log(lctx, SP_LOG_ERROR, "Lua thread ended with error: %s!\n", av_err2str(ret));
    else
        sp_log(lctx, SP_LOG_DEBUG, "Lua thread ended\n");

    if (clean_stack) {
        lua_pop(L, nresults);
        nresults = 0;
    }

    /* Unlock if needed */
    if (!ctx_is_locked)
        sp_lua_unlock_interface(lctx, 0);

    return ret >= 0 ? nresults : ret;
}
