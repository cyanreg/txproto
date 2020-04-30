#pragma once

#include <pthread.h>
#include <assert.h>
#include <libavcodec/packet.h>

enum SPPacketFIFOFlags {
    PACKET_FIFO_BLOCK_MAX_OUTPUT = (1 << 0),
    PACKET_FIFO_BLOCK_NO_INPUT = (1 << 1),
};

#define FRENAME(x) PACKET_FIFO_ ## x
#define RENAME(x)  sp_packet_ ##x
#define FNAME      enum SPPacketFIFOFlags
#define SNAME      SPPacketFIFO
#define FREE_FN    av_packet_free
#define CLONE_FN   av_packet_clone
#define TYPE       AVPacket

#include "fifo_template.c"

#undef TYPE
#undef CLONE_FN
#undef FREE_FN
#undef SNAME
#undef FNAME
#undef RENAME
#undef FRENAME
