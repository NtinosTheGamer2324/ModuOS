#include "libc.h"

int md_main(long argc, char **argv) {    
    if (argc < 3) {
        printf("%s usage: invoke [Path/To/UFS/Node] [Request Data]\n", argv[0]);
        return 1;
    }

    int fd = open(argv[1], O_RDWR, 0);
    if (fd < 0) {
        printf("Error: Could not open %s\n", argv[1]);
        return 1;
    }

    char response[1024]; 
    
    ssize_t bytes_read = invoke(fd, argv[2], strlen(argv[2]) + 1, response, sizeof(response));

    if (bytes_read > 0) {
        printf("Result (%zd bytes): %s\n", bytes_read, response);
    } else if (bytes_read == 0) {
        printf("Success: Invocation complete (no data returned).\n");
    } else {
        printf("Error: Invocation failed (errno: %d)\n", errno);
    }

    close(fd);
    return 0;
}