/*
 * main.c - hilo de comandos (UI) de Pingu Player.
 *
 * Este hilo SOLO lee comandos y llama a la API del Player; nunca decodifica ni
 * toca el audio, por eso responde de inmediato aunque suene musica.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "player.h"
#include "ui.h"

static volatile sig_atomic_t g_quit;

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

/* ------------------------------------------------------------ helpers */

static int is_audio_name(const char *name)
{
    static const char *ext[] = { ".wav", ".mp3", ".flac", ".ogg", ".oga",
                                 ".opus", ".m4a", ".aac", NULL };
    const char *dot = strrchr(name, '.');
    if (!dot)
        return 0;
    for (int i = 0; ext[i]; i++)
        if (strcasecmp(dot, ext[i]) == 0)
            return 1;
    return 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int add_dir(Player *p, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return -1;

    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || !is_audio_name(e->d_name))
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            char **t = realloc(names, cap * sizeof *t);
            if (!t)
                break;
            names = t;
        }
        names[n] = strdup(e->d_name);
        if (names[n])
            n++;
    }
    closedir(d);

    qsort(names, n, sizeof *names, cmp_str);
    int added = 0;
    for (size_t i = 0; i < n; i++) {
        char full[PATH_MAX];
        int len = snprintf(full, sizeof full, "%s/%s", dir, names[i]);
        if (len > 0 && (size_t)len < sizeof full && player_add(p, full) == 0)
            added++;
        free(names[i]);
    }
    free(names);
    return added;
}

static int add_path(Player *p, const char *arg)
{
    char real[PATH_MAX];
    if (!realpath(arg, real)) {
        ui_printf("  no existe: %s\n", arg);
        return 0;
    }
    struct stat sb;
    if (stat(real, &sb) != 0)
        return 0;
    if (S_ISDIR(sb.st_mode)) {
        int k = add_dir(p, real);
        if (k < 0)
            ui_printf("  no se pudo abrir la carpeta: %s\n", arg);
        return k < 0 ? 0 : k;
    }
    if (!S_ISREG(sb.st_mode)) {
        ui_printf("  no es un archivo regular: %s\n", arg);
        return 0;
    }
    return player_add(p, real) == 0 ? 1 : 0;
}

static int add_arg(Player *p, const char *arg)
{
    glob_t g;
    int total = 0;
    if (glob(arg, GLOB_TILDE | GLOB_NOCHECK, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++)
            total += add_path(p, g.gl_pathv[i]);
    }
    globfree(&g);
    return total;
}

/* Divide la linea en palabras; admite "comillas dobles" para rutas con espacios. */
static int tokenize(char *line, char **argv, int max)
{
    int n = 0;
    char *s = line;
    while (*s && n < max) {
        while (isspace((unsigned char)*s))
            s++;
        if (!*s)
            break;
        if (*s == '"') {
            argv[n++] = ++s;
            while (*s && *s != '"')
                s++;
        } else {
            argv[n++] = s;
            while (*s && !isspace((unsigned char)*s))
                s++;
        }
        if (*s)
            *s++ = 0;
    }
    return n;
}

/* numero 1-based escrito por el usuario -> indice 0-based. */
static int parse_pos(const char *s, size_t *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || *end || v < 1)
        return -1;
    *out = (size_t)(v - 1);
    return 0;
}

static void usage_cli(const char *prog)
{
    fprintf(stderr,
            "Uso: %s [opciones] [archivos o carpetas...]\n"
            "  --backend auto|aplay|paplay|null   salida de audio (por defecto auto)\n"
            "  --shuffle                          mezcla la lista al iniciar\n"
            "  --repeat off|one|all               modo de repeticion\n"
            "  --play                             empieza a reproducir al iniciar\n"
            "  -h, --help                         esta ayuda\n",
            prog);
}

/* -------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    OutBackend backend = OUT_AUTO;
    int opt_shuffle = 0, opt_play = 0;
    RepeatMode opt_repeat = REPEAT_OFF;
    int first_path = argc;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--backend") && i + 1 < argc) {
            if (out_parse(argv[++i], &backend) != 0) {
                usage_cli(argv[0]);
                return 2;
            }
        } else if (!strcmp(argv[i], "--shuffle")) {
            opt_shuffle = 1;
        } else if (!strcmp(argv[i], "--play")) {
            opt_play = 1;
        } else if (!strcmp(argv[i], "--repeat") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "off"))      opt_repeat = REPEAT_OFF;
            else if (!strcmp(v, "one")) opt_repeat = REPEAT_ONE;
            else if (!strcmp(v, "all")) opt_repeat = REPEAT_ALL;
            else { usage_cli(argv[0]); return 2; }
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage_cli(argv[0]);
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1] == '-') {
            usage_cli(argv[0]);
            return 2;
        } else {
            first_path = i;
            break;
        }
    }

    ui_init();

    /* SIGPIPE: escribir a un aplay que murio debe dar EPIPE, no matarnos.
     * SIGINT/SIGTERM: salida ordenada (sin SA_RESTART para interrumpir fgets). */
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    static Player player;
    Player *p = &player;
    int rc = player_init(p, backend, ui_event, NULL);
    if (rc < 0) {
        fprintf(stderr, "No se pudo iniciar la salida de audio '%s'. "
                        "Instala alsa-utils (sudo apt install alsa-utils) o usa --backend null.\n",
                backend == OUT_PAPLAY ? "paplay" : "aplay");
        return 1;
    }

    ui_banner();
    ui_printf("  salida de audio: %s\n", out_name(&p->out));
    if (rc == 1)
        ui_printf("  AVISO: no se encontro aplay ni paplay; se reproduce SIN sonido.\n"
                  "         Instala alsa-utils:  sudo apt install alsa-utils\n");
    ui_printf("\n");

    player_set_repeat(p, opt_repeat);
    int loaded = 0;
    for (int i = first_path; i < argc; i++)
        loaded += add_arg(p, argv[i]);
    if (loaded > 0)
        ui_printf("  %d pista(s) cargadas.\n", loaded);
    if (opt_shuffle) {
        player_set_shuffle(p, 1);          /* modo mezcla permanente + mezcla total inicial */
        ui_printf("  modo mezcla ACTIVADO.\n");
    }
    if (opt_play && loaded > 0)
        player_play(p, -1);

    char line[PATH_MAX + 64];
    while (!g_quit) {
        ui_prompt();
        if (!fgets(line, sizeof line, stdin)) {
            if (errno == EINTR && !g_quit) {
                clearerr(stdin);
                continue;
            }
            break;                          /* EOF o Ctrl-D */
        }
        line[strcspn(line, "\r\n")] = 0;

        char *av[32];
        int ac = tokenize(line, av, 32);
        if (ac == 0)
            continue;
        const char *cmd = av[0];
        size_t a, b;
        int r;

        if (!strcmp(cmd, "help") || !strcmp(cmd, "h") || !strcmp(cmd, "?")) {
            ui_help();
        } else if (!strcmp(cmd, "add") || !strcmp(cmd, "a")) {
            if (ac < 2) { ui_printf("  uso: add <archivo|carpeta> [...]\n"); continue; }
            int k = 0;
            for (int i = 1; i < ac; i++)
                k += add_arg(p, av[i]);
            ui_printf("  +%d pista(s)\n", k);
        } else if (!strcmp(cmd, "addir")) {
            if (ac < 2) { ui_printf("  uso: addir <carpeta>\n"); continue; }
            ui_printf("  +%d pista(s)\n", add_arg(p, av[1]));
        } else if (!strcmp(cmd, "ls") || !strcmp(cmd, "list") || !strcmp(cmd, "l")) {
            PlayerStatus st;
            player_status(p, &st);
            ui_list(&p->pl, st.state);
        } else if (!strcmp(cmd, "rm") || !strcmp(cmd, "del")) {
            if (ac < 2 || parse_pos(av[1], &a) != 0) { ui_printf("  uso: rm <n>\n"); continue; }
            if (player_remove(p, a) != 0) ui_printf("  no existe la pista %s\n", av[1]);
            else ui_printf("  pista %s eliminada\n", av[1]);
        } else if (!strcmp(cmd, "mv") || !strcmp(cmd, "move")) {
            if (ac < 3 || parse_pos(av[1], &a) != 0 || parse_pos(av[2], &b) != 0) {
                ui_printf("  uso: mv <de> <a>\n");
                continue;
            }
            if (player_move(p, a, b) != 0) ui_printf("  posicion fuera de rango\n");
            else ui_printf("  movida la pista %s a la posicion %s\n", av[1], av[2]);
        } else if (!strcmp(cmd, "shuffle") || !strcmp(cmd, "mix") || !strcmp(cmd, "mezclar")) {
            if (ac >= 2 && !strcmp(av[1], "on")) {
                player_set_shuffle(p, 1);
                ui_printf("  modo mezcla ACTIVADO\n");
            } else if (ac >= 2 && !strcmp(av[1], "off")) {
                player_set_shuffle(p, 0);
                ui_printf("  modo mezcla desactivado (el orden actual se conserva)\n");
                continue;
            } else {
                player_shuffle(p);
                ui_printf("  lista mezclada\n");
            }
            PlayerStatus st;
            player_status(p, &st);
            ui_list(&p->pl, st.state);
        } else if (!strcmp(cmd, "clear")) {
            player_clear(p);
            ui_printf("  lista vacia\n");
        } else if (!strcmp(cmd, "play") || !strcmp(cmd, "p")) {
            long idx = -1;
            if (ac >= 2) {
                if (parse_pos(av[1], &a) != 0) { ui_printf("  uso: play [n]\n"); continue; }
                idx = (long)a;
            }
            r = player_play(p, idx);
            if (r == -2) ui_printf("  la lista esta vacia\n");
            else if (r != 0) ui_printf("  no existe la pista %s\n", av[1]);
        } else if (!strcmp(cmd, "pause")) {
            if (player_pause(p) != 0) ui_printf("  no se esta reproduciendo nada\n");
        } else if (!strcmp(cmd, "resume") || !strcmp(cmd, "r")) {
            if (player_resume(p) != 0) ui_printf("  no estaba en pausa\n");
        } else if (!strcmp(cmd, "stop") || !strcmp(cmd, "s")) {
            player_stop(p);
        } else if (!strcmp(cmd, "next") || !strcmp(cmd, "n")) {
            if (player_next(p) != 0) ui_printf("  la lista esta vacia\n");
        } else if (!strcmp(cmd, "prev") || !strcmp(cmd, "b")) {
            if (player_prev(p) != 0) ui_printf("  la lista esta vacia\n");
        } else if (!strcmp(cmd, "repeat")) {
            if (ac < 2) { ui_printf("  uso: repeat off|one|all\n"); continue; }
            if (!strcmp(av[1], "off"))      player_set_repeat(p, REPEAT_OFF);
            else if (!strcmp(av[1], "one")) player_set_repeat(p, REPEAT_ONE);
            else if (!strcmp(av[1], "all")) player_set_repeat(p, REPEAT_ALL);
            else { ui_printf("  uso: repeat off|one|all\n"); continue; }
            ui_printf("  repeat: %s\n", av[1]);
        } else if (!strcmp(cmd, "status") || !strcmp(cmd, "st")) {
            PlayerStatus st;
            player_status(p, &st);
            ui_status(&st);
        } else if (!strcmp(cmd, "penguin") || !strcmp(cmd, "pingu")) {
            PlayerStatus st;
            player_status(p, &st);
            ui_penguin(st.state);
        } else if (!strcmp(cmd, "quit") || !strcmp(cmd, "exit") || !strcmp(cmd, "q")) {
            break;
        } else {
            ui_printf("  comando desconocido: '%s' (escribe 'help')\n", cmd);
        }
    }

    ui_printf("\n  Hasta luego!\n");
    player_destroy(p);
    return 0;
}
