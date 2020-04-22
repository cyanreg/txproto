#include <pthread.h>
#include <assert.h>
#include <libavcodec/avcodec.h>

/* Block if a push to a full FIFO is attempted */
#define FIFO_BLOCK_MAX_OUTPUT (1 << 0)

/* Block if a pull from an empty FIFO is attempted */
#define FIFO_BLOCK_NO_INPUT (1 << 1)

typedef struct AVFrameFIFO {
    void **queued_frames;
    int num_queued_frames;
    int max_queued_frames;
    uint32_t block_flags;
    unsigned int queued_frames_alloc;
    pthread_mutex_t lock;
    pthread_cond_t cond_in;
    pthread_cond_t cond_out;
    pthread_mutex_t cond_lock_in;
    pthread_mutex_t cond_lock_out;
} AVFrameFIFO;

static inline int fifo_get_max_queued(AVFrameFIFO *buf)
{
    return buf->max_queued_frames;
}

static inline void init_fifo(AVFrameFIFO *buf, int max_queued_frames,
                             uint32_t block_flags)
{
    pthread_mutex_init(&buf->lock, NULL);
    pthread_cond_init(&buf->cond_in, NULL);
    pthread_mutex_init(&buf->cond_lock_in, NULL);
    pthread_cond_init(&buf->cond_out, NULL);
    pthread_mutex_init(&buf->cond_lock_out, NULL);
    buf->num_queued_frames = 0;
    buf->queued_frames_alloc = 0;
    buf->block_flags = block_flags;
    buf->max_queued_frames = max_queued_frames;
    buf->queued_frames = NULL;
}

static inline int get_fifo_size(AVFrameFIFO *buf)
{
    pthread_mutex_lock(&buf->lock);
    int ret = buf->num_queued_frames;
    pthread_mutex_unlock(&buf->lock);
    return ret;
}

static inline int queue_is_full(AVFrameFIFO *buf)
{
    int full = 0;

    pthread_mutex_lock(&buf->lock);

    if ((buf->max_queued_frames > 0) &&
        (buf->num_queued_frames > (buf->max_queued_frames + 1)))
        full = 1;

    pthread_mutex_unlock(&buf->lock);

    return full;
}

static inline int push_to_fifo(AVFrameFIFO *buf, void *f)
{
    int err = 0;

    pthread_mutex_lock(&buf->cond_lock_out);
    pthread_mutex_lock(&buf->lock);

    if ((buf->max_queued_frames > 0) &&
        (buf->num_queued_frames > (buf->max_queued_frames + 1))) {
        if (!(buf->block_flags & FIFO_BLOCK_MAX_OUTPUT)) {
            err = AVERROR(ENOBUFS);
            goto unlock;
        }
        pthread_mutex_unlock(&buf->lock);
        pthread_cond_wait(&buf->cond_out, &buf->cond_lock_out);
        pthread_mutex_lock(&buf->lock);
    }

    unsigned int oalloc = buf->queued_frames_alloc;
    void **fq = av_fast_realloc(buf->queued_frames, &buf->queued_frames_alloc,
                                sizeof(void *)*(buf->num_queued_frames + 1));
    if (!fq) {
        buf->queued_frames_alloc = oalloc;
        err = AVERROR(ENOMEM);
        goto unlock;
    }

    buf->queued_frames = fq;
    buf->queued_frames[buf->num_queued_frames++] = f;

    pthread_cond_signal(&buf->cond_in);

unlock:
    pthread_mutex_unlock(&buf->lock);
    pthread_mutex_unlock(&buf->cond_lock_out);

    return err;
}

static inline void *pop_from_fifo(AVFrameFIFO *buf)
{
    void *rf = NULL;

    pthread_mutex_lock(&buf->cond_lock_in);
    pthread_mutex_lock(&buf->lock);

    if (!buf->num_queued_frames) {
        if (!(buf->block_flags & FIFO_BLOCK_NO_INPUT))
            goto unlock;
        pthread_mutex_unlock(&buf->lock);
        pthread_cond_wait(&buf->cond_in, &buf->cond_lock_in);
        pthread_mutex_lock(&buf->lock);
    }

    buf->num_queued_frames--;
    assert(buf->num_queued_frames >= 0);

    rf = buf->queued_frames[0];

    memmove(&buf->queued_frames[0], &buf->queued_frames[1],
            buf->num_queued_frames*sizeof(*buf->queued_frames));

    if (buf->max_queued_frames > 0)
        pthread_cond_signal(&buf->cond_out);

unlock:
    pthread_mutex_unlock(&buf->lock);
    pthread_mutex_unlock(&buf->cond_lock_in);

    return rf;
}

static inline void *peek_from_fifo(AVFrameFIFO *buf)
{
    void *rf = NULL;

    pthread_mutex_lock(&buf->cond_lock_in);
    pthread_mutex_lock(&buf->lock);

    if (!buf->num_queued_frames) {
        if (!(buf->block_flags & FIFO_BLOCK_NO_INPUT))
            goto unlock;
        pthread_mutex_unlock(&buf->lock);
        pthread_cond_wait(&buf->cond_in, &buf->cond_lock_in);
        pthread_mutex_lock(&buf->lock);
    }

    rf = buf->queued_frames[0];

unlock:
    pthread_mutex_unlock(&buf->lock);
    pthread_mutex_unlock(&buf->cond_lock_in);

    return rf;
}

static inline void free_fifo(AVFrameFIFO *buf, int is_packets)
{
    pthread_mutex_lock(&buf->lock);
    if (buf->num_queued_frames) {
        for (int i = 0; i < buf->num_queued_frames; i++) {
            if (is_packets)
                av_packet_free((AVPacket **)&buf->queued_frames[i]);
            else
                av_frame_free((AVFrame **)&buf->queued_frames[i]);
        }
    }
    av_freep(&buf->queued_frames);
    pthread_mutex_unlock(&buf->lock);
}
