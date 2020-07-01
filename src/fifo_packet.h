#pragma once

#include <pthread.h>
#include <assert.h>
#include <libavcodec/packet.h>

enum SPPacketFIFOFlags {
    PACKET_FIFO_BLOCK_MAX_OUTPUT = (1 << 0),
    PACKET_FIFO_BLOCK_NO_INPUT   = (1 << 1),
};

#define FRENAME(x) PACKET_FIFO_ ## x
#define RENAME(x)  sp_packet_ ##x
#define FNAME      enum SPPacketFIFOFlags
#define TYPE       AVPacket

#include "fifo_template.h"

#undef TYPE
#undef FNAME
#undef RENAME
#undef FRENAME
