typedef struct SNAME {
    int cur_out_refs;
    int out_ref_count;
    TYPE **queued;
    int num_queued;
    int max_queued;
    FNAME block_flags;
    unsigned int queued_alloc_size;
    pthread_mutex_t lock;
    pthread_cond_t cond_in;
    pthread_cond_t cond_out;
    pthread_mutex_t cond_lock_in;
    pthread_mutex_t cond_lock_out;
} SNAME;

static inline void RENAME(fifo_init)(SNAME *buf, int max_queued, FNAME block_flags)
{
    pthread_mutex_init(&buf->lock, NULL);
    pthread_cond_init(&buf->cond_in, NULL);
    pthread_mutex_init(&buf->cond_lock_in, NULL);
    pthread_cond_init(&buf->cond_out, NULL);
    pthread_mutex_init(&buf->cond_lock_out, NULL);

    buf->cur_out_refs = 0;
    buf->out_ref_count = 1;
    buf->num_queued = 0;
    buf->queued_alloc_size = 0;
    buf->block_flags = block_flags;
    buf->max_queued = max_queued;
    buf->queued = NULL;
}

static inline int RENAME(fifo_get_max_size)(SNAME *buf)
{
    return buf->max_queued;
}

static inline int RENAME(fifo_get_size)(SNAME *buf)
{
    pthread_mutex_lock(&buf->lock);
    int ret = buf->num_queued;
    pthread_mutex_unlock(&buf->lock);
    return ret;
}

static inline int RENAME(fifo_is_full)(SNAME *buf)
{
    pthread_mutex_lock(&buf->lock);
    int full = buf->max_queued > 0 && (buf->num_queued > (buf->max_queued + 1));
    pthread_mutex_unlock(&buf->lock);
    return full;
}

static inline int RENAME(fifo_push)(SNAME *buf, TYPE *f)
{
    int err = 0;

    pthread_mutex_lock(&buf->cond_lock_out);
    pthread_mutex_lock(&buf->lock);

    /* Block or error, but only for non-NULL pushes */
    if (f && (buf->max_queued > 0) &&
        (buf->num_queued > (buf->max_queued + 1))) {
        if (!(buf->block_flags & FRENAME(BLOCK_MAX_OUTPUT))) {
            err = AVERROR(ENOBUFS);
            goto unlock;
        }
        pthread_mutex_unlock(&buf->lock);
        pthread_cond_wait(&buf->cond_out, &buf->cond_lock_out);
        pthread_mutex_lock(&buf->lock);
    }

    unsigned int oalloc = buf->queued_alloc_size;
    TYPE **fq = av_fast_realloc(buf->queued, &buf->queued_alloc_size,
                                sizeof(TYPE *)*(buf->num_queued + 1));
    if (!fq) {
        buf->queued_alloc_size = oalloc;
        err = AVERROR(ENOMEM);
        goto unlock;
    }

    buf->queued = fq;
    buf->queued[buf->num_queued++] = f;

    pthread_cond_signal(&buf->cond_in);

unlock:
    pthread_mutex_unlock(&buf->lock);
    pthread_mutex_unlock(&buf->cond_lock_out);

    return err;
}

static inline TYPE *RENAME(fifo_pop)(SNAME *buf)
{
    TYPE *rf = NULL;

    pthread_mutex_lock(&buf->cond_lock_in);
    pthread_mutex_lock(&buf->lock);

    if (!buf->num_queued) {
        if (!(buf->block_flags & FRENAME(BLOCK_NO_INPUT)))
            goto unlock;
        pthread_mutex_unlock(&buf->lock);
        pthread_cond_wait(&buf->cond_in, &buf->cond_lock_in);
        pthread_mutex_lock(&buf->lock);
    }

    rf = buf->queued[0];
    if ((++buf->cur_out_refs) < buf->out_ref_count) {
        rf = CLONE_FN(rf);
        goto unlock;
    }

    buf->cur_out_refs = 0;
    buf->num_queued--;
    assert(buf->num_queued >= 0);

    memmove(&buf->queued[0], &buf->queued[1], buf->num_queued*sizeof(TYPE *));

    if (buf->max_queued > 0)
        pthread_cond_signal(&buf->cond_out);

unlock:
    pthread_mutex_unlock(&buf->lock);
    pthread_mutex_unlock(&buf->cond_lock_in);

    return rf;
}

static inline void RENAME(fifo_set_refs)(SNAME *buf, int num)
{
    pthread_mutex_lock(&buf->lock);

    if (!buf->num_queued)
        assert(!buf->cur_out_refs);

    buf->out_ref_count = FFMAX(num, 1);
    if (buf->cur_out_refs >= buf->out_ref_count && buf->num_queued) {

        FREE_FN(&buf->queued[0]);

        buf->cur_out_refs = 0;
        buf->num_queued--;
        assert(buf->num_queued >= 0);

        memmove(&buf->queued[0], &buf->queued[1], buf->num_queued*sizeof(TYPE *));

        if (buf->max_queued > 0)
            pthread_cond_signal(&buf->cond_out);
    }

    pthread_mutex_unlock(&buf->lock);
}

static inline TYPE *RENAME(fifo_peek)(SNAME *buf)
{
    void *rf = NULL;

    pthread_mutex_lock(&buf->cond_lock_in);
    pthread_mutex_lock(&buf->lock);

    if (!buf->num_queued) {
        if (!(buf->block_flags & FRENAME(BLOCK_NO_INPUT)))
            goto unlock;
        pthread_mutex_unlock(&buf->lock);
        pthread_cond_wait(&buf->cond_in, &buf->cond_lock_in);
        pthread_mutex_lock(&buf->lock);
    }

    rf = buf->queued[0];

unlock:
    pthread_mutex_unlock(&buf->lock);
    pthread_mutex_unlock(&buf->cond_lock_in);

    return rf;
}

static inline void RENAME(fifo_free)(SNAME *buf)
{
    pthread_mutex_lock(&buf->lock);


    for (int i = 0; i < buf->num_queued; i++)
        FREE_FN(&buf->queued[i]);

    av_freep(&buf->queued);

    pthread_mutex_unlock(&buf->lock);

    pthread_cond_destroy(&buf->cond_in);
    pthread_cond_destroy(&buf->cond_out);
    pthread_mutex_destroy(&buf->cond_lock_in);
    pthread_mutex_destroy(&buf->cond_lock_out);
    pthread_mutex_destroy(&buf->lock);
}
