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

/* Needed to enable pthread_setname_np extension on Darwin */
# ifdef __APPLE__
#  define _DARWIN_C_SOURCE 1
# endif

#include "os_compat.h"
#include "utils.h"

#if defined(HAVE_PTHREAD_SETNAME_NP) && !defined(__APPLE__)
#include <string.h>
#include <errno.h>
void sp_set_thread_name_self(const char *name)
{
    pthread_t self = pthread_self();
    if (pthread_setname_np(self, name) == ERANGE) {
        char trunc_name[16];
        strncpy(trunc_name, name, 16);
        trunc_name[15] = '\0';
        pthread_setname_np(self, trunc_name);
    }
}
#elif defined(HAVE_PTHREAD_SETNAME_NP) && defined(__APPLE__)
void sp_set_thread_name_self(const char *name)
{
    pthread_setname_np(name);
}
#else
void sp_set_thread_name_self(const char *name)
{
    return;
}
#endif

/* ================================================ */
/* WAKEUP PIPE SECTION                              */
/* ================================================ */
#include <unistd.h>
#include <stdbool.h>
#include <fcntl.h>

/* Set the CLOEXEC flag on the given fd. */
static bool set_cloexec(int fd)
{
    if (fd >= 0) {
        int flags = fcntl(fd, F_GETFD);
        if (flags == -1)
            return AVERROR(errno);
        if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1)
            return AVERROR(errno);
    }

    return 0;
}

static int sp_make_cloexec_pipe(int pipes[2])
{
    int ret;

    if (pipe(pipes) != 0) {
        pipes[0] = pipes[1] = -1;
        return AVERROR(errno);
    }

    ret = set_cloexec(pipes[0]);
    if (ret < 0)
        return ret;

    return set_cloexec(pipes[1]);
}

/* create a pipe, and set it to non-blocking (and also set FD_CLOEXEC) */
int sp_make_wakeup_pipe(int pipes[2])
{
    if (sp_make_cloexec_pipe(pipes) < 0)
        return -1;

    for (int i = 0; i < 2; i++) {
        int val = fcntl(pipes[i], F_GETFL) | O_NONBLOCK;
        fcntl(pipes[i], F_SETFL, val);
    }

    return 0;
}

void sp_write_wakeup_pipe(int pipes[2], int64_t val)
{
    if ((pipes[0] == -1) || (pipes[1] == -1))
        return;

    (void)write(pipes[1], &val, sizeof(val));
}

int64_t sp_flush_wakeup_pipe(int pipes[2])
{
    sp_assert((pipes[0] >= 0) && (pipes[1] >= 0));
    int64_t res = INT64_MIN;
    (void)read(pipes[0], &res, sizeof(res));
    return res;
}

void sp_close_wakeup_pipe(int pipes[2])
{
    for (int i = 0; i < 2; i++) {
        if (pipes[i] >= 0)
            close(pipes[i]);
    }
}
