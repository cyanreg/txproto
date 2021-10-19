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

#include <stdarg.h>

#include <stdatomic.h>
#include <libavutil/bprint.h>
#include <libavutil/crc.h>
#include <libavutil/dict.h>
#include <libavutil/random_seed.h>

#include "logging.h"
#include "utils.h"
#include "os_compat.h"

#define CANARY_PATTERN 0x7e1eca57ca5ab1a9

struct SPClass {
    uint64_t canary;
    char *name;
    uint32_t id;
    enum SPType type;
    void *parent;
    pthread_mutex_t lock;
};

struct SPLogState {
    struct {
        void *ctx;
        void (*cb)(void *ctx, int newline_started);
    } prompt;

    struct {
        char *str;
        int lines;
        enum SPStatusFlags lock;
    } status;

    int last_was_newline;

    pthread_mutex_t term_lock;
    pthread_mutex_t file_lock;
    pthread_mutex_t levels_lock;

    SPClass ffclass;

    FILE *log_file;
    AVDictionary *log_levels;
} static log_ctx = {
    .last_was_newline = 1,
    .term_lock = PTHREAD_MUTEX_INITIALIZER,
    .file_lock = PTHREAD_MUTEX_INITIALIZER,
    .levels_lock = PTHREAD_MUTEX_INITIALIZER,

    .ffclass = (SPClass){
        .name = "ffmpeg",
        .type = SP_TYPE_EXTERNAL,
        .lock = PTHREAD_MUTEX_INITIALIZER,
    },
};

static inline SPClass *get_class(void *ctx)
{
    if (!ctx)
        return NULL;

    struct {
        SPClass *class;
    } *s = ctx;

    sp_assert(!s->class);
    sp_assert(s->class->canary != CANARY_PATTERN);

    return s->class;
}

static inline const char *get_class_color(SPClass *class)
{
    if (!class)
        return "";
    else if (class->type == SP_TYPE_NONE)
        return "\033[38;5;243m";
    else if (class->type & SP_TYPE_SCRIPT)
        return "\033[38;5;150m";
    else if (class->type & SP_TYPE_CONTEXT)
        return "\033[036m";
    else if (class->type & SP_TYPE_INTERFACE)
        return "\033[38;5;129m";
    else if (class->type & (SP_TYPE_AUDIO_BIDIR | SP_TYPE_VIDEO_BIDIR))
        return "\033[035m";
    else if (class->type & (SP_TYPE_FILTER))
        return "\033[38;5;99m";
    else if (class->type & (SP_TYPE_CODEC))
        return "\033[38;5;199m";
    else if (class->type & (SP_TYPE_MUXING))
        return "\033[38;5;178m";
    else if (class->type & (SP_TYPE_EXTERNAL))
        return "\033[38;5;60m";
    else
        return "";
}

static char *build_line(SPClass *class, enum SPLogLevel lvl, int with_color,
                        const char *format, va_list args, int *ends_in_nl,
                        int last_was_nl)
{
    AVBPrint bpc;
    av_bprint_init(&bpc, 256, AV_BPRINT_SIZE_AUTOMATIC);

    if (!with_color) {
        if (lvl == SP_LOG_FATAL)
            av_bprintf(&bpc, "(fatal)");
        else if (lvl == SP_LOG_ERROR)
            av_bprintf(&bpc, "(error)");
        else if (lvl == SP_LOG_WARN)
            av_bprintf(&bpc, "(warn)");
        else if (lvl == SP_LOG_VERBOSE)
            av_bprintf(&bpc, "(info)");
        else if (lvl == SP_LOG_DEBUG)
            av_bprintf(&bpc, "(debug)");
        else if (lvl == SP_LOG_TRACE)
            av_bprintf(&bpc, "(trace)");
    }

    if (class && last_was_nl) {
        SPClass *parent = get_class(class->parent);
        if (parent && strlen(class->name))
            av_bprintf(&bpc, "[%s%s%s->%s%s%s]",
                       with_color ? get_class_color(parent) : "",
                       parent->name,
                       with_color ? "\033[0m" : "",
                       with_color ? get_class_color(class) : "",
                       class->name,
                       with_color ? "\033[0m" : "");
        else
            av_bprintf(&bpc, "[%s%s%s]",
                       with_color ? get_class_color(class) : "",
                       strlen(class->name) ? class->name :
                       parent ? parent->name : "misc",
                       with_color ? "\033[0m" : "");
        av_bprint_chars(&bpc, ' ', 1);
    }

    int colord = ~with_color;
    if (lvl == SP_LOG_FATAL && ++colord)
        av_bprintf(&bpc, "\033[1;031m");
    else if (lvl == SP_LOG_ERROR && ++colord)
        av_bprintf(&bpc, "\033[1;031m");
    else if (lvl == SP_LOG_WARN && ++colord)
        av_bprintf(&bpc, "\033[1;033m");
    else if (lvl == SP_LOG_VERBOSE && ++colord)
        av_bprintf(&bpc, "\033[38;5;46m");
    else if (lvl == SP_LOG_DEBUG && ++colord)
        av_bprintf(&bpc, "\033[38;5;34m");
    else if (lvl == SP_LOG_TRACE && ++colord)
        av_bprintf(&bpc, "\033[38;5;28m");

    int format_ends_with_newline = format[strlen(format) - 1] == '\n';
    *ends_in_nl = format_ends_with_newline;

    if (with_color && colord) {
        if (format_ends_with_newline) {
            char *fmt_copy = av_strdup(format);
            fmt_copy[strlen(fmt_copy) - 1] = '\0';
            format = fmt_copy;
        }
    }

    av_vbprintf(&bpc, format, args);

    if (with_color && colord) {
        av_bprintf(&bpc, "\033[0m");
        if (format_ends_with_newline) {
            av_bprint_chars(&bpc, '\n', 1);
            av_free((void *)format);
        }
    }

    char *ret;
    av_bprint_finalize(&bpc, &ret);

    return ret;
}

static int decide_print_line(SPClass *class, enum SPLogLevel lvl)
{
    pthread_mutex_lock(&log_ctx.levels_lock);

    AVDictionaryEntry *global_lvl_entry = av_dict_get(log_ctx.log_levels, "global", NULL, 0);
    AVDictionaryEntry *local_lvl_entry = NULL;
    AVDictionaryEntry *parent_lvl_entry = NULL;

    if (class) {
        local_lvl_entry = av_dict_get(log_ctx.log_levels, class->name, NULL, 0);
        if (class->parent)
            parent_lvl_entry = av_dict_get(log_ctx.log_levels, sp_class_get_name(class->parent), NULL, 0);
    }

    pthread_mutex_unlock(&log_ctx.levels_lock);

    if (local_lvl_entry) {
        int local_lvl = strtol(local_lvl_entry->value, NULL, 10);
        return local_lvl >= lvl;
    } else if (parent_lvl_entry) {
        int parent_lvl = strtol(parent_lvl_entry->value, NULL, 10);
        return parent_lvl >= lvl;
    } else {
        int global_lvl = global_lvl_entry ? strtol(global_lvl_entry->value, NULL, 10) : SP_LOG_INFO;
        return global_lvl >= lvl;
    }

    return 1;
}

static void main_log(SPClass *class, enum SPLogLevel lvl, const char *format, va_list args)
{
    int with_color = 1;
    int print_line = decide_print_line(class, lvl);
    int log_line = !!log_ctx.log_file;
    int nl_end, last_nl_end;
    char *pline = NULL, *lline = NULL;

    /* Lock needed for log_ctx.last_was_newline */
    pthread_mutex_lock(&log_ctx.term_lock);

    last_nl_end = lvl <= SP_LOG_ERROR ? 1 : log_ctx.last_was_newline;

    if (print_line) {
        pline = build_line(class, lvl, with_color, format, args, &nl_end, last_nl_end);

        if (pline) {
            /* Tell CLI to erase its line */
            if (log_ctx.prompt.cb && last_nl_end)
                log_ctx.prompt.cb(log_ctx.prompt.ctx, 0);

            /* Erase status */
            if (last_nl_end && log_ctx.status.str) {
                for (int i = 0; i < log_ctx.status.lines; i++)
                    printf("\033[2K\033[1F");
                printf("\033[2K");
            }

            printf("%s", pline);

            /* Reprint status */
            if (nl_end && log_ctx.status.str)
                printf("%s", log_ctx.status.str);

            /* Reprint CLI prompt */
            if (log_ctx.prompt.cb && nl_end)
                log_ctx.prompt.cb(log_ctx.prompt.ctx, 1);

            /* Update newline state */
            log_ctx.last_was_newline = nl_end;
        }
    }

    if (log_line) {
        if (pline && !with_color)
            SPSWAP(char *, lline, pline);
        else
            lline = build_line(class, lvl, 0, format, args, &nl_end, last_nl_end);

        if (lline) {
            pthread_mutex_lock(&log_ctx.file_lock);
            fprintf(log_ctx.log_file, "%s", lline);
            pthread_mutex_unlock(&log_ctx.file_lock);
        }
    }

    pthread_mutex_unlock(&log_ctx.term_lock);

    av_free(pline);
    av_free(lline);
}

void sp_log(void *classed_ctx, enum SPLogLevel lvl, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    SPClass *class = get_class(classed_ctx);
    main_log(class, lvl, format, args);
    va_end(args);
}

void sp_log_sync(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int nl_end = format[strlen(format) - 1] == '\n';

    pthread_mutex_lock(&log_ctx.term_lock);

    /* Tell CLI to erase its line */
    if (log_ctx.prompt.cb && log_ctx.last_was_newline)
        log_ctx.prompt.cb(log_ctx.prompt.ctx, 0);

    /* Erase status */
    if (log_ctx.last_was_newline && log_ctx.status.str) {
        for (int i = 0; i < log_ctx.status.lines; i++)
            printf("\033[2K\033[1F");
        printf("\033[2K");
    }

    vfprintf(stdout, format, args);

    /* Reprint status */
    if (nl_end && log_ctx.status.str)
        printf("%s", log_ctx.status.str);

    /* Reprint CLI prompt */
    if (log_ctx.prompt.cb && nl_end)
        log_ctx.prompt.cb(log_ctx.prompt.ctx, 1);

    /* Update newline state */
    log_ctx.last_was_newline = nl_end;

    pthread_mutex_unlock(&log_ctx.term_lock);
    va_end(args);
}

static void log_ff_cb(void *ctx, int lvl, const char *format, va_list args)
{
    SPClass *ffmpeg_class = &log_ctx.ffclass;
    pthread_mutex_lock(&ffmpeg_class->lock);

    const struct {
        AVClass *class;
    } *tmp = ctx;
    const AVClass *avclass = ctx ? tmp->class : NULL;

    struct {
        SPClass *class;
    } tmp_f = { ffmpeg_class };

    SPClass top = {
        .name   = (avclass && avclass->item_name(ctx)) ? (char *)avclass->item_name(ctx) : "",
        .parent = &tmp_f,
        .lock   = PTHREAD_MUTEX_INITIALIZER,
        .type   = SP_TYPE_NONE,
    };

    if (avclass) {
        switch (avclass->get_category ? avclass->get_category(ctx) : avclass->category) {
        case AV_CLASS_CATEGORY_DEVICE_INPUT:        top.type = SP_TYPE_CONTEXT;      break;
        case AV_CLASS_CATEGORY_DEVICE_OUTPUT:       top.type = SP_TYPE_CONTEXT;      break;
        case AV_CLASS_CATEGORY_DEVICE_AUDIO_INPUT:  top.type = SP_TYPE_AUDIO_SOURCE; break;
        case AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT: top.type = SP_TYPE_AUDIO_SINK;   break;
        case AV_CLASS_CATEGORY_DEVICE_VIDEO_INPUT:  top.type = SP_TYPE_VIDEO_SOURCE; break;
        case AV_CLASS_CATEGORY_DEVICE_VIDEO_OUTPUT: top.type = SP_TYPE_VIDEO_SINK;   break;
        case AV_CLASS_CATEGORY_SWRESAMPLER:         top.type = SP_TYPE_FILTER;       break;
        case AV_CLASS_CATEGORY_SWSCALER:            top.type = SP_TYPE_FILTER;       break;
        case AV_CLASS_CATEGORY_BITSTREAM_FILTER:    top.type = SP_TYPE_BSF;          break;
        case AV_CLASS_CATEGORY_FILTER:              top.type = SP_TYPE_FILTER;       break;
        case AV_CLASS_CATEGORY_DECODER:             top.type = SP_TYPE_DECODER;      break;
        case AV_CLASS_CATEGORY_ENCODER:             top.type = SP_TYPE_ENCODER;      break;
        case AV_CLASS_CATEGORY_DEMUXER:             top.type = SP_TYPE_DEMUXER;      break;
        case AV_CLASS_CATEGORY_MUXER:               top.type = SP_TYPE_MUXER;        break;
        case AV_CLASS_CATEGORY_INPUT:
        case AV_CLASS_CATEGORY_OUTPUT:              top.type = SP_TYPE_CONTEXT;      break;
        case AV_CLASS_CATEGORY_NA:
        default:                                    top.type = SP_TYPE_NONE;         break;
        }
    }

    enum SPLogLevel splvl;
    switch (lvl) {
    case AV_LOG_QUIET:   splvl = SP_LOG_QUIET;   break;
    case AV_LOG_PANIC:
    case AV_LOG_FATAL:   splvl = SP_LOG_QUIET;   break;
    case AV_LOG_ERROR:   splvl = SP_LOG_ERROR;   break;
    case AV_LOG_WARNING: splvl = SP_LOG_WARN;    break;
    case AV_LOG_INFO:    splvl = SP_LOG_INFO;    break;
    case AV_LOG_VERBOSE: splvl = SP_LOG_VERBOSE; break;
    case AV_LOG_DEBUG:   splvl = SP_LOG_DEBUG;   break;
    case AV_LOG_TRACE:
    default:             splvl = SP_LOG_TRACE;   break;
    }

    main_log(&top, splvl, format, args);

    pthread_mutex_unlock(&ffmpeg_class->lock);
}

void sp_log_set_ff_cb(void)
{
    av_log_set_callback(log_ff_cb);
}

int sp_class_alloc(void *ctx, const char *name, enum SPType type, void *parent)
{
    struct {
        SPClass *class;
    } *s = ctx;

    s->class = av_mallocz(sizeof(SPClass));
    if (!s->class)
        return AVERROR(ENOMEM);

    s->class->canary = CANARY_PATTERN;
    s->class->name = av_strdup(name);
    s->class->type = type;
    s->class->parent = parent;

    uint32_t id = av_get_random_seed();
    if (name)
        id ^= av_crc(av_crc_get_table(AV_CRC_32_IEEE), UINT32_MAX, name, strlen(name));

    id ^= av_crc(av_crc_get_table(AV_CRC_32_IEEE), UINT32_MAX, (void *)ctx, sizeof(void *));

    s->class->id = id;

    pthread_mutex_init(&s->class->lock, NULL);

    return 0;
}

void sp_class_free(void *ctx)
{
    SPClass *class = get_class(ctx);
    if (!class)
        return;

    pthread_mutex_destroy(&class->lock);
    av_free(class->name);
    av_free(class);
}

int sp_class_set_name(void *ctx, const char *name)
{
    SPClass *class = get_class(ctx);
    if (!class)
        return AVERROR(EINVAL);

    pthread_mutex_lock(&class->lock);

    char *dup = av_strdup(name);
    if (!dup)
        return AVERROR(ENOMEM);

    av_free(class->name);
    class->name = dup;

    pthread_mutex_unlock(&class->lock);

    return 0;
}

const char *sp_class_get_name(void *ctx)
{
    SPClass *class = get_class(ctx);
    if (!class)
        return NULL;
    return class->name;
}

const char *sp_class_get_parent_name(void *ctx)
{
    SPClass *class = get_class(ctx);
    if (class) {
        SPClass *parent = get_class(class->parent);
        if (parent)
            return parent->name;
    }
    return NULL;
}

uint32_t sp_class_get_id(void *ctx)
{
    SPClass *class = get_class(ctx);
    if (class)
        return class->id;
    return 0x0;
}

enum SPType sp_class_get_type(void *ctx)
{
    SPClass *class = get_class(ctx);
    if (class)
        return class->type;
    return SP_TYPE_NONE;
}

const char *sp_class_type_string(void *ctx)
{
    SPClass *class = get_class(ctx);
    if (!class)
        return NULL;

    switch (class->type) {
    case SP_TYPE_NONE:         return "none";
    case SP_TYPE_INTERFACE:    return "interface";
    case SP_TYPE_CONTEXT:      return "context";
    case SP_TYPE_EXTERNAL:     return "external";
    case SP_TYPE_SCRIPT:       return "script";

    case SP_TYPE_AUDIO_SOURCE: return "audio input";
    case SP_TYPE_AUDIO_SINK:   return "audio output";
    case SP_TYPE_AUDIO_BIDIR:  return "audio in+out";

    case SP_TYPE_VIDEO_SOURCE: return "video input";
    case SP_TYPE_VIDEO_SINK:   return "video output";
    case SP_TYPE_VIDEO_BIDIR:  return "video in+out";

    case SP_TYPE_SUB_SOURCE:   return "subtitle input";
    case SP_TYPE_SUB_SINK:     return "subtitle output";
    case SP_TYPE_SUB_BIDIR:    return "subtitle in+out";

    case SP_TYPE_CLOCK_SOURCE: return "clock source";
    case SP_TYPE_CLOCK_SINK:   return "clock sink";

    case SP_TYPE_FILTER:       return "filter";

    case SP_TYPE_ENCODER:      return "encoder";
    case SP_TYPE_DECODER:      return "decoder";

    case SP_TYPE_BSF:          return "bsf";

    case SP_TYPE_MUXER:        return "muxer";
    case SP_TYPE_DEMUXER:      return "demuxer";

    /* zalgofied because this should never happen */
    default:                   return "û̴̼n̷̡̎̄k̸͍̓͒ṅ̵̨̅ò̷̢̏w̷̙͍͌n̸̩̦̅";
    }
}

enum SPType sp_avcategory_to_type(AVClassCategory category)
{
    switch (category) {
    case AV_CLASS_CATEGORY_DEVICE_AUDIO_INPUT:  return SP_TYPE_AUDIO_SOURCE;
    case AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT: return SP_TYPE_AUDIO_SINK;
    case AV_CLASS_CATEGORY_DEVICE_VIDEO_INPUT:  return SP_TYPE_VIDEO_SOURCE;
    case AV_CLASS_CATEGORY_DEVICE_VIDEO_OUTPUT: return SP_TYPE_VIDEO_SINK;
    case AV_CLASS_CATEGORY_BITSTREAM_FILTER:    return SP_TYPE_BSF;
    case AV_CLASS_CATEGORY_FILTER:              return SP_TYPE_FILTER;
    case AV_CLASS_CATEGORY_ENCODER:             return SP_TYPE_ENCODER;
    case AV_CLASS_CATEGORY_DECODER:             return SP_TYPE_DECODER;
    case AV_CLASS_CATEGORY_MUXER:               return SP_TYPE_MUXER;
    case AV_CLASS_CATEGORY_DEMUXER:             return SP_TYPE_DEMUXER;
    default:                                    return SP_TYPE_NONE;
    }
}

int sp_log_set_ctx_lvl_str(const char *component, const char *lvl)
{
    enum SPLogLevel res;
    if (!strcmp(lvl, "quiet"))
        res = SP_LOG_QUIET;
    else if (!strcmp(lvl, "fatal"))
        res = SP_LOG_FATAL;
    else if (!strcmp(lvl, "error"))
        res = SP_LOG_ERROR;
    else if (!strcmp(lvl, "warn"))
        res = SP_LOG_WARN;
    else if (!strcmp(lvl, "info"))
        res = SP_LOG_INFO;
    else if (!strcmp(lvl, "verbose"))
        res = SP_LOG_VERBOSE;
    else if (!strcmp(lvl, "debug"))
        res = SP_LOG_DEBUG;
    else if (!strcmp(lvl, "trace"))
        res = SP_LOG_TRACE;
    else if (!lvl || !component || !strlen(component) || !strlen(lvl))
        return AVERROR(EINVAL);
    else
        return AVERROR(EINVAL);

    pthread_mutex_lock(&log_ctx.levels_lock);
    av_dict_set_int(&log_ctx.log_levels, component, res, 0);
    pthread_mutex_unlock(&log_ctx.levels_lock);

    return 0;
}

void sp_log_set_ctx_lvl(const char *component, enum SPLogLevel lvl)
{
    pthread_mutex_lock(&log_ctx.levels_lock);
    av_dict_set_int(&log_ctx.log_levels, component, lvl, 0);
    pthread_mutex_unlock(&log_ctx.levels_lock);
}

int sp_log_set_file(const char *path)
{
    int ret = 0;
    pthread_mutex_lock(&log_ctx.file_lock);

    if (log_ctx.log_file) {
        fflush(log_ctx.log_file);
        fclose(log_ctx.log_file);
    }

    log_ctx.log_file = av_fopen_utf8(path, "w");
    if (!log_ctx.log_file)
        ret = AVERROR(errno);

    pthread_mutex_unlock(&log_ctx.file_lock);
    return ret;
}

void sp_log_set_prompt_callback(void *ctx, void (*cb)(void *ctx, int newline_started))
{
    pthread_mutex_lock(&log_ctx.term_lock);
    log_ctx.prompt.ctx = ctx;
    log_ctx.prompt.cb = cb;
    pthread_mutex_unlock(&log_ctx.term_lock);
}

int sp_log_set_status(const char *status, enum SPStatusFlags flags)
{
    int status_newlines = 0;
    char *newstatus = NULL;

    pthread_mutex_lock(&log_ctx.term_lock);

    if (flags & (SP_STATUS_LOCK | SP_STATUS_UNLOCK)) {
        log_ctx.status.lock = flags & SP_STATUS_LOCK;
    } else if (log_ctx.status.lock) {
        pthread_mutex_unlock(&log_ctx.term_lock);
        return 0;
    }

    if (!status)
        goto print;

    int status_len = strlen(status);
    newstatus = av_malloc(status_len + 1 + 1);
    if (!newstatus) {
        pthread_mutex_unlock(&log_ctx.term_lock);
        return AVERROR(ENOMEM);
    }

    memcpy(newstatus, status, status_len);

    if (status[status_len - 1] != '\n')
        newstatus[status_len++] = '\n';

    newstatus[status_len] = '\0';

    int32_t cp;
    const char *str = newstatus, *end = str + status_len;

    while (str < end) {
        int err = av_utf8_decode(&cp, (const uint8_t **)&str, end,
                                 AV_UTF8_FLAG_ACCEPT_ALL);
        if (err < 0) {
            printf("Error parsing status string: %s!\n", av_err2str(err));
            pthread_mutex_unlock(&log_ctx.term_lock);
            av_free(newstatus);
            return err;
        }

        if (cp == '\n')
            status_newlines++;
    }

print:
    /* Tell CLI to erase its line */
    if (log_ctx.prompt.cb)
        log_ctx.prompt.cb(log_ctx.prompt.ctx, 0);

    /* Erase status if needed */
    if (!(flags & SP_STATUS_NO_CLEAR) && log_ctx.status.str) {
        for (int i = 0; i < log_ctx.status.lines; i++)
            printf("\033[2K\033[1F");
        printf("\033[2K");
    }

    /* Free old status and replace it with the new */
    av_free(log_ctx.status.str);
    log_ctx.status.str = newstatus;
    log_ctx.status.lines = status_newlines;

    /* Print new status */
    if (newstatus)
        printf("%s", newstatus);

    /* Reprint CLI prompt */
    if (log_ctx.prompt.cb)
        log_ctx.prompt.cb(log_ctx.prompt.ctx, 1);

    log_ctx.last_was_newline = 1;

    newstatus = NULL;

    pthread_mutex_unlock(&log_ctx.term_lock);

    return 0;
}

void sp_log_end(void)
{
    av_log_set_callback(av_log_default_callback);

    pthread_mutex_lock(&log_ctx.levels_lock);
    pthread_mutex_lock(&log_ctx.file_lock);
    pthread_mutex_lock(&log_ctx.term_lock);

    if (log_ctx.log_file) {
        fflush(log_ctx.log_file);
        fclose(log_ctx.log_file);
        log_ctx.log_file = NULL;
    }

    av_dict_free(&log_ctx.log_levels);
    av_freep(&log_ctx.status.str);
    log_ctx.status.lines = 0;
    log_ctx.status.lock = 0;

    pthread_mutex_unlock(&log_ctx.term_lock);
    pthread_mutex_unlock(&log_ctx.file_lock);
    pthread_mutex_unlock(&log_ctx.levels_lock);
}
