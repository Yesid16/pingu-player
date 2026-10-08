/*
 * playlist.h - lista de reproduccion compartida (problema Lectores-Escritores).
 *
 * Un unico pthread_rwlock protege TODA la estructura (arreglo de pistas, indice
 * de la pista actual y modo de repeticion).
 *   - Lectores (varios a la vez): pl_current, pl_peek_next, pl_snapshot, pl_count
 *   - Escritores (exclusivos):    pl_add, pl_remove, pl_move, pl_shuffle,
 *                                  pl_clear, pl_goto, pl_step, pl_advance_from,
 *                                  pl_set_repeat
 * El rwlock se configura con preferencia a escritores para que el flujo
 * continuo de lecturas del decodificador no deje en inanicion al usuario.
 *
 * Ninguna funcion devuelve punteros internos: siempre se entregan COPIAS
 * (strdup) de las rutas, asi nadie usa memoria liberada por un rm/clear.
 * Cada pista tiene un id unico y estable, que permite detectar que "la pista
 * actual" cambio mientras el decodificador trabajaba.
 */
#ifndef PLAYLIST_H
#define PLAYLIST_H

#include <pthread.h>
#include <stddef.h>

typedef enum { REPEAT_OFF = 0, REPEAT_ONE, REPEAT_ALL } RepeatMode;

typedef struct {
    unsigned long id;
    char *path;
} Track;

typedef struct {
    pthread_rwlock_t lock;
    Track *items;
    size_t n, cap;
    long cur;                 /* indice de la pista actual, -1 si la lista esta vacia */
    unsigned long next_id;
    unsigned rng;
    RepeatMode repeat;
    int shuffle_on;
} Playlist;

int    pl_init(Playlist *pl);
void   pl_destroy(Playlist *pl);

/* --- escritores --- */
int    pl_add(Playlist *pl, const char *path, unsigned long *id_out);
int    pl_remove(Playlist *pl, size_t idx, int *was_current);
int    pl_move(Playlist *pl, size_t from, size_t to);
/* Mezcla (Fisher-Yates). keep_current=1: la pista actual pasa al inicio y sigue
 * sonando; keep_current=0: mezcla total y el inicio es aleatorio. */
void   pl_shuffle(Playlist *pl, int keep_current);
/* Modo mezcla permanente: mezcla ahora, las pistas nuevas se insertan en
 * posiciones aleatorias y al terminar la lista se vuelve a mezclar. */
void   pl_set_shuffle(Playlist *pl, int on, int keep_current);
int    pl_get_shuffle(Playlist *pl);
void   pl_clear(Playlist *pl);
int    pl_goto(Playlist *pl, size_t idx);
int    pl_step(Playlist *pl, int dir);         /* +1 siguiente, -1 anterior (circular) */
void   pl_set_repeat(Playlist *pl, RepeatMode m);
/* Fin natural de la pista `id`: 1 = hay otra pista (o se repite), 0 = fin de la
 * lista (cur vuelve a 0), -1 = obsoleto (la pista actual ya no es `id`). */
int    pl_advance_from(Playlist *pl, unsigned long id);

/* --- lectores --- */
size_t pl_count(Playlist *pl);
int    pl_current(Playlist *pl, unsigned long *id, char **path_copy);
int    pl_peek_next(Playlist *pl, char **path_copy);
RepeatMode pl_get_repeat(Playlist *pl);

typedef struct { unsigned long id; char *path; } PlItem;
size_t pl_snapshot(Playlist *pl, PlItem **out, long *cur_out);
void   pl_snapshot_free(PlItem *items, size_t n);

#endif
