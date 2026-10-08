#define _GNU_SOURCE
#include "player.h"
#include "common.h"
#include "source.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ util */

static void emit(Player *p, const char *fmt, ...)
{
    if (!p->on_event)
        return;
    char b[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    p->on_event(p->ctx, b);               /* nunca se llama con Player.mtx tomado */
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static void set_now(Player *p, const char *path)
{
    pthread_mutex_lock(&p->mtx);
    snprintf(p->now, sizeof p->now, "%s", base_name(path));
    pthread_mutex_unlock(&p->mtx);
}

static void poke(Player *p)                /* despierta al decodificador y a la salida */
{
    pthread_mutex_lock(&p->mtx);
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mtx);
}

/* Fin de la lista: pasa a STOPPED solo si seguimos en la misma generacion y
 * estabamos reproduciendo (no pisa una orden del usuario que llego justo ahora). */
static void stop_if_gen(Player *p, unsigned long gen)
{
    pthread_mutex_lock(&p->mtx);
    if (rb_gen(&p->rb) == gen && p->state == PS_PLAYING)
        p->state = PS_STOPPED;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mtx);
}

/* ------------------------------------------------- hilo decodificador */

static void *decoder_main(void *arg)
{
    Player *p = arg;
    uint8_t chunk[CHUNK_BYTES];
    size_t fails = 0;

    for (;;) {
        unsigned long gen;

        /* Espera (sin espera activa) a que haya algo que reproducir.
         * La generacion se toma DENTRO del mutex, mientras el estado es
         * PLAYING/PAUSED: si luego llega un Stop, su flush incrementa gen y
         * cualquier escritura nuestra sera rechazada. */
        pthread_mutex_lock(&p->mtx);
        while (!atomic_load(&p->quit) &&
               (p->state == PS_STOPPED || pl_count(&p->pl) == 0)) {
            atomic_store(&p->decoding, 0);
            pthread_cond_wait(&p->cv, &p->mtx);
        }
        int quit = atomic_load(&p->quit);
        gen = rb_gen(&p->rb);
        pthread_mutex_unlock(&p->mtx);
        if (quit)
            break;

        unsigned long id;
        char *path;
        if (pl_current(&p->pl, &id, &path) != 0)
            continue;                      /* la lista se vacio justo ahora: reevaluar */

        set_now(p, path);

        char err[200];
        Source *s = src_open(path, err, sizeof err);
        if (!s) {
            emit(p, "No se pudo abrir '%s': %s", base_name(path), err);
            free(path);
            fails++;
            int r = pl_advance_from(&p->pl, id);
            if (r == 0 || fails >= pl_count(&p->pl)) {
                fails = 0;
                stop_if_gen(p, gen);
            }
            continue;
        }
        fails = 0;
        atomic_store(&p->decoding, 1);
        emit(p, "Reproduciendo: %s", base_name(path));

        int aborted = 0;
        ssize_t n;
        while ((n = src_read(s, chunk, sizeof chunk)) > 0) {
            if (rb_write(&p->rb, chunk, (size_t)n, gen) != 0) {
                aborted = 1;               /* flush (Next/Prev/Stop/...) o cierre */
                break;
            }
        }
        if (n < 0 && !aborted)
            emit(p, "Error leyendo '%s'", base_name(path));
        src_close(s);
        free(path);

        if (aborted)
            continue;

        /* Fin natural de la pista. */
        if (rb_gen(&p->rb) != gen)
            continue;                      /* el usuario cambio de pista justo al final */
        int r = pl_advance_from(&p->pl, id);
        if (r == 0) {
            atomic_store(&p->decoding, 0);
            stop_if_gen(p, gen);
            emit(p, "Fin de la lista de reproduccion");
        }
    }
    atomic_store(&p->decoding, 0);
    return NULL;
}

/* ------------------------------------------------------ hilo de salida */

/* Bloquea mientras el estado sea PAUSED. 0 = hay que terminar. */
static int wait_unpaused(Player *p)
{
    pthread_mutex_lock(&p->mtx);
    while (p->state == PS_PAUSED && !atomic_load(&p->quit))
        pthread_cond_wait(&p->cv, &p->mtx);
    int ok = !atomic_load(&p->quit);
    pthread_mutex_unlock(&p->mtx);
    return ok;
}

static void *output_main(void *arg)
{
    Player *p = arg;
    uint8_t chunk[CHUNK_BYTES];

    for (;;) {
        if (!wait_unpaused(p))
            break;

        unsigned long g;
        ssize_t n = rb_read(&p->rb, chunk, sizeof chunk, &g, atomic_load(&p->decoding));
        if (n <= 0)
            break;                         /* bufer cerrado */

        if (!wait_unpaused(p))             /* pausa mientras esperabamos datos */
            break;
        if (rb_gen(&p->rb) != g)           /* trozo "viejo": hubo Next/Stop mientras tanto */
            continue;

        if (out_write(&p->out, chunk, (size_t)n) != 0) {
            if (atomic_load(&p->quit))
                break;
            emit(p, "Error escribiendo en la salida de audio (%s). Reproduccion detenida.",
                 out_name(&p->out));
            pthread_mutex_lock(&p->mtx);
            p->state = PS_STOPPED;
            pthread_mutex_unlock(&p->mtx);
            rb_flush(&p->rb);
            /* la salida murio: dejamos de intentar para no inundar de errores */
            p->out.kind = OUT_NULL;
        }
    }
    return NULL;
}

/* ------------------------------------------------------ ciclo de vida */

int player_init(Player *p, OutBackend backend, PlayerEventFn cb, void *ctx)
{
    memset(p, 0, sizeof *p);
    p->on_event = cb;
    p->ctx = ctx;
    atomic_init(&p->quit, 0);
    atomic_init(&p->decoding, 0);
    p->state = PS_STOPPED;
    strcpy(p->now, "-");

    if (pl_init(&p->pl) != 0)
        return -1;
    if (rb_init(&p->rb, RING_BYTES) != 0) {
        pl_destroy(&p->pl);
        return -1;
    }
    pthread_mutex_init(&p->mtx, NULL);
    pthread_cond_init(&p->cv, NULL);

    int orc = out_open(&p->out, backend);
    if (orc < 0) {
        rb_destroy(&p->rb);
        pl_destroy(&p->pl);
        pthread_mutex_destroy(&p->mtx);
        pthread_cond_destroy(&p->cv);
        return -1;
    }

    if (pthread_create(&p->dec_th, NULL, decoder_main, p) != 0 ||
        pthread_create(&p->out_th, NULL, output_main, p) != 0) {
        /* simplificacion: en un fallo parcial se aborta la inicializacion */
        atomic_store(&p->quit, 1);
        rb_close(&p->rb);
        return -1;
    }
    p->threads_started = 1;
    return orc;
}

void player_destroy(Player *p)
{
    pthread_mutex_lock(&p->mtx);
    atomic_store(&p->quit, 1);
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mtx);

    rb_close(&p->rb);                      /* despierta a productor y consumidor */
    out_abort(&p->out);                    /* desbloquea un write() a la tuberia */

    if (p->threads_started) {
        pthread_join(p->dec_th, NULL);
        pthread_join(p->out_th, NULL);
    }
    out_close(&p->out);

    pl_destroy(&p->pl);
    rb_destroy(&p->rb);
    pthread_mutex_destroy(&p->mtx);
    pthread_cond_destroy(&p->cv);
}

/* ------------------------------------------------------------ comandos */

int player_play(Player *p, long idx)
{
    if (pl_count(&p->pl) == 0)
        return -2;                         /* lista vacia */

    if (idx >= 0 && pl_goto(&p->pl, (size_t)idx) != 0)
        return -1;                         /* indice fuera de rango */

    if (idx < 0) {
        pthread_mutex_lock(&p->mtx);
        PState old = p->state;
        if (old == PS_PAUSED) {            /* "play" sin argumento reanuda */
            p->state = PS_PLAYING;
            pthread_cond_broadcast(&p->cv);
        }
        pthread_mutex_unlock(&p->mtx);
        if (old != PS_STOPPED)
            return 0;
    }

    /* Empezar pista nueva: primero se descarta lo viejo, DESPUES se pone PLAYING,
     * asi el decodificador toma la generacion nueva. */
    rb_flush(&p->rb);
    pthread_mutex_lock(&p->mtx);
    p->state = PS_PLAYING;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mtx);
    return 0;
}

int player_pause(Player *p)
{
    int rc = -1;
    pthread_mutex_lock(&p->mtx);
    if (p->state == PS_PLAYING) {
        p->state = PS_PAUSED;
        rc = 0;
    }
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mtx);
    return rc;
}

int player_resume(Player *p)
{
    int rc = -1;
    pthread_mutex_lock(&p->mtx);
    if (p->state == PS_PAUSED) {
        p->state = PS_PLAYING;
        rc = 0;
    }
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mtx);
    return rc;
}

int player_stop(Player *p)
{
    pthread_mutex_lock(&p->mtx);
    p->state = PS_STOPPED;                 /* primero el estado... */
    strcpy(p->now, "-");
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mtx);
    rb_flush(&p->rb);                      /* ...despues el flush */
    return 0;
}

static int step(Player *p, int dir)
{
    if (pl_step(&p->pl, dir) != 0)
        return -2;
    rb_flush(&p->rb);                      /* respuesta inmediata: se descarta el audio en cola */
    poke(p);
    return 0;
}

int player_next(Player *p) { return step(p, +1); }
int player_prev(Player *p) { return step(p, -1); }

int player_add(Player *p, const char *path)
{
    if (pl_add(&p->pl, path, NULL) != 0)
        return -1;
    poke(p);                               /* por si el decodificador esperaba una lista vacia */
    return 0;
}

int player_remove(Player *p, size_t idx)
{
    int was_current = 0;
    if (pl_remove(&p->pl, idx, &was_current) != 0)
        return -1;
    if (was_current)
        rb_flush(&p->rb);                  /* sonaba esa pista: se corta y pasa a la siguiente */
    poke(p);
    return 0;
}

int player_move(Player *p, size_t from, size_t to)
{
    return pl_move(&p->pl, from, to);
}

static int keep_current(Player *p)         /* hay audio en curso que no debe cortarse */
{
    pthread_mutex_lock(&p->mtx);
    int k = (p->state != PS_STOPPED);
    pthread_mutex_unlock(&p->mtx);
    return k;
}

void player_shuffle(Player *p)
{
    /* sonando: la pista en curso sigue sin cortes y el resto se mezcla.
     * detenido: mezcla total, la primera pista tambien es aleatoria. */
    pl_shuffle(&p->pl, keep_current(p));
}

void player_set_shuffle(Player *p, int on)
{
    pl_set_shuffle(&p->pl, on, keep_current(p));
}

void player_clear(Player *p)
{
    pl_clear(&p->pl);
    rb_flush(&p->rb);
    pthread_mutex_lock(&p->mtx);
    strcpy(p->now, "-");
    pthread_mutex_unlock(&p->mtx);
    poke(p);
}

void player_set_repeat(Player *p, RepeatMode m)
{
    pl_set_repeat(&p->pl, m);
}

void player_status(Player *p, PlayerStatus *st)
{
    memset(st, 0, sizeof *st);

    pthread_mutex_lock(&p->mtx);
    st->state = p->state;
    snprintf(st->now, sizeof st->now, "%s", p->now);
    pthread_mutex_unlock(&p->mtx);

    st->decoding = atomic_load(&p->decoding);
    st->buf_fill = rb_level(&p->rb, &st->buf_cap, &st->underruns);

    PlItem *items;
    long cur;
    size_t n = pl_snapshot(&p->pl, &items, &cur);
    st->count = n;
    st->cur = cur;
    st->repeat = pl_get_repeat(&p->pl);
    st->shuffle = pl_get_shuffle(&p->pl);
    pl_snapshot_free(items, n);

    char *nx = NULL;
    if (pl_peek_next(&p->pl, &nx) == 0) {
        snprintf(st->next, sizeof st->next, "%s", base_name(nx));
        free(nx);
    } else {
        strcpy(st->next, "-");
    }
}
