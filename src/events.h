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

#include <pthread.h>

#include "bufferlist.h"

/**
 * Event flags.
 *
 * SP_EVENT_ON_*   flags dictate when an event will run once sp_eventlist_dispatch()
 *                 gets called. Events with such flags may not have any SP_EVENT_CTRL_*
 *                 flags set.
 *
 * SP_EVENT_TYPE_* flags describe which parts the event touches. This allows
 *                 users to filter through events.
 *
 * SP_EVENT_CTRL_* flags describe a control event. These modify contexts.
 *                 Those described as **immediate** do not require a commit.
 *                 Events with such flags may not have any SP_EVENT_ON_* flags
 *                 set.
 *
 * SP_EVENT_FLAG_* flags modify events, and may be added to any event.
 */
typedef enum SPEventType {
    /* [0:15] - Triggers */
    SP_EVENT_ON_COMMIT       = (1ULL <<  0), /* NULL data, removes all events marked with ON_DISCARD */
    SP_EVENT_ON_DISCARD      = (1ULL <<  1), /* NULL data, runs all ON_DISCARD events, removes all new events */
    SP_EVENT_ON_CONFIG       = (1ULL <<  2), /* NULL data, emitted before configuring */
    SP_EVENT_ON_INIT         = (1ULL <<  3), /* NULL data, emitted after initialization */
    SP_EVENT_ON_CHANGE       = (1ULL <<  4), /* NULL data */
    SP_EVENT_ON_STATS        = (1ULL <<  5), /* 0-terminated SPGenericData array */
    SP_EVENT_ON_EOS          = (1ULL <<  6), /* NULL data */
    SP_EVENT_ON_ERROR        = (1ULL <<  7), /* 32-bit integer */
    SP_EVENT_ON_DESTROY      = (1ULL <<  8), /* NULL data, or a 0-terminated SPGenericData array */
    SP_EVENT_ON_OUTPUT       = (1ULL <<  9), /* SPRationalValue */
    SP_EVENT_ON_MASK         = (((1ULL << 16) - 1) <<  0), /* 16 bits reserved for event triggers */

    /* [16:31] - Types, if ORd when comitting, will only run events with that type */
    SP_EVENT_TYPE_LINK       = (1ULL << 16), /* NOTE: This is treated as a special pseudo-control event! */
    SP_EVENT_TYPE_SOURCE     = (1ULL << 17),
    SP_EVENT_TYPE_SINK       = (1ULL << 18),
    SP_EVENT_TYPE_FILTER     = (1ULL << 19),
    SP_EVENT_TYPE_BSF        = (1ULL << 20),
    SP_EVENT_TYPE_ENCODER    = (1ULL << 21),
    SP_EVENT_TYPE_DECODER    = (1ULL << 22),
    SP_EVENT_TYPE_MUXER      = (1ULL << 23),
    SP_EVENT_TYPE_DEMUXER    = (1ULL << 24),
    SP_EVENT_TYPE_MASK       = (((1ULL << 16) - 1) << 16), /* 16 bits reserved for event type */

    /* [32: 47] - Controls - same as type, but at least either a type or a ctrl must be present */
    SP_EVENT_CTRL_START      = (1ULL << 32), /* Pointer to an atomic int64_t that must be valid at the time of commit */
    SP_EVENT_CTRL_STOP       = (1ULL << 33), /* No aruments */
    SP_EVENT_CTRL_OPTS       = (1ULL << 34), /* Pointer to an AVDictionary, will be validated before returning */
    SP_EVENT_CTRL_FLUSH      = (1ULL << 35), /* Flush all input (for sources) or output (for sinks) */
    SP_EVENT_CTRL_COMMAND    = (1ULL << 36), /* AVDictionary * */ /* For custom lavf commands, argument must be a string */
    SP_EVENT_CTRL_NEW_EVENT  = (1ULL << 37), /* Immediate */ /* Pointer to an AVBufferRef */
    SP_EVENT_CTRL_DEL_EVENT  = (1ULL << 38), /* Immediate */ /* Pointer to an uint32_t identifier */
    SP_EVENT_CTRL_COMMIT     = (1ULL << 39), /* Immediate */ /* Optional pointer to an SPEventType for filtering */
    SP_EVENT_CTRL_DISCARD    = (1ULL << 40), /* Immediate */ /* Unrefs all uncommited events */
    SP_EVENT_CTRL_SIGNAL     = (1ULL << 41), /* Immediate */ /* Must be ORd with a trigger, arg must be an AVBufferRef event */
    SP_EVENT_CTRL_MASK       = (((1ULL << 16) - 1) << 32), /* 16 bits reserved for control events */

    /* [48: 63] - Flags */
    SP_EVENT_FLAG_UNIQUE     = (1ULL << 48), /* Do not deduplicate this event, even if a matching even appears */
    SP_EVENT_FLAG_DEPENDENCY = (1ULL << 49), /* Event has a dependency, and upon dispatching, will wait to be signalled */
    SP_EVENT_FLAG_IMMEDIATE  = (1ULL << 50), /* If added to a ctrl will run the event immediately instead of on commit */
    SP_EVENT_FLAG_EXPIRED    = (1ULL << 51), /* Event will not be ran and will be deleted as soon as possible */
    SP_EVENT_FLAG_ONESHOT    = (1ULL << 52), /* Event will run only once. If added to a ctrl, will unref all events that ran */
    SP_EVENT_FLAG_MASK       = (((1ULL << 16) - 1) << 48), /* 16 bits reserved for flags */
} SPEventType;

typedef struct SPEvent SPEvent;

/**
 * Returns a SPEventType with flags set which match the context's flags
 * (muxer, demuxer, decoder, etc.). Convenience function, users are still
 * supposed to fill out the rest.
 */
SPEventType sp_class_to_event_type(void *ctx);

/**
 * Event callback type.
 *
 * @param  event         The event - do not unreference this directly
 * @param  callback_ctx  Same as av_buffer_get_opaque(event)
 * @param  ctx           The value given to sp_event_create.ctx
 * @param  dep_ctx       Set to p_event_create.dep_ctx, or if NULL, sp_eventlist_dispatch.ctx
 * @param  data          The value given to sp_eventlist_dispatch.data
 */
typedef int (*event_fn)(AVBufferRef *event_ref, void *callback_ctx, void *ctx,
                        void *dep_ctx, void *data);

/**
 * Event destruction callback.
 *
 * @param  callback_ctx  The callback context
 * @param  ctx           The value given to sp_event_create.ctx
 * @param  dep_ctx       Set to p_event_create.dep_ctx
 */
typedef void (*event_free)(void *callback_ctx, void *ctx, void *dep_ctx);

/**
 * Create an event
 *
 * @param  fn            Callback to perform the event
 * @param  destroy_cb    Function to call upon event destruction
 * @param  callback_ctx  Callback context to allocate, will appear in in the output's opaque field
 * @param  lock          Optional mutex to lock while executing, replaces the internal lock created
 * @param  type          Event flags
 * @param  ctx           Context on which the event operates on
 * @param  dep_ctx       Optional context, for which which this event relies on to be initialized
 *
 * @return The event, inside a reference-counted buffer, or NULL on out-of-memory
 *
 * @note   Specifying dep_ctx forms a dependency chain, where the event execution
 *         will occur after the last event in the inserted list where the
 *         context at dep_ctx was the target of an event.
 *
 * @note   If dep_ctx is NULL, then the context given to sp_eventlist_dispatch
 *         will be used when calling the event callback.
 */
AVBufferRef *sp_event_create(event_fn cb,
                             event_free destroy_cb,
                             size_t callback_ctx_size,
                             pthread_mutex_t *lock,
                             SPEventType type,
                             void *ctx,
                             void *dep_ctx);

/**
 * Add an event to an event list.
 *
 * @param  ctx    Context (used for logging only)
 * @param  list   Event list to insert the event into
 * @param  event  The event
 * @param  ref    Whether to reference the event (1) or take ownership on success (0)
 *
 * @note   If there's already an event with the same SP_EVENT_CTRL_ or
 *         SP_EVENT_TYPE_LINK flag, operating on the same contexts, event
 *         deduplication will happen, and the old event will be quietly
 *         removed from the list, unless either of the events has either the
 *         SP_EVENT_FLAG_UNIQUE flag or the SP_EVENT_CTRL_OPTS flag in its type.
 */
int sp_eventlist_add(void *ctx, SPBufferList *list, AVBufferRef *event, int ref);

/**
 * Same as sp_eventlist_add, but will signal and run the event once the list
 * gets dispatched with a type which matches the when argument.
 *
 * Event must have a SP_EVENT_FLAG_DEPENDENCY flag in its type, or this function
 * will return an error.
 *
 * @return  0 on success, negative value on error
 */
int sp_eventlist_add_signal(void *ctx, SPBufferList *list,
                            AVBufferRef *event, SPEventType when, int ref);

/**
 * Will unref the event, and expire it, causing it to not run and instead be
 * deleted as soon as possible.
 */
void sp_event_unref_expire(AVBufferRef **buf);

/**
 * Will block until event is signalled. Will do nothing if event lacks a
 * SP_EVENT_FLAG_DEPENDENCY flag in its type.
 */
void sp_event_unref_await(AVBufferRef **buf);

/**
 * Unrefs all events on DESTROY, only runs those which are marked as destroy
 */
int sp_eventlist_dispatch(void *ctx, SPBufferList *list, SPEventType type, void *data);

/**
 * Returns type(s, if type is a mask) if sp_eventlist_dispatch has been called
 * at least once with type
 */
SPEventType sp_eventlist_has_dispatched(SPBufferList *list, SPEventType type);

/**
 * Same as above, but if there's an outstanding one
 */
SPEventType sp_eventlist_has_queued(SPBufferList *list, SPEventType type);

/**
 * Returns a string of all flags, must be freed.
 */
char *sp_event_flags_to_str_buf(AVBufferRef *event);

/**
 * Returns a string of all flags, must be freed.
 */
char *sp_event_flags_to_str(SPEventType flags);

/**
 * Parses a string into flags.
 */
int sp_event_string_to_flags(void *ctx, SPEventType *dst, const char *in_str);

/**
 * Get the event's unique ID.
 */
uint64_t sp_event_get_id(AVBufferRef *event);

/**
 * Control function type. All interfaces with a control function must use
 * this template.
 */
typedef int (*ctrl_fn)(AVBufferRef *ctx_ref, SPEventType ctrl, void *arg);
