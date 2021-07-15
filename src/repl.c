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

#include "repl.h"
#include "logging.h"

static struct REPLState {
    SPClass *class;
    TXMainContext *ctx;

    atomic_int has_event;
    atomic_int do_not_update;
    atomic_int selfquit;

    SPBufferList *events;

    pthread_t repl_thread;
} repl_state = { 0 };

static const char *built_in_commands[] = {
    "quit",
    "load",
    "stats",
    "require",
    "help",
    "loglevel",
    "info",
    "run_gc",
    NULL,
};

static char **input_completion(const char *line, int start, int end)
{
    char **matches = NULL;
    int num_matches = 0;

    int line_len = strlen(line);
    int accept_all = !line || !line_len;

    int cnt = 0;
    const char *name;
    int name_len;
    while ((name = built_in_commands[cnt++])) {
        name_len = strlen(name);
        if (accept_all || !strncmp(name, line, SPMIN(name_len, line_len))) {
            matches = realloc(matches, sizeof(*matches) * (num_matches + 1));
            matches[num_matches++] = strdup(name);
        }
    }

    TXMainContext *ctx = repl_state.ctx;
    LUA_LOCK_INTERFACE(0);
    lua_pushglobaltable(ctx->lua);
    lua_pushnil(ctx->lua);
    while (lua_next(ctx->lua, -2)) {
        name = lua_tostring(ctx->lua, -2);
        name_len = strlen(name);
        if (!av_dict_get(ctx->lua_namespace, name, NULL, 0)) {
            if (accept_all || !strncmp(name, line, SPMIN(name_len, line_len))) {
                matches = realloc(matches, sizeof(*matches) * (num_matches + 1));
                matches[num_matches++] = strdup(name);
            }
        }
        lua_pop(ctx->lua, 1);
    }
    lua_pop(ctx->lua, 1);
    pthread_mutex_unlock(&ctx->lock);

    if (matches) {
        matches = realloc(matches, sizeof(*matches) * (num_matches + 1));
        matches[num_matches++] = NULL;
    }

    return matches;
}

static void prompt_newline_cb(void *s, int newline_started)
{
    if (newline_started) {
        printf("\r");
    } else if (!atomic_load(&repl_state.do_not_update)) {
        rl_forced_update_display();
    }
}

static void *repl_thread(void *arg)
{
    int ret;
    TXMainContext *ctx = arg;
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    sp_set_thread_name_self("repl");

    /* REPL */
    const char *name;
    char *line = NULL;
    char *prompt = LUA_PUB_PREFIX " > ";

    sp_log_set_prompt_callback(ctx, prompt_newline_cb);

    rl_readline_name = PROJECT_NAME;
    rl_attempted_completion_function = input_completion;
    rl_initialize();
    rl_prep_terminal(1);

    while ((line = readline(prompt))) {
        if (atomic_load(&repl_state.has_event)) {
            pthread_mutex_unlock(&ctx->lock);
            SPGenericData inp[] = { D_TYPE("input", NULL, line), { 0 } };
            sp_eventlist_dispatch(&repl_state, repl_state.events, SP_EVENT_ON_DESTROY, &inp);
            atomic_store(&repl_state.do_not_update, 0);
            pthread_mutex_unlock(&ctx->lock);
            atomic_store(&repl_state.has_event, 0);
            free(line);
            continue;
        } else if (!strlen(line)) {
            free(line);
            continue;
        } else if (!strncmp(line, "loglevel", strlen("loglevel"))) {
            ret = 0;
            char *line_mod = av_strdup(line);
            char *save, *token = av_strtok(line_mod, " ", &save);
            token = av_strtok(NULL, " ", &save); /* Skip "loglevel" */
            char *lvl = av_strtok(NULL, " ", &save);
            if (!lvl) {
                lvl = token;
                token = "global";
            }

            ret = sp_log_set_ctx_lvl_str(token, lvl);
            if (ret < 0)
                sp_log(&repl_state, SP_LOG_ERROR, "Invalid verbose level \"%s\", syntax "
                       "is loglevel \"component (optional)\" \"level\"!\n", lvl);

            av_free(line_mod);
            add_history(line);
            free(line);
            continue;
        } else if (!strcmp("quit", line) || !strcmp("exit", line)) {
            free(line);
            break;
        }

        LUA_LOCK_INTERFACE(0);
        atomic_store(&repl_state.do_not_update, 1);

        if (!strcmp("help", line)) {
            sp_log_sync("Lua globals:\n");
            lua_pushglobaltable(ctx->lua);
            lua_pushnil(ctx->lua);
            while (lua_next(ctx->lua, -2)) {
                name = lua_tostring(ctx->lua, -2);
                if (!av_dict_get(ctx->lua_namespace, name, NULL, 0))
                    sp_log_sync("    %s\n", name);
                lua_pop(ctx->lua, 1);
            }
            lua_pop(ctx->lua, 1);

            sp_log_sync("Built-in functions:\n");
            int cnt = 0;
            while ((name = built_in_commands[cnt++]))
                sp_log_sync("    %s\n", name);

            goto line_end;
        } else if (!strncmp(line, "load", strlen("load"))) {
            char *script_name = NULL;
            char *script_entrypoint = NULL;

            int num_arguments = 0;
            char *line_mod = av_strdup(line);
            char *save, *token = av_strtok(line_mod, " ", &save);
            token = av_strtok(NULL, " ", &save); /* Skip "load" */
            while (token) {
                if (!script_name) {
                    script_name = token;
                    if (sp_lfn_loadfile(ctx, script_name) < 0)
                        break;
                    if (lua_pcall(ctx->lua, 0, 0, 0) != LUA_OK) {
                        sp_log(&repl_state, SP_LOG_ERROR, "Lua script error: %s\n",
                               lua_tostring(ctx->lua, -1));
                        break;
                    }
                } else if (!script_entrypoint) {
                    script_entrypoint = token;
                    lua_getglobal(ctx->lua, script_entrypoint);
                    if (!lua_isfunction(ctx->lua, -1)) {
                        sp_log(&repl_state, SP_LOG_ERROR, "Entrypoint \"%s\" not found!\n",
                               script_entrypoint);
                        break;
                    }
                } else {
                    lua_pushstring(ctx->lua, token);
                    num_arguments++;
                }
                token = av_strtok(NULL, " ", &save);
            }

            /* No error encountered */
            if (!script_name) {
                sp_log(&repl_state, SP_LOG_ERROR, "Missing path for \"load\"!\n");
            } else if (!token) {
                if (script_entrypoint) {
                    if (lua_pcall(ctx->lua, num_arguments, 0, 0) != LUA_OK) {
                        sp_log(&repl_state, SP_LOG_ERROR, "Error running \"%s\": %s\n",
                               script_entrypoint, lua_tostring(ctx->lua, -1));
                    }
                }
            }

            av_free(line_mod);
            goto line_end;
        } else if (!strcmp("stats", line)) {
            size_t mem_used = 1024*lua_gc(ctx->lua, LUA_GCCOUNT) + lua_gc(ctx->lua, LUA_GCCOUNTB);
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

            unsigned long long lock_cnt = atomic_load(&ctx->lock_counter);
            unsigned long long contention_cnt = atomic_load(&ctx->contention_counter);
            unsigned long long skip_cnt = atomic_load(&ctx->skip_counter);
            atomic_store(&ctx->lock_counter, 0);
            atomic_store(&ctx->contention_counter, 0);
            atomic_store(&ctx->skip_counter, 0);

            sp_log(&repl_state, SP_LOG_INFO, "Lua memory used:     %.2f %s\n",
                   mem_used_f, mem_used_suffix);
            sp_log(&repl_state, SP_LOG_INFO, "Lua lock contention: %.2f%%\n",
                   (contention_cnt / (double)lock_cnt) * 100.0);
            sp_log(&repl_state, SP_LOG_INFO, "Lua skipped events: %.2f%%\n",
                   (skip_cnt / (double)lock_cnt) * 100.0);
            sp_log(&repl_state, SP_LOG_INFO, "Lua contexts: %i\n",
                   sp_bufferlist_len(ctx->lua_buf_refs));
            sp_log(&repl_state, SP_LOG_INFO, "Pending commands: %i\n",
                   sp_bufferlist_len(ctx->commit_list));
            goto line_end;
        } else if (!strncmp(line, "require", strlen("require"))) {
            char *save, *token = av_strtok(line, " ", &save);
            token = av_strtok(NULL, " ", &save); /* Skip "require" */
            if (!token) {
                sp_log(&repl_state, SP_LOG_ERROR, "Missing library name(s) for \"require\"!\n");
                goto line_end;
            }

            while (token) {
                sp_load_lua_library(ctx, token);
                token = av_strtok(NULL, " ", &save);
            }

            goto line_end;
        } else if (!strncmp(line, "info", strlen("info"))) {
            char *line_mod = av_strdup(line);
            char *save, *token = av_strtok(line_mod, " ", &save);
            token = av_strtok(NULL, " ", &save); /* Skip "info" */
            if (!token) {
                sp_log_sync("Specify command/object to query\n");
                av_free(line_mod);
                goto line_end;
            }

            int cnt = 0;
            while ((name = built_in_commands[cnt++])) {
                if (!strcmp(name, token)) {
                    sp_log_sync("\"%s\": built-in command\n", token);
                    av_free(line_mod);
                    goto line_end;
                }
            }

            lua_pushglobaltable(ctx->lua);
            lua_pushnil(ctx->lua);
            while (lua_next(ctx->lua, -2)) {
                name = lua_tostring(ctx->lua, -2);
                if (!strcmp(name, token)) {
                    sp_log_sync("\"%s\": lua %s%s\n",
                                token, lua_typename(ctx->lua, lua_type(ctx->lua, -1)),
                                (lua_type(ctx->lua, -1) != LUA_TTABLE &&
                                 lua_type(ctx->lua, -1) != LUA_TSTRING) ? "" :
                                ", contents:");

                    if (lua_type(ctx->lua, -1) == LUA_TTABLE) {
                        lua_pushnil(ctx->lua);
                        while (lua_next(ctx->lua, -2)) {
                            const char *fname;

                            /* If the "key" is a number, we have a keyless table */
                            if (lua_isnumber(ctx->lua, -2))
                                fname = lua_tostring(ctx->lua, -1);
                            else
                                fname = lua_tostring(ctx->lua, -2);

                            const char *type = lua_typename(ctx->lua, lua_type(ctx->lua, -1));
                            if (fname)
                                sp_log_sync("    \"%s.%s\": %s\n", name, fname, type);
                            else
                                sp_log_sync("    %s\n", type);

                            lua_pop(ctx->lua, 1);
                        }
                        lua_pop(ctx->lua, 1);
                    } else if (lua_type(ctx->lua, -1) == LUA_TSTRING) {
                        sp_log_sync("    \"%s\"\n", lua_tostring(ctx->lua, -1));
                    }

                    av_free(line_mod);
                    lua_pop(ctx->lua, 2);
                    goto line_end;
                }
                lua_pop(ctx->lua, 1);
            }
            lua_pop(ctx->lua, 1);

            sp_log_sync("No info for \"%s\"\n", token);
            av_free(line_mod);
            goto line_end;
        } else if (!strncmp(line, "run_gc", strlen("run_gc"))) {
            size_t mem_used = 1024*lua_gc(ctx->lua, LUA_GCCOUNT) + lua_gc(ctx->lua, LUA_GCCOUNTB);
            lua_gc(ctx->lua, LUA_GCCOLLECT, 0);
            mem_used -= 1024*lua_gc(ctx->lua, LUA_GCCOUNT) + lua_gc(ctx->lua, LUA_GCCOUNTB);
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

            sp_log(&repl_state, SP_LOG_INFO, "Lua memory freed: %.2f %s\n",
                   mem_used_f, mem_used_suffix);

            goto line_end;
        }

        ret = luaL_dostring(ctx->lua, line);
        if (lua_isstring(ctx->lua, -1)) {
            struct tmp {
                SPClass *class;
            } tmp;
            sp_class_alloc(&tmp, "lua", SP_TYPE_SCRIPT, ctx);
            sp_log(&tmp, ret == LUA_OK ? SP_LOG_INFO : SP_LOG_ERROR,
                   "%s\n", lua_tostring(ctx->lua, -1));
            sp_class_free(&tmp);
        }

        lua_pop(ctx->lua, 1);

line_end:
        add_history(line);
        atomic_store(&repl_state.do_not_update, 0);
        pthread_mutex_unlock(&ctx->lock);
        free(line);
    }

    if (!line)
        sp_log_sync("\n"); /* Handles Ctrl+D */

    atomic_store(&repl_state.selfquit, 1);

    raise(SIGINT);

    return NULL;
}

void sp_repl_init(TXMainContext *ctx)
{
    sp_class_alloc(&repl_state, "repl", SP_TYPE_SCRIPT, ctx);

    repl_state.events = sp_bufferlist_new();

    repl_state.ctx = ctx;
    repl_state.has_event = ATOMIC_VAR_INIT(0);
    repl_state.do_not_update = ATOMIC_VAR_INIT(0);
    repl_state.selfquit = ATOMIC_VAR_INIT(0);
    pthread_create(&repl_state.repl_thread, NULL, repl_thread, ctx);
}

int sp_repl_prompt_event(AVBufferRef *event, const char *msg)
{
    if (!repl_state.class || atomic_load(&repl_state.has_event))
        return AVERROR(EINVAL);

    sp_eventlist_add(&repl_state, repl_state.events, event);
    sp_log(&repl_state, SP_LOG_INFO, "%s\n", msg);
    atomic_store(&repl_state.has_event, 1);

    return 0;
}

void sp_repl_uninit(void)
{
    if (!atomic_load(&repl_state.selfquit) &&
        !atomic_load(&repl_state.do_not_update))
        pthread_cancel(repl_state.repl_thread);

    pthread_join(repl_state.repl_thread, NULL);

    rl_deprep_terminal();
    sp_log_set_prompt_callback(NULL, NULL);

    if (!atomic_load(&repl_state.selfquit) &&
        !atomic_load(&repl_state.do_not_update))
        sp_log_sync("\n");

    sp_bufferlist_free(&repl_state.events);

    sp_class_free(&repl_state);
}
