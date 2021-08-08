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

#include <libavutil/crc.h>

#include "iosys_common.h"
#include "utils.h"
#include "logging.h"
#include "../config.h"

AVBufferRef *sp_bufferlist_iosysentry_by_id(AVBufferRef *ref, void *opaque)
{
    IOSysEntry *entry = (IOSysEntry *)ref->data;
    if (entry->identifier == *((uint32_t *)opaque))
        return ref;
    return NULL;
}

uint32_t sp_iosys_gen_identifier(void *ctx, uint32_t num, uint32_t extra)
{
    const AVCRC *table = av_crc_get_table(AV_CRC_32_IEEE);

    uint32_t crc = UINT32_MAX;

    if (ctx) {
        uint32_t id = sp_class_get_type(ctx);
        crc = av_crc(table, crc, (void *)&id, sizeof(id));
    }

    crc = av_crc(table, crc, (void *)&num, sizeof(num));
    crc = av_crc(table, crc, (void *)&extra, sizeof(extra));
    return crc;
}

const char *sp_iosys_entry_type_string(enum IOType type)
{
    static const char *io_type_map[] = {
        [SP_IO_TYPE_NONE] = "none",
        [SP_IO_TYPE_VIDEO_DISPLAY] = "display",
        [SP_IO_TYPE_AUDIO_MICROPHONE] = "microphone",
        [SP_IO_TYPE_AUDIO_MONITOR] = "monitor",
        [SP_IO_TYPE_AUDIO_OUTPUT] = "output",
    };

    if (type < 0 || type > SP_ARRAY_ELEMS(io_type_map))
        return "none";

    return io_type_map[type];
}

#ifdef HAVE_LAVD
extern const IOSysAPI src_lavd;
#endif

#ifdef HAVE_PULSEAUDIO
extern const IOSysAPI src_pulse;
#endif

#ifdef HAVE_WAYLAND
extern const IOSysAPI src_wayland;
#endif

const IOSysAPI *sp_compiled_apis[] = {
#ifdef HAVE_LAVD
    &src_lavd,
#endif
#ifdef HAVE_PULSEAUDIO
    &src_pulse,
#endif
#ifdef HAVE_WAYLAND
    &src_wayland,
#endif
};

const int sp_compiled_apis_len = SP_ARRAY_ELEMS(sp_compiled_apis);
