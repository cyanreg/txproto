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
#include <libavutil/log.h>

enum SPType {
    SP_TYPE_NONE = 0,
    SP_TYPE_INTERFACE = (1 << 0),
    SP_TYPE_CONTEXT = (1 << 1),
    SP_TYPE_EXTERNAL = (1 << 2),
    SP_TYPE_SCRIPT = (1 << 3),

    SP_TYPE_VIDEO_SOURCE = (1 << 4),
    SP_TYPE_VIDEO_SINK = (1 << 5),
    SP_TYPE_VIDEO_BIDIR = SP_TYPE_VIDEO_SOURCE | SP_TYPE_VIDEO_SINK,

    SP_TYPE_AUDIO_SOURCE = (1 << 8),
    SP_TYPE_AUDIO_SINK = (1 << 9),
    SP_TYPE_AUDIO_BIDIR = SP_TYPE_AUDIO_SOURCE | SP_TYPE_AUDIO_SINK,

    SP_TYPE_SUB_SOURCE = (1 << 12),
    SP_TYPE_SUB_SINK = (1 << 13),
    SP_TYPE_SUB_BIDIR = SP_TYPE_SUB_SOURCE | SP_TYPE_SUB_SINK,

    SP_TYPE_SOURCE = SP_TYPE_VIDEO_SOURCE | SP_TYPE_AUDIO_SOURCE | SP_TYPE_SUB_SOURCE,
    SP_TYPE_SINK = SP_TYPE_VIDEO_SINK | SP_TYPE_AUDIO_SINK | SP_TYPE_SUB_SINK,
    SP_TYPE_INOUT = SP_TYPE_SOURCE | SP_TYPE_SINK,

    SP_TYPE_CLOCK_SOURCE = (1 << 14),
    SP_TYPE_CLOCK_SINK = (1 << 15),

    SP_TYPE_FILTER = (1 << 16),

    SP_TYPE_ENCODER = (1 << 20),
    SP_TYPE_DECODER = (1 << 21),
    SP_TYPE_CODEC = SP_TYPE_ENCODER | SP_TYPE_DECODER,

    SP_TYPE_BSF = (1 << 24),

    SP_TYPE_MUXER = (1 << 28),
    SP_TYPE_DEMUXER = (1 << 29),
    SP_TYPE_MUXING = SP_TYPE_MUXER | SP_TYPE_DEMUXER,
};

enum SPLogLevel {
    SP_LOG_QUIET    = -(1 << 0),
    SP_LOG_FATAL    =  (0 << 0),
    SP_LOG_ERROR    = +(1 << 0),
    SP_LOG_WARN     = +(1 << 1),
    SP_LOG_INFO     = +(1 << 2),
    SP_LOG_VERBOSE  = +(1 << 3),
    SP_LOG_DEBUG    = +(1 << 4),
    SP_LOG_TRACE    = +(1 << 5),

    /* Minimal styling, no recording, for CLI feedback and such */
    SP_NOLOG        = +(1 << 7),
    /* List logging */
    SP_LOG_LIST     = +(1 << 8),
    SP_LOG_LIST_END = +(1 << 9),
};

typedef struct SPClass SPClass;

#if defined(__GNUC__) || defined(__clang__)
#define sp_printf_format(fmtpos, attrpos) __attribute__((__format__(__printf__, fmtpos, attrpos)))
#else
#define sp_printf_format(fmtpos, attrpos)
#endif

/* Init/uninit functions */
int sp_log_init(enum SPLogLevel global_log_level);
void sp_log_uninit(void);

enum SPStatusFlags {
    SP_STATUS_UNLOCK   = 1 << 0,
    SP_STATUS_LOCK     = 1 << 1,
    SP_STATUS_NO_CLEAR = 1 << 2, /* Don't clear the old status */
};

/* Set a status that will always be on top (except for a prompt, if enabled). */
int sp_log_set_status(const char *status, enum SPStatusFlags flags);

/* Called once on starting to print a new line, and once after the newline has been printed */
void sp_log_set_prompt_callback(void *ctx, void (*cb)(void *ctx, int newline_started));

/* Context level */
enum SPLogLevel sp_log_get_ctx_lvl(const char *component);
int sp_log_set_ctx_lvl(const char *component, enum SPLogLevel lvl);
int sp_log_set_ctx_lvl_str(const char *component, const char *lvl);

/* JSON output setting, negative to leave as-is */
void sp_log_set_json_out(int file, int std);

/* Main logging */
void sp_log(void *ctx, int level, const char *fmt, ...) sp_printf_format(3, 4);

/* Set log file */
int sp_log_set_file(const char *path);

/* Enable/disable timestamp/component printing */
void sp_log_print_ts(int enable);

/* Class allocation */
int sp_class_alloc(void *ctx, const char *name, enum SPType type, void *parent);
void sp_class_free(void *ctx);

/* Getters */
uint32_t sp_class_get_id(void *ctx);
const char *sp_class_get_name(void *ctx);
const char *sp_class_get_parent_name(void *ctx);
int sp_class_set_name(void *ctx, const char *name);
enum SPType sp_class_get_type(void *ctx);
const char *sp_class_type_string(void *ctx);
enum SPType sp_avcategory_to_type(AVClassCategory category);
