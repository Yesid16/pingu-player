/* spawn.h - lanzar un proceso hijo con una tuberia a su stdin o stdout */
#ifndef SPAWN_H
#define SPAWN_H

#include <sys/types.h>

/*
 * Lanza `file` con `argv` (sin shell: no hay inyeccion por nombres de archivo).
 *  child_reads = 1 : el padre escribe en el stdin del hijo (ej. aplay)
 *  child_reads = 0 : el padre lee el stdout del hijo       (ej. ffmpeg)
 * El hijo queda en su propio grupo de procesos para que Ctrl-C no lo mate.
 * Devuelve 0 si el exec tuvo exito; -1 (con errno) si no se pudo ejecutar.
 */
int spawn_piped(const char *file, char *const argv[], int child_reads,
                int stderr_null, pid_t *pid_out, int *fd_out);

#endif
