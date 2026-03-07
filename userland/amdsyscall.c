#include "libc.h"

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;
    __asm__ volatile("syscall");
    return 0;
}