/*
 * gentone - genera un WAV de prueba (PCM 16 bits, 44100 Hz, estereo):
 * un arpegio suave sobre la nota dada, para poder probar el reproductor
 * sin tener musica en la VM.
 *
 * uso: gentone salida.wav frecuencia_hz segundos
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void w32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void w16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "uso: %s salida.wav frecuencia_hz segundos\n", argv[0]);
        return 2;
    }
    double f0 = atof(argv[2]);
    double secs = atof(argv[3]);
    if (f0 <= 0 || secs <= 0) {
        fprintf(stderr, "frecuencia y segundos deben ser positivos\n");
        return 2;
    }

    const int rate = 44100;
    uint32_t frames = (uint32_t)(secs * rate);
    uint32_t bytes = frames * 4;
    const double ratio[4] = { 1.0, 1.25, 1.5, 2.0 };       /* acorde mayor */
    const double note_len = 0.25;

    FILE *f = fopen(argv[1], "wb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    fwrite("RIFF", 1, 4, f); w32(f, 36 + bytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); w32(f, 16); w16(f, 1); w16(f, 2);
    w32(f, rate); w32(f, rate * 4); w16(f, 4); w16(f, 16);
    fwrite("data", 1, 4, f); w32(f, bytes);

    for (uint32_t i = 0; i < frames; i++) {
        double t = (double)i / rate;
        int k = (int)(t / note_len);
        double tn = t - k * note_len;                       /* tiempo dentro de la nota */
        double fr = f0 * ratio[k % 4];
        double env = exp(-6.0 * tn) * (tn < 0.005 ? tn / 0.005 : 1.0);   /* ataque corto, caida */
        double fade = 1.0;                                  /* fundidos para evitar chasquidos */
        if (t < 0.02)         fade = t / 0.02;
        if (secs - t < 0.05)  fade = (secs - t) / 0.05;
        double v = 0.45 * env * fade * (sin(2 * M_PI * fr * t) + 0.3 * sin(4 * M_PI * fr * t));
        int16_t s = (int16_t)(v * 32767.0 * 0.8);
        fwrite(&s, 2, 1, f);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    return 0;
}
