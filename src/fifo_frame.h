#pragma once

#include <pthread.h>
#include <assert.h>
#include <libavutil/frame.h>

enum SPFrameFIFOFlags {
    FRAME_FIFO_BLOCK_MAX_OUTPUT = (1 << 0),
    FRAME_FIFO_BLOCK_NO_INPUT = (1 << 1),
};

#define FRENAME(x) FRAME_FIFO_ ## x
#define RENAME(x)  sp_frame_ ##x
#define FNAME      enum SPFrameFIFOFlags
#define SNAME      SPFrameFIFO
#define FREE_FN    av_frame_free
#define CLONE_FN   av_frame_clone
#define TYPE       AVFrame

#include "fifo_template.c"

#undef TYPE
#undef CLONE_FN
#undef FREE_FN
#undef SNAME
#undef FNAME
#undef RENAME
#undef FRENAME
