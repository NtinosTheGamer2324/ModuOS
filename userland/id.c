#include "libc.h"
#include "string.h"

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;
    int uid = getuid();
    printf("uid=%d\n", uid);
    return 0;
}
