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

#include <libavutil/buffer.h>
#include <libavutil/rational.h>

#include <libtxproto/events.h>
#include <libtxproto/log.h>

enum IOType {
    SP_IO_TYPE_NONE = 0,

    SP_IO_TYPE_VIDEO_DISPLAY = 1,

    SP_IO_TYPE_AUDIO_MICROPHONE = 2,
    SP_IO_TYPE_AUDIO_MONITOR = 3,
    SP_IO_TYPE_AUDIO_OUTPUT = 4,
};

typedef const struct IOSysAPI IOSysAPI;

typedef struct IOSysEntry {
    SPClass *class;

    char *desc;
    enum IOType type;

    uint32_t identifier;
    uint32_t api_id;
    int is_default;

    IOSysAPI *api;
    AVBufferRef *api_ctx;

    /* Video specific input properties */
    int scale;
    int x;
    int y;
    int width;
    int height;
    AVRational framerate;

    /* Audio specific input properties */
    int sample_rate;
    int sample_fmt;
    int channels;
    uint64_t channel_layout;
    float volume;

    /* Input/output FIFO */
    AVBufferRef *frames;

    /* Command interface */
    int (*ctrl)(AVBufferRef *entry, SPEventType ctrl, void *arg);
    SPBufferList *events;

    /* API-specific private data */
    void *api_priv;
    void *io_priv;
} IOSysEntry;

const char *sp_iosys_entry_type_string(enum IOType type);

AVBufferRef *sp_io_alloc(TXMainContext *ctx,
                         const char **api_list,
                         event_fn source_event_cb,
                         event_free source_event_free,
                         size_t callback_ctx_size);

int sp_io_init(TXMainContext *ctx,
               AVBufferRef *source_event,
               const char **api_list);

AVBufferRef *sp_io_create(TXMainContext *ctx,
                          uint32_t identifier,
                          AVDictionary *opts);
