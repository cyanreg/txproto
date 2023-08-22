#include <libavutil/buffer.h>

#include <libtxproto/txproto_main.h>
#include <libtxproto/io.h>

#include "iosys_common.h"

AVBufferRef *sp_io_alloc(TXMainContext *ctx,
                         const char **api_list,
                         event_fn source_event_cb,
                         event_free source_event_free,
                         size_t callback_ctx_size)
{
    int err = 0;

    for (int i = 0; (api_list && api_list[i]); i++) {
        int j;
        for (j = 0; j < sp_compiled_apis_len; j++)
            if (!strcmp(api_list[i], sp_compiled_apis[j]->name))
                break;
        if (j == sp_compiled_apis_len) {
            char temp[99] = { 0 };
            snprintf(temp, sizeof(temp), "%s", api_list[i]);
            sp_log(ctx, SP_LOG_ERROR, "API \"%s\" not found!", temp);
            return NULL;
        }
    }

    if (!ctx->io_api_ctx)
        ctx->io_api_ctx = av_mallocz(sizeof(*ctx->io_api_ctx)*sp_compiled_apis_len);

    /* Initialize I/O APIs */
    int initialized_apis = 0, apis_initialized = 0;
    for (int i = 0; i < sp_compiled_apis_len; i++) {
        if (ctx->io_api_ctx[i]) {
            initialized_apis++;
            continue;
        }
        int found = 0;
        for (int j = 0; (api_list && api_list[j]); j++) {
            if (!strcmp(api_list[j], sp_compiled_apis[i]->name)) {
                found = 1;
                break;
            }
        }
        if (api_list && !found)
            continue;

        err = sp_compiled_apis[i]->init_sys(&ctx->io_api_ctx[i]);
        if (!api_list && err == AVERROR(ENOSYS)) {
            continue;
        } else if (err < 0) {
            sp_log(ctx, SP_LOG_ERROR, "Unable to load API \"%s\": %s!",
                   sp_compiled_apis[i]->name, av_err2str(err));
            return NULL;
        }

        initialized_apis++;
        apis_initialized++;
    }

    if (!initialized_apis) {
        if (api_list)
            sp_log(ctx, SP_LOG_ERROR, "No requested I/O API(s) available of the %d enabled at build time.\n",
                   sp_compiled_apis_len);
        else
            sp_log(ctx, SP_LOG_WARN, "No I/O APIs available.\n");

        return NULL;
    } else if (apis_initialized) {
        sp_log(ctx, SP_LOG_DEBUG, "%i I/O(s) initialized.\n", apis_initialized);
    }

    /* Prepare event */
    SPEventType type = SP_EVENT_TYPE_SOURCE    | SP_EVENT_ON_CHANGE |
                       SP_EVENT_FLAG_IMMEDIATE | SP_EVENT_FLAG_UNIQUE;

    return sp_event_create(source_event_cb,
                           source_event_free,
                           callback_ctx_size,
                           NULL,
                           type,
                           ctx,
                           NULL);
}

int sp_io_init(TXMainContext *ctx,
               AVBufferRef *source_event,
               const char **api_list)
{
    int err;

    /* Initialize APIs */
    for (int i = 0; i < sp_compiled_apis_len; i++) {
        if (!ctx->io_api_ctx[i])
            continue;
        int found = 0;
        for (int j = 0; (api_list && api_list[j]); j++) {
            if (!strcmp(api_list[j], sp_compiled_apis[i]->name)) {
                found = 1;
                break;
            }
        }
        if (api_list && !found)
            continue;

        /* Add and immediately run the event to signal all devices */
        err = sp_compiled_apis[i]->ctrl(ctx->io_api_ctx[i],
                                        SP_EVENT_CTRL_NEW_EVENT | SP_EVENT_FLAG_IMMEDIATE,
                                        source_event);
        if (err < 0) {
            sp_log(ctx, SP_LOG_ERROR, "Unable to add event to API \"%s\": %s!",
                   sp_compiled_apis[i]->name, av_err2str(err));
            return err;
        }
    }

    sp_bufferlist_append_noref(ctx->ext_buf_refs, source_event);

    return 0;
}

AVBufferRef *sp_io_create(TXMainContext *ctx,
                          uint32_t identifier,
                          AVDictionary *opts)
{
    int i = 0;
    AVBufferRef *entry = NULL;
    for (; i < sp_compiled_apis_len; i++)
        if (ctx->io_api_ctx[i] &&
            (entry = sp_compiled_apis[i]->ref_entry(ctx->io_api_ctx[i], identifier)))
            break;

    if (!entry) {
        sp_log(ctx, SP_LOG_ERROR, "Entry 0x%X not found!", identifier);
        return NULL;
    }

    int err = sp_compiled_apis[i]->init_io(ctx->io_api_ctx[i], entry, opts);
    if (err < 0) {
        sp_log(ctx, SP_LOG_ERROR, "Unable to init IO: %s!", av_err2str(err));
        av_buffer_unref(&entry);
        return NULL;
    }

    sp_bufferlist_append_noref(ctx->ext_buf_refs, entry);

    return entry;
}
