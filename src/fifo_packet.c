#include <libtxproto/fifo_packet.h>

#define FRENAME(x)     PACKET_FIFO_ ## x
#define RENAME(x)      sp_packet_ ##x
#define PRIV_RENAME(x) packet ##x
#define FNAME          enum SPPacketFIFOFlags
#define SNAME          SPPacketFIFO
#define FREE_FN        av_packet_free
#define CLONE_FN(x)    ((x) ? av_packet_clone((x)) : NULL)
#define TYPE           AVPacket

#include "fifo_template.c"

#undef TYPE
#undef CLONE_FN
#undef FREE_FN
#undef SNAME
#undef FNAME
#undef PRIV_RENAME
#undef RENAME
#undef FRENAME
