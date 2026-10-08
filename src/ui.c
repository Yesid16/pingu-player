#define _GNU_SOURCE
#include "ui.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Un unico mutex serializa TODA la salida por pantalla: los hilos del motor
 * (eventos) y el hilo de comandos nunca mezclan sus lineas. */
static pthread_mutex_t ui_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_tty;

#define PROMPT "pingu> "

#define C_RESET  (g_tty ? "\033[0m"  : "")
#define C_CYAN   (g_tty ? "\033[36m" : "")
#define C_YELLOW (g_tty ? "\033[33m" : "")
#define C_GREEN  (g_tty ? "\033[32m" : "")
#define C_DIM    (g_tty ? "\033[2m"  : "")
#define C_BOLD   (g_tty ? "\033[1m"  : "")

void ui_init(void)
{
    g_tty = isatty(STDOUT_FILENO);
}

void ui_printf(const char *fmt, ...)
{
    va_list ap;
    pthread_mutex_lock(&ui_mtx);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    pthread_mutex_unlock(&ui_mtx);
}

void ui_prompt(void)
{
    pthread_mutex_lock(&ui_mtx);
    printf("%s%s%s", C_BOLD, PROMPT, C_RESET);
    fflush(stdout);
    pthread_mutex_unlock(&ui_mtx);
}

void ui_event(void *ctx, const char *msg)
{
    (void)ctx;
    pthread_mutex_lock(&ui_mtx);
    if (g_tty)
        printf("\r\033[K");
    printf("%s[pingu]%s %s\n", C_CYAN, C_RESET, msg);
    if (g_tty)
        printf("%s%s%s", C_BOLD, PROMPT, C_RESET);
    fflush(stdout);
    pthread_mutex_unlock(&ui_mtx);
}

/* ------------------------------------------------------------ pinguino */

static void draw_penguin(const char *eyes, const char *d1, const char *d2)
{
    printf("%s", C_YELLOW);
    printf("      .--.    %s\n", d1);
    printf("     |%s |   %s\n", eyes, d2);
    printf("     |:_/ |\n");
    printf("    //   \\ \\\n");
    printf("   (|     | )\n");
    printf("  /'\\_   _/`\\\n");
    printf("  \\___)=(___/\n");
    printf("%s", C_RESET);
}

void ui_penguin(PState st)
{
    static unsigned frame;
    pthread_mutex_lock(&ui_mtx);
    switch (st) {
    case PS_PLAYING:
        if ((frame++ & 1) == 0)
            draw_penguin("^_^", "\xE2\x99\xAA", "  \xE2\x99\xAB");
        else
            draw_penguin("^o^", "  \xE2\x99\xAB", "\xE2\x99\xAA");
        break;
    case PS_PAUSED:
        draw_penguin("-_-", "z", " Z");
        break;
    default:
        draw_penguin("o_o", "", "");
        break;
    }
    fflush(stdout);
    pthread_mutex_unlock(&ui_mtx);
}

void ui_banner(void)
{
    pthread_mutex_lock(&ui_mtx);
    printf("%s", C_YELLOW);
    printf("      .--.\n");
    printf("     |o_o |    %s%sPINGU PLAYER%s%s\n", C_RESET, C_BOLD, C_RESET, C_YELLOW);
    printf("     |:_/ |    %sreproductor concurrente con playlist y mezclador%s%s\n",
           C_RESET, C_DIM, C_YELLOW);
    printf("    //   \\ \\   %sParcial 2 - Sistemas Operativos - EAFIT%s%s\n",
           C_RESET, C_DIM, C_YELLOW);
    printf("   (|     | )\n");
    printf("  /'\\_   _/`\\   %sEscribe 'help' para ver los comandos%s\n", C_RESET, C_YELLOW);
    printf("  \\___)=(___/\n");
    printf("%s\n", C_RESET);
    fflush(stdout);
    pthread_mutex_unlock(&ui_mtx);
}

void ui_help(void)
{
    ui_printf(
        "%sReproduccion%s\n"
        "  play [n]        reproduce (desde la pista n, o reanuda)\n"
        "  pause | resume  pausa / reanuda\n"
        "  stop            detiene\n"
        "  next | prev     pista siguiente / anterior\n"
        "%sLista%s\n"
        "  add <ruta..>    agrega archivos/carpetas (admite comodines y \"comillas\")\n"
        "  addir <carpeta> agrega todo el audio de una carpeta\n"
        "  ls              muestra la lista\n"
        "  rm <n>          elimina la pista n\n"
        "  mv <de> <a>     mueve la pista de la posicion <de> a <a>\n"
        "  shuffle         MEZCLA la lista una vez (si suena algo, esa pista sigue primera)\n"
        "  shuffle on|off  modo mezcla permanente (pistas nuevas y nuevas vueltas se mezclan)\n"
        "  clear           vacia la lista\n"
        "  repeat off|one|all   modo de repeticion\n"
        "%sOtros%s\n"
        "  status          estado del motor (bufer, underruns, ...)\n"
        "  penguin         saluda el pinguino\n"
        "  quit            salir\n",
        C_BOLD, C_RESET, C_BOLD, C_RESET, C_BOLD, C_RESET);
}

static const char *base(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static const char *state_name(PState s)
{
    return s == PS_PLAYING ? "reproduciendo" : s == PS_PAUSED ? "en pausa" : "detenido";
}

static const char *repeat_name(RepeatMode m)
{
    return m == REPEAT_ONE ? "una" : m == REPEAT_ALL ? "toda la lista" : "off";
}

void ui_status(const PlayerStatus *st)
{
    int pct = st->buf_cap ? (int)(st->buf_fill * 100 / st->buf_cap) : 0;
    char bar[21];
    for (int i = 0; i < 20; i++)
        bar[i] = (i < pct / 5) ? '#' : '.';
    bar[20] = 0;

    ui_penguin(st->state);
    ui_printf("  estado:      %s%s%s\n"
              "  pista:       %s\n"
              "  siguiente:   %s\n"
              "  lista:       %zu pistas, repeat: %s, mezcla: %s\n"
              "  bufer:       [%s] %3d%%  (underruns: %lu)\n"
              "  decodificando: %s\n",
              C_GREEN, state_name(st->state), C_RESET, st->now, st->next, st->count,
              repeat_name(st->repeat), st->shuffle ? "ON" : "off", bar, pct, st->underruns,
              st->decoding ? "si" : "no");
}

void ui_list(Playlist *pl, PState st)
{
    PlItem *items;
    long cur;
    size_t n = pl_snapshot(pl, &items, &cur);

    pthread_mutex_lock(&ui_mtx);
    if (n == 0) {
        printf("  (la lista esta vacia - usa 'add <archivo>')\n");
    } else {
        printf("  Lista (%zu pistas)\n", n);
        for (size_t i = 0; i < n; i++) {
            int is_cur = ((long)i == cur);
            const char *mark = !is_cur ? " " : st == PS_PLAYING ? ">" : st == PS_PAUSED ? "|" : "*";
            printf("  %s%s %3zu. %s%s\n", is_cur ? C_GREEN : "", mark, i + 1,
                   base(items[i].path), C_RESET);
        }
    }
    fflush(stdout);
    pthread_mutex_unlock(&ui_mtx);
    pl_snapshot_free(items, n);
}
