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

/* Bufferlist */
typedef struct SPBufferList SPBufferList;
typedef AVBufferRef *(*sp_buflist_find_fn)(AVBufferRef *entry, void *opaque);
SPBufferList *sp_bufferlist_new(void);
void          sp_bufferlist_free(SPBufferList **s);
int           sp_bufferlist_len(SPBufferList *list);
int           sp_bufferlist_copy(SPBufferList *dst, SPBufferList *src);
int           sp_bufferlist_append(SPBufferList *list, AVBufferRef *entry);
int           sp_bufferlist_append_noref(SPBufferList *list, AVBufferRef *entry);

/* Find */
AVBufferRef  *sp_bufferlist_ref(SPBufferList *list, sp_buflist_find_fn find, void *find_opaque);
AVBufferRef  *sp_bufferlist_pop(SPBufferList *list, sp_buflist_find_fn find, void *find_opaque);
AVBufferRef  *sp_bufferlist_find_fn_first(AVBufferRef *entry, void *opaque);
AVBufferRef  *sp_bufferlist_find_fn_data(AVBufferRef *entry, void *opaque);

/* Iterate */
AVBufferRef  *sp_bufferlist_iter_ref(SPBufferList *list);
void          sp_bufferlist_iter_halt(SPBufferList *list);
