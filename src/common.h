/* common.h - constantes compartidas del reproductor */
#ifndef COMMON_H
#define COMMON_H

#define SAMPLE_RATE   44100
#define CHANNELS      2
#define BYTES_PER_FRAME 4            /* 16 bits * 2 canales */

#define CHUNK_BYTES   4096           /* ~23 ms de audio por trozo */
#define RING_BYTES    (1 << 17)      /* 128 KiB ~ 0.74 s de audio */

#endif
