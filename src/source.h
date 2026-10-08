/*
 * source.h - fuente de audio (decodificador).
 * Entrega siempre PCM s16le, 44100 Hz, estereo.
 *   - WAV PCM 16 bits / 44100 Hz (mono o estereo): lectura nativa en C.
 *   - Cualquier otro formato (mp3, flac, ogg, wav distinto...): se delega en
 *     `ffmpeg` (proceso hijo, sin shell) si esta instalado.
 */
#ifndef SOURCE_H
#define SOURCE_H

#include <stddef.h>
#include <sys/types.h>

#define SRC_MAX_CHUNK 8192

typedef struct Source Source;

Source *src_open(const char *path, char *err, size_t errlen);
/* n <= SRC_MAX_CHUNK. Devuelve bytes leidos, 0 = fin de pista, -1 = error. */
ssize_t src_read(Source *s, void *buf, size_t n);
void    src_close(Source *s);

#endif
