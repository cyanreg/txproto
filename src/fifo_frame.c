#include "fifo_frame.h"

#define FRENAME(x)     FRAME_FIFO_ ## x
#define RENAME(x)      sp_frame_ ##x
#define PRIV_RENAME(x) frame_ ##x
#define FNAME          enum SPFrameFIFOFlags
#define SNAME          SPFrameFIFO
#define FREE_FN        av_frame_free
#define CLONE_FN(x)    ((x) ? av_frame_clone((x)) : NULL)
#define TYPE           AVFrame

#include "fifo_template.c"

#undef TYPE
#undef CLONE_FN
#undef FREE_FN
#undef SNAME
#undef FNAME
#undef PRIV_RENAME
#undef RENAME
#undef FRENAME
