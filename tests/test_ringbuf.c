/*
 * test_ringbuf - productor y consumidor concurrentes + un hilo que hace flush
 * al azar. Verifica que:
 *   - ningun byte se pierde ni se duplica dentro de una misma generacion,
 *   - nunca llegan datos "viejos" de una generacion anterior mezclados,
 *   - no hay interbloqueos (el test termina).
 */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "ringbuf.h"

#define TOTAL (300 * 1000)

static RingBuf rb;
static atomic_int done;
static atomic_long flushes;

static unsigned char expect_byte(unsigned long gen, size_t i)
{
    return (unsigned char)((i * 7 + gen * 13) & 0xFF);
}

static void *producer(void *arg)
{
    (void)arg;
    unsigned seed = 1;
    unsigned char piece[3000];

    while (!atomic_load(&done)) {
        unsigned long gen = rb_gen(&rb);
        size_t idx = 0;
        int aborted = 0;
        while (idx < TOTAL) {
            size_t len = 1 + (size_t)rand_r(&seed) % sizeof piece;
            if (len > TOTAL - idx)
                len = TOTAL - idx;
            for (size_t k = 0; k < len; k++)
                piece[k] = expect_byte(gen, idx + k);
            if (rb_write(&rb, piece, len, gen) != 0) {
                aborted = 1;
                break;
            }
            idx += len;
        }
        if (!aborted) {                         /* espera a que el consumidor termine o haya flush */
            while (!atomic_load(&done) && rb_gen(&rb) == gen)
                usleep(200);
        }
    }
    return NULL;
}

static void *flusher(void *arg)
{
    (void)arg;
    unsigned seed = 7;
    for (int k = 0; k < 60 && !atomic_load(&done); k++) {
        usleep(500 + rand_r(&seed) % 3000);
        rb_flush(&rb);
        atomic_fetch_add(&flushes, 1);
    }
    return NULL;
}

int main(void)
{
    assert(rb_init(&rb, 16 * 1024) == 0);
    atomic_init(&done, 0);

    pthread_t tp, tf;
    pthread_create(&tp, NULL, producer, NULL);
    pthread_create(&tf, NULL, flusher, NULL);

    unsigned long cur_gen = (unsigned long)-1;
    size_t got = 0;
    unsigned char buf[2500];
    unsigned seed = 3;

    for (;;) {
        unsigned long g;
        size_t want = 1 + (size_t)rand_r(&seed) % sizeof buf;
        ssize_t n = rb_read(&rb, buf, want, &g, 0);
        assert(n > 0);
        usleep(20);                            /* consumidor lento: obliga a que haya flushes a mitad de pista */
        if (g != cur_gen) {                    /* nueva generacion: el flujo empieza de cero */
            cur_gen = g;
            got = 0;
        }
        for (ssize_t k = 0; k < n; k++) {
            if (buf[k] != expect_byte(g, got + (size_t)k)) {
                fprintf(stderr, "FALLO: dato corrupto/perdido gen=%lu pos=%zu\n", g,
                        got + (size_t)k);
                return 1;
            }
        }
        got += (size_t)n;
        if (got == TOTAL) {
            atomic_store(&done, 1);
            break;
        }
    }

    pthread_join(tf, NULL);
    pthread_join(tp, NULL);

    /* cierre: un lector bloqueado debe despertar */
    rb_close(&rb);
    unsigned long g;
    assert(rb_read(&rb, buf, sizeof buf, &g, 0) == 0);
    assert(rb_write(&rb, buf, 10, rb_gen(&rb)) == -1);
    rb_destroy(&rb);

    printf("test_ringbuf OK (%ld flushes durante la transferencia)\n", atomic_load(&flushes));
    return 0;
}
