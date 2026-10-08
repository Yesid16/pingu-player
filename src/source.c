#define _GNU_SOURCE
#include "source.h"
#include "common.h"
#include "spawn.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct Source {
    int is_ffmpeg;
    FILE *fp;
    pid_t pid;
    int channels;
    long remaining;                 /* bytes de datos por leer; -1 = hasta EOF */
    uint8_t scratch[SRC_MAX_CHUNK];
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* 0 = abierto como WAV nativo; 1 = no es un WAV soportado (usar ffmpeg);
 * -1 = error de E/S (errno). */
static int wav_open(Source *s, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;

    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        fclose(f);
        return 1;
    }

    int have_fmt = 0;
    uint16_t tag = 0, ch = 0, bits = 0;
    uint32_t rate = 0;

    for (;;) {
        uint8_t hd[8];
        if (fread(hd, 1, 8, f) != 8) {
            fclose(f);
            return 1;
        }
        uint32_t sz = rd32(hd + 4);

        if (!memcmp(hd, "fmt ", 4)) {
            uint8_t fm[16];
            if (sz < 16 || fread(fm, 1, 16, f) != 16) {
                fclose(f);
                return 1;
            }
            tag = rd16(fm);
            ch = rd16(fm + 2);
            rate = rd32(fm + 4);
            bits = rd16(fm + 14);
            have_fmt = 1;
            long skip = (long)sz - 16 + (long)(sz & 1);
            if (skip > 0)
                fseek(f, skip, SEEK_CUR);
        } else if (!memcmp(hd, "data", 4)) {
            if (!have_fmt || tag != 1 || bits != 16 || rate != SAMPLE_RATE ||
                (ch != 1 && ch != 2)) {
                fclose(f);
                return 1;
            }
            s->fp = f;
            s->channels = ch;
            s->remaining = (sz == 0 || sz == 0xFFFFFFFFu) ? -1 : (long)sz;
            return 0;
        } else {
            if (fseek(f, (long)sz + (long)(sz & 1), SEEK_CUR) != 0) {
                fclose(f);
                return 1;
            }
        }
    }
}

static int ffmpeg_open(Source *s, const char *path, char *err, size_t errlen)
{
    char in[4200];
    snprintf(in, sizeof in, "file:%s", path);   /* evita que '-x' o 'a:b' se interpreten */

    char *argv[] = { "ffmpeg", "-v", "quiet", "-nostdin", "-i", in,
                     "-f", "s16le", "-ar", "44100", "-ac", "2", "-", NULL };
    pid_t pid;
    int fd;
    if (spawn_piped("ffmpeg", argv, 0, 1, &pid, &fd) != 0) {
        snprintf(err, errlen,
                 "formato no soportado de forma nativa y ffmpeg no esta disponible "
                 "(sudo apt install ffmpeg)");
        return -1;
    }
    s->fp = fdopen(fd, "r");
    if (!s->fp) {
        close(fd);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        snprintf(err, errlen, "fdopen: %s", strerror(errno));
        return -1;
    }
    s->pid = pid;
    s->is_ffmpeg = 1;
    s->remaining = -1;
    return 0;
}

Source *src_open(const char *path, char *err, size_t errlen)
{
    Source *s = calloc(1, sizeof *s);
    if (!s) {
        snprintf(err, errlen, "sin memoria");
        return NULL;
    }
    int r = wav_open(s, path);
    if (r == 0)
        return s;
    if (r < 0) {
        snprintf(err, errlen, "%s", strerror(errno));
        free(s);
        return NULL;
    }
    if (ffmpeg_open(s, path, err, errlen) != 0) {
        free(s);
        return NULL;
    }
    return s;
}

ssize_t src_read(Source *s, void *buf, size_t n)
{
    if (n > SRC_MAX_CHUNK)
        n = SRC_MAX_CHUNK;

    if (s->is_ffmpeg) {
        size_t got = fread(buf, 1, n, s->fp);
        if (got == 0)
            return ferror(s->fp) ? -1 : 0;
        return (ssize_t)got;
    }

    /* WAV nativo */
    size_t want = (s->channels == 2) ? n : n / 2;
    want &= ~(size_t)3;
    if (s->channels == 1)
        want = (n / 4) * 2;
    if (s->remaining >= 0 && (long)want > s->remaining)
        want = (size_t)s->remaining;
    if (want == 0)
        return 0;

    if (s->channels == 2) {
        size_t got = fread(buf, 1, want, s->fp);
        if (s->remaining >= 0)
            s->remaining -= (long)got;
        got &= ~(size_t)3;
        if (got == 0)
            return ferror(s->fp) ? -1 : 0;
        return (ssize_t)got;
    }

    /* mono -> estereo duplicando cada muestra */
    size_t got = fread(s->scratch, 1, want, s->fp);
    if (s->remaining >= 0)
        s->remaining -= (long)got;
    got &= ~(size_t)1;
    if (got == 0)
        return ferror(s->fp) ? -1 : 0;
    uint8_t *o = buf;
    for (size_t i = 0; i < got; i += 2) {
        o[2 * i + 0] = o[2 * i + 2] = s->scratch[i];
        o[2 * i + 1] = o[2 * i + 3] = s->scratch[i + 1];
    }
    return (ssize_t)(got * 2);
}

void src_close(Source *s)
{
    if (!s)
        return;
    if (s->is_ffmpeg && s->pid > 0)
        kill(s->pid, SIGTERM);
    if (s->fp)
        fclose(s->fp);
    if (s->is_ffmpeg && s->pid > 0)
        waitpid(s->pid, NULL, 0);
    free(s);
}
