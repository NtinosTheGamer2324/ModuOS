#include "../libc/libc.h"

static int parse_pid(const char *text, int *out_pid) {
    if (!text || !*text || !out_pid) return -1;

    unsigned int pid = 0;
    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        unsigned int digit = (unsigned int)(*p - '0');
        if (pid > ((unsigned int)INT_MAX - digit) / 10) return -1;
        pid = pid * 10 + digit;
    }

    if (pid == 0) return -1;
    *out_pid = (int)pid;
    return 0;
}

int md_main(long argc, char **argv) {
    const char *name = (argc > 0 && argv && argv[0]) ? argv[0] : "kill";
    if (argc != 2 || !argv || !argv[1]) {
        printf("Usage: %s <PID>\n", name);
        return 2;
    }

    int pid;
    if (parse_pid(argv[1], &pid) != 0) {
        printf("%s: invalid PID: %s\n", name, argv[1]);
        return 2;
    }

    if (pid == 1) {
        printf("%s: refusing to kill PID 1\n", name);
        return 1;
    }

    if (kill(pid, SIGTERM) != 0) {
        printf("%s: cannot signal PID %d (errno=%d)\n", name, pid, errno);
        return 1;
    }

    return 0;
}