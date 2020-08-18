#include <unistd.h>

#include <libavutil/pixdesc.h>
#include <libavutil/avstring.h>
#include <libavutil/time.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include "linenoise.h"

#include "iosys_common.h"
#include "encoding.h"
#include "muxing.h"
#include "filtering.h"
#include "utils.h"
#include "version.h"
#include "../config.h"

#include "default.lua.bin.h"

typedef struct MainContext {
    AVClass *class;
    int log_lvl_offset;

    lua_State *lua;
    pthread_mutex_t lock;

    int source_update_cb_ref;

    atomic_int_fast64_t epoch_value;

    const IOSysAPI **api_fns;
    AVBufferRef **api_ctx;
    int num_apis;

    SPBufferList *commit_list;
    SPBufferList *discard_list;
    SPBufferList *lua_buf_refs;
} MainContext;

#include "event_templates.h"

#define LUA_PRIV_PREFIX "sp"
#define LUA_PUB_PREFIX "tx"

static void *lua_alloc_fn(void *opaque, void *ptr, size_t type, size_t nsize)
{
    return av_realloc(ptr, nsize);
}

static int lua_panic_handler(lua_State *L)
{
    luaL_traceback(L, L, NULL, 1);
    av_log(NULL, AV_LOG_ERROR, "%s\n", lua_tostring(L, -1));
    return 0;
}

static void lua_warning_handler(void *opaque, const char *msg, int contd)
{
    av_log(opaque, AV_LOG_WARNING, "%s%c", msg, contd ? '\0' : '\n');
}

#define LUA_CLEANUP_FN_DEFS(fnname)   \
    AVBufferRef **cleanup_ref = NULL; \
    const char *fn_name = fnname;

#define LUA_SET_CLEANUP(ref) \
    do {                     \
        cleanup_ref = &ref;  \
    } while (0)

#define LUA_ERROR(fmt, ...)                                               \
    do {                                                                  \
        av_log(ctx, AV_LOG_ERROR, "%s: " fmt "\n", fn_name, __VA_ARGS__); \
        lua_pushfstring(L, fmt, __VA_ARGS__);                             \
        if (cleanup_ref && *cleanup_ref)                                  \
            av_buffer_unref(cleanup_ref);                                 \
        pthread_mutex_unlock(&ctx->lock);                                 \
        return lua_error(L);                                              \
    } while (0)

#define LUA_INTERFACE_END(ret)            \
    do {                                  \
        pthread_mutex_unlock(&ctx->lock); \
        return (ret);                     \
    } while (0)

#define LUA_INTERFACE_BOILERPLATE()                                            \
    do {                                                                       \
        pthread_mutex_lock(&ctx->lock);                                        \
        if (lua_gettop(L) != 1)                                                \
            LUA_ERROR("Invalid number of arguments, expected 1, got %i!",      \
                      lua_gettop(L));                                          \
        if (!lua_istable(L, -1))                                               \
            LUA_ERROR("Invalid argument type, expected table, got \"%s\"!",    \
                      lua_typename(L, lua_type(L, -1)));                       \
    } while (0)

#define LUA_CHECK_OPT_VAL(key, expected)                                               \
    int type = lua_getfield(L, -1, key);                                               \
    if (type == LUA_TNIL || type == LUA_TNONE) {                                       \
        lua_pop(L, 1);                                                                 \
        break;                                                                         \
    } else if (type != expected)                                                       \
        LUA_ERROR("Invalid value type for entry \"%s\", expected \"%s\", got \"%s\"!", \
                  key, lua_typename(L, expected), lua_typename(L, type));              \

#define GET_OPTS_CLASS(dst, key)                                              \
    do {                                                                      \
        LUA_CHECK_OPT_VAL(key, LUA_TTABLE)                                    \
        err = lua_parse_table_to_avopt(ctx, L, dst);                          \
        if (err < 0)                                                          \
            LUA_ERROR("Unable to parse %s: %s!", key, av_err2str(err));       \
        lua_pop(L, 1);                                                        \
    } while (0)

#define GET_OPTS_DICT(dst, key)                                               \
    do {                                                                      \
        LUA_CHECK_OPT_VAL(key, LUA_TTABLE)                                    \
        err = lua_parse_table_to_avdict(L, &dst);                             \
        if (err < 0)                                                          \
            LUA_ERROR("Unable to parse %s: %s!", key, av_err2str(err));       \
        lua_pop(L, 1);                                                        \
    } while (0)

#define GET_OPTS_LIST(dst, key)                                               \
    do {                                                                      \
        LUA_CHECK_OPT_VAL(key, LUA_TTABLE)                                    \
        err = lua_parse_table_to_list(L, &dst);                               \
        if (err < 0)                                                          \
            LUA_ERROR("Unable to parse %s: %s!", key, av_err2str(err));       \
        lua_pop(L, 1);                                                        \
    } while (0)

#define GET_OPT_STR(dst, key)               \
    do {                                    \
        LUA_CHECK_OPT_VAL(key, LUA_TSTRING) \
        dst = lua_tostring(L, -1);          \
        lua_pop(L, 1);                      \
    } while (0)

#define SET_OPT_BOOL(src, key) \
    lua_pushboolean(L, src);   \
    lua_setfield(L, -2, key);

#define SET_OPT_STR(src, key) \
    lua_pushstring(L, src);   \
    lua_setfield(L, -2, key);

#define GET_OPT_NUM(dst, key)                                                    \
    do {                                                                         \
        LUA_CHECK_OPT_VAL(key, LUA_TNUMBER)                                      \
        dst = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : lua_tonumber(L, -1); \
        lua_pop(L, 1);                                                           \
    } while (0)

#define SET_OPT_NUM(src, key) \
    lua_pushnumber(L, src);   \
    lua_setfield(L, -2, key);

#define SET_OPT_INT(src, key) \
    lua_pushinteger(L, src);  \
    lua_setfield(L, -2, key);

#define GET_OPT_LIGHTUSERDATA(dst, key)            \
    do {                                           \
        LUA_CHECK_OPT_VAL(key, LUA_TLIGHTUSERDATA) \
        dst = (void *)lua_touserdata(L, -1);       \
        lua_pop(L, 1);                             \
    } while (0)

#define SET_OPT_LIGHTUSERDATA(src, key) \
    lua_pushlightuserdata(L, src);      \
    lua_setfield(L, -2, key);

static int lua_parse_table_to_avopt(MainContext *ctx, lua_State *L, void *dst)
{
    lua_pushnil(L);

    while (lua_next(L, -2)) {
        int err;
        switch (lua_type(L, -1)) {
        case LUA_TBOOLEAN:
            err = av_opt_set_int(dst, lua_tostring(L, -2), lua_toboolean(L, -1), AV_OPT_SEARCH_CHILDREN);
            break;
        case LUA_TNUMBER:
            if (lua_isinteger(L, -1))
                err = av_opt_set_int(dst, lua_tostring(L, -2), lua_tointeger(L, -1), AV_OPT_SEARCH_CHILDREN);
            else
                err = av_opt_set_double(dst, lua_tostring(L, -2), lua_tonumber(L, -1), AV_OPT_SEARCH_CHILDREN);
            break;
        case LUA_TSTRING:
        default:
            err = av_opt_set(dst, lua_tostring(L, -2), lua_tostring(L, -1), AV_OPT_SEARCH_CHILDREN);
            break;
        }

        if (err < 0) {
            av_log(ctx, AV_LOG_ERROR, "Error setting option \"%s\": %s!\n", lua_tostring(L, -2), av_err2str(err));
            return err;
        }

        lua_pop(L, 1);
    }

    return 0;
}

static int lua_parse_table_to_list(lua_State *L, char ***dst)
{
    lua_pushnil(L);

    char **str = NULL;
    int num = 0;

    while (lua_next(L, -2)) {
        if (!lua_isstring(L, -1))
            return AVERROR(EINVAL);
        str = av_realloc(str, sizeof(*str) * (num + 1));
        str[num++] = av_strdup(lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    str = av_realloc(str, sizeof(*str) * (num + 1));
    str[num++] = NULL;
    *dst = str;

    return 0;
}

static int lua_parse_table_to_avdict(lua_State *L, AVDictionary **dict)
{
    lua_pushnil(L);

    while (lua_next(L, -2)) {
        int err;
        const char *key = lua_tostring(L, -2);
        const char *val;
        if (lua_isboolean(L, -1))
            val = lua_toboolean(L, -1) ? "1" : "0";
        else
            val = lua_tostring(L, -1);

        err = av_dict_set(dict, key, val, 0);
        if (err < 0)
            return err;

        lua_pop(L, 1);
    }

    return 0;
}

static int lua_create_muxer(lua_State *L)
{
    int err;
    MainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));

    LUA_CLEANUP_FN_DEFS(LUA_PUB_PREFIX".create_muxer")
    LUA_INTERFACE_BOILERPLATE();

    AVBufferRef *mctx_ref = sp_muxer_alloc();
    MuxingContext *mctx = (MuxingContext *)mctx_ref->data;
    sp_bufferlist_append_noref(ctx->lua_buf_refs, mctx_ref);

    LUA_SET_CLEANUP(mctx_ref);

    GET_OPT_STR(mctx->name, "name");
    GET_OPT_STR(mctx->out_url, "out_url");
    GET_OPT_STR(mctx->out_format, "out_format");

    err = sp_muxer_init(mctx_ref);
    if (err < 0)
        LUA_ERROR("Unable to init muxer: %s!", av_err2str(err));

    SET_OPT_STR(mctx->name, "name");
    SET_OPT_STR(mctx->out_url, "out_url");
    SET_OPT_STR(mctx->out_format, "out_format");

    SET_OPT_LIGHTUSERDATA(mctx_ref, LUA_PRIV_PREFIX "_priv");
    GET_OPTS_CLASS(mctx->avf, "options");
    GET_OPTS_DICT(mctx->priv_options, "priv_options");

    LUA_INTERFACE_END(0);
}

static int lua_create_encoder(lua_State *L)
{
    int err;
    const char *temp_str;
    MainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));

    LUA_CLEANUP_FN_DEFS(LUA_PUB_PREFIX".create_encoder")
    LUA_INTERFACE_BOILERPLATE();

    AVBufferRef *ectx_ref = sp_encoder_alloc();
    EncodingContext *ectx = (EncodingContext *)ectx_ref->data;
    sp_bufferlist_append_noref(ctx->lua_buf_refs, ectx_ref);

    LUA_SET_CLEANUP(ectx_ref);

    const char *enc_name = NULL;
    GET_OPT_STR(enc_name, "encoder");
    ectx->codec = avcodec_find_encoder_by_name(enc_name);
    if (!ectx->codec)
        LUA_ERROR("Encoder \"%s\" not found!", enc_name);

    GET_OPT_STR(ectx->name, "name");
    err = sp_encoder_init(ectx_ref);
    if (err < 0)
        LUA_ERROR("Unable to init encoder: %s!", av_err2str(err));

    SET_OPT_STR(ectx->name, "name");

    SET_OPT_LIGHTUSERDATA(ectx_ref, LUA_PRIV_PREFIX "_priv");
    GET_OPTS_CLASS(ectx->avctx, "options");
    GET_OPTS_CLASS(ectx->swr, "resampler_options");
    GET_OPTS_DICT(ectx->priv_options, "priv_options");

    SET_OPT_INT(ectx->width, "width");
    SET_OPT_INT(ectx->height, "height");
    SET_OPT_INT(ectx->width, "sample_rate");

    temp_str = NULL;
    GET_OPT_STR(temp_str, "pix_fmt");
    if (temp_str) {
        ectx->pix_fmt = av_get_pix_fmt(temp_str);
        if (ectx->pix_fmt == AV_PIX_FMT_NONE && strcmp(temp_str, "none"))
            LUA_ERROR("Invalid pixel format \"%s\"!\n", temp_str);
    }

    temp_str = NULL;
    GET_OPT_STR(temp_str, "sample_fmt");
    if (temp_str) {
        ectx->sample_fmt = av_get_sample_fmt(temp_str);
        if (ectx->sample_fmt == AV_SAMPLE_FMT_NONE && strcmp(temp_str, "none"))
            LUA_ERROR("Invalid sample format \"%s\"!\n", temp_str);
    }

    temp_str = NULL;
    GET_OPT_STR(temp_str, "channel_layout");
    if (temp_str) {
        ectx->channel_layout = av_get_channel_layout(temp_str);
        if (!ectx->sample_fmt)
            LUA_ERROR("Invalid channel layout \"%s\"!\n", temp_str);
    }

    LUA_INTERFACE_END(0);
}

static int lua_create_io(lua_State *L)
{
    MainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));
    LUA_CLEANUP_FN_DEFS(LUA_PUB_PREFIX".create_io")

    pthread_mutex_lock(&ctx->lock);

    int num_args = lua_gettop(L);
    if (num_args != 1 && num_args != 2)
        LUA_ERROR("Invalid number of arguments, expected 1 or 2, got %i!", num_args);
    if (num_args == 2 && !lua_istable(L, -1))
        LUA_ERROR("Invalid argument, expected \"table\", got \"%s\"!", lua_typename(L, lua_type(L, -1)));
    if (!lua_islightuserdata(L, -num_args))
        LUA_ERROR("Invalid argument, expected \"lightuserdata\", got \"%s\"!", lua_typename(L, lua_type(L, -num_args)));

    AVDictionary *opts = NULL;
    if (num_args == 2) {
        lua_parse_table_to_avdict(L, &opts);
        lua_pop(L, 1);
    }

    uint32_t identifier = (uintptr_t)lua_touserdata(L, -1);

    int i = 0;
    AVBufferRef *entry = NULL;
    for (; i < ctx->num_apis; i++)
        if ((entry = ctx->api_fns[i]->ref_entry(ctx->api_ctx[i], identifier)))
            break;

    if (!entry) {
        lua_pushnil(L);
        av_dict_free(&opts);
        LUA_INTERFACE_END(1);
    }

    ctx->api_fns[i]->init_io(ctx->api_ctx[i], entry, opts);
    sp_bufferlist_append_noref(ctx->lua_buf_refs, entry);

    lua_pushlightuserdata(L, entry);

    LUA_INTERFACE_END(1);
}

static int lua_create_filter(lua_State *L)
{
    int err;
    MainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));

    LUA_CLEANUP_FN_DEFS(LUA_PUB_PREFIX".create_filter")
    LUA_INTERFACE_BOILERPLATE();

    AVBufferRef *fctx_ref = sp_filter_alloc();
    sp_bufferlist_append_noref(ctx->lua_buf_refs, fctx_ref);

    LUA_SET_CLEANUP(fctx_ref);

    const char *name = NULL;
    GET_OPT_STR(name, "name");

    const char *filter = NULL;
    GET_OPT_STR(filter, "filter");

    AVDictionary *opts = NULL;
    GET_OPTS_DICT(opts, "options");

    enum AVPixelFormat req_fmt = AV_PIX_FMT_NONE;
    const char *temp_str = NULL;
    GET_OPT_STR(temp_str, "pix_fmt");
    if (temp_str) {
        req_fmt = av_get_pix_fmt(temp_str);
        if (req_fmt == AV_PIX_FMT_NONE && strcmp(temp_str, "none"))
            LUA_ERROR("Invalid pixel format \"%s\"!\n", temp_str);
    }

    char **in_pads = NULL;
    GET_OPTS_LIST(in_pads, "input_pads");

    char **out_pads = NULL;
    GET_OPTS_LIST(out_pads, "output_pads");

    err = sp_init_filter_single(fctx_ref, name, filter, in_pads, out_pads, req_fmt, opts, NULL, AV_HWDEVICE_TYPE_NONE);
    if (err < 0)
        LUA_ERROR("Unable to init filter: %s!", av_err2str(err));

    SET_OPT_LIGHTUSERDATA(fctx_ref, LUA_PRIV_PREFIX "_priv");

    LUA_INTERFACE_END(0);
}

typedef struct SPLinkSourceEncoderCtx {
    AVBufferRef *src_ref;
    AVBufferRef *enc_ref;
} SPLinkSourceEncoderCtx;

static int api_link_src_enc_cb(AVBufferRef *opaque, void *src_ctx)
{
    SPLinkSourceEncoderCtx *ctx = (SPLinkSourceEncoderCtx *)opaque->data;
    IOSysEntry *sctx = (IOSysEntry *)ctx->src_ref->data;
    EncodingContext *ectx = (EncodingContext *)ctx->enc_ref->data;
    return sp_frame_fifo_mirror(ectx->src_frames, sctx->frames);
}

static void api_link_src_enc_free(void *opaque, uint8_t *data)
{
    SPLinkSourceEncoderCtx *ctx = (SPLinkSourceEncoderCtx *)data;
    av_buffer_unref(&ctx->src_ref);
    av_buffer_unref(&ctx->enc_ref);
    av_free(data);
}

typedef struct SPLinkSourceFilterCtx {
    AVBufferRef *src_ref;
    AVBufferRef *filt_ref;
    const char *filt_pad;
} SPLinkSourceFilterCtx;

static int api_link_src_filt_cb(AVBufferRef *opaque, void *src_ctx)
{
    SPLinkSourceFilterCtx *ctx = (SPLinkSourceFilterCtx *)opaque->data;
    IOSysEntry *sctx = (IOSysEntry *)ctx->src_ref->data;
    return sp_map_fifo_to_pad(ctx->filt_ref, sctx->frames, ctx->filt_pad, 0);
}

static void api_link_src_filt_free(void *opaque, uint8_t *data)
{
    SPLinkSourceFilterCtx *ctx = (SPLinkSourceFilterCtx *)data;
    av_buffer_unref(&ctx->src_ref);
    av_buffer_unref(&ctx->filt_ref);
    av_free(data);
}

typedef struct SPLinkFilterFilterCtx {
    AVBufferRef *src_ref;
    AVBufferRef *dst_ref;
    const char *src_filt_pad;
    const char *dst_filt_pad;
} SPLinkFilterFilterCtx;

static int api_link_filt_filt_cb(AVBufferRef *opaque, void *src_ctx)
{
    SPLinkFilterFilterCtx *ctx = (SPLinkFilterFilterCtx *)opaque->data;
    return sp_map_pad_to_pad(ctx->dst_ref, ctx->dst_filt_pad,
                             ctx->src_ref, ctx->src_filt_pad);
}

static void api_link_filt_filt_free(void *opaque, uint8_t *data)
{
    SPLinkFilterFilterCtx *ctx = (SPLinkFilterFilterCtx *)data;
    av_buffer_unref(&ctx->src_ref);
    av_buffer_unref(&ctx->dst_ref);
    av_free(data);
}

typedef struct SPLinkFilterEncoderCtx {
    AVBufferRef *filt_ref;
    AVBufferRef *enc_ref;
    const char *filt_pad;
} SPLinkFilterEncoderCtx;

static int api_link_filt_enc_cb(AVBufferRef *opaque, void *src_ctx)
{
    SPLinkFilterEncoderCtx *ctx = (SPLinkFilterEncoderCtx *)opaque->data;
    EncodingContext *ectx = (EncodingContext *)ctx->enc_ref->data;
    return sp_map_fifo_to_pad(ctx->filt_ref, ectx->src_frames, ctx->filt_pad, 1);
}

static void api_link_filt_enc_free(void *opaque, uint8_t *data)
{
    SPLinkFilterEncoderCtx *ctx = (SPLinkFilterEncoderCtx *)data;
    av_buffer_unref(&ctx->filt_ref);
    av_buffer_unref(&ctx->enc_ref);
    av_free(data);
}

typedef struct SPLinkEncoderMuxerCtx {
    AVBufferRef *enc_ref;
    AVBufferRef *mux_ref;
} SPLinkEncoderMuxerCtx;

static int api_link_enc_mux_cb(AVBufferRef *opaque, void *src_ctx)
{
    SPLinkEncoderMuxerCtx *ctx = (SPLinkEncoderMuxerCtx *)opaque->data;
    int err = sp_muxer_add_stream(ctx->mux_ref, ctx->enc_ref);
    if (err < 0)
        return err;

    EncodingContext *ectx = (EncodingContext *)ctx->enc_ref->data;
    MuxingContext *mctx = (MuxingContext *)ctx->mux_ref->data;

    return sp_packet_fifo_mirror(mctx->src_packets, ectx->dst_packets);
}

static void api_link_enc_mux_free(void *opaque, uint8_t *data)
{
    SPLinkEncoderMuxerCtx *ctx = (SPLinkEncoderMuxerCtx *)data;
    av_buffer_unref(&ctx->enc_ref);
    av_buffer_unref(&ctx->mux_ref);
    av_free(data);
}

static int lua_link_objects(lua_State *L)
{
    int err;
    MainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));
    LUA_CLEANUP_FN_DEFS(LUA_PUB_PREFIX".link_objects")

    pthread_mutex_lock(&ctx->lock);

    int nargs = lua_gettop(L);
    if (nargs != 2 && nargs != 3 && nargs != 4)
        LUA_ERROR("Invalid number of arguments, expected 2, 3 or 4, got %i!", nargs);

    const char *extra_args[2] = { 0 };
    for (; nargs > 2; nargs--) {
        if (!lua_isstring(L, -1))
            LUA_ERROR("Invalid argument, expected \"string\", got \"%s\"!",
                      lua_typename(L, lua_type(L, -1)));
        extra_args[nargs - 3] = lua_tostring(L, -1);
        lua_pop(L, 1);
    }

    if (!lua_istable(L, -2) && !lua_islightuserdata(L, -2))
        LUA_ERROR("Invalid argument, expected \"table\" or \"lightuserdata\", got \"%s\"!", lua_typename(L, lua_type(L, -2)));
    if (!lua_istable(L, -1) && !lua_islightuserdata(L, -1))
        LUA_ERROR("Invalid argument, expected \"table\" or \"lightuserdata\", got \"%s\"!", lua_typename(L, lua_type(L, -1)));

    AVBufferRef *src_ctx_ref = NULL;
    if (lua_istable(L, -1))
        GET_OPT_LIGHTUSERDATA(src_ctx_ref, LUA_PRIV_PREFIX "_priv");
    else
        src_ctx_ref = lua_touserdata(L, -1);

    lua_pop(L, 1);

    AVBufferRef *dst_ctx_ref = NULL;
    if (lua_istable(L, -1))
        GET_OPT_LIGHTUSERDATA(dst_ctx_ref, LUA_PRIV_PREFIX "_priv");
    else
        dst_ctx_ref = lua_touserdata(L, -1);

    AVClass *dst_class = *((AVClass **)dst_ctx_ref->data);
    AVClass *src_class = *((AVClass **)src_ctx_ref->data);

    if (src_class->category == AV_CLASS_CATEGORY_ENCODER && dst_class->category == AV_CLASS_CATEGORY_MUXER) {
        EncodingContext *ectx = (EncodingContext *)src_ctx_ref->data;
        MuxingContext *mctx = (MuxingContext *)dst_ctx_ref->data;

        if (!atomic_load(&ectx->initialized))
            ectx->need_global_header = mctx->avf->oformat->flags & AVFMT_GLOBALHEADER;

        SP_EVENT_BUFFER_CTX_ALLOC(SPLinkEncoderMuxerCtx, link_enc_mux, api_link_enc_mux_free, NULL)
        link_enc_mux->enc_ref = av_buffer_ref(src_ctx_ref);
        link_enc_mux->mux_ref = av_buffer_ref(dst_ctx_ref);

        enum SPEventType flags = SP_EVENT_FLAG_ONESHOT | SP_EVENT_TYPE_LINK;
        if (atomic_load(&ectx->initialized))
            flags |= SP_EVENT_ON_COMMIT;
        else
            flags |= SP_EVENT_ON_CHANGE;

        AVBufferRef *link_event = sp_event_create(api_link_enc_mux_cb, NULL,
                                                  flags, link_enc_mux_ref,
                                                  sp_event_gen_identifier(ectx, mctx, SP_EVENT_TYPE_LINK));

        sp_encoder_ctrl(src_ctx_ref, SP_EVENT_CTRL_NEW_EVENT, link_event);
        if (!atomic_load(&mctx->initialized))
            sp_muxer_ctrl(dst_ctx_ref, SP_EVENT_CTRL_DEP | SP_EVENT_ON_INIT, link_event);

        av_buffer_unref(&link_event);

        if (atomic_load(&ectx->initialized))
            add_commit_fn_to_list(ctx, sp_encoder_ctrl, src_ctx_ref);

        LUA_INTERFACE_END(0);
    } else if (dst_class->category == AV_CLASS_CATEGORY_ENCODER &&
               (src_class->category == AV_CLASS_CATEGORY_DEVICE_VIDEO_INPUT ||
                src_class->category == AV_CLASS_CATEGORY_DEVICE_VIDEO_OUTPUT ||
                src_class->category == AV_CLASS_CATEGORY_DEVICE_AUDIO_INPUT)) {
        IOSysEntry *sctx = (IOSysEntry *)src_ctx_ref->data;
        EncodingContext *ectx = (EncodingContext *)dst_ctx_ref->data;

        SP_EVENT_BUFFER_CTX_ALLOC(SPLinkSourceEncoderCtx, link_src_enc, api_link_src_enc_free, NULL)
        link_src_enc->src_ref = av_buffer_ref(src_ctx_ref);
        link_src_enc->enc_ref = av_buffer_ref(dst_ctx_ref);

        enum SPEventType flags = SP_EVENT_FLAG_ONESHOT | SP_EVENT_TYPE_LINK | SP_EVENT_ON_COMMIT;
        AVBufferRef *link_event = sp_event_create(api_link_src_enc_cb, NULL,
                                                  flags, link_src_enc_ref,
                                                  sp_event_gen_identifier(sctx, ectx, SP_EVENT_TYPE_LINK));

        err = sp_bufferlist_append_event(ctx->commit_list, link_event);
        av_buffer_unref(&link_event);

        if (err < 0)
            LUA_ERROR("Error linking: %s\n", av_err2str(err));

        LUA_INTERFACE_END(0);
    } else if (dst_class->category == AV_CLASS_CATEGORY_ENCODER &&
               src_class->category == AV_CLASS_CATEGORY_FILTER) {
        FilterContext *fctx = (FilterContext *)src_ctx_ref->data;
        EncodingContext *ectx = (EncodingContext *)dst_ctx_ref->data;

        SP_EVENT_BUFFER_CTX_ALLOC(SPLinkFilterEncoderCtx, link_filt_enc, api_link_filt_enc_free, NULL)
        link_filt_enc->filt_ref = av_buffer_ref(src_ctx_ref);
        link_filt_enc->enc_ref = av_buffer_ref(dst_ctx_ref);
        link_filt_enc->filt_pad = av_strdup(extra_args[0]);

        enum SPEventType flags = SP_EVENT_FLAG_ONESHOT | SP_EVENT_TYPE_LINK | SP_EVENT_ON_COMMIT;
        AVBufferRef *link_event = sp_event_create(api_link_filt_enc_cb, NULL,
                                                  flags, link_filt_enc_ref,
                                                  sp_event_gen_identifier(fctx, ectx, SP_EVENT_TYPE_LINK));

        err = sp_bufferlist_append_event(ctx->commit_list, link_event);
        av_buffer_unref(&link_event);

        if (err < 0)
            LUA_ERROR("Error linking: %s\n", av_err2str(err));

        LUA_INTERFACE_END(0);
    } else if (dst_class->category == AV_CLASS_CATEGORY_FILTER &&
               (src_class->category == AV_CLASS_CATEGORY_DEVICE_VIDEO_INPUT ||
                src_class->category == AV_CLASS_CATEGORY_DEVICE_VIDEO_OUTPUT ||
                src_class->category == AV_CLASS_CATEGORY_DEVICE_AUDIO_INPUT)) {
        IOSysEntry *sctx = (IOSysEntry *)src_ctx_ref->data;
        FilterContext *fctx = (FilterContext *)dst_ctx_ref->data;

        SP_EVENT_BUFFER_CTX_ALLOC(SPLinkSourceFilterCtx, link_src_filt, api_link_src_filt_free, NULL)
        link_src_filt->src_ref = av_buffer_ref(src_ctx_ref);
        link_src_filt->filt_ref = av_buffer_ref(dst_ctx_ref);
        link_src_filt->filt_pad = av_strdup(extra_args[0]);

        enum SPEventType flags = SP_EVENT_FLAG_ONESHOT | SP_EVENT_TYPE_LINK | SP_EVENT_ON_COMMIT;
        AVBufferRef *link_event = sp_event_create(api_link_src_filt_cb, NULL,
                                                  flags, link_src_filt_ref,
                                                  sp_event_gen_identifier(sctx, fctx, SP_EVENT_TYPE_LINK));

        err = sp_bufferlist_append_event(ctx->commit_list, link_event);
        av_buffer_unref(&link_event);

        if (err < 0)
            LUA_ERROR("Error linking: %s\n", av_err2str(err));

        LUA_INTERFACE_END(0);
    } else if (dst_class->category == AV_CLASS_CATEGORY_FILTER &&
               src_class->category == AV_CLASS_CATEGORY_FILTER) {
        FilterContext *f1ctx = (FilterContext *)src_ctx_ref->data;
        FilterContext *f2ctx = (FilterContext *)dst_ctx_ref->data;

        SP_EVENT_BUFFER_CTX_ALLOC(SPLinkFilterFilterCtx, link_filt_filt, api_link_filt_filt_free, NULL)
        link_filt_filt->src_ref = av_buffer_ref(src_ctx_ref);
        link_filt_filt->dst_ref = av_buffer_ref(dst_ctx_ref);

        if (extra_args[0])
            link_filt_filt->src_filt_pad = av_strdup(extra_args[0]);
        else if (extra_args[1])
            link_filt_filt->src_filt_pad = av_strdup(extra_args[1]);

        if (extra_args[1])
            link_filt_filt->dst_filt_pad = av_strdup(extra_args[1]);
        else if (extra_args[0])
            link_filt_filt->dst_filt_pad = av_strdup(extra_args[0]);

        enum SPEventType flags = SP_EVENT_FLAG_ONESHOT | SP_EVENT_TYPE_LINK | SP_EVENT_ON_COMMIT;
        AVBufferRef *link_event = sp_event_create(api_link_filt_filt_cb, NULL,
                                                  flags, link_filt_filt_ref,
                                                  sp_event_gen_identifier(f1ctx, f2ctx, SP_EVENT_TYPE_LINK));

        err = sp_bufferlist_append_event(ctx->commit_list, link_event);
        av_buffer_unref(&link_event);

        if (err < 0)
            LUA_ERROR("Error linking: %s\n", av_err2str(err));

        LUA_INTERFACE_END(0);
    }

    LUA_ERROR("Unable to link \"%s\" (%s) to \"%s\" (%s)!",
              src_class->class_name, sp_map_class_to_string(src_class->category),
              dst_class->class_name, sp_map_class_to_string(dst_class->category));
}

enum EpochMode {
    EP_MODE_OFFSET,   /* Timestamps will start at an offset (value = system time + offset) */
    EP_MODE_SYSTEM,   /* Timestamps will start at the system arbitrary time (value = 0) */
    EP_MODE_SOURCE,   /* Timestamps will be set to the first output's PTS */
    EP_MODE_EXTERNAL, /* A Lua function will give us the epoch */
};

typedef struct EpochEventCtx {
    enum EpochMode mode;
    int64_t value;
    AVBufferRef *src_ref;
    int fn_ref;
} EpochEventCtx;

static int epoch_event_cb(AVBufferRef *opaque, void *src_ctx)
{
    int err = 0;
    MainContext *ctx = av_buffer_get_opaque(opaque);
    EpochEventCtx *epoch_ctx = (EpochEventCtx *)opaque->data;

    pthread_mutex_lock(&ctx->lock);

    int64_t val;
    switch(epoch_ctx->mode) {
    case EP_MODE_OFFSET:
        val = av_gettime_relative() + epoch_ctx->value;
        break;
    case EP_MODE_SYSTEM:
        val = 0;
        break;
    case EP_MODE_SOURCE:
        av_log(ctx, AV_LOG_ERROR, "Warning epoch mode source, using epoch of system!\n");
        val = 0;
        break;
    case EP_MODE_EXTERNAL:
        if (epoch_ctx->fn_ref == LUA_NOREF || epoch_ctx->fn_ref == LUA_REFNIL) {
            av_log(ctx, AV_LOG_ERROR, "Invalid Lua epoch callback \"nil\"!\n");
            err = AVERROR_EXTERNAL;
            goto end;
        }

        lua_State *L = ctx->lua;
        lua_rawgeti(L, LUA_REGISTRYINDEX, epoch_ctx->fn_ref);

        /* 1 argument: system time */
        lua_pushinteger(L, av_gettime_relative());

        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            av_log(ctx, AV_LOG_ERROR, "Error calling external epoch callback: %s!\n",
                   lua_tostring(L, -1));
            err = AVERROR_EXTERNAL;
            goto end;
        }

        if (!lua_isinteger(L, -1) && !lua_isnumber(L, -1)) {
            av_log(ctx, AV_LOG_ERROR, "Invalid return value for epoch function, "
                   "expected \"integer\" or \"number\", got \"%s\"!",
                   lua_typename(L, lua_type(L, -1)));
            err = AVERROR_EXTERNAL;
            goto end;
        }

        val = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : lua_tonumber(L, -1);

        break;
    }

    atomic_store(&ctx->epoch_value, val);

end:
    pthread_mutex_unlock(&ctx->lock);

    return 0;
}

static void epoch_event_free(void *opaque, uint8_t *data)
{
    MainContext *ctx = opaque;
    EpochEventCtx *epoch_ctx = (EpochEventCtx *)data;

    luaL_unref(ctx->lua, LUA_REGISTRYINDEX, epoch_ctx->fn_ref);
    av_buffer_unref(&epoch_ctx->src_ref);
}

static int lua_set_epoch(lua_State *L)
{
    MainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));
    LUA_CLEANUP_FN_DEFS(LUA_PUB_PREFIX".set_epoch")

    pthread_mutex_lock(&ctx->lock);

    if (lua_gettop(L) != 1)
        LUA_ERROR("Invalid number of arguments, expected 1, got %i!",
                  lua_gettop(L));

    SP_EVENT_BUFFER_CTX_ALLOC(EpochEventCtx, epoch_ctx, epoch_event_free, ctx)
    epoch_ctx->fn_ref = LUA_NOREF;

    if (lua_istable(L, -1) || lua_islightuserdata(L, -1)) {
        AVBufferRef *ext_ref = NULL;

        if (lua_istable(L, -1))
            GET_OPT_LIGHTUSERDATA(ext_ref, LUA_PRIV_PREFIX "_priv");
        else
            ext_ref = lua_touserdata(L, -1);

        if (!ext_ref)
            LUA_ERROR("Invalid source reference, got \"%s\"!", "nil");

        AVClass *ext_class = *((AVClass **)ext_ref->data);
        if (ext_class->category != AV_CLASS_CATEGORY_DEVICE_VIDEO_INPUT &&
            ext_class->category != AV_CLASS_CATEGORY_DEVICE_AUDIO_INPUT)
            LUA_ERROR("Invalid reference category, expected \"video input\" or \"audio input\", got \"%s\"!",
                      sp_map_class_to_string(ext_class->category));

        epoch_ctx->mode = EP_MODE_SOURCE;
        epoch_ctx->src_ref = av_buffer_ref(ext_ref);
    } else if (lua_isnumber(L, -1) || lua_isinteger(L, -1)) {
        epoch_ctx->mode = EP_MODE_OFFSET;
        epoch_ctx->value = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : lua_tonumber(L, -1);
    } else if (lua_isstring(L, -1)) {
        const char *str = lua_tostring(L, -1);
        if (!strcmp(str, "zero")) {
            epoch_ctx->mode = EP_MODE_OFFSET;
            epoch_ctx->value = 0;
        } else if (!strcmp(str, "system")) {
            epoch_ctx->mode = EP_MODE_SYSTEM;
        } else {
            av_buffer_unref(&epoch_ctx_ref);
            LUA_ERROR("Invalid epoch mode, expected \"zero\" or \"system\", got \"%s\"!", str);
        }
    } else if (lua_isfunction(L, -1)) {
        epoch_ctx->mode = EP_MODE_EXTERNAL;
        epoch_ctx->fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
        LUA_ERROR("Invalid argument, expected \"string\", \"table\", \"integer\", "
                  "\"number\", \"function\" or \"lightuserdata\", " "got \"%s\"!",
                  lua_typename(L, lua_type(L, -1)));
    }

    enum SPEventType flags = SP_EVENT_FLAG_ONESHOT | SP_EVENT_ON_COMMIT;
    AVBufferRef *epoch_event = sp_event_create(epoch_event_cb, NULL,
                                               flags, epoch_ctx_ref,
                                               sp_event_gen_identifier(ctx, NULL, flags | SP_EVENT_FLAG_EPOCH));

    sp_bufferlist_append_event(ctx->commit_list, epoch_event);
    av_buffer_unref(&epoch_event);

    LUA_INTERFACE_END(0);
}

static int lua_commit(lua_State *L)
{
    MainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));
    /* No need to lock here, if anything needs locking the commit functions
     * will take care of it */
    sp_bufferlist_dispatch_events(ctx->commit_list, ctx, SP_EVENT_ON_COMMIT);
    sp_bufferlist_discard_new_events(ctx->discard_list);

    return 0;
}

static int lua_discard(lua_State *L)
{
    MainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));
    sp_bufferlist_dispatch_events(ctx->discard_list, ctx, SP_EVENT_ON_COMMIT);
    sp_bufferlist_discard_new_events(ctx->commit_list);

    return 0;
}

static int lua_ctrl(lua_State *L)
{
    MainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));
    LUA_CLEANUP_FN_DEFS(LUA_PUB_PREFIX".ctrl")

    pthread_mutex_lock(&ctx->lock);

    if (lua_gettop(L) != 2)
        LUA_ERROR("Invalid number of arguments, expected 2, got %i!",
                  lua_gettop(L));
    if (!lua_islightuserdata(L, -2) && !lua_istable(L, -2))
        LUA_ERROR("Invalid argument, expected \"lightuserdata\" or \"table\", got \"%s\"!",
                  lua_typename(L, lua_type(L, -2)));
    if (!lua_isstring(L, -1) && !lua_istable(L, -1))
        LUA_ERROR("Invalid argument, expected \"string\" or \"table\", got \"%s\"!",
                  lua_typename(L, lua_type(L, -1)));

    AVBufferRef *src_ctx_ref = NULL;
    if (lua_islightuserdata(L, -2)) {
        src_ctx_ref = lua_touserdata(L, -2);
    } else {
        lua_getfield(L, -2, LUA_PRIV_PREFIX "_priv");
        if (!lua_islightuserdata(L, -1))
            LUA_ERROR("Invalid data in table, got \"%s\"!\n", lua_typename(L, lua_type(L, -1)));
        src_ctx_ref = lua_touserdata(L, -1);
        lua_pop(L, 1);
    }

    AVClass *src_class = *((AVClass **)src_ctx_ref->data);
    if (src_class->category != AV_CLASS_CATEGORY_DEVICE_VIDEO_INPUT &&
        src_class->category != AV_CLASS_CATEGORY_DEVICE_AUDIO_INPUT &&
        src_class->category != AV_CLASS_CATEGORY_DEVICE_VIDEO_OUTPUT &&
        src_class->category != AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT &&
        src_class->category != AV_CLASS_CATEGORY_ENCODER &&
        src_class->category != AV_CLASS_CATEGORY_MUXER &&
        src_class->category != AV_CLASS_CATEGORY_FILTER)
        LUA_ERROR("Invalid class, expected video/audio input/output, muxer or encoder, got \"%s\"!\n",
                  sp_map_class_to_string(src_class->category));

    void *arg = NULL;
    enum SPEventType ctrl_type = 0x0;
    if (lua_istable(L, -1)) {
        AVDictionary *opts = NULL;
        lua_parse_table_to_avdict(L, &opts);
        ctrl_type = SP_EVENT_CTRL_OPTS;
        arg = opts;
    } else {
        const char *str = lua_tostring(L, -1);
        if (!strcmp(str, "start")) {
            ctrl_type = SP_EVENT_CTRL_START;
            arg = &ctx->epoch_value;
        } else if (!strcmp(str, "stop")) {
            ctrl_type = SP_EVENT_CTRL_STOP;
        } else {
            LUA_ERROR("Invalid ctrl mode, expected \"start\" or \"stop\", got \"%s\"!", str);
        }
    }

    ctrl_fn fn = NULL;
    if (src_class->category == AV_CLASS_CATEGORY_ENCODER) {
        fn = sp_encoder_ctrl;
    } else if (src_class->category == AV_CLASS_CATEGORY_MUXER) {
        fn = sp_muxer_ctrl;
    } else if (src_class->category == AV_CLASS_CATEGORY_FILTER) {
        fn = sp_filter_ctrl;
    } else {
        IOSysEntry *sctx = (IOSysEntry *)src_ctx_ref->data;
        fn = sctx->ctrl;
    }

    fn(src_ctx_ref, ctrl_type, arg);
    if (ctrl_type & SP_EVENT_CTRL_OPTS)
        av_dict_free((AVDictionary **)&arg);

    add_commit_fn_to_list(ctx, fn, src_ctx_ref);

    LUA_INTERFACE_END(0);
}

typedef struct SPSourceEventCbCtx {
    const IOSysAPI *api_fns;
} SPSourceEventCbCtx;

static int source_event_cb(AVBufferRef *opaque, void *src_ctx)
{
    SPSourceEventCbCtx *source_cb_ctx = (SPSourceEventCbCtx *)opaque->data;
    MainContext *ctx = av_buffer_get_opaque(opaque);
    IOSysEntry *entry = src_ctx;

    pthread_mutex_lock(&ctx->lock);

    if (ctx->source_update_cb_ref != LUA_NOREF &&
        ctx->source_update_cb_ref != LUA_REFNIL) {
        lua_State *L = ctx->lua;

        lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->source_update_cb_ref);

        lua_pushlightuserdata(L, (void *)(uintptr_t)entry->identifier);

        if (entry->gone) {
            lua_pushnil(L);
            lua_call(L, 2, 0);
            goto end;
        }

        lua_newtable(L);

        SET_OPT_STR(entry->name, "name");
        SET_OPT_STR(source_cb_ctx->api_fns->name, "api");
        SET_OPT_STR(av_get_media_type_string(sp_classed_ctx_to_media_type(entry)), "type");
        SET_OPT_STR(entry->desc, "description");
        SET_OPT_LIGHTUSERDATA((void *)(uintptr_t)entry->identifier, "identifier");
        SET_OPT_INT(entry->api_id, "api_id");
        SET_OPT_BOOL(entry->is_default, "default");

        if (sp_classed_ctx_to_media_type(entry) == AVMEDIA_TYPE_VIDEO) {
            lua_newtable(L);

            SET_OPT_INT(entry->width, "width");
            SET_OPT_INT(entry->height, "height");
            SET_OPT_INT(entry->scale, "scale");
            SET_OPT_NUM(av_q2d(entry->framerate), "framerate");

            lua_setfield(L, -2, "video");
        } else if (sp_classed_ctx_to_media_type(entry) == AVMEDIA_TYPE_AUDIO) {
            lua_newtable(L);

            char map_str[256];
            av_get_channel_layout_string(map_str, sizeof(map_str),
                                         entry->channels, entry->channel_layout);

            SET_OPT_STR(map_str, "channel_layout");
            SET_OPT_INT(entry->sample_rate, "sample_rate");
            SET_OPT_INT(entry->channels, "channels");
            SET_OPT_NUM(entry->volume, "volume");
            SET_OPT_STR(av_get_sample_fmt_name(entry->sample_fmt), "format");

            lua_setfield(L, -2, "audio");
        }

        if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
            av_log(entry, AV_LOG_ERROR, "Error calling external source update callback: %s!\n",
                   lua_tostring(L, -1));
            return AVERROR_EXTERNAL;
        }
    }

end:
    pthread_mutex_unlock(&ctx->lock);

    return 0;
}

static int lua_register_io_cb(lua_State *L)
{
    MainContext *ctx = lua_touserdata(L, lua_upvalueindex(1));
    LUA_CLEANUP_FN_DEFS(LUA_PUB_PREFIX".register_io_cb")

    pthread_mutex_lock(&ctx->lock);

    if (lua_gettop(L) != 1)
        LUA_ERROR("Invalid number of arguments, expected 1, got %i!",
                  lua_gettop(L));
    if (!lua_isfunction(L, -1))
        LUA_ERROR("Invalid argument, expected \"function\", got \"%s\"!",
                  lua_typename(L, lua_type(L, -1)));

    luaL_unref(L, LUA_REGISTRYINDEX, ctx->source_update_cb_ref);
    ctx->source_update_cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    for (int i = 0; i < ctx->num_apis; i++) {
        SP_EVENT_BUFFER_CTX_ALLOC(SPSourceEventCbCtx, source_event_ctx, av_buffer_default_free, ctx)

        source_event_ctx->api_fns = ctx->api_fns[i];

        uint32_t identifier = sp_event_gen_identifier(ctx, NULL, SP_EVENT_TYPE_SOURCE | SP_EVENT_ON_CHANGE);

        AVBufferRef *source_event = sp_event_create(source_event_cb, NULL,
                                                    SP_EVENT_TYPE_SOURCE | SP_EVENT_ON_CHANGE,
                                                    source_event_ctx_ref,
                                                    identifier);

        /* Add and immediately run the event to signal all devices */
        ctx->api_fns[i]->ctrl(ctx->api_ctx[i], SP_EVENT_CTRL_NEW_EVENT | SP_EVENT_FLAG_IMMEDIATE, source_event);

        av_buffer_unref(&source_event);
    }

    LUA_INTERFACE_END(0);
}

static const struct luaL_Reg lua_lib_fns[] = {
    { "register_io_cb", lua_register_io_cb },

    { "create_io", lua_create_io },
    { "create_muxer", lua_create_muxer },
    { "create_encoder", lua_create_encoder },
    { "create_filter", lua_create_filter },
//    { "create_filterchain", lua_create_filterchain },

    { "link", lua_link_objects },
//    { "unlink", lua_unlink_objects },

    { "ctrl", lua_ctrl },
//    { "on_event", lua_onevent },

    { "set_epoch", lua_set_epoch },

    { "commit", lua_commit },
    { "discard", lua_discard },

//    { "destroy", lua_destroy },

//    { "terminate", lua_terminate },

    { NULL, NULL },
};

static void ln_completion(const char *str, linenoiseCompletions *lc)
{
    if (!strncmp(str, LUA_PUB_PREFIX ".", strlen(str)))
        linenoiseAddCompletion(lc, LUA_PUB_PREFIX ".");

    for (int i = 0; i < SP_ARRAY_ELEMS(lua_lib_fns) - 1; i++) {
        int len = strlen(LUA_PUB_PREFIX ".") + strlen(lua_lib_fns[i].name) + strlen("()") + 1;
        char *comp = av_malloc(len);
        av_strlcpy(comp, LUA_PUB_PREFIX ".", len);
        av_strlcat(comp, lua_lib_fns[i].name, len);
        if (!strncmp(str, comp, strlen(str))) {
            if (lua_lib_fns[i].func == lua_commit ||
                lua_lib_fns[i].func == lua_discard)
                av_strlcat(comp, "()", len);
            linenoiseAddCompletion(lc, comp);
        }
        av_free(comp);
    }
}

static char *ln_hints(const char *str, int *color, int *bold)
{
    int counts = 0, last_idx;

    for (int i = 0; i < SP_ARRAY_ELEMS(lua_lib_fns) - 1; i++) {
        int len = strlen(LUA_PUB_PREFIX ".") + strlen(lua_lib_fns[i].name) + 1;
        char *comp = av_malloc(len);
        av_strlcpy(comp, LUA_PUB_PREFIX ".", len);
        av_strlcat(comp, lua_lib_fns[i].name, len);
        if (!strncmp(str, comp, strlen(str))) {
            counts++;
            last_idx = i;
        }
        av_free(comp);
    }

    if (counts == 1) {
        const luaL_Reg *lr = &lua_lib_fns[last_idx];
        if (lr->func == lua_register_io_cb) {
            return av_strdup("(function)");
        } else if (lr->func == lua_create_io ||
                   lr->func == lua_create_muxer ||
                   lr->func == lua_create_encoder) {
            return av_strdup("(table/identifier, [table]");
        } else if (lr->func == lua_link_objects) {
            return av_strdup("(table/userdata>, table/userdata)");
        } else if (lr->func == lua_ctrl) {
            return av_strdup("(table/userdata, table/string)");
        } else if (lr->func == lua_set_epoch) {
            return av_strdup("(table/userdata/number/integer/string/function)");
        } else if (lr->func == lua_commit ||
                   lr->func == lua_discard) {
            return NULL;
        }
    }

    return NULL;
}

static int lfn_loadfile(MainContext *ctx, const char *script_name)
{
    int err = luaL_loadfilex(ctx->lua, script_name, "bt");
    if (err == LUA_ERRFILE) {
        av_log(ctx, AV_LOG_ERROR, "File \"%s\" not found!\n", script_name);
        return AVERROR(EINVAL);
    } else if (err) {
        av_log(ctx, AV_LOG_ERROR, "%s\n", lua_tostring(ctx->lua, -1));
        return AVERROR_EXTERNAL;
    }

    return 0;
}

static void load_lua_library(MainContext *ctx, const char *lib)
{
#define LUA_BASELIBNAME "base"
    static const luaL_Reg lua_internal_lib_fns[] = {
        { LUA_BASELIBNAME, luaopen_base },
        { LUA_COLIBNAME, luaopen_coroutine },
        { LUA_TABLIBNAME, luaopen_table },
        { LUA_IOLIBNAME, luaopen_io },
        { LUA_OSLIBNAME, luaopen_os },
        { LUA_STRLIBNAME, luaopen_string },
        { LUA_UTF8LIBNAME, luaopen_utf8 },
        { LUA_MATHLIBNAME, luaopen_math },
        { LUA_DBLIBNAME, luaopen_debug },
    };

    lua_State *L = ctx->lua;

    int i = 0;
    for (; i < FF_ARRAY_ELEMS(lua_internal_lib_fns); i++) {
        if (!strcmp(lua_internal_lib_fns[i].name, lib)) {
            luaL_requiref(L, lua_internal_lib_fns[i].name,
                          lua_internal_lib_fns[i].func, 1);
            lua_pop(ctx->lua, 1);
            return;
        }
    }
}

int main(int argc, char *argv[])
{
    int err = 0;
    MainContext *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return AVERROR(ENOMEM);

    pthread_mutexattr_t main_lock_attr;
    pthread_mutexattr_init(&main_lock_attr);
    pthread_mutexattr_settype(&main_lock_attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&ctx->lock, &main_lock_attr);

    av_max_alloc(SIZE_MAX);
    av_log_set_level(AV_LOG_INFO);
    av_dict_set(&sp_component_log_levels, "global", "info", 0);
    av_log_set_callback(av_log_default_callback);

    err = sp_alloc_class(ctx, PROJECT_NAME, AV_CLASS_CATEGORY_NA, &ctx->log_lvl_offset, NULL);
    if (err < 0)
        return AVERROR(ENOMEM);

    ctx->commit_list = sp_bufferlist_new();
    ctx->discard_list = sp_bufferlist_new();
    ctx->lua_buf_refs = sp_bufferlist_new();
    ctx->epoch_value = ATOMIC_VAR_INIT(0);
    ctx->source_update_cb_ref = LUA_NOREF;

    /* Options */
    int disable_repl = 0;
    const char *script_name = NULL;
    const char *script_entrypoint = NULL;
    char *lua_libs_list = av_strdup(LUA_BASELIBNAME","
                                    LUA_COLIBNAME","
                                    LUA_TABLIBNAME","
                                    LUA_STRLIBNAME","
                                    LUA_UTF8LIBNAME","
                                    LUA_MATHLIBNAME","
                                    LUA_DBLIBNAME); /* io, os and require not loaded due to security concerns */

    /* Options parsing */
    int opt;
    while ((opt = getopt(argc, argv, "Nvhs:e:r:V:")) != -1) {
        switch (opt) {
        case 's':
            script_name = optarg;
            break;
        case 'e':
            script_entrypoint = optarg;
            break;
        case 'v':
            printf("%s %s (%s)\n", PROJECT_NAME, PROJECT_VERSION_STRING, vcstag);
            goto end;
        case 'N':
            disable_repl = 1;
            break;
        case 'r':
            {
                size_t new_len = strlen(lua_libs_list) + strlen(optarg) + strlen(",") + 1;
                char *new_str = av_malloc(new_len);
                av_strlcpy(new_str, lua_libs_list, new_len);
                av_strlcat(new_str, ",", new_len);
                av_strlcat(new_str, optarg, new_len);
                lua_libs_list = new_str;
            }
            break;
        case 'V':
            {
                int llvl = sp_str_to_log_lvl(optarg);
                if (llvl != INT_MAX) {
                    av_dict_set(&sp_component_log_levels, "global", optarg, 0);
                    break;
                }

                err = av_dict_parse_string(&sp_component_log_levels, optarg, "=", ",", 0);
                if (err < 0) {
                    av_log(ctx, AV_LOG_ERROR, "Cannot parse log levels: %s!\n", av_err2str(err));
                    goto end;
                }
            }
            break;
        default:
            av_log(ctx, AV_LOG_ERROR, "Unrecognized option \'%c\'!\n", optopt);
            err = AVERROR(EINVAL);
        case 'h':
            printf("Usage info:\n"
                   "    -s <path>                     External Lua script name\n"
                   "    -e <entrypoint>               Entrypoint to call into the custom script\n"
                   "    -r <string1>,<string2>        Additional comma-separated Lua libraries/packages to load\n"
                   "    -V <component>=<level>,...    Per-component log level, set \"global\" or leave component out for global\n"
                   "    -N                            Disable command line interface\n"
                   "    -v                            Print program version\n"
                   "    -h                            Usage help (this)\n"
                   "    <trailing arguments>          Given directly to the script's entrypoint to interpret\n"
                   );
            goto end;
        }
    }

    AVDictionaryEntry *entry = NULL;
    while ((entry = av_dict_get(sp_component_log_levels, "", entry, AV_DICT_IGNORE_SUFFIX))) {
        int llvl = sp_str_to_log_lvl(entry->value);
        if (llvl == INT_MAX) {
            av_log(ctx, AV_LOG_ERROR, "Invalid loglevel \"%s\"!\n", entry->value);
            err = AVERROR(EINVAL);
            goto end;
        }
        if (!strcmp(entry->key, "global"))
            av_log_set_level(llvl);
    }

    if (script_entrypoint && !script_name) {
        av_log(ctx, AV_LOG_ERROR, "Must specify custom script to use a custom entrypoint!\n");
        err = AVERROR(EINVAL);
        goto end;
    }

    av_log(ctx, AV_LOG_INFO, "Starting %s %s (%s)\n",
           PROJECT_NAME, PROJECT_VERSION_STRING, vcstag);

    /* Initialize I/O APIs */
    ctx->api_fns = av_malloc(sp_compiled_apis_len * sizeof(*ctx->api_fns));
    ctx->api_ctx = av_malloc(sp_compiled_apis_len * sizeof(*ctx->api_ctx));
    for (int i = 0; i < sp_compiled_apis_len; i++) {
        ctx->api_fns[ctx->num_apis] = sp_compiled_apis[i];
        err = ctx->api_fns[ctx->num_apis]->init_sys(&ctx->api_ctx[i]);
        if (err < 0) {
            av_log(ctx, AV_LOG_WARNING, "Unable to load API \"%s\"!\n",
                   ctx->api_fns[ctx->num_apis]->name);
            continue;
        }

        ctx->num_apis++;
    }

    /* Create Lua context */
    ctx->lua = lua_newstate(lua_alloc_fn, ctx);
    lua_gc(ctx->lua, LUA_GCGEN);
    lua_setwarnf(ctx->lua, lua_warning_handler, ctx);
    lua_atpanic(ctx->lua, lua_panic_handler);

    { /* Load needed Lua libraries */
        char *save, *token = av_strtok(lua_libs_list, ",", &save);
        while (token) {
            load_lua_library(ctx, token);
            token = av_strtok(NULL, ",", &save);
        }
    }

    /* Load our lib */
    luaL_newlibtable(ctx->lua, lua_lib_fns);
    lua_pushlightuserdata(ctx->lua, ctx);
    luaL_setfuncs(ctx->lua, lua_lib_fns, 1);
    lua_setglobal(ctx->lua, LUA_PUB_PREFIX);

    AVDictionary *lua_globals = NULL;
    lua_pushglobaltable(ctx->lua);
    lua_pushnil(ctx->lua);
    while (lua_next(ctx->lua, -2)) {
        av_dict_set(&lua_globals, lua_tostring(ctx->lua, -2), "init_time", 0);
        lua_pop(ctx->lua, 1);
    }
    lua_pop(ctx->lua, 1);

    /* Load the script */
    if (script_name) {
        if ((err = lfn_loadfile(ctx, script_name)))
            goto end;
    } else {
        err = luaL_loadbufferx(ctx->lua, scripts_default_lua_bin,
                               scripts_default_lua_bin_len, "built-in script", "b");
        if (err) {
            av_log(ctx, AV_LOG_ERROR, "%s\n", lua_tostring(ctx->lua, -1));
            err = AVERROR_EXTERNAL;
            goto end;
        }
        script_entrypoint = "initial_config";
    }

    /* Run the script */
    if (lua_pcall(ctx->lua, 0, 0, 0) != LUA_OK) {
        av_log(ctx, AV_LOG_ERROR, "Lua script error: %s\n", lua_tostring(ctx->lua, -1));
        goto end;
    }

    /* Run script entrypoint if specified */
    if (script_entrypoint) {
        lua_getglobal(ctx->lua, script_entrypoint);
        if (!lua_isfunction(ctx->lua, -1)) {
            av_log(ctx, AV_LOG_ERROR, "Entrypoint \"%s\" not found!\n",
                   script_entrypoint);
            goto end;
        }
        int args = argc - optind;
        for(; optind < argc; optind++)
            lua_pushstring(ctx->lua, argv[optind]);
        if (lua_pcall(ctx->lua, args, 0, 0) != LUA_OK) {
            av_log(ctx, AV_LOG_ERROR, "Error running \"%s\": %s\n",
                   script_entrypoint, lua_tostring(ctx->lua, -1));
            goto end;
        }
    }

    if (disable_repl) {
        sleep(9999);
        av_log(ctx, AV_LOG_INFO, "Quitting!\n");
        goto end;
    }

    /* Linenoise */
    linenoiseSetCompletionCallback(ln_completion);
    linenoiseSetHintsCallback(ln_hints);
    linenoiseSetFreeHintsCallback(av_free);

    /* REPL */
    char *line;
    while ((line = linenoise("lua > ")) != NULL) {
        pthread_mutex_lock(&ctx->lock);

        if (!strcmp("quit", line) || !strcmp("exit", line)) {
            pthread_mutex_unlock(&ctx->lock);
            linenoiseFree(line);
            break;
        } else if (!strcmp("help", line)) {
            printf("txproto library:\n");
            for (int i = 0; i < SP_ARRAY_ELEMS(lua_lib_fns) - 1; i++) {
                int len = strlen(LUA_PUB_PREFIX ".") + strlen(lua_lib_fns[i].name) + strlen("()") + 1;
                char *comp = av_malloc(len);
                av_strlcpy(comp, LUA_PUB_PREFIX ".", len);
                av_strlcat(comp, lua_lib_fns[i].name, len);
                int col = 0, bold = 0;
                char *hint = ln_hints(comp, &col, &bold);
                if (lua_lib_fns[i].func == lua_commit ||
                    lua_lib_fns[i].func == lua_discard)
                    av_strlcat(comp, "()", len);
                if (hint) {
                    comp = av_realloc(comp, len + strlen(hint));
                    av_strlcat(comp, hint, len + strlen(hint));
                }
                printf("    %s\n", comp);
                av_free(comp);
            }

            printf("Lua globals:\n");
            lua_pushglobaltable(ctx->lua);
            lua_pushnil(ctx->lua);
            while (lua_next(ctx->lua, -2)) {
                const char *name = lua_tostring(ctx->lua, -2);
                if (!dict_get(lua_globals, name))
                    printf("    %s\n", name);
                lua_pop(ctx->lua, 1);
            }
            lua_pop(ctx->lua, 1);

            printf("Other functions:\n"
                   "    load <path> <optional entrypoint> <optional argument 1>...\n"
                   "    require <string> <string>...\n"
                   "    stat\n"
                   "    help\n"
                   "    exit\n"
                   "    quit\n");
            goto repl_end;
        } else if (!strncmp(line, "load", strlen("load"))) {
            script_name = NULL;
            script_entrypoint = NULL;

            int num_arguments = 0;
            char *line_mod = av_strdup(line);
            char *save, *token = av_strtok(line_mod, " ", &save);
            token = av_strtok(NULL, " ", &save); /* Skip "load" */
            while (token) {
                if (!script_name) {
                    script_name = token;
                    if (lfn_loadfile(ctx, script_name) < 0)
                        break;
                    if (lua_pcall(ctx->lua, 0, 0, 0) != LUA_OK) {
                        av_log(ctx, AV_LOG_ERROR, "Lua script error: %s\n",
                               lua_tostring(ctx->lua, -1));
                        break;
                    }
                } else if (!script_entrypoint) {
                    script_entrypoint = token;
                    lua_getglobal(ctx->lua, script_entrypoint);
                    if (!lua_isfunction(ctx->lua, -1)) {
                        av_log(ctx, AV_LOG_ERROR, "Entrypoint \"%s\" not found!\n",
                               script_entrypoint);
                        break;
                    }
                } else {
                    lua_pushstring(ctx->lua, token);
                    num_arguments++;
                }
                token = av_strtok(NULL, " ", &save);
            }

            /* No error encountered */
            if (!script_name) {
                av_log(ctx, AV_LOG_ERROR, "Missing path for \"load\"!\n");
            } else if (!token) {
                if (!script_entrypoint) {
                    linenoiseHistoryAdd(line);
                } else if (lua_pcall(ctx->lua, num_arguments, 0, 0) != LUA_OK) {
                    av_log(ctx, AV_LOG_ERROR, "Error running \"%s\": %s\n",
                           script_entrypoint, lua_tostring(ctx->lua, -1));
                } else {
                    linenoiseHistoryAdd(line);
                }
            }

            av_free(line_mod);
            goto repl_end;
        } else if (!strcmp("stat", line)) {
            size_t mem_used = lua_gc(ctx->lua, LUA_GCCOUNT) * 1024 + lua_gc(ctx->lua, LUA_GCCOUNTB);

            av_log(ctx, AV_LOG_INFO, "Lua memory used: %.2f KiB\n", mem_used / 1024.0f);
            goto repl_end;
        } else if (!strncmp(line, "require", strlen("require"))) {
            char *save, *token = av_strtok(line, " ", &save);
            token = av_strtok(NULL, " ", &save); /* Skip "require" */
            if (!token) {
                av_log(ctx, AV_LOG_ERROR, "Missing library name(s) for \"require\"!\n");
                goto repl_end;
            }

            while (token) {
                load_lua_library(ctx, token);
                token = av_strtok(NULL, " ", &save);
            }

            goto repl_end;
        }

        int ret = luaL_dostring(ctx->lua, line);
        if (ret == LUA_OK)
            linenoiseHistoryAdd(line);
        if (lua_isstring(ctx->lua, -1))
            av_log(NULL, ret == LUA_OK ? AV_LOG_INFO : AV_LOG_ERROR,
                   "%s\n", lua_tostring(ctx->lua, -1));

        lua_pop(ctx->lua, 1);

repl_end:
        pthread_mutex_unlock(&ctx->lock);
        linenoiseFree(line);
    }

    av_log(ctx, AV_LOG_INFO, "Quitting!\n");

end:
    for (int i = 0; i < ctx->num_apis; i++)
        av_buffer_unref(&ctx->api_ctx[i]);
    av_free(ctx->api_fns);
    av_free(ctx->api_ctx);

    sp_bufferlist_free(&ctx->lua_buf_refs);

    sp_bufferlist_free(&ctx->commit_list);
    sp_bufferlist_free(&ctx->discard_list);

    if (ctx->lua) {
        if (ctx->source_update_cb_ref != LUA_NOREF &&
            ctx->source_update_cb_ref != LUA_REFNIL)
            luaL_unref(ctx->lua, LUA_REGISTRYINDEX, ctx->source_update_cb_ref);
        lua_close(ctx->lua);
    }

    pthread_mutex_destroy(&ctx->lock);
    av_dict_free(&sp_component_log_levels);
    av_dict_free(&lua_globals);
    av_free(lua_libs_list);
    sp_free_class(ctx);
    av_free(ctx);

    return err;
}
