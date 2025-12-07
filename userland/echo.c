#include "libc.h"

int md_main(long argc, char** argv) {
    // Skip argv[0] (program name)
    for (long i = 1; i < argc; i++) {
        printf("%s", argv[i]);
        if (i + 1 < argc)
            printf(" ");
    }
    printf("\n");

    return 0; // return 0 as exit code
}
