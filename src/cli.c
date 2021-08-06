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

#include <signal.h>
#include <pthread.h>

#include <editline/readline.h>

#include "cli.h"
#include "logging.h"

struct TXCLIContext {
    SPClass *class;

    TXMainContext *main_ctx;
    TXLuaContext *lua;

    atomic_int has_event;
    atomic_int move_newline;
    atomic_int selfquit;

    SPBufferList *events;

    pthread_t thread;
};

_Thread_local TXCLIContext *cli_ctx_tls_ptr = NULL;

static const char *built_in_commands[] = {
    "quit",
    "load",
    "dump",
    "stats",
    "require",
    "help",
    "loglevel",
    "logfile",
    "info",
    "run_gc",
    "clear",
    NULL,
};

static const char *lua_ctx_default_blacklist[] = {
    "dofile",
    "type",
    "next",
    "getmetatable",
    "warn",
    "coroutine",
    "tonumber",
    "rawset",
    "rawequal",
    "error",
    "math",
    "pairs",
    "utf8",
    "tostring",
    "select",
    "_VERSION",
    "load",
    "pcall",
    "loadfile",
    "string",
    "collectgarbage",
    "base",
    "assert",
    "rawget",
    "setmetatable",
    "table",
    "print",
    "rawlen",
    "debug",
    "xpcall",
    "_G",
    "ipairs",
};

static char **input_completion(const char *line, int start, int end)
{
    TXCLIContext *cli_ctx = cli_ctx_tls_ptr;
    char **matches = NULL;
    int num_matches = 0;

    int line_len = strlen(line);
    int accept_all = !line || !line_len;

    int cnt = 0;
    const char *name;
    int name_len;

    if (!start) {
        while ((name = built_in_commands[cnt++])) {
            name_len = strlen(name);
            if (accept_all || !strncmp(name, line, SPMIN(name_len, line_len))) {
                matches = realloc(matches, sizeof(*matches) * (num_matches + 1));
                matches[num_matches++] = strdup(name);
            }
        }
    }

    lua_State *L = sp_lua_lock_interface(cli_ctx->lua);

    lua_pushglobaltable(L);
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        name = lua_tostring(L, -2);
        name_len = strlen(name);
        if (accept_all) {
            int i;
            for (i = 0; i < SP_ARRAY_ELEMS(lua_ctx_default_blacklist); i++)
                if (!strcmp(lua_ctx_default_blacklist[i], name))
                    break;
            if (i == SP_ARRAY_ELEMS(lua_ctx_default_blacklist)) {
                matches = realloc(matches, sizeof(*matches) * (num_matches + 1));
                matches[num_matches++] = strdup(name);
            }
        } else if (!strncmp(name, line, SPMIN(name_len, line_len))) {
            matches = realloc(matches, sizeof(*matches) * (num_matches + 1));
            matches[num_matches++] = strdup(name);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    sp_lua_unlock_interface(cli_ctx->lua, 0);

    if (matches) {
        /* libedit wants two empty NULLs if there's a single match and start
         * is not equal to 0. The fuck? So we just always append 2 NULLs */
        matches = realloc(matches, sizeof(*matches) * (num_matches + 2));
        matches[num_matches + 0] = NULL;
        matches[num_matches + 1] = NULL;
    }

    return matches;
}

static void prompt_newline_cb(void *s, int newline_end)
{
    TXCLIContext *cli_ctx = s;

    if (newline_end && atomic_load(&cli_ctx->move_newline))
        rl_forced_update_display();
    else
        printf("\033[2K\r");
}

static void *cli_thread_fn(void *arg)
{
    int ret;
    TXCLIContext *cli_ctx = arg;
    TXMainContext *ctx = cli_ctx->main_ctx;
    cli_ctx_tls_ptr = cli_ctx;

    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    sp_set_thread_name_self("cli");

    const char *name;
    char *line = NULL, *line_mod = NULL;
    char *prompt = LUA_PUB_PREFIX " > ";

    sp_log_set_prompt_callback(cli_ctx, prompt_newline_cb);

    rl_readline_name = PROJECT_NAME;
    rl_attempted_completion_function = input_completion;
    rl_initialize();
    rl_prep_terminal(1);

    while ((line = readline(prompt))) {
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
        atomic_store(&cli_ctx->move_newline, 0);

        if (atomic_load(&cli_ctx->has_event)) {
            ; /* Falls through */
        } else if (!strlen(line)) {
            atomic_store(&cli_ctx->move_newline, 1);
            free(line);
            pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
            continue;
        } else if (!strncmp(line, "loglevel", strlen("loglevel"))) {
            line_mod = av_strdup(line);
            char *save, *token = av_strtok(line_mod, " ", &save);
            token = av_strtok(NULL, " ", &save); /* Skip "loglevel" */

            if (!token) {
                sp_log(cli_ctx, SP_LOG_ERROR, "Missing loglevel, syntax "
                       "is loglevel \"component (optional)\" \"level\"!\n");
                goto line_end_nolock;
            }

            char *lvl = av_strtok(NULL, " ", &save);
            if (!lvl) {
                lvl = token;
                token = "global";
            }

            ret = sp_log_set_ctx_lvl_str(token, lvl);
            if (ret < 0)
                sp_log(cli_ctx, SP_LOG_ERROR, "Invalid loglevel \"%s\", syntax "
                       "is loglevel \"component (optional)\" \"level\"!\n", lvl);

            goto line_end_nolock;
        } else if (!strncmp(line, "logfile", strlen("logfile"))) {
            line_mod = av_strdup(line);
            char *save, *token = av_strtok(line_mod, " ", &save);
            token = av_strtok(NULL, " ", &save); /* Skip "logfile" */

            if (!token) {
                sp_log(cli_ctx, SP_LOG_ERROR, "Missing logfile path!\n");
            } else {
                ret = sp_log_set_file(token);
                if (ret < 0)
                    sp_log(cli_ctx, SP_LOG_ERROR, "Unable to set logfile to \"%s\": %s!\n",
                           token, av_err2str(ret));
            }

            goto line_end_nolock;
        } else if (!strcmp("clear", line)) {
            sp_log_sync("\033[2J");
            goto line_end_nolock;
        } else if (!strcmp("quit", line) || !strcmp("exit", line)) {
            atomic_store(&cli_ctx->move_newline, 1);
            free(line);
            pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
            break;
        }

        lua_State *L = sp_lua_lock_interface(cli_ctx->lua);

        if (atomic_load(&cli_ctx->has_event)) {
            SPGenericData inp[] = { D_TYPE("input", NULL, line), { 0 } };
            sp_eventlist_dispatch(cli_ctx, cli_ctx->events, SP_EVENT_ON_DESTROY, &inp);
            atomic_store(&cli_ctx->has_event, 0);
            goto line_end;
        } else if (!strcmp("help", line)) {
            sp_log_sync("Lua globals (\"help all\" to list all):\n");
            line_mod = av_strdup(line);
            char *save, *token = av_strtok(line_mod, " ", &save);
            token = av_strtok(NULL, " ", &save); /* Skip "help" */
            int list_all = token ? !strcmp(token, "all") : 0;

            lua_pushglobaltable(L);
            lua_pushnil(L);
            while (lua_next(L, -2)) {
                name = lua_tostring(L, -2);

                int i = 0;
                if (!list_all) {
                    for (; i < SP_ARRAY_ELEMS(lua_ctx_default_blacklist); i++)
                        if (!strcmp(lua_ctx_default_blacklist[i], name))
                            break;
                }

                if (list_all || i == SP_ARRAY_ELEMS(lua_ctx_default_blacklist))
                    sp_log_sync("    %s\n", name);

                lua_pop(L, 1);
            }
            lua_pop(L, 1);

            sp_log_sync("Built-in functions:\n");
            int cnt = 0;
            while ((name = built_in_commands[cnt++]))
                sp_log_sync("    %s\n", name);

            goto line_end;
        } else if (!strncmp(line, "load", strlen("load"))) {
            char *script_name = NULL;
            char *script_entrypoint = NULL;

            int num_arguments = 0;
            line_mod = av_strdup(line);
            char *save, *token = av_strtok(line_mod, " ", &save);
            token = av_strtok(NULL, " ", &save); /* Skip "load" */

            while (token) {
                if (!script_name) {
                    script_name = token;
                    if (sp_lua_load_file(cli_ctx->lua, script_name) < 0)
                        break;
                    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                        sp_log(cli_ctx, SP_LOG_ERROR, "Lua script error: %s\n",
                               lua_tostring(L, -1));
                        break;
                    }
                } else if (!script_entrypoint) {
                    script_entrypoint = token;
                    lua_getglobal(L, script_entrypoint);
                    if (!lua_isfunction(L, -1)) {
                        sp_log(cli_ctx, SP_LOG_ERROR, "Entrypoint \"%s\" not found!\n",
                               script_entrypoint);
                        break;
                    }
                } else {
                    lua_pushstring(L, token);
                    num_arguments++;
                }
                token = av_strtok(NULL, " ", &save);
            }

            /* No error encountered */
            if (!script_name) {
                sp_log(cli_ctx, SP_LOG_ERROR, "Missing path for \"load\"!\n");
            } else if (!token) {
                if (script_entrypoint) {
                    if (lua_pcall(L, num_arguments, 0, 0) != LUA_OK) {
                        sp_log(cli_ctx, SP_LOG_ERROR, "Error running \"%s\": %s\n",
                               script_entrypoint, lua_tostring(L, -1));
                    }
                }
            }

            goto line_end;
        } else if (!strncmp(line, "dump", strlen("dump"))) {
            line_mod = av_strdup(line);
            char *save, *fn_name = av_strtok(line_mod, " ", &save);
            fn_name = av_strtok(NULL, " ", &save); /* Skip "load" */

            if (!fn_name) {
                sp_log(cli_ctx, SP_LOG_ERROR, "Missing path for \"dump\", "
                       "syntax is dump <function> <path>!\n");
                goto line_end;
            }

            char *path  = av_strtok(NULL, " ", &save);
            if (!path) {
                sp_log(cli_ctx, SP_LOG_ERROR, "Missing path for \"dump\", "
                       "syntax is dump <function> <path>!\n");
                goto line_end;
            }

            lua_getglobal(L, fn_name);

            ret = sp_lua_write_file(cli_ctx->lua, path);
            if (ret >= 0)
                sp_log(cli_ctx, SP_LOG_INFO, "Lua function \"%s\" successfully written to \"%s\", "
                       "length: %i bytes\n", fn_name, path, ret);

            goto line_end;
        } else if (!strcmp("stats", line)) {
            size_t mem_used = 1024*lua_gc(L, LUA_GCCOUNT) + lua_gc(L, LUA_GCCOUNTB);
            double mem_used_f;
            const char *mem_used_suffix;
            if (mem_used >= 1048576) {
                mem_used_f = (double)mem_used / 1048576.0;
                mem_used_suffix = "MiB";
            } else if (mem_used >= 1024) {
                mem_used_f = (double)mem_used / 1024.0;
                mem_used_suffix = "KiB";
            } else {
                mem_used_f = (double)mem_used;
                mem_used_suffix = "B";
            }

            sp_log(cli_ctx, SP_LOG_INFO, "Lua memory used:     %.2f %s\n",
                   mem_used_f, mem_used_suffix);
            sp_log(cli_ctx, SP_LOG_INFO, "Lua contexts: %i\n",
                   sp_bufferlist_len(cli_ctx->main_ctx->ext_buf_refs));
            sp_log(cli_ctx, SP_LOG_INFO, "Pending commands: %i\n",
                   sp_bufferlist_len(ctx->commit_list));
            goto line_end;
        } else if (!strncmp(line, "require", strlen("require"))) {
            line_mod = av_strdup(line);
            char *save, *token = av_strtok(line_mod, " ", &save);
            token = av_strtok(NULL, " ,", &save); /* Skip "require" */

            if (!token) {
                sp_log(cli_ctx, SP_LOG_ERROR, "Missing library name(s) for \"require\"!\n");
                goto line_end;
            }

            while (token) {
                if ((ret = sp_lua_load_library(cli_ctx->lua, token)) < 0) {
                    sp_log(cli_ctx, SP_LOG_ERROR, "Error loading library \"%s\": %s!\n",
                           token, av_err2str(ret));
                    goto line_end;
                }
                token = av_strtok(NULL, " ,", &save);
            }

            goto line_end;
        } else if (!strncmp(line, "info", strlen("info"))) {
            line_mod = av_strdup(line);
            char *save, *token = av_strtok(line_mod, " ", &save);
            token = av_strtok(NULL, " ", &save); /* Skip "info" */
            if (!token) {
                sp_log_sync("Specify command/object to query\n");
                goto line_end;
            }

            int cnt = 0;
            while ((name = built_in_commands[cnt++])) {
                if (!strcmp(name, token)) {
                    sp_log_sync("\"%s\": built-in command\n", token);
                    goto line_end;
                }
            }

            lua_pushglobaltable(L);
            lua_pushnil(L);
            while (lua_next(L, -2)) {
                name = lua_tostring(L, -2);
                if (!strcmp(name, token)) {
                    sp_log_sync("\"%s\": lua %s%s\n",
                                token, lua_typename(L, lua_type(L, -1)),
                                (lua_type(L, -1) != LUA_TTABLE &&
                                 lua_type(L, -1) != LUA_TSTRING) ? "" :
                                ", contents:");

                    if (lua_type(L, -1) == LUA_TTABLE) {
                        lua_pushnil(L);
                        while (lua_next(L, -2)) {
                            const char *fname;

                            /* If the "key" is a number, we have a keyless table */
                            if (lua_isnumber(L, -2))
                                fname = lua_tostring(L, -1);
                            else
                                fname = lua_tostring(L, -2);

                            const char *type = lua_typename(L, lua_type(L, -1));
                            if (fname)
                                sp_log_sync("    %s.%s: %s\n", name, fname, type);
                            else
                                sp_log_sync("    %s\n", type);

                            lua_pop(L, 1);
                        }
                        lua_pop(L, 1);
                    } else if (lua_type(L, -1) == LUA_TSTRING) {
                        sp_log_sync("    %s\n", lua_tostring(L, -1));
                    }

                    lua_pop(L, 2);
                    goto line_end;
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 1);

            sp_log_sync("No info for \"%s\"\n", token);
            goto line_end;
        } else if (!strncmp(line, "run_gc", strlen("run_gc"))) {
            size_t mem_used = 1024*lua_gc(L, LUA_GCCOUNT) + lua_gc(L, LUA_GCCOUNTB);
            lua_gc(L, LUA_GCCOLLECT, 0);
            mem_used -= 1024*lua_gc(L, LUA_GCCOUNT) + lua_gc(L, LUA_GCCOUNTB);
            double mem_used_f;
            const char *mem_used_suffix;
            if (mem_used >= 1048576) {
                mem_used_f = (double)mem_used / 1048576.0;
                mem_used_suffix = "MiB";
            } else if (mem_used >= 1024) {
                mem_used_f = (double)mem_used / 1024.0;
                mem_used_suffix = "KiB";
            }

            if (mem_used >= 1024) {
                sp_log(cli_ctx, SP_LOG_INFO, "Lua memory freed: %.2f %s\n",
                       mem_used_f, mem_used_suffix);
            } else {
                sp_log(cli_ctx, SP_LOG_INFO, "Lua memory freed: %li B\n",
                       mem_used);
            }

            goto line_end;
        }

        ret = luaL_dostring(L, line);
        int items = lua_gettop(L);
        if (items && lua_isstring(L, -1))
            sp_log(cli_ctx->lua, ret == LUA_OK ? SP_LOG_INFO : SP_LOG_ERROR,
                   "%s\n", lua_tostring(L, -1));
        lua_pop(L, items);

line_end:
        sp_lua_unlock_interface(cli_ctx->lua, 0);
line_end_nolock:
        add_history(line);
        atomic_store(&cli_ctx->move_newline, 1);
        av_freep(&line_mod);
        free(line);
        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    }

    if (!line)
        sp_log_sync("\n"); /* Handles Ctrl+D */

    atomic_store(&cli_ctx->selfquit, 1);

    raise(SIGINT);

    return NULL;
}

int sp_cli_init(TXCLIContext **s, TXMainContext *ctx)
{
    TXCLIContext *cli_ctx = av_mallocz(sizeof(*cli_ctx));

    int err = sp_class_alloc(cli_ctx, "cli", SP_TYPE_SCRIPT, ctx);
    if (err < 0)
        return err;

    cli_ctx->lua = sp_lua_create_thread(ctx->lua);
    if (!cli_ctx->lua) {
        sp_class_free(&cli_ctx);
        return AVERROR(ENOMEM);
    }

    cli_ctx->events = sp_bufferlist_new();
    if (!cli_ctx->events) {
        sp_lua_close_ctx(&cli_ctx->lua);
        sp_class_free(&cli_ctx);
        return AVERROR(ENOMEM);
    }

    cli_ctx->main_ctx = ctx;
    cli_ctx->has_event = ATOMIC_VAR_INIT(0);
    cli_ctx->move_newline = ATOMIC_VAR_INIT(1);
    cli_ctx->selfquit = ATOMIC_VAR_INIT(0);

    pthread_create(&cli_ctx->thread, NULL, cli_thread_fn, cli_ctx);

    *s = cli_ctx;

    return 0;
}

int sp_cli_prompt_event(TXCLIContext *cli_ctx, AVBufferRef *event, const char *msg)
{
    if (!cli_ctx)
        return 0;

    if (!cli_ctx->class || atomic_load(&cli_ctx->has_event))
        return AVERROR(EINVAL);

    sp_eventlist_add(cli_ctx, cli_ctx->events, event);
    sp_log(cli_ctx, SP_LOG_INFO, "%s\n", msg);
    atomic_store(&cli_ctx->has_event, 1);

    return 0;
}

void sp_cli_uninit(TXCLIContext **s)
{
    if (!s || !(*s))
        return;

    TXCLIContext *cli_ctx = *s;

    if (!atomic_load(&cli_ctx->selfquit))
        pthread_cancel(cli_ctx->thread);

    pthread_join(cli_ctx->thread, NULL);

    rl_deprep_terminal();
    sp_log_set_prompt_callback(NULL, NULL);

    if (!atomic_load(&cli_ctx->selfquit))
        sp_log_sync("\n");

    sp_bufferlist_free(&cli_ctx->events);

    sp_lua_close_ctx(&cli_ctx->lua);

    sp_class_free(cli_ctx);
    av_freep(s);
}
