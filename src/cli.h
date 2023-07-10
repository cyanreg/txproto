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

#include <libtxproto/txproto_main.h>

typedef struct TXCLIContext TXCLIContext;

/* Init CLI */
int sp_cli_init(TXCLIContext **s, TXMainContext *ctx);

/* Hook an event to prompt the user to type */
int sp_cli_prompt_event(TXCLIContext *cli_ctx, AVBufferRef *event, const char *msg);

/* Free CLI state */
void sp_cli_uninit(TXCLIContext **s);
