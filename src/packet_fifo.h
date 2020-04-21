#include <pthread.h>
#include <assert.h>
#include <libavcodec/avcodec.h>

typedef struct AVPacketFIFO {
    AVPacket **queued_packets;
    int num_queued_packets;
    int max_queued_packets;
    unsigned int queued_packets_alloc;
    pthread_mutex_t lock;
    pthread_cond_t cond_in;
    pthread_cond_t cond_out;
    pthread_mutex_t cond_lock_in;
    pthread_mutex_t cond_lock_out;
} AVPacketFIFO;

static inline int pkt_fifo_get_max_queued(AVPacketFIFO *buf)
{
    return buf->max_queued_packets;
}

static inline void pkt_init_fifo(AVPacketFIFO *buf, int max_queued_packets)
{
    pthread_mutex_init(&buf->lock, NULL);
    pthread_cond_init(&buf->cond_in, NULL);
    pthread_mutex_init(&buf->cond_lock_in, NULL);
    pthread_cond_init(&buf->cond_out, NULL);
    pthread_mutex_init(&buf->cond_lock_out, NULL);
    buf->num_queued_packets = 0;
    buf->queued_packets_alloc = 0;
    buf->max_queued_packets = max_queued_packets;
    buf->queued_packets = NULL;
}

static inline int pkt_get_fifo_size(AVPacketFIFO *buf)
{
    pthread_mutex_lock(&buf->lock);
    int ret = buf->num_queued_packets;
    pthread_mutex_unlock(&buf->lock);
    return ret;
}

static inline int pkt_push_to_fifo(AVPacketFIFO *buf, AVPacket *f)
{
    int err = 0;
    pthread_mutex_lock(&buf->cond_lock_out);
    pthread_mutex_lock(&buf->lock);

    if ((buf->max_queued_packets > 0) &&
        (buf->num_queued_packets > (buf->max_queued_packets + 1))) {
        pthread_mutex_unlock(&buf->lock);
        pthread_cond_wait(&buf->cond_out, &buf->cond_lock_out);
        pthread_mutex_lock(&buf->lock);
    }

    unsigned int oalloc = buf->queued_packets_alloc;
    AVPacket **fq = av_fast_realloc(buf->queued_packets, &buf->queued_packets_alloc,
                                   sizeof(AVPacket *)*(buf->num_queued_packets + 1));
    if (!fq) {
        buf->queued_packets_alloc = oalloc;
        err = AVERROR(ENOMEM);
        goto fail;
    }

    buf->queued_packets = fq;
    buf->queued_packets[buf->num_queued_packets++] = f;

    pthread_cond_signal(&buf->cond_in);

fail:
    pthread_mutex_unlock(&buf->lock);
    pthread_mutex_unlock(&buf->cond_lock_out);

    return err;
}

static inline AVPacket *pkt_pop_from_fifo(AVPacketFIFO *buf)
{
    pthread_mutex_lock(&buf->cond_lock_in);
    pthread_mutex_lock(&buf->lock);

    if (!buf->num_queued_packets) {
        pthread_mutex_unlock(&buf->lock);
        pthread_cond_wait(&buf->cond_in, &buf->cond_lock_in);
        pthread_mutex_lock(&buf->lock);
    }

    buf->num_queued_packets--;
    assert(buf->num_queued_packets >= 0);

    AVPacket *rf = buf->queued_packets[0];

    memmove(&buf->queued_packets[0], &buf->queued_packets[1],
            buf->num_queued_packets*sizeof(*buf->queued_packets));

    if (buf->max_queued_packets > 0)
        pthread_cond_signal(&buf->cond_out);

    pthread_mutex_unlock(&buf->lock);
    pthread_mutex_unlock(&buf->cond_lock_in);

    return rf;
}

static inline AVPacket *pkt_peek_from_fifo(AVPacketFIFO *buf)
{
    pthread_mutex_lock(&buf->cond_lock_in);
    pthread_mutex_lock(&buf->lock);

    if (!buf->num_queued_packets) {
        pthread_mutex_unlock(&buf->lock);
        pthread_cond_wait(&buf->cond_in, &buf->cond_lock_in);
        pthread_mutex_lock(&buf->lock);
    }

    AVPacket *rf = buf->queued_packets[0];

    pthread_mutex_unlock(&buf->lock);
    pthread_mutex_unlock(&buf->cond_lock_in);

    return rf;
}

static inline void pkt_free_fifo(AVPacketFIFO *buf)
{
    pthread_mutex_lock(&buf->lock);
    if (buf->num_queued_packets)
        for (int i = 0; i < buf->num_queued_packets; i++)
            av_packet_free(&buf->queued_packets[i]);
    av_freep(&buf->queued_packets);
    pthread_mutex_unlock(&buf->lock);
}
