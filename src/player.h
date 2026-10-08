/*
 * player.h - motor de reproduccion concurrente.
 *
 *   Hilo UI (main) --comandos--> [Player] <--- hilo DECODIFICADOR (productor)
 *                                    |                 | rb_write
 *                                    |            [RingBuf]
 *                                    |                 | rb_read
 *                                    +------------ hilo SALIDA (consumidor) --> aplay/paplay
 *
 * Jerarquia de bloqueos (siempre en este orden, nunca al reves):
 *      Player.mtx  ->  Playlist.lock (rwlock)
 *      RingBuf.mtx es una "hoja": nadie toma otro bloqueo estando dentro de el.
 * Los comandos de la UI usan la playlist y el bufer SIN sostener Player.mtx,
 * salvo para cambiar el estado.
 */
#ifndef PLAYER_H
#define PLAYER_H

#include <pthread.h>
#include <stdatomic.h>

#include "audio_out.h"
#include "playlist.h"
#include "ringbuf.h"

typedef enum { PS_STOPPED = 0, PS_PLAYING, PS_PAUSED } PState;

typedef void (*PlayerEventFn)(void *ctx, const char *msg);

typedef struct {
    PState state;
    char now[256];
    size_t count;
    long cur;
    RepeatMode repeat;
    int shuffle;
    size_t buf_fill, buf_cap;
    unsigned long underruns;
    int decoding;
    char next[256];
} PlayerStatus;

typedef struct Player {
    Playlist pl;
    RingBuf rb;
    AudioOut out;

    pthread_t dec_th, out_th;
    int threads_started;

    pthread_mutex_t mtx;          /* protege state, quit, now */
    pthread_cond_t cv;            /* cambios de estado / lista / quit */
    PState state;
    atomic_int quit;
    char now[256];

    atomic_int decoding;
    PlayerEventFn on_event;
    void *ctx;
} Player;

/* 0 = ok, 1 = ok pero sin reproductor de audio (se usa el sumidero nulo), -1 = error */
int  player_init(Player *p, OutBackend backend, PlayerEventFn cb, void *ctx);
void player_destroy(Player *p);

/* idx = -1: reanudar/empezar donde se quedo; idx >= 0: saltar a esa pista (0-based) */
int  player_play(Player *p, long idx);
int  player_pause(Player *p);
int  player_resume(Player *p);
int  player_stop(Player *p);
int  player_next(Player *p);
int  player_prev(Player *p);

int  player_add(Player *p, const char *path);
int  player_remove(Player *p, size_t idx);
int  player_move(Player *p, size_t from, size_t to);
void player_shuffle(Player *p);                 /* mezcla una vez */
void player_set_shuffle(Player *p, int on);     /* modo mezcla permanente */
void player_clear(Player *p);
void player_set_repeat(Player *p, RepeatMode m);

void player_status(Player *p, PlayerStatus *st);

#endif
