#include "ringbuf.h"

#include <stdlib.h>
#include <string.h>

int rb_init(RingBuf *rb, size_t cap)
{
    memset(rb, 0, sizeof *rb);
    rb->buf = malloc(cap);
    if (!rb->buf)
        return -1;
    rb->cap = cap;
    pthread_mutex_init(&rb->mtx, NULL);
    pthread_cond_init(&rb->not_full, NULL);
    pthread_cond_init(&rb->not_empty, NULL);
    return 0;
}

void rb_destroy(RingBuf *rb)
{
    pthread_mutex_destroy(&rb->mtx);
    pthread_cond_destroy(&rb->not_full);
    pthread_cond_destroy(&rb->not_empty);
    free(rb->buf);
    rb->buf = NULL;
}

int rb_write(RingBuf *rb, const void *data, size_t len, unsigned long gen)
{
    const uint8_t *p = data;
    int rc = 0;

    pthread_mutex_lock(&rb->mtx);
    while (len > 0) {
        while (!rb->closed && rb->gen == gen && rb->count == rb->cap)
            pthread_cond_wait(&rb->not_full, &rb->mtx);

        if (rb->closed || rb->gen != gen) {
            rc = -1;
            break;
        }

        size_t space = rb->cap - rb->count;
        size_t n = len < space ? len : space;
        size_t first = rb->cap - rb->tail;
        if (first > n)
            first = n;
        memcpy(rb->buf + rb->tail, p, first);
        memcpy(rb->buf, p + first, n - first);
        rb->tail = (rb->tail + n) % rb->cap;
        rb->count += n;
        rb->primed = 1;
        p += n;
        len -= n;
        pthread_cond_signal(&rb->not_empty);
    }
    pthread_mutex_unlock(&rb->mtx);
    return rc;
}

ssize_t rb_read(RingBuf *rb, void *out, size_t max, unsigned long *gen_out,
                int track_underrun)
{
    pthread_mutex_lock(&rb->mtx);

    if (rb->count == 0 && rb->primed && track_underrun && !rb->closed)
        rb->underruns++;

    while (rb->count == 0 && !rb->closed)
        pthread_cond_wait(&rb->not_empty, &rb->mtx);

    if (rb->count == 0) {                 /* cerrado y vacio */
        pthread_mutex_unlock(&rb->mtx);
        return 0;
    }

    size_t n = max < rb->count ? max : rb->count;
    size_t first = rb->cap - rb->head;
    if (first > n)
        first = n;
    memcpy(out, rb->buf + rb->head, first);
    memcpy((uint8_t *)out + first, rb->buf, n - first);
    rb->head = (rb->head + n) % rb->cap;
    rb->count -= n;
    if (gen_out)
        *gen_out = rb->gen;
    pthread_cond_signal(&rb->not_full);
    pthread_mutex_unlock(&rb->mtx);
    return (ssize_t)n;
}

unsigned long rb_flush(RingBuf *rb)
{
    pthread_mutex_lock(&rb->mtx);
    rb->gen++;
    rb->head = rb->tail = rb->count = 0;
    rb->primed = 0;
    unsigned long g = rb->gen;
    pthread_cond_broadcast(&rb->not_full);   /* productores bloqueados: reintentan y ven gen nueva */
    pthread_mutex_unlock(&rb->mtx);
    return g;
}

unsigned long rb_gen(RingBuf *rb)
{
    pthread_mutex_lock(&rb->mtx);
    unsigned long g = rb->gen;
    pthread_mutex_unlock(&rb->mtx);
    return g;
}

void rb_close(RingBuf *rb)
{
    pthread_mutex_lock(&rb->mtx);
    rb->closed = 1;
    pthread_cond_broadcast(&rb->not_full);
    pthread_cond_broadcast(&rb->not_empty);
    pthread_mutex_unlock(&rb->mtx);
}

size_t rb_level(RingBuf *rb, size_t *cap_out, unsigned long *underruns_out)
{
    pthread_mutex_lock(&rb->mtx);
    size_t c = rb->count;
    if (cap_out)
        *cap_out = rb->cap;
    if (underruns_out)
        *underruns_out = rb->underruns;
    pthread_mutex_unlock(&rb->mtx);
    return c;
}
