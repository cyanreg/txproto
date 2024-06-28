#include <libavutil/buffer.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>

#include <libtxproto/commit.h>
#include <libtxproto/control.h>
#include <libtxproto/decode.h>
#include <libtxproto/demux.h>
#include <libtxproto/encode.h>
#include <libtxproto/epoch.h>
#include <libtxproto/events.h>
#include <libtxproto/filter.h>
#include <libtxproto/link.h>
#include <libtxproto/mux.h>
#include <libtxproto/txproto_main.h>
#include <libtxproto/txproto.h>
#include "iosys_common.h"

TXMainContext *tx_new(void)
{
    TXMainContext *ctx = av_mallocz(sizeof(*ctx));
    return ctx;
}

int tx_init(TXMainContext *ctx)
{
    int err;

    if ((err = sp_log_init(SP_LOG_INFO)) < 0)
        return err;

    if ((err = sp_class_alloc(ctx, "tx", SP_TYPE_NONE, NULL)) < 0) {
        sp_log_uninit();
        av_free(ctx);
        return err;
    }

    /* Print timestamps in logs */
    sp_log_print_ts(1);

    ctx->events = sp_bufferlist_new();
    ctx->ext_buf_refs = sp_bufferlist_new();
    ctx->epoch_value = ATOMIC_VAR_INIT(0);
    ctx->source_update_cb_ref = LUA_NOREF;

    return 0;
}

void tx_free(TXMainContext *ctx)
{
    if (!ctx)
        return;

    sp_log_set_status(NULL, SP_STATUS_LOCK | SP_STATUS_NO_CLEAR);

    /* Discard queued events */
    sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_DISCARD, NULL);

    /* Free lists that may carry contexts around */
    sp_bufferlist_free(&ctx->events);

    /* Free all contexts */
    sp_bufferlist_free(&ctx->ext_buf_refs);

    /* Shut the I/O APIs off */
    if (ctx->io_api_ctx) {
        for (int i = 0; i < sp_compiled_apis_len; i++)
            if (ctx->io_api_ctx[i])
                av_buffer_unref(&ctx->io_api_ctx[i]);
        av_free(ctx->io_api_ctx);
    }

    /* Stop logging */
    sp_log_uninit();

    /* Free any auxiliary data */
    sp_class_free(ctx);
    av_free(ctx);
}

int tx_epoch_set(TXMainContext *ctx, int64_t value)
{
    AVBufferRef *epoch_event = sp_epoch_event_new(ctx);

    int err = sp_epoch_event_set_offset(epoch_event, value);
    if (err < 0) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to set epoch offset: %s!", av_err2str(err));
        goto err;
    }

    err = sp_eventlist_add(ctx, ctx->events, epoch_event, 0);
    if (err < 0)
        goto err;

    return 0;

err:
    av_buffer_unref(&epoch_event);
    return err;
}

int tx_commit(TXMainContext *ctx)
{
    return sp_eventlist_dispatch(ctx, ctx->events, SP_EVENT_ON_COMMIT, NULL);
}

int tx_ctrl(TXMainContext *ctx, AVBufferRef *ref, SPEventType flags, void *arg)
{
    return sp_generic_ctrl(ctx, ref, flags, arg);
}

AVBufferRef *tx_demuxer_create(
    TXMainContext *ctx,
    const char *name,
    const char *in_url,
    const char *in_format,
    AVDictionary *start_options,
    AVDictionary *init_opts
) {
    int err;
    AVBufferRef *mctx_ref = sp_demuxer_alloc();
    DemuxingContext *mctx = (DemuxingContext *)mctx_ref->data;

    mctx->name = name;
    mctx->in_url = in_url;
    mctx->in_format = in_format;
    mctx->start_options = start_options;

    err = sp_demuxer_init(mctx_ref);
    if (err < 0) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to init demuxer: %s!", av_err2str(err));
        goto err;
    }

    if (init_opts) {
        err = sp_demuxer_ctrl(mctx_ref, SP_EVENT_CTRL_OPTS | SP_EVENT_FLAG_IMMEDIATE, init_opts);
        if (err < 0) {
            sp_log(ctx, SP_LOG_ERROR, "Unable to set options: %s!", av_err2str(err));
            goto err;
        }
    }

    sp_bufferlist_append_noref(ctx->ext_buf_refs, mctx_ref);

    return mctx_ref;

err:
    av_buffer_unref(&mctx_ref);
    return NULL;
}

AVBufferRef *tx_decoder_create(
    TXMainContext *ctx,
    const char *dec_name,
    AVDictionary *init_opts
) {
    int err;
    AVBufferRef *dctx_ref = sp_decoder_alloc();
    DecodingContext *dctx = (DecodingContext *)dctx_ref->data;

    dctx->codec = avcodec_find_decoder_by_name(dec_name);
    if (!dctx->codec) {
        sp_log(ctx, SP_LOG_ERROR,"Decoder \"%s\" not found!", dec_name);
        goto err;
    }

    err = sp_decoder_init(dctx_ref);
    if (err < 0) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to init decoder: %s!", av_err2str(err));
        goto err;
    }

    if (init_opts) {
        err = sp_decoder_ctrl(dctx_ref, SP_EVENT_CTRL_OPTS | SP_EVENT_FLAG_IMMEDIATE, init_opts);
        if (err < 0) {
            sp_log(ctx, SP_LOG_ERROR, "Unable to set options: %s!", av_err2str(err));
            goto err;
        }
    }

    sp_bufferlist_append_noref(ctx->ext_buf_refs, dctx_ref);

    return dctx_ref;

err:
    av_buffer_unref(&dctx_ref);
    return NULL;
}

AVBufferRef *tx_encoder_create(
    TXMainContext *ctx,
    const char *enc_name,
    const char *name,
    AVDictionary *options,
    AVDictionary *init_opts
) {
    int err;
    AVBufferRef *ectx_ref = sp_encoder_alloc();
    EncodingContext *ectx = (EncodingContext *)ectx_ref->data;

    ectx->codec = avcodec_find_encoder_by_name(enc_name);
    if (!ectx->codec) {
        sp_log(ctx, SP_LOG_ERROR, "Encoder \"%s\" not found!", enc_name);
        goto err;
    }

    ectx->name = name;

    err = sp_encoder_init(ectx_ref);
    if (err < 0) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to init encoder: %s!", av_err2str(err));
        goto err;
    }

    ectx->codec_config = options;

    if (init_opts) {
        err = sp_encoder_ctrl(ectx_ref, SP_EVENT_CTRL_OPTS | SP_EVENT_FLAG_IMMEDIATE, init_opts);
        if (err < 0) {
            sp_log(ctx, SP_LOG_ERROR, "Unable to set options: %s!", av_err2str(err));
            goto err;
        }
    }

    sp_bufferlist_append_noref(ctx->ext_buf_refs, ectx_ref);

    return ectx_ref;

err:
    av_buffer_unref(&ectx_ref);
    return NULL;
}

AVBufferRef *tx_muxer_create(
    TXMainContext *ctx,
    const char *out_url,
    const char *out_format,
    AVDictionary *options,
    AVDictionary *init_opts
) {
    int err;
    AVBufferRef *mctx_ref = sp_muxer_alloc();
    MuxingContext *mctx = (MuxingContext *)mctx_ref->data;

    mctx->out_url = out_url;
    mctx->out_format = out_format;

    err = sp_muxer_init(mctx_ref);
    if (err < 0) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to init muxer: %s!", av_err2str(err));
        goto err;
    }

    if (options) {
        err = sp_set_avopts(mctx, mctx->avf, options);
        if (err < 0) {
            sp_log(ctx, SP_LOG_ERROR, "Unable to configure muxer: %s!", av_err2str(err));
            goto err;
        }
    }

    if (init_opts) {
        err = sp_muxer_ctrl(mctx_ref, SP_EVENT_CTRL_OPTS | SP_EVENT_FLAG_IMMEDIATE, init_opts);
        if (err < 0) {
            sp_log(ctx, SP_LOG_ERROR, "Unable to set options: %s!", av_err2str(err));
            goto err;
        }
    }

    sp_bufferlist_append_noref(ctx->ext_buf_refs, mctx_ref);

    return mctx_ref;

err:
    av_buffer_unref(&mctx_ref);
    return NULL;
}

AVBufferRef *tx_filtergraph_create(
    TXMainContext *ctx,
    const char *graph,
    enum AVHWDeviceType hwctx_type,
    AVDictionary *init_opts
) {
    int err;
    AVBufferRef *fctx_ref = sp_filter_alloc();

    const char *name = NULL;
    AVDictionary *opts = NULL;
    char **in_pads = NULL;
    char **out_pads = NULL;

    err = sp_init_filter_graph(fctx_ref, name, graph, in_pads, out_pads, opts, hwctx_type);
    if (err < 0) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to init filter: %s!", av_err2str(err));
        goto err;
    }

    if (init_opts) {
        err = sp_filter_ctrl(fctx_ref, SP_EVENT_CTRL_OPTS | SP_EVENT_FLAG_IMMEDIATE, init_opts);
        if (err < 0) {
            sp_log(ctx, SP_LOG_ERROR, "Unable to set options: %s!\n", av_err2str(err));
            goto err;
        }
    }
    av_dict_free(&init_opts);

    sp_bufferlist_append_noref(ctx->ext_buf_refs, fctx_ref);

    return fctx_ref;

err:
    av_buffer_unref(&fctx_ref);
    return NULL;
}

int tx_link(
    TXMainContext *ctx,
    AVBufferRef *src,
    AVBufferRef *dst,
    int src_stream_id
) {
    return sp_generic_link(
        ctx,
        src,
        dst,
        1, // autostart,
        NULL, // src_pad_name,
        NULL, // dst_pad_name,
        src_stream_id,
        NULL // src_stream_desc
    );
}

int tx_filtergraph_command(TXMainContext *ctx, AVBufferRef *graph,
                           const char *filter_target, AVDictionary *commands)
{
    int err = 0;

    err = av_dict_set(&commands, "sp_filter_target", filter_target, 0);
    if (err < 0)
        return err;

    err = sp_filter_ctrl(graph, SP_EVENT_CTRL_COMMAND, commands);
    if (err < 0)
        return err;

    err = sp_add_commit_fn_to_list(ctx, sp_filter_ctrl, graph);
    if (err < 0)
        return err;

    av_dict_free(&commands);

    return 0;
}

int tx_destroy(
    TXMainContext *ctx,
    AVBufferRef **ref
) {
    (void)sp_bufferlist_pop(ctx->ext_buf_refs, sp_bufferlist_find_fn_data, *ref);
    av_buffer_unref(ref);
    return 0;
}

int tx_event_register(
    TXMainContext *ctx,
    AVBufferRef *target,
    AVBufferRef *event
) {
    ctrl_fn target_ctrl_fn = sp_get_ctrl_fn(target->data);
    if (!target_ctrl_fn) {
        sp_log(ctx, SP_LOG_ERROR, "Unsupported CTRL type: %s!",
               sp_class_type_string(target->data));
        return AVERROR(EINVAL);
    }

    return target_ctrl_fn(target, SP_EVENT_CTRL_NEW_EVENT, event);
}

typedef struct SourceEventCtx {
    int (*cb)(IOSysEntry *entry, void *userdata);
    void *userdata;
} SourceEventCtx;

static int source_event_cb(AVBufferRef *event, void *callback_ctx, void *ctx,
                           void *dep_ctx, void *data)
{
    SourceEventCtx *source_cb_ctx = callback_ctx;
    IOSysEntry *entry = dep_ctx;

    return (*source_cb_ctx->cb)(entry, source_cb_ctx->userdata);
}

int tx_event_destroy(
    TXMainContext *ctx,
    AVBufferRef *event
) {
    (void)sp_bufferlist_pop(ctx->ext_buf_refs, sp_bufferlist_find_fn_data, event);
    sp_event_unref_expire(&event);

    return 0;
}

AVBufferRef *tx_io_register_cb(
    TXMainContext *ctx,
    const char **api_list,
    int (*cb)(IOSysEntry *entry, void *userdata),
    void *userdata
) {
    AVBufferRef *source_event;
    source_event = sp_io_alloc(ctx, (const char **)api_list, source_event_cb,
                               NULL, sizeof(SourceEventCtx));
    if (!source_event)
        return NULL;

    SourceEventCtx *source_event_ctx = av_buffer_get_opaque(source_event);
    source_event_ctx->cb = cb;
    source_event_ctx->userdata = userdata;

    int err = sp_io_init(ctx, source_event, (const char **)api_list);
    if (err < 0) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to reference %s!", "function");
        av_buffer_unref(&source_event);
        return NULL;
    }

    return source_event;
}

AVBufferRef *tx_io_create(TXMainContext *ctx,
                          uint32_t identifier,
                          AVDictionary *opts)
{
    return sp_io_create(ctx, identifier, opts);
}
