#include <unistd.h>
#include <stdbool.h>
#include <fcntl.h>

/* Set the CLOEXEC flag on the given fd.
 * On error, false is returned (and errno set). */
bool wls_set_cloexec(int fd)
{
    if (fd >= 0) {
        int flags = fcntl(fd, F_GETFD);
        if (flags == -1)
            return false;
        if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1)
            return false;
    }
    return true;
}

static int wls_make_cloexec_pipe(int pipes[2])
{
    if (pipe(pipes) != 0) {
        pipes[0] = pipes[1] = -1;
        return -1;
    }

    for (int i = 0; i < 2; i++)
        wls_set_cloexec(pipes[i]);
    return 0;
}

/* create a pipe, and set it to non-blocking (and also set FD_CLOEXEC) */
int wls_make_wakeup_pipe(int pipes[2])
{
    if (wls_make_cloexec_pipe(pipes) < 0)
        return -1;

    for (int i = 0; i < 2; i++) {
        int val = fcntl(pipes[i], F_GETFL) | O_NONBLOCK;
        fcntl(pipes[i], F_SETFL, val);
    }
    return 0;
}

void wls_write_wakeup_pipe(int pipes[2])
{
    (void)write(pipes[1], &(char){ 0 }, 1);
}

void wls_flush_wakeup_pipe(int pipes[2])
{
    char buf[100];
    (void)read(pipes[0], buf, sizeof(buf));
}

void wls_close_wakeup_pipe(int pipes[2])
{
    for (int i = 0; i < 2; i++)
        close(pipes[i]);
}
