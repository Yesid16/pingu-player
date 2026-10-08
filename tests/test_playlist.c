/*
 * test_playlist - (1) pruebas deterministas de la logica de indices y
 * (2) estres: varios hilos lectores y escritores a la vez. Pensado para
 * correr tambien con ThreadSanitizer / AddressSanitizer (make tsan / asan).
 */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "playlist.h"

static void names(Playlist *pl, char *out, size_t cap)
{
    PlItem *it;
    long cur;
    size_t n = pl_snapshot(pl, &it, &cur);
    out[0] = 0;
    for (size_t i = 0; i < n; i++) {
        strncat(out, it[i].path, cap - strlen(out) - 1);
        if ((long)i == cur)
            strncat(out, "*", cap - strlen(out) - 1);
        strncat(out, " ", cap - strlen(out) - 1);
    }
    pl_snapshot_free(it, n);
}

static void check(Playlist *pl, const char *want)
{
    char got[256];
    names(pl, got, sizeof got);
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FALLO: esperaba [%s] y obtuve [%s]\n", want, got);
        exit(1);
    }
}

static void deterministic(void)
{
    Playlist pl;
    assert(pl_init(&pl) == 0);
    unsigned long id;
    char *path;

    assert(pl_current(&pl, &id, &path) == -1);       /* vacia */
    assert(pl_step(&pl, 1) == -1);

    pl_add(&pl, "A", NULL); pl_add(&pl, "B", NULL); pl_add(&pl, "C", NULL);
    pl_add(&pl, "D", NULL); pl_add(&pl, "E", NULL);
    check(&pl, "A* B C D E ");

    assert(pl_goto(&pl, 2) == 0);                    /* actual = C */
    check(&pl, "A B C* D E ");
    assert(pl_move(&pl, 0, 4) == 0);                 /* A al final: C pasa a indice 1 */
    check(&pl, "B C* D E A ");
    assert(pl_move(&pl, 4, 0) == 0);                 /* A al inicio: C vuelve a indice 2 */
    check(&pl, "A B C* D E ");
    assert(pl_move(&pl, 2, 4) == 0);                 /* mover la actual: el cursor la sigue */
    check(&pl, "A B D E C* ");

    int was;
    assert(pl_remove(&pl, 0, &was) == 0 && !was);    /* antes de la actual: cursor ajusta */
    check(&pl, "B D E C* ");
    assert(pl_remove(&pl, 3, &was) == 0 && was);     /* borrar la actual (ultima): vuelve al inicio */
    check(&pl, "B* D E ");
    assert(pl_remove(&pl, 99, &was) == -1);

    assert(pl_step(&pl, -1) == 0);                   /* circular hacia atras */
    check(&pl, "B D E* ");

    /* fin de pista: avance, obsolescencia y repeticion */
    assert(pl_current(&pl, &id, &path) == 0 && !strcmp(path, "E"));
    free(path);
    assert(pl_advance_from(&pl, id + 1000) == -1);   /* id obsoleto */
    assert(pl_advance_from(&pl, id) == 0);           /* fin de lista, vuelve a 0 */
    check(&pl, "B* D E ");
    pl_set_repeat(&pl, REPEAT_ALL);
    assert(pl_goto(&pl, 2) == 0);
    assert(pl_current(&pl, &id, &path) == 0);
    free(path);
    assert(pl_advance_from(&pl, id) == 1);
    check(&pl, "B* D E ");

    /* shuffle: conserva el contenido y deja la pista actual primera */
    for (int k = 0; k < 20; k++) {
        assert(pl_goto(&pl, 1) == 0);
        assert(pl_current(&pl, &id, &path) == 0);
        pl_shuffle(&pl, 1);
        unsigned long id2;
        char *p2;
        assert(pl_current(&pl, &id2, &p2) == 0 && id2 == id && !strcmp(p2, path));
        PlItem *it;
        long cur;
        assert(pl_snapshot(&pl, &it, &cur) == 3 && cur == 0 && it[0].id == id);
        pl_snapshot_free(it, 3);
        free(p2);
        free(path);
    }

    /* mezcla total: conserva el contenido y el inicio es aleatorio (no siempre la misma pista) */
    pl_clear(&pl);
    for (int i = 0; i < 20; i++) {
        char b[8];
        snprintf(b, sizeof b, "S%d", i);
        pl_add(&pl, b, NULL);
    }
    int first_changed = 0, order_changed = 0;
    for (int k = 0; k < 30; k++) {
        pl_shuffle(&pl, 0);
        PlItem *it;
        long cur;
        assert(pl_snapshot(&pl, &it, &cur) == 20 && cur == 0);
        if (strcmp(it[0].path, "S0") != 0) first_changed = 1;
        if (strcmp(it[1].path, "S1") != 0) order_changed = 1;
        pl_snapshot_free(it, 20);
    }
    assert(first_changed && order_changed);

    /* modo mezcla: las pistas nuevas NO van siempre al final, y no se pierde ninguna */
    pl_set_shuffle(&pl, 1, 1);
    assert(pl_get_shuffle(&pl) == 1);
    int not_last = 0;
    for (int i = 0; i < 40; i++) {
        char b[8];
        snprintf(b, sizeof b, "N%d", i);
        pl_add(&pl, b, NULL);
        PlItem *it;
        long cur;
        size_t n = pl_snapshot(&pl, &it, &cur);
        assert(n == (size_t)21 + (size_t)i);
        if (strcmp(it[n - 1].path, b) != 0) not_last = 1;
        pl_snapshot_free(it, n);
    }
    assert(not_last);
    assert(pl_count(&pl) == 60);
    pl_set_shuffle(&pl, 0, 1);

    pl_clear(&pl);
    assert(pl_count(&pl) == 0 && pl_current(&pl, &id, &path) == -1);
    pl_destroy(&pl);
    printf("test_playlist: pruebas deterministas OK\n");
}

/* ------------------------------------------------------------- estres */

static Playlist g_pl;
static atomic_int g_stop;
static atomic_long g_ops;

static void *writer(void *arg)
{
    unsigned seed = (unsigned)(size_t)arg * 7919u + 1;
    char buf[32];
    int was;
    while (!atomic_load(&g_stop)) {
        switch (rand_r(&seed) % 8) {
        case 0: case 1:
            snprintf(buf, sizeof buf, "t%u", rand_r(&seed) % 1000);
            pl_add(&g_pl, buf, NULL);
            break;
        case 2: pl_remove(&g_pl, rand_r(&seed) % 40, &was); break;
        case 3: pl_move(&g_pl, rand_r(&seed) % 40, rand_r(&seed) % 40); break;
        case 4: pl_shuffle(&g_pl, rand_r(&seed) & 1); break;
        case 5: pl_step(&g_pl, (rand_r(&seed) & 1) ? 1 : -1); break;
        case 6: pl_goto(&g_pl, rand_r(&seed) % 40); break;
        default:
            if (rand_r(&seed) % 40 == 0)
                pl_clear(&g_pl);
            break;
        }
        atomic_fetch_add(&g_ops, 1);
    }
    return NULL;
}

static void *reader(void *arg)
{
    (void)arg;
    while (!atomic_load(&g_stop)) {
        unsigned long id;
        char *path;
        if (pl_current(&g_pl, &id, &path) == 0) {
            assert(path[0] == 't');                  /* la copia es valida aunque otro haga rm/clear */
            pl_advance_from(&g_pl, id);              /* simula el fin de pista del decodificador */
            free(path);
        }
        if (pl_peek_next(&g_pl, &path) == 0)
            free(path);

        PlItem *it;
        long cur;
        size_t n = pl_snapshot(&g_pl, &it, &cur);
        assert(cur >= -1 && cur < (long)n + (n == 0));
        for (size_t i = 0; i < n; i++) {
            assert(it[i].path && it[i].path[0] == 't');
            for (size_t j = i + 1; j < n; j++)
                assert(it[i].id != it[j].id);        /* ids unicos */
        }
        pl_snapshot_free(it, n);
        (void)pl_count(&g_pl);
        atomic_fetch_add(&g_ops, 1);
    }
    return NULL;
}

static void stress(void)
{
    assert(pl_init(&g_pl) == 0);
    for (int i = 0; i < 10; i++) {
        char b[16];
        snprintf(b, sizeof b, "t%d", i);
        pl_add(&g_pl, b, NULL);
    }
    enum { NW = 4, NR = 4 };
    pthread_t th[NW + NR];
    for (long i = 0; i < NW; i++) pthread_create(&th[i], NULL, writer, (void *)i);
    for (long i = 0; i < NR; i++) pthread_create(&th[NW + i], NULL, reader, NULL);

    struct timespec ts = { .tv_sec = 2 };
    nanosleep(&ts, NULL);
    atomic_store(&g_stop, 1);
    for (int i = 0; i < NW + NR; i++)
        pthread_join(th[i], NULL);

    PlItem *it;
    long cur;
    size_t n = pl_snapshot(&g_pl, &it, &cur);
    assert((n == 0 && cur == -1) || (cur >= 0 && cur < (long)n));
    pl_snapshot_free(it, n);
    pl_destroy(&g_pl);
    printf("test_playlist: estres OK (%ld operaciones concurrentes)\n", atomic_load(&g_ops));
}

int main(void)
{
    deterministic();
    stress();
    return 0;
}
