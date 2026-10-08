#define _GNU_SOURCE
#include "spawn.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int spawn_piped(const char *file, char *const argv[], int child_reads,
                int stderr_null, pid_t *pid_out, int *fd_out)
{
    int pfd[2], efd[2];

    if (pipe2(pfd, O_CLOEXEC) < 0)
        return -1;
    if (pipe2(efd, O_CLOEXEC) < 0) {
        close(pfd[0]); close(pfd[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        int e = errno;
        close(pfd[0]); close(pfd[1]); close(efd[0]); close(efd[1]);
        errno = e;
        return -1;
    }

    if (pid == 0) {                       /* ---- hijo ---- */
        setpgid(0, 0);
        signal(SIGPIPE, SIG_DFL);
        int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (child_reads) {
            dup2(pfd[0], 0);
            dup2(devnull, 1);
        } else {
            dup2(devnull, 0);
            dup2(pfd[1], 1);
        }
        if (stderr_null)
            dup2(devnull, 2);
        execvp(file, argv);
        int e = errno;                    /* exec fallo: avisar al padre */
        ssize_t w = write(efd[1], &e, sizeof e);
        (void)w;
        _exit(127);
    }

    /* ---- padre ---- */
    close(efd[1]);
    close(child_reads ? pfd[0] : pfd[1]);

    int e = 0;
    ssize_t r;
    do { r = read(efd[0], &e, sizeof e); } while (r < 0 && errno == EINTR);
    close(efd[0]);

    int mine = child_reads ? pfd[1] : pfd[0];
    if (r > 0) {                          /* el exec fallo */
        close(mine);
        waitpid(pid, NULL, 0);
        errno = e;
        return -1;
    }
    *pid_out = pid;
    *fd_out = mine;
    return 0;
}
