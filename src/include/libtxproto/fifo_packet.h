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
#include <libavcodec/packet.h>

#define FRENAME(x) PACKET_FIFO_ ## x
#define RENAME(x)  sp_packet_ ##x
#define FNAME      SPPacketFIFOFlags
#define TYPE       AVPacket

typedef enum FNAME {
    FRENAME(BLOCK_MAX_OUTPUT) = (1 << 0),
    FRENAME(BLOCK_NO_INPUT)   = (1 << 1),
    FRENAME(PULL_NO_BLOCK)    = (1 << 2),
    FRENAME(PULL_POKE)        = (1 << 3),
} FNAME;

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
int   RENAME(fifo_poke)(AVBufferRef *dst);
int   RENAME(fifo_push)(AVBufferRef *dst, TYPE *in);
TYPE *RENAME(fifo_pop)(AVBufferRef *src);
int   RENAME(fifo_pop_flags)(AVBufferRef *src, TYPE **dst, FNAME flags);
TYPE *RENAME(fifo_peek)(AVBufferRef *src);
int   RENAME(fifo_peek_flags)(AVBufferRef *src, TYPE **dst, FNAME flags);

#undef TYPE
#undef FNAME
#undef RENAME
#undef FRENAME
