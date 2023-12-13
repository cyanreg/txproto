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
#include <libavutil/dict.h>
#include <libavutil/time.h>
#include <libavutil/random_seed.h>

#include <libtxproto/log.h>
#include <libtxproto/utils.h>
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

typedef struct SPComponentLogLevel {
    char *component;
    enum SPLogLevel lvl;
} SPComponentLogLevel;

typedef struct SPIncompleteLineCache {
    AVBPrint bpo;
    AVBPrint bpf;
    uint32_t id;
    pthread_t thread;
    int used;
    int list_entry_incomplete;
    uint32_t list_id;
} SPIncompleteLineCache;

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

    int json_file;
    int json_out;
    int color_out;
    atomic_int print_ts;

    SPIncompleteLineCache classless_ic;
    SPIncompleteLineCache *ic;
    int ic_len;
    unsigned int ic_alloc;

    int64_t time_offset;

    pthread_mutex_t ctx_lock;
    pthread_mutex_t term_lock;
    pthread_mutex_t file_lock;

    SPClass null_class;
    SPClass ffclass;

    FILE *log_file;
    SPComponentLogLevel *log_levels;
    int num_log_levels;
} static log_ctx = {
    .term_lock = PTHREAD_MUTEX_INITIALIZER,
    .file_lock = PTHREAD_MUTEX_INITIALIZER,
    .ctx_lock = PTHREAD_MUTEX_INITIALIZER,

    .color_out = 1,
    .json_out  = 0,
    .json_file = 0,
    .print_ts = ATOMIC_VAR_INIT(0),

    .null_class = (SPClass){
        .name = "noname",
        .type = SP_TYPE_NONE,
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .canary = CANARY_PATTERN,
    },

    .ffclass = (SPClass){
        .name = "ffmpeg",
        .type = SP_TYPE_EXTERNAL,
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .canary = CANARY_PATTERN,
    },
};

static int log_done = 1;

static inline SPClass *get_class(void *ctx)
{
    if (!ctx)
        return NULL;

    struct {
        SPClass *class;
    } *s = ctx;

    if (!s->class)
        return NULL;

    sp_assert(s->class->canary == CANARY_PATTERN);

    return s->class;
}

static inline enum SPLogLevel get_component_level_locked(const char *component)
{
    for (int i = 0; i < log_ctx.num_log_levels; i++)
        if (!strcmp(log_ctx.log_levels[i].component, component))
            return log_ctx.log_levels[i].lvl;

    return INT_MAX;
}

static inline int decide_print_line(SPClass *class, enum SPLogLevel lvl)
{
    enum SPLogLevel global_lvl = log_ctx.log_levels[0].lvl;
    enum SPLogLevel local_lvl = INT_MAX;
    enum SPLogLevel parent_lvl = INT_MAX;

    if (class) {
        local_lvl = get_component_level_locked(class->name);
        if ((local_lvl == INT_MAX) && class->parent)
            parent_lvl = get_component_level_locked(sp_class_get_name(class->parent));
    }

    return local_lvl != INT_MAX ? local_lvl >= lvl :
           (parent_lvl != INT_MAX ? parent_lvl >= lvl : (global_lvl >= lvl));
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

static inline const char *type_to_string(enum SPType type)
{
    switch (type) {
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
    default: break;
    }

    return "û̴̼n̷̡̎̄k̸͍̓͒ṅ̵̨̅ò̷̢̏w̷̙͍͌n̸̩̦̅";
}

static const char *lvl_to_str(enum SPLogLevel lvl)
{
    switch (lvl) {
    case SP_LOG_QUIET:   return "quiet";
    case SP_LOG_FATAL:   return "fatal";
    case SP_LOG_ERROR:   return "error";
    case SP_LOG_WARN:    return "warn";
    case SP_LOG_INFO:    return "info";
    case SP_LOG_VERBOSE: return "verbose";
    case SP_LOG_DEBUG:   return "debug";
    case SP_LOG_TRACE:   return "trace";
    default: break;
    }

    return "unknown";
}

static int build_line_json(SPClass *class, AVBPrint *bpc, enum SPLogLevel lvl,
                           int with_color, int cont, int nolog, int64_t time_o,
                           const char *format, va_list args, int list, int list_end,
                           int *list_entry_incomplete, uint32_t list_id)
{
    if (!(*list_entry_incomplete) && (list || list_end || !cont)) {
        av_bprintf(bpc, "{\n");
        av_bprintf(bpc, "    \"level\": \"%s\",\n", lvl_to_str(lvl));
        av_bprintf(bpc, "    \"component\": \"%s\",\n", class->name);
        av_bprintf(bpc, "    \"component_id\": %u,\n", class->id);
        av_bprintf(bpc, "    \"component_type\": \"%s\",\n", type_to_string(class->type));

        SPClass *parent = get_class(class->parent);
        if (parent) {
            av_bprintf(bpc, "    \"parent\": \"%s\",\n", parent->name);
            av_bprintf(bpc, "    \"parent_id\": %u,\n", parent->id);
            av_bprintf(bpc, "    \"parent_type\": \"%s\",\n", type_to_string(parent->type));
        }
        av_bprintf(bpc, "    \"time_us\": %lu,\n", time_o);

        if (list || list_end) {
            av_bprintf(bpc, "    \"part_of\": %u,\n", list_id);
            av_bprintf(bpc, "    \"part_nb\": %u,\n", cont);
        }

        av_bprintf(bpc, "    \"message\": \"");
    }

    AVBPrint tmp;
    av_bprint_init(&tmp, 128, AV_BPRINT_SIZE_UNLIMITED);

    av_vbprintf(&tmp, format, args);

    char *to_escape;
    av_bprint_finalize(&tmp, &to_escape);

    av_bprint_escape(bpc, to_escape, "\"",
                     AV_ESCAPE_MODE_BACKSLASH, AV_ESCAPE_FLAG_STRICT);

    av_free(to_escape);

    int ends_line = 1;
    if (bpc->len && av_bprint_is_complete(bpc))
        ends_line = bpc->str[FFMIN(bpc->len, bpc->size - 1) - 1] == '\n';

    if (list || list_end)
        *list_entry_incomplete = !ends_line;

    if (ends_line) {
        bpc->str[FFMIN(bpc->len, bpc->size) - 1] = '\0';
        bpc->len--;
        av_bprintf(bpc, "\"\n");
        av_bprintf(bpc, "}\n");
    }

    return list && !list_end ? 0 : ends_line;
}

static int build_line_norm(SPClass *class, AVBPrint *bpc, enum SPLogLevel lvl,
                           int with_color, int cont, int nolog, int64_t time_o,
                           const char *format, va_list args, int list, int list_end,
                           int *list_entry_incomplete, uint32_t list_id)
{
    if (!(*list_entry_incomplete) && (list || list_end || !cont) &&
        atomic_load(&log_ctx.print_ts)) {
        if (!with_color && !nolog) {
            if (lvl == SP_LOG_FATAL)
                av_bprintf(bpc, "(fatal)");
            else if (lvl == SP_LOG_ERROR)
                av_bprintf(bpc, "(error)");
            else if (lvl == SP_LOG_WARN)
                av_bprintf(bpc, "(warn)");
            else if (lvl == SP_LOG_VERBOSE)
                av_bprintf(bpc, "(info)");
            else if (lvl == SP_LOG_DEBUG)
                av_bprintf(bpc, "(debug)");
            else if (lvl == SP_LOG_TRACE)
                av_bprintf(bpc, "(trace)");
        }

        if (!nolog)
            av_bprintf(bpc, "[%.3f%s", time_o/(1000000.0f), class ? "|" : "] ");

        if (class && !nolog) {
            SPClass *parent = get_class(class->parent);
            if (parent && strlen(class->name))
                av_bprintf(bpc, "%s%s%s->%s%s%s]",
                           with_color ? get_class_color(parent) : "",
                           parent->name,
                           with_color ? "\033[0m" : "",
                           with_color ? get_class_color(class) : "",
                           class->name,
                           with_color ? "\033[0m" : "");
            else
                av_bprintf(bpc, "%s%s%s]",
                           with_color ? get_class_color(class) : "",
                           strlen(class->name) ? class->name :
                           parent ? parent->name : "misc",
                           with_color ? "\033[0m" : "");
            av_bprint_chars(bpc, ' ', 1);
        }

        if ((list || list_end) && cont > 0)
            av_bprintf(bpc, "    %02i: ", cont);
    }

    int colored_message = 0;
    if (with_color) {
        switch (lvl) {
        case SP_LOG_FATAL:
            av_bprintf(bpc, "\033[1;031m");   colored_message = 1; break;
        case SP_LOG_ERROR:
            av_bprintf(bpc, "\033[1;031m");   colored_message = 1; break;
        case SP_LOG_WARN:
            av_bprintf(bpc, "\033[1;033m");   colored_message = 1; break;
        case SP_LOG_VERBOSE:
            av_bprintf(bpc, "\033[38;5;46m"); colored_message = 1; break;
        case SP_LOG_DEBUG:
            av_bprintf(bpc, "\033[38;5;34m"); colored_message = 1; break;
        case SP_LOG_TRACE:
            av_bprintf(bpc, "\033[38;5;28m"); colored_message = 1; break;
        default:
            break;
        }
    }

    av_vbprintf(bpc, format, args);

    int ends_line = 1;
    if (bpc->len && av_bprint_is_complete(bpc))
        ends_line = bpc->str[FFMIN(bpc->len, bpc->size - 1) - 1] == '\n';

    if (list || list_end)
        *list_entry_incomplete = !ends_line;

    if (colored_message) {
        /* Remove newline, if there was one */
        if (ends_line && bpc->len && av_bprint_is_complete(bpc)) {
            bpc->str[FFMIN(bpc->len, bpc->size) - 1] = '\0';
            bpc->len--;
        }

        /* Reset text color */
        av_bprintf(bpc, "\033[0m");

        /* Readd new line, if there was one */
        if (ends_line)
            av_bprint_chars(bpc, '\n', 1);
    }

    return list && !list_end ? 0 : ends_line;
}

static void main_log(SPClass *class, enum SPLogLevel lvl, const char *format,
                     va_list args)
{
    if (log_done)
        return;

    int cont = 0, no_class = 0, ends_line = 1;
    SPIncompleteLineCache *ic = NULL, *ic_unused = NULL;
    int nolog = lvl & SP_NOLOG;
    int list_entry = lvl & SP_LOG_LIST;
    int list_end = lvl & SP_LOG_LIST_END;
    pthread_t thread = pthread_self();

    lvl &= ~(SP_NOLOG | SP_LOG_LIST | SP_LOG_LIST_END);
    if ((nolog || list_entry || list_end) && !lvl)
        lvl = SP_LOG_INFO;

    pthread_mutex_lock(&log_ctx.ctx_lock);

    int print_line = nolog ? 1 : decide_print_line(class, lvl);
    int json_out = log_ctx.json_out;
    int json_file = log_ctx.json_file;
    int with_color = log_ctx.color_out;
    int log_line = !!log_ctx.log_file && !nolog;
    int64_t time_offset = av_gettime_relative() - log_ctx.time_offset;

    if (!class) {
        class = &log_ctx.null_class;
        no_class = 1;
    }

    if (!print_line && !log_line) {
        pthread_mutex_unlock(&log_ctx.ctx_lock);
        return;
    }

    for (int i = 0; i < log_ctx.ic_len; i++) {
        if (log_ctx.ic[i].used && (class->id == log_ctx.ic[i].id) &&
            pthread_equal(thread, log_ctx.ic[i].thread)) {
            ic = &log_ctx.ic[i];
            cont = log_ctx.ic[i].used++;
            break;
        } else if (!ic_unused && !log_ctx.ic[i].used) {
            ic_unused = &log_ctx.ic[i];
        }
    }

    if (!ic) {
        if (!ic_unused) {
            int alloc = 2;

            ic = av_fast_realloc(log_ctx.ic, &log_ctx.ic_alloc,
                                 (log_ctx.ic_len + alloc) * sizeof(*ic));
            if (!ic) {
                pthread_mutex_unlock(&log_ctx.ctx_lock);
                return;
            }

            log_ctx.ic = ic;

            ic_unused = &ic[log_ctx.ic_len];

            for (int i = 0; i < alloc; i++) {
                ic = &log_ctx.ic[log_ctx.ic_len + i];
                memset(ic, 0, sizeof(*ic));
                av_bprint_init(&ic->bpo, 32, AV_BPRINT_SIZE_UNLIMITED);
                av_bprint_init(&ic->bpf, 32, AV_BPRINT_SIZE_UNLIMITED);
                ic->list_id = av_get_random_seed();
            }

            log_ctx.ic_len += alloc;

            ic = ic_unused;
        } else {
            ic = ic_unused;
        }

        ic->id     = class->id;
        ic->thread = thread;
        ic->used   = 1;
        ic->list_entry_incomplete = 0;
        ic->list_id++;
    }

    class = no_class ? NULL : class;

    if (log_line) {
        if (json_file)
            ends_line = build_line_json(class, &ic->bpf, lvl, 0, cont, nolog,
                                        time_offset, format, args, list_entry, list_end,
                                        &ic->list_entry_incomplete, ic->list_id);
        else
            ends_line = build_line_norm(class, &ic->bpf, lvl, 0, cont, nolog,
                                        time_offset, format, args, list_entry, list_end,
                                        &ic->list_entry_incomplete, ic->list_id);

        if (ends_line) {
            pthread_mutex_lock(&log_ctx.file_lock);

            if (av_bprint_is_complete(&ic->bpf))
                fwrite(ic->bpf.str, FFMIN(ic->bpf.len, ic->bpf.size - 1), 1,
                       log_ctx.log_file);

            pthread_mutex_unlock(&log_ctx.file_lock);
        }
    }

    if (print_line) {
        AVBPrint *bpc;
        int reuse_file = log_line && json_out == json_file && !with_color;

        if (reuse_file) {
            bpc = &ic->bpf;
        } else {
            bpc = &ic->bpo;
            if (json_out)
                ends_line = build_line_json(class, bpc, lvl, with_color, cont, nolog,
                                            time_offset, format, args, list_entry, list_end,
                                            &ic->list_entry_incomplete, ic->list_id);
            else
                ends_line = build_line_norm(class, bpc, lvl, with_color, cont, nolog,
                                            time_offset, format, args, list_entry, list_end,
                                            &ic->list_entry_incomplete, ic->list_id);
        }

        if (ends_line) {
            pthread_mutex_lock(&log_ctx.term_lock);

            /* Tell CLI to erase its line */
            if (log_ctx.prompt.cb)
                log_ctx.prompt.cb(log_ctx.prompt.ctx, 0);

            /* Erase status */
            if (log_ctx.status.str) {
                for (int i = 0; i < log_ctx.status.lines; i++)
                    printf("\033[2K\033[1F");
                printf("\033[2K");
            }

            if (av_bprint_is_complete(bpc))
                fwrite(bpc->str, FFMIN(bpc->len + 1, bpc->size), 1,
                       lvl == SP_LOG_ERROR ? stderr : stdout);

            /* Reprint status */
            if (log_ctx.status.str)
                printf("%s", log_ctx.status.str);

            /* Reprint CLI prompt */
            if (log_ctx.prompt.cb)
                log_ctx.prompt.cb(log_ctx.prompt.ctx, 1);

            pthread_mutex_unlock(&log_ctx.term_lock);
        }
    }

    if (ends_line) {
        av_bprint_clear(&ic->bpo);
        av_bprint_clear(&ic->bpf);
        ic->used = 0;
        ic->list_entry_incomplete = 0;

        /* "Free" if we have more than we'd like and it's the last one. */
        if (log_ctx.ic_len > 28 && (ic == &log_ctx.ic[log_ctx.ic_len - 1])) {
            av_bprint_finalize(&ic->bpo, NULL);
            av_bprint_finalize(&ic->bpf, NULL);

            ic = av_fast_realloc(log_ctx.ic, &log_ctx.ic_alloc,
                                 (log_ctx.ic_len - 1) * sizeof(*ic));
            if (ic) {
                log_ctx.ic = ic;
                log_ctx.ic_len--;
            }
        } else if (ic->bpo.size > 1048576 || ic->bpf.size > 1048576) {
            av_bprint_finalize(&ic->bpo, NULL);
            av_bprint_finalize(&ic->bpf, NULL);
            av_bprint_init(&ic->bpo, 256, AV_BPRINT_SIZE_UNLIMITED);
            av_bprint_init(&ic->bpf, 256, AV_BPRINT_SIZE_UNLIMITED);
        }
    } else if (ic->list_entry_incomplete) {
        ic->used--;
    }

    pthread_mutex_unlock(&log_ctx.ctx_lock);
}

void sp_log(void *classed_ctx, enum SPLogLevel lvl, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    SPClass *class = get_class(classed_ctx);
    main_log(class, lvl, format, args);
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

int sp_class_alloc(void *ctx, const char *name, enum SPType type, void *parent)
{
    struct {
        SPClass *class;
    } *s = ctx;

    s->class = av_mallocz(sizeof(SPClass));
    if (!s->class)
        return AVERROR(ENOMEM);

    s->class->canary = CANARY_PATTERN;
    s->class->name   = av_strdup(name);
    s->class->type   = type;
    s->class->parent = parent;
    s->class->id     = av_get_random_seed();

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

    return type_to_string(class->type);
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

    return sp_log_set_ctx_lvl(component, res);
}

enum SPLogLevel sp_log_get_ctx_lvl(const char *component)
{
    pthread_mutex_lock(&log_ctx.ctx_lock);

    enum SPLogLevel lvl = log_ctx.log_levels[0].lvl;

    for (int i = 1; i < log_ctx.num_log_levels; i++) {
        if (!strcmp(log_ctx.log_levels[i].component, component)) {
            lvl = log_ctx.log_levels[i].lvl;
            break;
        }
    }

    pthread_mutex_unlock(&log_ctx.ctx_lock);

    return lvl;
}

int sp_log_set_ctx_lvl(const char *component, enum SPLogLevel lvl)
{
    pthread_mutex_lock(&log_ctx.ctx_lock);

    SPComponentLogLevel *level = NULL;
    for (int i = 0; i < log_ctx.num_log_levels; i++) {
        if (!strcmp(log_ctx.log_levels[i].component, component)) {
            component = log_ctx.log_levels[i].component;
            level = &log_ctx.log_levels[i];
            break;
        }
    }

    if (!level) {
        component = av_strdup(component);
        if (!component) {
            pthread_mutex_unlock(&log_ctx.ctx_lock);
            return AVERROR(ENOMEM);
        }

        SPComponentLogLevel *new_levels;
        size_t new_levels_size = sizeof(*new_levels)*(log_ctx.num_log_levels + 1);
        new_levels = av_realloc(log_ctx.log_levels, new_levels_size);
        if (!new_levels) {
            av_free((char *)component);
            pthread_mutex_unlock(&log_ctx.ctx_lock);
            return AVERROR(ENOMEM);
        }

        log_ctx.log_levels = new_levels;
        level = &new_levels[log_ctx.num_log_levels];
        log_ctx.num_log_levels++;
    }

    level->component = (char *)component;
    level->lvl = lvl;

    pthread_mutex_unlock(&log_ctx.ctx_lock);

    return 0;
}

int sp_log_set_file(const char *path)
{
    int ret = 0;
    pthread_mutex_lock(&log_ctx.ctx_lock);

    if (log_ctx.log_file) {
        fflush(log_ctx.log_file);
        fclose(log_ctx.log_file);
    }

    log_ctx.log_file = av_fopen_utf8(path, "w");
    if (!log_ctx.log_file)
        ret = AVERROR(errno);

    pthread_mutex_unlock(&log_ctx.ctx_lock);

    return ret;
}

void sp_log_set_prompt_callback(void *ctx, void (*cb)(void *ctx, int newline_started))
{
    pthread_mutex_lock(&log_ctx.ctx_lock);

    log_ctx.prompt.ctx = ctx;
    log_ctx.prompt.cb = cb;

    pthread_mutex_unlock(&log_ctx.ctx_lock);
}

int sp_log_set_status(const char *status, enum SPStatusFlags flags)
{
    int status_newlines = 0;
    char *newstatus = NULL;

    pthread_mutex_lock(&log_ctx.ctx_lock);
    pthread_mutex_lock(&log_ctx.term_lock);

    if (flags & (SP_STATUS_LOCK | SP_STATUS_UNLOCK)) {
        log_ctx.status.lock = flags & SP_STATUS_LOCK;
    } else if (log_ctx.status.lock) {
        pthread_mutex_unlock(&log_ctx.term_lock);
        pthread_mutex_unlock(&log_ctx.ctx_lock);
        return 0;
    }

    if (!status)
        goto print;

    int status_len = strlen(status);
    newstatus = av_malloc(status_len + 1 + 1);
    if (!newstatus) {
        pthread_mutex_unlock(&log_ctx.term_lock);
        pthread_mutex_unlock(&log_ctx.ctx_lock);
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
            pthread_mutex_unlock(&log_ctx.ctx_lock);
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

    newstatus = NULL;

    pthread_mutex_unlock(&log_ctx.term_lock);
    pthread_mutex_unlock(&log_ctx.ctx_lock);

    return 0;
}

void sp_log_set_json_out(int file, int out)
{
    pthread_mutex_lock(&log_ctx.ctx_lock);

    if (file > -1)
        log_ctx.json_file = file;
    if (out > -1)
        log_ctx.json_out = out;

    pthread_mutex_unlock(&log_ctx.ctx_lock);
}

void sp_log_print_ts(int enable)
{
    atomic_store(&log_ctx.print_ts, enable);
}

int sp_log_init(enum SPLogLevel global_log_level)
{
    int ret = 0;
    sp_log_uninit();

    log_done = 0;

    pthread_mutex_lock(&log_ctx.ctx_lock);
    pthread_mutex_lock(&log_ctx.file_lock);
    pthread_mutex_lock(&log_ctx.term_lock);

    log_ctx.log_levels = av_malloc(sizeof(*log_ctx.log_levels));
    if (!log_ctx.log_levels) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    log_ctx.log_levels[0].component = av_strdup("global");
    if (!log_ctx.log_levels[0].component) {
        av_freep(&log_ctx.log_levels);
        ret = AVERROR(ENOMEM);
        goto end;
    }

    log_ctx.log_levels[0].lvl = global_log_level;
    log_ctx.num_log_levels = 1;

    log_ctx.time_offset = av_gettime_relative();
    av_log_set_callback(log_ff_cb);

end:
    pthread_mutex_unlock(&log_ctx.term_lock);
    pthread_mutex_unlock(&log_ctx.file_lock);
    pthread_mutex_unlock(&log_ctx.ctx_lock);

    return ret;
}

void sp_log_uninit(void)
{
    av_log_set_callback(av_log_default_callback);

    pthread_mutex_lock(&log_ctx.ctx_lock);
    pthread_mutex_lock(&log_ctx.file_lock);
    pthread_mutex_lock(&log_ctx.term_lock);

    for (int i = 0; i < log_ctx.ic_len; i++) {
        av_bprint_finalize(&log_ctx.ic[i].bpo, NULL);
        av_bprint_finalize(&log_ctx.ic[i].bpf, NULL);
    }
    av_freep(&log_ctx.ic);

    if (log_ctx.log_file) {
        fflush(log_ctx.log_file);
        fclose(log_ctx.log_file);
        log_ctx.log_file = NULL;
    }

    for (int i = 0; i < log_ctx.num_log_levels; i++)
        av_free(log_ctx.log_levels[i].component);
    av_freep(&log_ctx.log_levels);
    log_ctx.num_log_levels = 0;

    av_freep(&log_ctx.status.str);
    log_ctx.status.lines = 0;
    log_ctx.status.lock = 0;

    pthread_mutex_unlock(&log_ctx.term_lock);
    pthread_mutex_unlock(&log_ctx.file_lock);
    pthread_mutex_unlock(&log_ctx.ctx_lock);

    log_done = 1;
}
