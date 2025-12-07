#include "libc.h"

int md_main(long argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: %s <filename>\n", argv[0]);
        return 1;
    }

    const char* filename = argv[1];

    int fd = open(filename, O_RDONLY, 0);
        
    if (fd < 0) {
        printf("cat: cannot open '%s': No such file or directory\n", filename);
        return 1;
    }

    char buffer[513];
    ssize_t bytes_read;

    while ((bytes_read = read(fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';
        puts_raw(buffer);
    }

    if (bytes_read < 0) {
        printf("\ncat: error reading file\n");
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}