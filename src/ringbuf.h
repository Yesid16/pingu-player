/*
 * ringbuf.h - bufer circular de bytes, productor/consumidor.
 *
 * Sincronizacion: UN mutex + DOS variables de condicion (not_full, not_empty).
 * No hay espera activa: el productor duerme cuando el bufer esta lleno y el
 * consumidor duerme cuando esta vacio.
 *
 * "Generacion" (gen): contador que se incrementa en cada rb_flush(). Sirve para
 * que un productor que quedo con datos viejos en la mano (por ejemplo, de la
 * pista anterior tras un Next) no los inserte despues del flush: rb_write()
 * rechaza escrituras cuya generacion ya no es la vigente.
 */
#ifndef RINGBUF_H
#define RINGBUF_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct {
    uint8_t *buf;
    size_t cap, head, tail, count;
    unsigned long gen;
    int closed;
    int primed;                 /* hubo datos desde el ultimo flush */
    unsigned long underruns;
    pthread_mutex_t mtx;
    pthread_cond_t not_full, not_empty;
} RingBuf;

int  rb_init(RingBuf *rb, size_t cap);
void rb_destroy(RingBuf *rb);

/* Escribe TODOS los bytes (bloquea si esta lleno).
 * 0 = ok; -1 = la generacion cambio (flush) o el bufer fue cerrado. */
int rb_write(RingBuf *rb, const void *data, size_t len, unsigned long gen);

/* Lee entre 1 y max bytes (bloquea si esta vacio). Devuelve 0 si el bufer fue
 * cerrado y esta vacio. *gen_out = generacion a la que pertenecen los datos.
 * track_underrun != 0: si al entrar estaba vacio (y habia datos antes), cuenta
 * un underrun. */
ssize_t rb_read(RingBuf *rb, void *out, size_t max, unsigned long *gen_out,
                int track_underrun);

unsigned long rb_flush(RingBuf *rb);     /* descarta todo; devuelve la nueva gen */
unsigned long rb_gen(RingBuf *rb);
void rb_close(RingBuf *rb);              /* despierta a todos; fin de la sesion */
size_t rb_level(RingBuf *rb, size_t *cap_out, unsigned long *underruns_out);

#endif
