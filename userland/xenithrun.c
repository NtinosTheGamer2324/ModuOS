#include "libc.h"
#include "string.h"

/*
 * xenithrun.sqr
 *
 * Single-shell launcher for Xenith26 MVP:
 *  - forks xenithd
 *  - forks xenithwm
 *  - forks xenithdemo
 *
 * This avoids needing shell backgrounding.
 */

static int spawn(const char *path) {
    int pid = fork();
    if (pid < 0) {
        puts_raw("xenithrun: fork failed\n");
        return -1;
    }
    if (pid == 0) {
        char *argv[2];
        argv[0] = (char*)path;
        argv[1] = NULL;
        execve(path, argv, NULL);
        puts_raw("xenithrun: execve failed\n");
        exit(1);
    }
    return pid;
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    puts_raw("xenithrun: starting xenithd + xenithwm + xenithdemo...\n");

    int p1 = spawn("/Apps/xenithd.sqr");
    if (p1 < 0) return 1;

    /* Give server time to claim gui0 */
    for (int i = 0; i < 200; i++) yield();

    int p2 = spawn("/Apps/xenithwm.sqr");
    if (p2 < 0) return 1;

    for (int i = 0; i < 200; i++) yield();

    int p3 = spawn("/Apps/xenithdemo.sqr");
    if (p3 < 0) return 1;

    puts_raw("xenithrun: launched.\n");
    return 0;
}
