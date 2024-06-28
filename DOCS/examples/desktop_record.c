#include <stdio.h>
#include <assert.h>
#include <limits.h>
#include <pthread.h>

#include <libavutil/buffer.h>
#include <libavutil/dict.h>
#include <libavutil/time.h>

#include <libtxproto/events.h>
#include <libtxproto/txproto.h>

// gcc -Wall -g desktop_record.c -o desktop_record `pkg-config --cflags --libs txproto libavutil`

typedef struct IoEntryCb {
    uint32_t identifier;
} IoEntryCb;

static int source_event_cb(IOSysEntry *entry, void *userdata)
{
    IoEntryCb *cb_data = userdata;

    printf("%s\n", __func__);
    printf("\tName: %s\n", sp_class_get_name(entry));
    printf("\tType: %s\n", sp_iosys_entry_type_string(entry->type));
    printf("\tIdentifier: 0x%X\n", entry->identifier);
    printf("\tDefault: %d\n", entry->is_default);

    if (sp_class_get_type(entry) & SP_TYPE_VIDEO_BIDIR) {
        printf("\tX: %d\n", entry->x);
        printf("\tY: %d\n", entry->y);
        printf("\tWidth: %d\n", entry->width);
        printf("\tHeight: %d\n", entry->height);
        printf("\tScale: %d\n", entry->scale);
        printf("\tFramerate: %f\n", av_q2d(entry->framerate));

        if (!cb_data->identifier && entry->is_default)
            cb_data->identifier = entry->identifier;
    }

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

    // Enumerate displays
    IoEntryCb cb_data;
    cb_data.identifier = 0;

    AVBufferRef *event = tx_io_register_cb(
        ctx,
        NULL, // api_list
        source_event_cb,
        &cb_data
    );
    assert(event);

    tx_event_destroy(ctx, event);

    // Create encoding pipeline
    AVBufferRef *video_source = tx_io_create(
        ctx,
        cb_data.identifier,
        NULL // opts
    );
    assert(video_source);

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

    err = tx_link(ctx, video_source, encoder, 0);
    assert(err == 0);

    // Muxer
    AVDictionary *muxer_options = NULL;
    err = av_dict_set_int(&muxer_options, "low_latency", 1, 0);
    assert(err == 0);

    err = av_dict_set(&muxer_options, "sdp_file", "video.sdp", 0);
    assert(err == 0);

    AVBufferRef *muxer = tx_muxer_create(
        ctx,
        "rtp://127.0.0.1:30000",
        "rtp",
        muxer_options,
        NULL
    );

    err = tx_link(ctx, encoder, muxer, 0);
    assert(err == 0);

    // Commit
    err = tx_commit(ctx);
    assert(err == 0);

    av_usleep(30000000);

    tx_free(ctx);

    return 0;
}
