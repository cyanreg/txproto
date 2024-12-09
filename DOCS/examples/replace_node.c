#include <stdio.h>
#include <assert.h>
#include <limits.h>
#include <pthread.h>

#include <libavutil/buffer.h>
#include <libavutil/dict.h>
#include <libavutil/time.h>

#include <libtxproto/events.h>
#include <libtxproto/txproto.h>

/**
 * @file example of how one can dynamically replace a filtergraph node
 * @example replace_node.c
 *
 * This example shows how a filtergraph node can be replaced dynamically on a
 * running stream. It is recommended to run this example with a real-time input
 * like an MPEGTS or RTP stream (see example usage below).
 *
 * The graph initially starts as:
 *
 *     ┌───────┐   ┌────┐   ┌─────┐   ┌───────┐   ┌─────┐
 *     │demuxer│──►│h264│──►│hflip│──►│libx264│──►│muxer│
 *     └───────┘   └────┘   └─────┘   └───────┘   └─────┘
 *
 * When the user presses enter, `hflip` is destroyed and a new filtergraph
 * `vflip` is created:
 *
 *                           ┌ ─ ─ ┐
 *                        ┌ ► hflip ─ ┐
 *     ┌───────┐   ┌────┐    └ ─ ─ ┘     ┌───────┐   ┌─────┐
 *     │demuxer│──►│h264│─┤           ├─►│libx264│──►│muxer│
 *     └───────┘   └────┘ │  ┌─────┐  │  └───────┘   └─────┘
 *                        └─►│vflip│──┘
 *                           └─────┘
 *
 * The user can press enter again to repeat the process indefinitely,
 * alternating between an `hflip` and a `vflip` filter. Note that the filters
 * are purposefully destroyed and recreated each time and aren't being reused.
 *
 * Building
 * --------
 *
 *     gcc -Wall -g replace_node.c $(pkg-config --cflags --libs txproto libavutil)
 *
 * Usage
 * -----
 *
 *     ./a.out <in-url> <decoder> <encoder> <out-url>
 *
 * or
 *
 *     ./a.out <in-url> <in-fmt> <decoder> <encoder> <out-fmt> <out-url>
 *
 * Example
 * -------
 *
 * Start the example:
 *
 *     ./a.out udp://127.0.0.1:9000 h264 libx264 udp://127.0.0.1:9001
 *
 * Then, in another terminal, start the player:
 *
 *     ffplay udp://127.0.0.1:9001
 *
 * Finally, in yet another terminal, start the source:
 *
 *     ffmpeg -re -f lavfi -i testsrc=r=30:s=hd720 -c:v libx264 -g 60 -f mpegts udp://127.1:9000
 *
 */

struct Args {
    const char *in_url;
    const char *in_fmt;
    const char *decoder;
    const char *encoder;
    const char *out_fmt;
    const char *out_url;
};

static void print_usage(FILE *f, const char *arg0)
{
    fprintf(
        f,
        "Usage:\n"
        "  %1$s <in-url> <decoder> <encoder> <out-url>\n"
        "  %1$s <in-url> <in-fmt> <decoder> <encoder> <out-fmt> <out-url>\n",
        arg0
    );
}

// helper macros for error handling
#define TRY(e)                   \
    do {                         \
        int err = e;             \
        if (err < 0) return err; \
    } while(0)
#define EXPECT(e)         \
    do {                  \
        int err = e;      \
        assert(err >= 0); \
    } while(0)

static AVDictionary *make_filter_init_opts()
{
    AVDictionary *init_opts = NULL;

    /* by default, filters send an EOS signal to their outputs when they get
     * destroyed. we don't want that, as that would stop the encoder! */
    EXPECT(av_dict_set(&init_opts, "send_eos", "false", 0));

    return init_opts;
}

int main(int argc, char *argv[])
{
    struct Args args;

    if (argc == 1) {
        print_usage(stdout, argv[0]);
        return 0;
    } else if (argc == 5) {
        args.in_url = argv[1];
        args.in_fmt = NULL;
        args.decoder = argv[2];
        args.encoder = argv[3];
        args.out_fmt = NULL;
        args.out_url = argv[4];
    } else if (argc == 7) {
        args.in_url = argv[1];
        args.in_fmt = argv[2];
        args.decoder = argv[3];
        args.encoder = argv[4];
        args.out_fmt = argv[5];
        args.out_url = argv[6];
    } else {
        fprintf(stderr, "Expected 4 or 6 arguments, got %d\n", argc - 1);
        print_usage(stderr, argv[0]);
        return 1;
    }

    TXMainContext *ctx = tx_new();

    EXPECT(tx_init(ctx));
    EXPECT(tx_epoch_set(ctx, 0));

    printf("Creating nodes...\n");
    AVBufferRef *demuxer = tx_demuxer_create(
        ctx,
        NULL,        // Name
        args.in_url, // in_url
        args.in_fmt, // in_format
        NULL,        // start_options
        NULL         // init_opts
    );
    AVBufferRef *decoder = tx_decoder_create(
        ctx,
        args.decoder, // dec_name
        NULL          // init_opts
    );
    AVBufferRef *filter = tx_filtergraph_create(
        ctx,
        "hflip",
        AV_HWDEVICE_TYPE_NONE,
        make_filter_init_opts()
    );
    AVBufferRef *encoder = tx_encoder_create(
        ctx,
        args.encoder,
        NULL,                        // name
        NULL,                        // options
        NULL // init_opts
    );
    AVBufferRef *muxer = tx_muxer_create(
        ctx,
        args.out_url,
        args.out_fmt, // out_format
        NULL,         // options
        NULL          // init_opts
    );

    printf("Initial setup...\n");
    EXPECT(tx_link(ctx, demuxer, decoder, 0));
    EXPECT(tx_link(ctx, decoder, filter, 0));
    EXPECT(tx_link(ctx, filter, encoder, 0));
    EXPECT(tx_link(ctx, encoder, muxer, 0));
    EXPECT(tx_commit(ctx));

    int hflip = 1;
    while (1) {
        printf("Press enter to change filter...\n");
        getchar();

        hflip = !hflip;

        if (hflip) {
            printf("Replacing vflip with hflip...\n");
        } else {
            printf("Replacing hflip with vflip...\n");
        }

        // destroy previous filter
        EXPECT(tx_destroy(ctx, &filter));

        // create the new one
        filter = tx_filtergraph_create(
            ctx,
            hflip ? "hflip" : "vflip",
            AV_HWDEVICE_TYPE_NONE,
            make_filter_init_opts()
        );

        EXPECT(tx_link(ctx, decoder, filter, 0));
        EXPECT(tx_link(ctx, filter, encoder, 0));
        EXPECT(tx_commit(ctx));
    }

    printf("Freeing...\n");
    tx_free(ctx);

    return 0;
}
