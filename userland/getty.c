/*
 * getty.c - attach a login session to a virtual console.
 *
 *   getty <N|/path>      e.g.  getty 2     (runs on $/dev/tty2)
 *
 * Like Linux getty: become a session leader, open the tty, make it the
 * controlling terminal, point fd 0/1/2 at it and exec /Apps/login.sqr.
 * Respawns login when it exits.
 *
 * Autostart (/ModuOS/System64/auto/autostart), one line per extra console:
 *     start /Apps/getty.sqr 2
 */
#include "libc/libc.h"
#include "libc/tty.h"

#define LOGIN_PATH "/Apps/login.sqr"

static void build_path(char *out, size_t sz, const char *arg) {
    if (arg[0] >= '1' && arg[0] <= '9' && arg[1] == 0) {
        const char *pre = "$/dev/tty";
        size_t i = 0;
        while (pre[i] && i + 2 < sz) { out[i] = pre[i]; i++; }
        out[i++] = arg[0]; out[i] = 0;
    } else {
        size_t i = 0;
        while (arg[i] && i + 1 < sz) { out[i] = arg[i]; i++; }
        out[i] = 0;
    }
}

int md_main(long argc, char **argv) {
    if (argc < 2) { printf("usage: getty <vt-number|device>\n"); return 1; }
    char path[64];
    build_path(path, sizeof path, argv[1]);

    for (;;) {
        pid_t pid = fork();
        if (pid < 0) { printf("getty: fork failed\n"); sleep(1); continue; }
        if (pid == 0) {
            /* child: new session, then claim the tty */
            if (setsid() < 0) { _exit(2); }
            int fd = open(path, O_RDWR, 0);
            if (fd < 0) _exit(3);
            if (tty_set_ctty(fd) < 0) _exit(4);
            dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
            if (fd > 2) close(fd);
            char *av[] = { (char *)LOGIN_PATH, NULL };
            char *ev[] = { NULL };
            execve(LOGIN_PATH, av, ev);
            _exit(127);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        sleep(1);               /* avoid a hot respawn loop */
    }
    return 0;
}