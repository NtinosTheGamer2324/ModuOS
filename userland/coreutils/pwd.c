#include "libc.h"

int md_main(long argc, char** argv) {
    char buf[512];
    size_t buf_size = sizeof(buf);

    if (getcwd(buf, buf_size) < 0) {
        printf("%s: error getting current directory\n", argv[0]);
        return 1;
    }

    printf("%s\n", buf);
    return 0;
}