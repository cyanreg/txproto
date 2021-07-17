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

#include "../config.h"

#ifdef HAVE_PTHREAD_SETNAME_NP
#include <pthread.h>
#include <string.h>
#include <errno.h>
static inline void sp_set_thread_name_self(const char *name)
{
    pthread_t self = pthread_self();
    if (pthread_setname_np(self, name) == ERANGE) {
        char trunc_name[16];
        strncpy(trunc_name, name, 16);
        trunc_name[15] = '\0';
        pthread_setname_np(self, trunc_name);
    }
}
#else
static inline void sp_set_thread_name_self(const char *name)
{
    return;
}
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define noreturn _Noreturn
#elif defined(__GNUC__)
#define noreturn __attribute__((noreturn))
#else
#define noreturn
#endif

/* TODO Windows */
#include <dlfcn.h>
