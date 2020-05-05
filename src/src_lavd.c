#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <libavdevice/avdevice.h>
#include <libavutil/avstring.h>
#include <libavutil/time.h>

#include "src_common.h"
#include "utils.h"

typedef struct LavdCaptureCtx {
    struct LavdCtx *main;
    AVInputFormat *src;
    char *src_name;
    AVFormatContext *avf;
    pthread_t pull_thread;
    AVCodecContext *avctx;
    SPFrameFIFO *fifo;
    int64_t delay;
    atomic_bool quit;
} LavdCaptureCtx;

typedef struct LavdCtx {
    AVClass *class;

    int64_t epoch;

    LavdCaptureCtx **capture_ctx;
    int capture_ctx_num;

    /* This is just temporary data, update it in the sources function */
    SourceInfo *tmp_sources;
    int num_tmp_sources;
} LavdCtx;

FN_CREATING(LavdCtx, LavdCaptureCtx, capture_ctx, capture_ctx, capture_ctx_num)

void *lavd_thread(void *s)
{
    int64_t pts;
    int err = 0, flushed = 0;
    LavdCaptureCtx *ctx = s;

    pthread_setname_np(pthread_self(), ctx->avf->iformat->name);

    while (!flushed) {
        AVPacket *pkt = NULL;
        if (atomic_load(&ctx->quit))
            goto send;

        pkt = av_packet_alloc();

        err = av_read_frame(ctx->avf, pkt);
        if (err) {
            av_log(ctx->main, AV_LOG_ERROR, "Unable to read frame: %s!\n", av_err2str(err));
            goto end;
        }

        if (!ctx->delay)
            ctx->delay = av_gettime_relative() - ctx->main->epoch - pkt->pts;
        pkt->pts += ctx->delay;

send:
        pts = pkt ? pkt->pts : AV_NOPTS_VALUE;

        /* Send frame for decoding */
        err = avcodec_send_packet(ctx->avctx, pkt);
        av_packet_free(&pkt);
        if (err == AVERROR_EOF) {
            av_log(ctx->main, AV_LOG_INFO, "Decoder flushed!\n");
            err = 0;
            break; /* decoder flushed */
        } else if (err && (err != AVERROR(EAGAIN))) {
            av_log(ctx->main, AV_LOG_ERROR, "Unable to decode frame: %s!\n", av_err2str(err));
            goto end;
        }

        AVFrame *frame = av_frame_alloc();
        err = avcodec_receive_frame(ctx->avctx, frame);
        if (err == AVERROR_EOF) {
            av_log(ctx->main, AV_LOG_INFO, "Decoder flushed!\n");
            av_frame_free(&frame);
            err = 0;
            break;
        } else if (err && (err != AVERROR(EAGAIN))) {
            av_log(ctx->main, AV_LOG_ERROR, "Unable to get decoded frame: %s!\n", av_err2str(err));
            av_frame_free(&frame);
            goto end;
        }

        if (frame) {
            /* avcodec_open2 changes the timebase */
            frame->pts = av_rescale_q(pts, ctx->avf->streams[0]->time_base, ctx->avctx->time_base);

            frame->opaque_ref = av_buffer_allocz(sizeof(FormatExtraData));

            FormatExtraData *fe = (FormatExtraData *)frame->opaque_ref->data;
            fe->time_base       = ctx->avctx->time_base;
            fe->avg_frame_rate  = ctx->avctx->framerate;

            if (ctx->fifo)
                sp_frame_fifo_push(ctx->fifo, frame);
            else
                av_frame_free(&frame);
        }
    }

end:
    return NULL;
}

static int start_lavd(void *s, uint64_t identifier, AVDictionary *opts, SPFrameFIFO *dst,
                      error_handler *err_cb, void *error_handler_ctx)
{
    int err = 0;
    LavdCtx *ctx = s;
    SourceInfo *src = NULL;

    for (int i = 0; i < ctx->num_tmp_sources; i++) {
        if (identifier == ctx->tmp_sources[i].identifier) {
            src = &ctx->tmp_sources[i];
            break;
        }
    }

    if (!src)
        return AVERROR(EINVAL);

    LavdCaptureCtx *cap_ctx = create_capture_ctx(ctx);

    cap_ctx->main = ctx;
    cap_ctx->fifo = dst;
    cap_ctx->quit = ATOMIC_VAR_INIT(0);
    cap_ctx->src = (AVInputFormat *)src->opaque;
    cap_ctx->src_name = av_strdup(src->name);

    err = avformat_open_input(&cap_ctx->avf, src->name, cap_ctx->src, &opts);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Unable to open context for source \"%s\": %s\n",
               src->name, av_err2str(err));
        return err;
    }

    err = avformat_find_stream_info(cap_ctx->avf, NULL);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Unable to get stream info for source \"%s\": %s\n",
               src->name, av_err2str(err));
        return err;
    }

    AVCodecParameters *codecpar = cap_ctx->avf->streams[0]->codecpar;

    AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);

    cap_ctx->avctx = avcodec_alloc_context3(codec);

    avcodec_parameters_to_context(cap_ctx->avctx, codecpar);
    cap_ctx->avctx->time_base = cap_ctx->avf->streams[0]->time_base;
    cap_ctx->avctx->framerate = cap_ctx->avf->streams[0]->avg_frame_rate;

    err = avcodec_open2(cap_ctx->avctx, codec, NULL);
	if (err) {
		av_log(ctx, AV_LOG_ERROR, "Cannot open encoder: %s!\n", av_err2str(err));
		return err;
	}

    pthread_create(&cap_ctx->pull_thread, NULL, lavd_thread, cap_ctx);

    return 0;
}

static int stop_lavd(void  *s, uint64_t identifier)
{
    LavdCtx *ctx = s;

    SourceInfo *src = NULL;
    for (int i = 0; i < ctx->num_tmp_sources; i++) {
        if (identifier == ctx->tmp_sources[i].identifier) {
            src = &ctx->tmp_sources[i];
            break;
        }
    }
    if (!src)
        return AVERROR(EINVAL);

    LavdCaptureCtx *cap_ctx = NULL;
    for (int i = 0; i < ctx->capture_ctx_num; i++) {
        if ((AVInputFormat *)src->opaque == ctx->capture_ctx[i]->src &&
            !strcmp(src->name, ctx->capture_ctx[i]->src_name)) {
            cap_ctx = ctx->capture_ctx[i];
            break;
        }
    }
    if (!cap_ctx)
        return AVERROR(EINVAL);

    atomic_store(&cap_ctx->quit, 1);

    pthread_join(cap_ctx->pull_thread, NULL);

    /* EOF */
    if (cap_ctx->fifo) {
        sp_frame_fifo_push(cap_ctx->fifo, NULL);

        /* This is really stupid, but v4l2 has to memory unmap all its buffers at
         * uninit time, and if we don't do this, it'll segfault when we try to
         * encode or filter the residual frames */
        while (sp_frame_fifo_get_size(cap_ctx->fifo) > 0);
    }

    /* Free */
    avcodec_free_context(&cap_ctx->avctx);
    avformat_flush(cap_ctx->avf);
    avformat_close_input(&cap_ctx->avf);
    av_free(cap_ctx->src_name);

    return 0;
}

static void free_tmp_sources(LavdCtx *ctx)
{
    for (int i = 0; i < ctx->num_tmp_sources; i++) {
        av_free(ctx->tmp_sources[i].name);
        av_free(ctx->tmp_sources[i].desc);
    }
    av_freep(&ctx->tmp_sources);
}

static void sources_lavd(void *s, SourceInfo **sources, int *num)
{
    int err;
    LavdCtx *ctx = s;

    free_tmp_sources(ctx);

    AVInputFormat *cur = NULL;
    while ((cur = av_input_video_device_next(cur))) {
        AVDeviceInfoList *list = NULL;
        err = avdevice_list_input_sources(cur, NULL, NULL, &list);
        if ((err && (err != AVERROR(ENOSYS)))) {
            av_log(ctx, AV_LOG_DEBUG, "Unable to retrieve device list for source \"%s\": %s\n",
                   cur->name, av_err2str(err));
            continue;
        }

        if (!list || (err == AVERROR(ENOSYS))) {
            ctx->tmp_sources = av_realloc(ctx->tmp_sources,
                                          (++ctx->num_tmp_sources)*sizeof(SourceInfo));

            memset(&ctx->tmp_sources[ctx->num_tmp_sources - 1], 0, sizeof(SourceInfo));
            ctx->tmp_sources[ctx->num_tmp_sources - 1].name = av_strdup(cur->name);
            ctx->tmp_sources[ctx->num_tmp_sources - 1].desc = av_strdup(cur->long_name);
            ctx->tmp_sources[ctx->num_tmp_sources - 1].identifier = ctx->num_tmp_sources - 1;
            ctx->tmp_sources[ctx->num_tmp_sources - 1].opaque = (uint64_t)cur;
            continue;
        }

        int nb_devs = list->nb_devices;
        if (!nb_devs) {
            av_log(ctx, AV_LOG_WARNING, "Device \"%s\" has no entries in its devices list.\n", cur->name);
            continue;
        }

        for (int i = 0; i < nb_devs; i++) {
            AVDeviceInfo *info = list->devices[i];
            ctx->tmp_sources = av_realloc(ctx->tmp_sources,
                                          (++ctx->num_tmp_sources)*sizeof(SourceInfo));

            memset(&ctx->tmp_sources[ctx->num_tmp_sources - 1], 0, sizeof(SourceInfo));

            ctx->tmp_sources[ctx->num_tmp_sources - 1].name = av_strdup(info->device_name);
            ctx->tmp_sources[ctx->num_tmp_sources - 1].desc = av_strdup(info->device_description);
            ctx->tmp_sources[ctx->num_tmp_sources - 1].identifier = ctx->num_tmp_sources - 1;
            ctx->tmp_sources[ctx->num_tmp_sources - 1].opaque = (uint64_t)cur;
        }

        avdevice_free_list_devices(&list);
    }

    *sources = ctx->tmp_sources;
    *num = ctx->num_tmp_sources;
}

static void uninit_lavd(void **s)
{
    if (!s || !*s)
        return;

    LavdCtx *ctx = *s;
    av_free(ctx->class);
    av_freep(s);
}

static int init_lavd(void **s, int64_t epoch)
{
    LavdCtx *ctx = av_mallocz(sizeof(*ctx));
    ctx->class = av_mallocz(sizeof(*ctx->class));
    *ctx->class = (AVClass) {
        .class_name = "lavd",
        .item_name  = av_default_item_name,
        .version    = LIBAVUTIL_VERSION_INT,
    };

    avdevice_register_all();

    ctx->epoch = epoch;

    *s = ctx;

    return 0;
}

const CaptureSource src_lavd = {
    .name    = "libavdevice",
    .init    = init_lavd,
    .start   = start_lavd,
    .sources = sources_lavd,
    .stop    = stop_lavd,
    .free    = uninit_lavd,
};
