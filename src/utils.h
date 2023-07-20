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

#include <libtxproto/fifo_frame.h>
#include <libtxproto/fifo_packet.h>

static inline void sp_event_send_eos_frame(void *ctx, SPBufferList *events, AVBufferRef *fifo, int reason)
{
    int tmp = reason;
    sp_eventlist_dispatch(ctx, events, SP_EVENT_ON_EOS, &tmp);
    if (tmp != 0)
        sp_frame_fifo_push(fifo, NULL);
}

static inline void sp_event_send_eos_packet(void *ctx, SPBufferList *events, AVBufferRef *fifo, int reason)
{
    int tmp = reason;
    sp_eventlist_dispatch(ctx, events, SP_EVENT_ON_EOS, &tmp);
    if (tmp != 0)
        sp_packet_fifo_push(fifo, NULL);
}

static inline void sp_event_send_eos_packets(void *ctx, SPBufferList *events, AVBufferRef **fifo, int nb_fifo, int reason)
{
    int tmp = reason;
    sp_eventlist_dispatch(ctx, events, SP_EVENT_ON_EOS, &tmp);
    if (tmp != 0) {
        for (int i = 0; i < nb_fifo; i++)
            sp_packet_fifo_push(fifo[i], NULL);
    }
}
