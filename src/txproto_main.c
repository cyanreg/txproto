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
#include <setjmp.h>

#include <libavutil/pixdesc.h>
#include <libavutil/avstring.h>
#include <libavutil/time.h>
#include <libavutil/bprint.h>

#include "lua_api.h"
#include "iosys_common.h"
#include "txproto_main.h"

#ifdef HAVE_LIBEDIT
#include "cli.h"
#endif

/* Built-in script */
#include "default.lua.bin.h"

jmp_buf quit_loc;
static void on_quit_signal(int signo)
{
    static int signal_received = 0;
    if (signal_received++) {
        printf("\nQuitting gracelessly!\n");
        exit(-1);
    }
    longjmp(quit_loc, signo);
}

static void cleanup_fn(TXMainContext *ctx)
{
    /* Free lists that may carry contexts around */
    sp_bufferlist_free(&ctx->commit_list);
    sp_bufferlist_free(&ctx->discard_list);

    /* Free all contexts */
    sp_bufferlist_free(&ctx->ext_buf_refs);

    /* Shut the I/O APIs off */
    if (ctx->io_api_ctx) {
        for (int i = 0; i < sp_compiled_apis_len; i++)
            if (ctx->io_api_ctx[i])
                av_buffer_unref(&ctx->io_api_ctx[i]);
        av_free(ctx->io_api_ctx);
    }

    /* Free all Lua data */
    sp_lua_close_ctx(&ctx->lua);

    /* Stop logging */
    sp_log_end();

    /* Free any auxiliary data */
    sp_class_free(ctx);
    av_free(ctx);
}

int main(int argc, char *argv[])
{
    int err = 0;
    TXMainContext *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return AVERROR(ENOMEM);

    sp_log_set_ctx_lvl("global", SP_LOG_INFO);
    sp_log_set_ff_cb();

    av_max_alloc(SIZE_MAX);
    av_log_set_level(AV_LOG_INFO);

    err = sp_class_alloc(ctx, "tx", SP_TYPE_NONE, NULL);
    if (err < 0)
        return err;

    ctx->commit_list = sp_bufferlist_new();
    ctx->discard_list = sp_bufferlist_new();
    ctx->ext_buf_refs = sp_bufferlist_new();
    ctx->epoch_value = ATOMIC_VAR_INIT(0);
    ctx->source_update_cb_ref = LUA_NOREF;

    /* Options */
    int disable_cli = 0;
    const char *script_name = NULL;
    const char *script_entrypoint = NULL;

    /* io, os and require not loaded due to security concerns */
    char *lua_libs_list = av_strdup(LUA_BASELIBNAME","
                                    LUA_COLIBNAME","
                                    LUA_TABLIBNAME","
                                    LUA_STRLIBNAME","
                                    LUA_UTF8LIBNAME","
                                    LUA_MATHLIBNAME","
                                    LUA_DBLIBNAME);

    /* Options parsing */
    int opt;
    while ((opt = getopt(argc, argv, "Nvhs:e:r:V:L:")) != -1) {
        switch (opt) {
        case 's':
            script_name = optarg;
            break;
        case 'e':
            script_entrypoint = optarg;
            break;
        case 'v':
            printf("%s %s (%s)\n", PROJECT_NAME, PROJECT_VERSION_STRING, vcstag);
            goto end;
        case 'N':
            disable_cli = 1;
            break;
        case 'r':
            {
                size_t new_len = strlen(lua_libs_list) + strlen(optarg) + strlen(",") + 1;
                char *new_str = av_malloc(new_len);
                av_strlcpy(new_str, lua_libs_list, new_len);
                av_strlcat(new_str, ",", new_len);
                av_strlcat(new_str, optarg, new_len);
                lua_libs_list = new_str;
            }
            break;
        case 'V':
            {
                int ret = 0;
                char *save, *token = av_strtok(optarg, ",", &save);
                while (token) {
                    char *val = strstr(token, "=");
                    if (val) {
                        val[0] = '\0';
                        val++;
                    } else {
                        val = token;
                        token = "global";
                    }

                    ret = sp_log_set_ctx_lvl_str(token, val);
                    if (ret < 0) {
                        sp_log(ctx, SP_LOG_ERROR, "Invalid verbose level \"%s\"!\n", val);
                        err = AVERROR(EINVAL);
                        goto end;
                    }

                    token = av_strtok(NULL, ",", &save);
                }
            }
            break;
        case 'L':
            {
                int ret = sp_log_set_file(optarg);
                if (ret < 0) {
                    sp_log(ctx, SP_LOG_ERROR, "Unable to open logfile \"%s\" for writing!\n", optarg);
                    goto end;
                }
            }
            break;
        default:
            sp_log(ctx, SP_LOG_ERROR, "Unrecognized option \'%c\'!\n", optopt);
            err = AVERROR(EINVAL);
        case 'h':
            printf("Usage info:\n"
                   "    -s <filename>                 "
                            "External Lua script name to load\n"
                   "    -e <entrypoint>               "
                            "Entrypoint to call into the custom script\n"
                   "    -r <string1>,<string2>        "
                            "Additional comma-separated Lua libraries/packages to load\n"
                   "    -V <component>=<level>,...    "
                            "Per-component log level, set \"global\" or leave component out for global\n"
                   "    -L <filename>                 "
                            "Logfile destination (warning: produces huge files)\n"
                   "    -N                            "
                            "Disable command line interface\n"
                   "    -v                            "
                            "Print program version\n"
                   "    -h                            "
                            "Usage help (this)\n"
                   "    <trailing arguments>          "
                            "Given directly to the script's entrypoint to interpret\n"
                   );
            goto end;
        }
    }

    if (script_entrypoint && !script_name) {
        sp_log(ctx, SP_LOG_ERROR, "Must specify custom script to use a custom entrypoint!\n");
        err = AVERROR(EINVAL);
        goto end;
    }

    sp_log(ctx, SP_LOG_INFO, "Starting %s %s (%s)\n",
           PROJECT_NAME, PROJECT_VERSION_STRING, vcstag);

    /* Setup signal handlers */
    int quit = setjmp(quit_loc);
    if (quit)
        goto end;

    if (signal(SIGINT, on_quit_signal) == SIG_ERR) {
        av_log(ctx, AV_LOG_ERROR, "Unable to install signal handler: %s!\n",
               av_err2str(AVERROR(errno)));
        return AVERROR(EINVAL);
    }

    /* Create Lua context */
    err = sp_lua_create_ctx(&ctx->lua, ctx, lua_libs_list);
    av_free(lua_libs_list);
    if (err < 0)
        goto end;

    /* Load the Lua API into the context */
    err = sp_lua_load_main_api(ctx->lua, ctx);
    if (err < 0)
        goto end;

    lua_State *L = sp_lua_lock_interface(ctx->lua);

    /* Run the utils script to put it into memory */
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        sp_log(ctx, SP_LOG_ERROR, "Lua script error: %s\n", lua_tostring(L, -1));
        goto end;
    }

    /* Load the initial script */
    if (script_name) {
        if ((err = sp_lua_load_file(ctx->lua, script_name)))
            goto end;
    } else {
        err = luaL_loadbufferx(L, default_lua_bin, default_lua_bin_len,
                               "built-in script", "b");
        if (err) {
            sp_log(ctx, SP_LOG_ERROR, "%s\n", lua_tostring(L, -1));
            err = AVERROR_EXTERNAL;
            goto end;
        }
        script_entrypoint = "initial_config";
    }

    /* Run the script */
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        sp_log(ctx, SP_LOG_ERROR, "Lua script error: %s\n", lua_tostring(L, -1));
        goto end;
    }

    /* Run script entrypoint if specified */
    if (script_entrypoint) {
        lua_getglobal(L, script_entrypoint);
        if (!lua_isfunction(L, -1)) {
            sp_log(ctx, SP_LOG_ERROR, "Entrypoint \"%s\" not found!\n",
                   script_entrypoint);
            goto end;
        }
        int args = argc - optind;
        for(; optind < argc; optind++)
            lua_pushstring(L, argv[optind]);
        if (lua_pcall(L, args, 0, 0) != LUA_OK) {
            sp_log(ctx, SP_LOG_ERROR, "Error running \"%s\": %s\n",
                   script_entrypoint, lua_tostring(L, -1));
            goto end;
        }
    }

    sp_lua_unlock_interface(ctx->lua, 0);

#ifdef HAVE_LIBEDIT
    if (!disable_cli && (err = sp_cli_init(&ctx->cli, ctx) < 0))
        goto end;
#endif

    av_usleep(INT_MAX);

end:
#ifdef HAVE_LIBEDIT
    sp_cli_uninit(&ctx->cli);
#endif

    err = ctx->lua_exit_code ? ctx->lua_exit_code : err;
    sp_log(ctx, SP_LOG_INFO, "Quitting%s!\n",
           err == AVERROR(ENOMEM) ? ": out of memory" : "");

    cleanup_fn(ctx);

    return err;
}
