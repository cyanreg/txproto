#include <unistd.h>
#include <stdbool.h>
#include <fcntl.h>

#include <libavutil/opt.h>
#include <libavutil/dict.h>

/* Set the CLOEXEC flag on the given fd.
 * On error, false is returned (and errno set). */
bool sp_set_cloexec(int fd)
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

static int sp_make_cloexec_pipe(int pipes[2])
{
    if (pipe(pipes) != 0) {
        pipes[0] = pipes[1] = -1;
        return -1;
    }

    for (int i = 0; i < 2; i++)
        sp_set_cloexec(pipes[i]);
    return 0;
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

void sp_write_wakeup_pipe(int pipes[2])
{
    (void)write(pipes[1], &(char){ 0 }, 1);
}

void sp_flush_wakeup_pipe(int pipes[2])
{
    char buf[100];
    (void)read(pipes[0], buf, sizeof(buf));
}

void sp_close_wakeup_pipe(int pipes[2])
{
    for (int i = 0; i < 2; i++)
        close(pipes[i]);
}

// If the name starts with "@", try to interpret it as a number, and set *name
// to the name of the n-th parameter.
static void resolve_positional_arg(void *avobj, char **name)
{
    if (!*name || (*name)[0] != '@' || !avobj)
        return;

    char *end = NULL;
    int pos = strtol(*name + 1, &end, 10);
    if (!end || *end)
        return;

    const AVOption *opt = NULL;
    int offset = -1;
    while (1) {
        opt = av_opt_next(avobj, opt);
        if (!opt)
            return;
        // This is what libavfilter's parser does to skip aliases.
        if (opt->offset != offset && opt->type != AV_OPT_TYPE_CONST)
            pos--;
        if (pos < 0) {
            *name = (char *)opt->name;
            return;
        }
        offset = opt->offset;
    }
}

// Like mp_set_avopts(), but the posargs argument is used to resolve positional
// arguments. If posargs==NULL, positional args are disabled.
int sp_set_avopts_pos(void *log, void *avobj, void *posargs, AVDictionary *dict)
{
    int success = 0;
    const AVDictionaryEntry *d = NULL;
    while ((d = av_dict_get(dict, "", d, AV_DICT_IGNORE_SUFFIX))) {
        char *k = d->key;
        char *v = d->value;
        resolve_positional_arg(posargs, &k);
        int r = av_opt_set(avobj, k, v, AV_OPT_SEARCH_CHILDREN);
        if (r == AVERROR_OPTION_NOT_FOUND) {
            av_log(log, AV_LOG_ERROR, "AVOption \"%s\" not found!\n", k);
            success = -1;
        } else if (r < 0) {
            char errstr[80];
            av_strerror(r, errstr, sizeof(errstr));
            av_log(log, AV_LOG_ERROR, "Could not set AVOption \"%s\" to \"%s\": "
                   "%s!\n", k, v, errstr);
            success = -1;
        }
    }
    return success;
}

// Set these options on given avobj (using av_opt_set..., meaning avobj must
// point to a struct that has AVClass as first member).
// Options which fail to set (error or not found) are printed to log.
// Returns: >=0 success, <0 failed to set an option
int sp_set_avopts(void *log, void *avobj, AVDictionary *dict)
{
    return sp_set_avopts_pos(log, avobj, avobj, dict);
}
