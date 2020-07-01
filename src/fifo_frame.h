#pragma once

#include <pthread.h>
#include <assert.h>
#include <libavutil/frame.h>

enum SPFrameFIFOFlags {
    FRAME_FIFO_BLOCK_MAX_OUTPUT = (1 << 0),
    FRAME_FIFO_BLOCK_NO_INPUT   = (1 << 1),
};

#define FRENAME(x) FRAME_FIFO_ ## x
#define RENAME(x)  sp_frame_ ##x
#define FNAME      enum SPFrameFIFOFlags
#define TYPE       AVFrame

#include "fifo_template.h"

#undef TYPE
#undef FNAME
#undef RENAME
#undef FRENAME

/* opaque data each frame carries */
typedef struct FormatExtraData {
    AVRational avg_frame_rate;
    int bits_per_sample;
    AVRational time_base;
} FormatExtraData;
