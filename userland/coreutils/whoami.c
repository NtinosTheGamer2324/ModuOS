#include "libc.h"

int md_main(long argc, char **argv)
{
    (void)argc; (void)argv;
    int uid = getuid();

    printf("You are: uid=%d\n", uid);
    return 0;
}