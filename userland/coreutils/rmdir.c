// rmdir.c - remove empty directories
#include "libc.h"
#include "string.h"

static void usage(void) {
    printf("Usage: rmdir <path>\n");
}

int md_main(long argc, char **argv) {
    if (argc < 2 || !argv[1]) {
        usage();
        return 1;
    }

    const char *path = argv[1];
    int rc = rmdir(path);
    if (rc != 0) {
        printf("rmdir: failed rc=%d path='%s'\n", rc, path);
        return 1;
    }

    return 0;
}
