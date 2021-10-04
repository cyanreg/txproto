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

#include <stdint.h>

#include <pthread.h>

#include "../config.h"

/* Sets the thread name, if on an implementation where it's available */
void sp_set_thread_name_self(const char *name);

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define noreturn _Noreturn
#elif defined(__GNUC__)
#define noreturn __attribute__((noreturn))
#else
#define noreturn
#endif

/* TODO Windows support for dlopen */
#include <dlfcn.h>

/* Create a non-blocking named/FIFO pipe */
int      sp_make_wakeup_pipe (int pipes[2]);
void     sp_write_wakeup_pipe(int pipes[2], int64_t val);
int64_t  sp_flush_wakeup_pipe(int pipes[2]);
void     sp_close_wakeup_pipe(int pipes[2]);
