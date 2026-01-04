#include "libc.h"

int md_main(long argc, char **argv) {
    int uid = getuid();

    printf("You are %d\n", uid);

}