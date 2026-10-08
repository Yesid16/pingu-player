#define _GNU_SOURCE
#include "audio_out.h"
#include "common.h"
#include "spawn.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long long now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static int try_spawn(AudioOut *o, OutBackend k)
{
    char *aplay[] = { "aplay", "-q", "-t", "raw", "-f", "S16_LE", "-r", "44100",
                      "-c", "2", "--buffer-time=100000", "-", NULL };
    char *paplay[] = { "paplay", "--raw", "--rate=44100", "--format=s16le",
                       "--channels=2", "--latency-msec=100", NULL };
    char *const *argv = (k == OUT_APLAY) ? aplay : paplay;

    pid_t pid;
    int fd;
    if (spawn_piped(argv[0], argv, 1, 0, &pid, &fd) != 0)
        return -1;
    /* tuberia pequena (1 pagina ~ 23 ms): pausa/skip casi inmediatos */
    fcntl(fd, F_SETPIPE_SZ, 4096);
    o->kind = k;
    o->pid = pid;
    o->fd = fd;
    return 0;
}

int out_open(AudioOut *o, OutBackend want)
{
    memset(o, 0, sizeof *o);
    o->fd = -1;

    if (want == OUT_NULL) {
        o->kind = OUT_NULL;
        return 0;
    }
    if (want == OUT_APLAY || want == OUT_AUTO)
        if (try_spawn(o, OUT_APLAY) == 0)
            return 0;
    if (want == OUT_PAPLAY || want == OUT_AUTO)
        if (try_spawn(o, OUT_PAPLAY) == 0)
            return 0;
    if (want == OUT_AUTO) {               /* ultimo recurso: sin sonido */
        o->kind = OUT_NULL;
        return 1;                         /* 1 = se uso el sumidero nulo por falta de reproductor */
    }
    return -1;
}

const char *out_name(const AudioOut *o)
{
    switch (o->kind) {
    case OUT_APLAY:  return "aplay (ALSA)";
    case OUT_PAPLAY: return "paplay (PulseAudio/PipeWire)";
    default:         return "null (sin sonido, solo temporizacion)";
    }
}

int out_write(AudioOut *o, const void *buf, size_t n)
{
    if (o->kind == OUT_NULL) {
        long long dur = (long long)n * 1000000000LL / (SAMPLE_RATE * BYTES_PER_FRAME);
        long long now = now_ns();
        if (!o->started || o->next_ns < now) {
            o->next_ns = now;
            o->started = 1;
        }
        o->next_ns += dur;
        struct timespec t = { .tv_sec = o->next_ns / 1000000000LL,
                              .tv_nsec = o->next_ns % 1000000000LL };
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL) == EINTR)
            ;
        return 0;
    }

    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(o->fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

void out_abort(AudioOut *o)
{
    if (o->pid > 0)
        kill(o->pid, SIGTERM);
}

void out_close(AudioOut *o)
{
    if (o->fd >= 0)
        close(o->fd);
    o->fd = -1;
    if (o->pid > 0) {
        waitpid(o->pid, NULL, 0);
        o->pid = 0;
    }
}

int out_parse(const char *name, OutBackend *b)
{
    if (!strcmp(name, "auto"))        *b = OUT_AUTO;
    else if (!strcmp(name, "aplay"))  *b = OUT_APLAY;
    else if (!strcmp(name, "paplay")) *b = OUT_PAPLAY;
    else if (!strcmp(name, "null"))   *b = OUT_NULL;
    else return -1;
    return 0;
}
