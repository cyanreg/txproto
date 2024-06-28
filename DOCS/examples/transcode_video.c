#include <stdio.h>
#include <assert.h>
#include <limits.h>
#include <pthread.h>

#include <libavutil/buffer.h>
#include <libavutil/dict.h>
#include <libavutil/time.h>

#include <libtxproto/events.h>
#include <libtxproto/txproto.h>

// gcc -Wall -g transcode_video.c -o transcode_video `pkg-config --cflags --libs txproto libavutil`

struct EosEvent {
    pthread_cond_t *cond;
    int eos;
};

static int muxer_eos_cb(AVBufferRef *event_ref, void *callback_ctx, void *ctx,
                 void *dep_ctx, void *data)
{
    struct EosEvent *event = callback_ctx;

    printf("End of stream!\n");
    event->eos = 1;

    int err = pthread_cond_signal(event->cond);
    assert(err == 0);

    return 0;
}

int main(int argc, char *argv[])
{
    int err;

    TXMainContext *ctx = tx_new();
    err = tx_init(ctx);
    assert(err == 0);

    err = tx_epoch_set(ctx, 0);
    assert(err == 0);

    // Demuxer
    AVBufferRef *demuxer = tx_demuxer_create(
        ctx,
        NULL, // Name
        "test.webm", // in_url
        NULL, // in_format
        NULL, // start_options
        NULL // init_opts
    );

    // Decoder
    AVBufferRef *decoder = tx_decoder_create(
        ctx,
        "vp9", // dec_name
        NULL // init_opts
    );

    err = tx_link(ctx, demuxer, decoder, 0);
    assert(err == 0);

    // Encoder
    AVDictionary *encoder_options = NULL;
    err = av_dict_set(&encoder_options, "b", "20M", 0);
    assert(err == 0);

    err = av_dict_set(&encoder_options, "bf", "0", 0);
    assert(err == 0);

    AVBufferRef *encoder = tx_encoder_create(
        ctx,
        &(TxEncoderOptions) {
            .enc_name = "h264_nvenc",
            .options = encoder_options,
        }
    );

    err = tx_link(ctx, decoder, encoder, 0);
    assert(err == 0);

    // Muxer
    AVBufferRef *muxer = tx_muxer_create(
        ctx,
        "out.mkv",
        NULL, // out_format
        NULL, // options
        NULL  // init_opts
    );

    err = tx_link(ctx, encoder, muxer, 0);
    assert(err == 0);

    // Setup End of stream handler
    pthread_mutex_t mtx;
    err = pthread_mutex_init(&mtx, NULL);
    assert(err == 0);

    pthread_cond_t cond;
    err = pthread_cond_init(&cond, NULL);
    assert(err == 0);

    AVBufferRef *muxer_eof_event = NULL;
    muxer_eof_event = sp_event_create(muxer_eos_cb,
                                     NULL,
                                     sizeof(struct EosEvent),
                                     NULL,
                                     SP_EVENT_ON_EOS,
                                     NULL,
                                     NULL);

    struct EosEvent *eos_event = av_buffer_get_opaque(muxer_eof_event);
    eos_event->cond = &cond;
    eos_event->eos = 0;

    err = tx_event_register(ctx, muxer, muxer_eof_event);
    assert(err == 0);

    // Commit
    err = tx_commit(ctx);
    assert(err == 0);

    // Wait for EOS
    err = pthread_mutex_lock(&mtx);
    assert(err == 0);

    while (!eos_event->eos) {
        err = pthread_cond_wait(&cond, &mtx);
        assert(err == 0);
    }

    err = pthread_mutex_unlock(&mtx);
    assert(err == 0);

    // Free everything
    av_buffer_unref(&muxer_eof_event);
    pthread_mutex_destroy(&mtx);
    pthread_cond_destroy(&cond);
    tx_free(ctx);

    return 0;
}
