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

#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>

#include "fifo_frame.h"
#include "utils.h"
#include "logging.h"

typedef const struct IOSysAPI {
    /**
     * API name
     */
    const char *name;

    /**
     * Initialize the API
     */
    int (*init_sys)(AVBufferRef **ctx);

    /**
     * Initialize a source/sink, but don't start it.
     */
    int (*init_io)(AVBufferRef *ctx_ref, AVBufferRef *entry, AVDictionary *opts);

    /**
     * Refs an IOSysEntry by identifier or returns NULL if none found.
     */
    AVBufferRef *(*ref_entry)(AVBufferRef *ctx_ref, uint32_t identifier);

    /**
     * IOCTL interface.
     */
    int (*ctrl)(AVBufferRef *ctx, enum SPEventType ctrl, void *arg);
} IOSysAPI;

typedef struct IOSysEntry {
    SPClass *class;

    char *desc;
    uint32_t identifier;
    uint32_t api_id;
    int is_default;

    IOSysAPI *api;
    AVBufferRef *api_ctx;

    /* Video specific input properties */
    int scale;
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
    int (*ctrl)(AVBufferRef *entry, enum SPEventType ctrl, void *arg);
    SPBufferList *events;

    /* API-specific private data */
    void *api_priv;
    void *io_priv;
} IOSysEntry;

AVBufferRef *sp_bufferlist_iosysentry_by_id(AVBufferRef *ref, void *opaque);
uint32_t sp_iosys_gen_identifier(void *ctx, uint32_t num, uint32_t extra);

extern const IOSysAPI *sp_compiled_apis[];
extern const int sp_compiled_apis_len;
