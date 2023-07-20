#include "libavutil/mem.h"
#include <libavutil/time.h>

#include <libtxproto/events.h>

#include <libtxproto/epoch.h>

static int epoch_event_cb(AVBufferRef *event, void *callback_ctx, void *_ctx,
                          void *dep_ctx, void *data)
{
    int err = 0;
    TXMainContext *ctx = _ctx;
    EpochEventCtx *epoch_ctx = callback_ctx;

    int64_t val;
    switch(epoch_ctx->mode) {
    case EP_MODE_OFFSET:
        val = av_gettime_relative() + epoch_ctx->value;
        break;
    case EP_MODE_SYSTEM:
        val = 0;
        break;
    case EP_MODE_SOURCE:
        sp_log(ctx, SP_LOG_ERROR, "Warning epoch mode source, using epoch of system!\n");
        val = 0;
        break;
    case EP_MODE_EXTERNAL:
        val = (*epoch_ctx->external_cb)(av_gettime_relative(), epoch_ctx->external_arg);
        break;
    }

    atomic_store(&ctx->epoch_value, val);

    return err;
}

static void epoch_event_free(void *callback_ctx, void *_ctx, void *dep_ctx)
{
    EpochEventCtx *epoch_ctx = callback_ctx;

    av_buffer_unref(&epoch_ctx->src_ref);
    av_buffer_unref(&epoch_ctx->external_arg);
}

AVBufferRef *sp_epoch_event_new(TXMainContext *ctx)
{
    const SPEventType flags = SP_EVENT_FLAG_ONESHOT | SP_EVENT_ON_COMMIT;

    return sp_event_create(epoch_event_cb,
                           epoch_event_free,
                           sizeof(EpochEventCtx),
                           NULL,
                           flags,
                           ctx,
                           NULL);
}

int sp_epoch_event_set_source(AVBufferRef *epoch_event,
                              AVBufferRef *obj)
{
    EpochEventCtx *epoch_ctx = av_buffer_get_opaque(epoch_event);

    enum SPType type = sp_class_get_type(obj->data);
    if (type != SP_TYPE_CLOCK_SOURCE)
        return AVERROR(EINVAL);

    epoch_ctx->mode = EP_MODE_SOURCE;
    epoch_ctx->src_ref = av_buffer_ref(obj);

    return 0;
}

int sp_epoch_event_set_offset(AVBufferRef *epoch_event,
                              int64_t value)
{
    EpochEventCtx *epoch_ctx = av_buffer_get_opaque(epoch_event);

    epoch_ctx->mode = EP_MODE_OFFSET;
    epoch_ctx->value = value;

    return 0;
}

int sp_epoch_event_set_system(AVBufferRef *epoch_event)
{
    EpochEventCtx *epoch_ctx = av_buffer_get_opaque(epoch_event);

    epoch_ctx->mode = EP_MODE_SYSTEM;

    return 0;
}

int sp_epoch_event_set_external(AVBufferRef *epoch_event,
                                int (*external_cb)(int64_t systemtime, AVBufferRef *arg),
                                AVBufferRef *external_arg)
{
    EpochEventCtx *epoch_ctx = av_buffer_get_opaque(epoch_event);

    epoch_ctx->mode = EP_MODE_EXTERNAL;
    epoch_ctx->external_cb = external_cb;
    epoch_ctx->external_arg = external_arg;

    return 0;
}
