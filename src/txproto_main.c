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

/* Needed to print the version of each library */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libswresample/swresample.h>

#include "lua_api.h"
#include "iosys_common.h"
#include "txproto_main.h"

#ifdef HAVE_LIBEDIT
#include "cli.h"
#endif

/* Built-in script */
#include "default.lua.bin.h"

#define DEFAULT_ENTRYPOINT "main"

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

static void print_version_features(TXMainContext *ctx)
{
    sp_log(ctx, SP_LOG_INFO, "Starting %s %s (%s, built with %s)\n",
           PROJECT_NAME, PROJECT_VERSION_STRING, vcstag, COMPILER);

    sp_log(ctx, SP_LOG_INFO, "Features: %s\n", FEATURE_SET);
}

static void print_ff_libs(TXMainContext *ctx)
{
    struct lib {
        const char *name;
        unsigned buildv;
        unsigned runv;
    } const libs[] = {
        {"libavutil",     LIBAVUTIL_VERSION_INT,     avutil_version()     },
        {"libavcodec",    LIBAVCODEC_VERSION_INT,    avcodec_version()    },
        {"libavformat",   LIBAVFORMAT_VERSION_INT,   avformat_version()   },
        {"libavfilter",   LIBAVFILTER_VERSION_INT,   avfilter_version()   },
        {"libswresample", LIBSWRESAMPLE_VERSION_INT, swresample_version() },
    };

    sp_log(ctx, SP_LOG_INFO | SP_LOG_LIST, "FFmpeg library versions:\n");

#define VER(x) (x) >> 16, (x) >> 8 & 255, (x) & 255
    for (int i = 0; i < SP_ARRAY_ELEMS(libs); i++) {
        const struct lib *l = &libs[i];
        enum SPLogLevel lvl = i == SP_ARRAY_ELEMS(libs) - 1 ? SP_LOG_LIST_END : SP_LOG_LIST;
        sp_log(ctx, SP_LOG_INFO | lvl, "   %-15s %d.%d.%d", l->name, VER(l->buildv));
        if (l->buildv != l->runv)
            sp_log(ctx, SP_LOG_INFO | lvl, " (runtime %d.%d.%d)", VER(l->runv));
        sp_log(ctx, SP_LOG_INFO | lvl, "\n");
    }
#undef VER
}

static void cleanup_fn(TXMainContext *ctx)
{
    /* Discard queued events */
    sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_DISCARD, NULL);

    /* Free lists that may carry contexts around */
    sp_bufferlist_free(&ctx->events);

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
    sp_log_uninit();

    /* Free any auxiliary data */
    sp_class_free(ctx);
    av_free(ctx);
}

int main(int argc, char *argv[])
{
    int ret, err = 0, print_quit = 0;
    TXMainContext *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return AVERROR(ENOMEM);

    if ((err = sp_log_init(SP_LOG_INFO)) < 0)
        return err;

    if ((err = sp_class_alloc(ctx, "tx", SP_TYPE_NONE, NULL)) < 0) {
        sp_log_uninit();
        av_free(ctx);
        return err;
    }

    ctx->events = sp_bufferlist_new();
    ctx->ext_buf_refs = sp_bufferlist_new();
    ctx->epoch_value = ATOMIC_VAR_INIT(0);
    ctx->source_update_cb_ref = LUA_NOREF;

    /* Options */
    int enable_cli = 0;
    int enable_json_stdout_log = -1;
    int enable_json_file_log = -1;
    const char *script_name = NULL;
    const char *script_entrypoint = DEFAULT_ENTRYPOINT;

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
    while ((opt = getopt(argc, argv, "hvCs:e:J:r:V:L:")) != -1) {
        switch (opt) {
        case 's':
            script_name = optarg;
            break;
        case 'e':
            script_entrypoint = optarg;
            break;
        case 'v':
            print_version_features(ctx);
            goto end;
        case 'C':
            enable_cli = 1;
            break;
        case 'J':
            if (!strcmp(optarg, "file")) {
                enable_json_file_log = 1;
                enable_json_stdout_log = 0;
            } else if (!strcmp(optarg, "stdout")) {
                enable_json_file_log = 0;
                enable_json_stdout_log = 1;
            } else if (!strcmp(optarg, "both")) {
                enable_json_file_log = 1;
                enable_json_stdout_log = 1;
            } else if (!strcmp(optarg, "none")) {
                enable_json_file_log = 0;
                enable_json_stdout_log = 0;
            } else {
                sp_log(ctx, SP_LOG_ERROR, "Invalid JSON log output setting \"%s\", "
                       "valid syntax is \"file\", \"stdout\", \"both\" or \"none\"!\n", optarg);
                err = AVERROR(EINVAL);
                goto end;
            }
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
                ret = sp_log_set_file(optarg);
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
            sp_log(ctx, SP_LOG_INFO | SP_LOG_LIST, "Usage info:\n");
            sp_log(ctx, SP_LOG_INFO | SP_LOG_LIST, "    -s <filename>                 ");
            sp_log(ctx, SP_LOG_INFO | SP_LOG_LIST, "External Lua script name to load\n");
            sp_log(ctx, SP_LOG_INFO | SP_LOG_LIST, "    -e <entrypoint>               ");
            sp_log(ctx, SP_LOG_INFO | SP_LOG_LIST, "Entrypoint to call into the custom script (default: \"" DEFAULT_ENTRYPOINT "\")\n");
            sp_log(ctx, SP_LOG_INFO | SP_LOG_LIST, "    -r <string1>,<string2>        ");
            sp_log(ctx, SP_LOG_INFO | SP_LOG_LIST, "Additional comma-separated Lua libraries/packages to load\n");
            sp_log(ctx, SP_LOG_INFO | SP_LOG_LIST, "    -V <component>=<level>,...    ");
            sp_log(ctx, SP_LOG_INFO | SP_LOG_LIST, "Per-component log level, set \"global\" or leave component out for global\n");
            sp_log(ctx, SP_LOG_INFO | SP_LOG_LIST, "    -L <filename>                 ");
            sp_log(ctx, SP_LOG_INFO | SP_LOG_LIST, "Logfile destination (warning: produces huge files)\n");
            sp_log(ctx, SP_LOG_INFO | SP_LOG_LIST, "    -C                            ");
            sp_log(ctx, SP_LOG_INFO | SP_LOG_LIST, "Enable the command line interface\n");
            sp_log(ctx, SP_LOG_INFO | SP_LOG_LIST, "    -v                            ");
            sp_log(ctx, SP_LOG_INFO | SP_LOG_LIST, "Print program version\n");
            sp_log(ctx, SP_LOG_INFO | SP_LOG_LIST, "    -h                            ");
            sp_log(ctx, SP_LOG_INFO | SP_LOG_LIST, "Usage help (this)\n");
            sp_log(ctx, SP_LOG_INFO | SP_LOG_LIST, "    <trailing arguments>          ");
            sp_log(ctx, SP_LOG_INFO | SP_LOG_LIST_END, "Given directly to the script's entrypoint to interpret\n");
            goto end;
        }
    }

    if (strcmp(script_entrypoint, DEFAULT_ENTRYPOINT) && !script_name) {
        sp_log(ctx, SP_LOG_ERROR, "Internal scripts must use the default entrypoint!\n");
        err = AVERROR(EINVAL);
        goto end;
    }

    sp_log_set_json_out(enable_json_file_log, enable_json_stdout_log);

    print_version_features(ctx);
    print_ff_libs(ctx);

    print_quit = 1;

    /* Setup signal handlers */
    int quit = setjmp(quit_loc);
    if (quit)
        goto end;

    if (signal(SIGINT, on_quit_signal) == SIG_ERR) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to install signal handler: %s!\n",
               av_err2str(AVERROR(errno)));
        return AVERROR(EINVAL);
    }

    /* Create Lua context */
    err = sp_lua_create_ctx(&ctx->lua, ctx, lua_libs_list);
    av_freep(&lua_libs_list);
    if (err < 0)
        goto end;

    /* Load the Lua API into the context */
    err = sp_lua_load_main_api(ctx->lua, ctx);
    if (err < 0)
        goto end;

    /* Load the initial script */
    if (script_name) {
        if ((err = sp_lua_load_file(ctx->lua, script_name)))
            goto end;
    } else {
        if ((err = sp_lua_load_chunk(ctx->lua, default_lua_bin,
                                     default_lua_bin_len)) < 0)
            goto end;

        script_entrypoint = DEFAULT_ENTRYPOINT;
    }

#ifdef HAVE_LIBEDIT
    if (enable_cli && (err = sp_cli_init(&ctx->cli, ctx) < 0))
        goto end;
#endif

    /* Run the script's root */
    if ((err = sp_lua_run_generic_yieldable(ctx->lua, 0, 1, 0)) < 0)
        goto end;

    /* Run script entrypoint if specified */
    if (script_entrypoint && strlen(script_entrypoint)) {
        lua_State *L = sp_lua_lock_interface(ctx->lua);

        lua_getglobal(L, script_entrypoint);
        if (!lua_isfunction(L, -1)) {
            sp_log(ctx, SP_LOG_ERROR, "Entrypoint \"%s\" not found!\n",
                   script_entrypoint);
            err = sp_lua_unlock_interface(ctx->lua, AVERROR(ENOENT));
            goto end;
        }

        int args = argc - optind;
        for(; optind < argc; optind++)
            lua_pushstring(L, argv[optind]);

        if ((err = sp_lua_run_generic_yieldable(ctx->lua, args, 1, 1)) < 0)
            goto end;

        sp_lua_unlock_interface(ctx->lua, 0);
    }

    /* Print timestamps in logs */
    sp_log_print_ts(1);

    /* We weren't told to exit or anything, so...
     * In the future, maybe we should put an event loop here to process
     * periodic events */
    while (1)
        av_usleep(UINT_MAX);

end:
    av_freep(&lua_libs_list);

#ifdef HAVE_LIBEDIT
    sp_cli_uninit(&ctx->cli);
#endif

    sp_log_set_status(NULL, SP_STATUS_LOCK | SP_STATUS_NO_CLEAR);

    err = ctx->lua_exit_code ? ctx->lua_exit_code : err;
    if (print_quit)
        sp_log(ctx, SP_LOG_INFO, "Quitting%s!\n",
               err == AVERROR(ENOMEM) ? ": out of memory" : "");

    cleanup_fn(ctx);

    return err == AVERROR_EXIT ? 0 : err;
}
