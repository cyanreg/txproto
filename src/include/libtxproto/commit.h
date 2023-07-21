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

#include <libavutil/buffer.h>

#include <libtxproto/events.h>
#include <libtxproto/txproto_main.h>

/**
 * This queues up control functions of other components to commit/discard when
 * the main context gets a commit or discard event.
 */
int sp_add_commit_fn_to_list(TXMainContext *ctx, ctrl_fn fn,
                             AVBufferRef *fn_ctx);

int sp_add_discard_fn_to_list(TXMainContext *ctx, ctrl_fn fn,
                              AVBufferRef *fn_ctx);
