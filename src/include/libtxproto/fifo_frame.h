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

#include <assert.h>
#include <libavutil/frame.h>

#include "os_compat.h"

enum SPFrameFIFOFlags {
    FRAME_FIFO_BLOCK_MAX_OUTPUT = (1 << 0),
    FRAME_FIFO_BLOCK_NO_INPUT   = (1 << 1),
    FRAME_FIFO_PULL_NO_BLOCK    = (1 << 2),
};

#define FRENAME(x) FRAME_FIFO_ ## x
#define RENAME(x)  sp_frame_ ##x
#define FNAME      enum SPFrameFIFOFlags
#define TYPE       AVFrame

/* Create */
AVBufferRef *RENAME(fifo_create)(void *opaque, int max_queued, FNAME block_flags); /* -1 = INF, 0 = none */
AVBufferRef *RENAME(fifo_ref)(AVBufferRef *src, int max_queued, FNAME block_flags);

/* Query */
int RENAME(fifo_is_full)(AVBufferRef *src);
int RENAME(fifo_get_size)(AVBufferRef *src);
int RENAME(fifo_get_max_size)(AVBufferRef *src);

/* Modify */
void RENAME(fifo_set_max_queued)(AVBufferRef *dst, int max_queued);
void RENAME(fifo_set_block_flags)(AVBufferRef *dst, FNAME block_flags);
int  RENAME(fifo_string_to_block_flags)(FNAME *dst, const char *in_str);

/* Up/downstreaming */
int RENAME(fifo_mirror)(AVBufferRef *dst, AVBufferRef *src);
int RENAME(fifo_unmirror)(AVBufferRef *dst, AVBufferRef *src);
int RENAME(fifo_unmirror_all)(AVBufferRef *dst);

/* I/O */
int   RENAME(fifo_push)(AVBufferRef *dst, TYPE *in);
TYPE *RENAME(fifo_pop)(AVBufferRef *src);
int   RENAME(fifo_pop_flags)(AVBufferRef *src, TYPE **ret, FNAME flags);
TYPE *RENAME(fifo_peek)(AVBufferRef *src);

#undef TYPE
#undef FNAME
#undef RENAME
#undef FRENAME

typedef enum SPRotation {
    ROTATION_IDENTITY = 0,
    ROTATION_ROTATE90,
    ROTATION_ROTATE180,
    ROTATION_ROTATE270,
} SPRotation;

/* opaque data each frame carries */
typedef struct FormatExtraData {
    AVRational avg_frame_rate;
    int bits_per_sample;
    AVRational time_base;
    SPRotation rotation;
} FormatExtraData;
