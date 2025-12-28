#include "moduos/kernel/exec.h"

/* Implemented in exec.c */
int exec_run(const char *args, int wait_for_exit);

void exec(const char *args) {
    (void)exec_run(args, 1);
}

int exec_async(const char *args) {
    return exec_run(args, 0);
}
