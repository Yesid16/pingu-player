#define _GNU_SOURCE
#include "playlist.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RD(pl)  pthread_rwlock_rdlock(&(pl)->lock)
#define WR(pl)  pthread_rwlock_wrlock(&(pl)->lock)
#define UN(pl)  pthread_rwlock_unlock(&(pl)->lock)

int pl_init(Playlist *pl)
{
    memset(pl, 0, sizeof *pl);
    pthread_rwlockattr_t a;
    pthread_rwlockattr_init(&a);
    pthread_rwlockattr_setkind_np(&a, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    int rc = pthread_rwlock_init(&pl->lock, &a);
    pthread_rwlockattr_destroy(&a);
    if (rc != 0)
        return -1;
    pl->cur = -1;
    pl->next_id = 1;
    pl->rng = (unsigned)time(NULL) ^ (unsigned)getpid();
    pl->repeat = REPEAT_OFF;
    return 0;
}

void pl_destroy(Playlist *pl)
{
    for (size_t i = 0; i < pl->n; i++)
        free(pl->items[i].path);
    free(pl->items);
    pthread_rwlock_destroy(&pl->lock);
}

/* ------------------------------ escritores ------------------------------ */

int pl_add(Playlist *pl, const char *path, unsigned long *id_out)
{
    char *copy = strdup(path);
    if (!copy)
        return -1;

    WR(pl);
    if (pl->n == pl->cap) {
        size_t ncap = pl->cap ? pl->cap * 2 : 16;
        Track *t = realloc(pl->items, ncap * sizeof *t);
        if (!t) {
            UN(pl);
            free(copy);
            return -1;
        }
        pl->items = t;
        pl->cap = ncap;
    }
    unsigned long id = pl->next_id++;
    size_t pos = pl->n;                   /* normalmente al final */
    if (pl->shuffle_on && pl->n > 0 && pl->cur >= 0) {
        /* modo mezcla: posicion aleatoria despues de la pista actual */
        size_t first = (size_t)pl->cur + 1;
        pos = first + (size_t)rand_r(&pl->rng) % (pl->n - first + 1);
        memmove(&pl->items[pos + 1], &pl->items[pos], (pl->n - pos) * sizeof(Track));
    }
    pl->items[pos].id = id;
    pl->items[pos].path = copy;
    pl->n++;
    if (pl->cur < 0)
        pl->cur = 0;
    UN(pl);

    if (id_out)
        *id_out = id;
    return 0;
}

int pl_remove(Playlist *pl, size_t idx, int *was_current)
{
    int was = 0;
    WR(pl);
    if (idx >= pl->n) {
        UN(pl);
        return -1;
    }
    free(pl->items[idx].path);
    memmove(&pl->items[idx], &pl->items[idx + 1],
            (pl->n - idx - 1) * sizeof(Track));
    pl->n--;

    if ((long)idx < pl->cur) {
        pl->cur--;
    } else if ((long)idx == pl->cur) {
        was = 1;                          /* la siguiente pista ocupa el mismo indice */
        if (pl->n == 0)
            pl->cur = -1;
        else if ((size_t)pl->cur >= pl->n)
            pl->cur = 0;                  /* se borro la ultima: vuelve al inicio */
    }
    UN(pl);

    if (was_current)
        *was_current = was;
    return 0;
}

int pl_move(Playlist *pl, size_t from, size_t to)
{
    WR(pl);
    if (from >= pl->n || to >= pl->n) {
        UN(pl);
        return -1;
    }
    if (from != to) {
        Track t = pl->items[from];
        if (from < to)
            memmove(&pl->items[from], &pl->items[from + 1], (to - from) * sizeof t);
        else
            memmove(&pl->items[to + 1], &pl->items[to], (from - to) * sizeof t);
        pl->items[to] = t;

        long c = pl->cur;
        if (c == (long)from)
            pl->cur = (long)to;
        else if ((long)from < c && (long)to >= c)
            pl->cur--;
        else if ((long)from > c && (long)to <= c)
            pl->cur++;
    }
    UN(pl);
    return 0;
}

/* Fisher-Yates. keep_id != 0: esa pista queda de primera. Si no, y
 * avoid_first_id != 0, se evita que la primera sea justo esa (para no repetir
 * la ultima pista sonada al volver a mezclar). Requiere el wrlock tomado. */
static void shuffle_locked(Playlist *pl, unsigned long keep_id, unsigned long avoid_first_id)
{
    if (pl->n == 0)
        return;
    if (pl->n >= 2) {
        for (size_t i = pl->n - 1; i > 0; i--) {
            size_t j = (size_t)rand_r(&pl->rng) % (i + 1);
            Track t = pl->items[i];
            pl->items[i] = pl->items[j];
            pl->items[j] = t;
        }
        if (keep_id) {
            for (size_t i = 0; i < pl->n; i++) {
                if (pl->items[i].id == keep_id) {
                    Track t = pl->items[0];
                    pl->items[0] = pl->items[i];
                    pl->items[i] = t;
                    break;
                }
            }
        } else if (avoid_first_id && pl->items[0].id == avoid_first_id) {
            Track t = pl->items[0];
            pl->items[0] = pl->items[1];
            pl->items[1] = t;
        }
    }
    pl->cur = 0;
}

void pl_shuffle(Playlist *pl, int keep_current)
{
    WR(pl);
    unsigned long keep = (keep_current && pl->cur >= 0) ? pl->items[pl->cur].id : 0;
    shuffle_locked(pl, keep, 0);
    UN(pl);
}

void pl_set_shuffle(Playlist *pl, int on, int keep_current)
{
    WR(pl);
    pl->shuffle_on = on;
    if (on) {
        unsigned long keep = (keep_current && pl->cur >= 0) ? pl->items[pl->cur].id : 0;
        shuffle_locked(pl, keep, 0);
    }
    UN(pl);
}

int pl_get_shuffle(Playlist *pl)
{
    RD(pl);
    int v = pl->shuffle_on;
    UN(pl);
    return v;
}

void pl_clear(Playlist *pl)
{
    WR(pl);
    for (size_t i = 0; i < pl->n; i++)
        free(pl->items[i].path);
    pl->n = 0;
    pl->cur = -1;
    UN(pl);
}

int pl_goto(Playlist *pl, size_t idx)
{
    int rc = -1;
    WR(pl);
    if (idx < pl->n) {
        pl->cur = (long)idx;
        rc = 0;
    }
    UN(pl);
    return rc;
}

int pl_step(Playlist *pl, int dir)
{
    int rc = -1;
    WR(pl);
    if (pl->n > 0) {
        long n = (long)pl->n;
        pl->cur = ((pl->cur + dir) % n + n) % n;
        rc = 0;
    }
    UN(pl);
    return rc;
}

void pl_set_repeat(Playlist *pl, RepeatMode m)
{
    WR(pl);
    pl->repeat = m;
    UN(pl);
}

int pl_advance_from(Playlist *pl, unsigned long id)
{
    int rc;
    WR(pl);
    if (pl->cur < 0 || pl->items[pl->cur].id != id) {
        rc = -1;
    } else if (pl->repeat == REPEAT_ONE) {
        rc = 1;
    } else if ((size_t)pl->cur + 1 < pl->n) {
        pl->cur++;
        rc = 1;
    } else if (pl->repeat == REPEAT_ALL) {
        if (pl->shuffle_on)
            shuffle_locked(pl, 0, id);    /* nueva vuelta, nuevo orden */
        else
            pl->cur = 0;
        rc = 1;
    } else {
        if (pl->shuffle_on)
            shuffle_locked(pl, 0, id);    /* la proxima vez empieza en otra pista */
        else
            pl->cur = 0;                  /* fin: listo para empezar de nuevo */
        rc = 0;
    }
    UN(pl);
    return rc;
}

/* ------------------------------- lectores ------------------------------- */

size_t pl_count(Playlist *pl)
{
    RD(pl);
    size_t n = pl->n;
    UN(pl);
    return n;
}

RepeatMode pl_get_repeat(Playlist *pl)
{
    RD(pl);
    RepeatMode m = pl->repeat;
    UN(pl);
    return m;
}

int pl_current(Playlist *pl, unsigned long *id, char **path_copy)
{
    int rc = -1;
    RD(pl);
    if (pl->cur >= 0) {
        char *c = strdup(pl->items[pl->cur].path);
        if (c) {
            *id = pl->items[pl->cur].id;
            *path_copy = c;
            rc = 0;
        }
    }
    UN(pl);
    return rc;
}

int pl_peek_next(Playlist *pl, char **path_copy)
{
    int rc = -1;
    RD(pl);
    if (pl->cur >= 0) {
        long nx = -1;
        if (pl->repeat == REPEAT_ONE)
            nx = pl->cur;
        else if ((size_t)pl->cur + 1 < pl->n)
            nx = pl->cur + 1;
        else if (pl->repeat == REPEAT_ALL)
            nx = 0;
        if (nx >= 0) {
            char *c = strdup(pl->items[nx].path);
            if (c) {
                *path_copy = c;
                rc = 0;
            }
        }
    }
    UN(pl);
    return rc;
}

size_t pl_snapshot(Playlist *pl, PlItem **out, long *cur_out)
{
    RD(pl);
    size_t n = pl->n;
    PlItem *a = n ? calloc(n, sizeof *a) : NULL;
    if (n && !a) {
        UN(pl);
        *out = NULL;
        if (cur_out)
            *cur_out = -1;
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        a[i].id = pl->items[i].id;
        a[i].path = strdup(pl->items[i].path);
    }
    if (cur_out)
        *cur_out = pl->cur;
    UN(pl);
    *out = a;
    return n;
}

void pl_snapshot_free(PlItem *items, size_t n)
{
    for (size_t i = 0; i < n; i++)
        free(items[i].path);
    free(items);
}
