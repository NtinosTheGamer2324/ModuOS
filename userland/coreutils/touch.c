#include "libc.h"
#include "string.h"

int md_main(long argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s <filename>\n", argv[0]);
        return 1;
    }

    const char* filename = argv[1];

    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        
    close(fd);
    return 0;
}