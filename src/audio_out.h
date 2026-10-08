/*
 * audio_out.h - salida de audio.
 * Envia PCM s16le/44100/estereo a un reproductor externo (aplay = ALSA,
 * paplay = PulseAudio/PipeWire) por una tuberia, o a un "sumidero nulo"
 * que solo respeta el ritmo de tiempo real (util para pruebas sin tarjeta
 * de sonido).
 */
#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

#include <stddef.h>
#include <sys/types.h>

typedef enum { OUT_AUTO = 0, OUT_APLAY, OUT_PAPLAY, OUT_NULL } OutBackend;

typedef struct {
    OutBackend kind;          /* backend realmente en uso */
    pid_t pid;
    int fd;
    int started;
    long long next_ns;        /* instante (CLOCK_MONOTONIC) en que debe terminar el trozo actual */
} AudioOut;

int         out_open(AudioOut *o, OutBackend want);
const char *out_name(const AudioOut *o);
int         out_write(AudioOut *o, const void *buf, size_t n);   /* 0 ok, -1 error */
void        out_abort(AudioOut *o);   /* mata al reproductor hijo (desbloquea un write) */
void        out_close(AudioOut *o);

int         out_parse(const char *name, OutBackend *b);

#endif
