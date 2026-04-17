#include "libc.h"
#include <stddef.h>

int md_main(long argc, char **argv) {
    printf("WOW");
    int fd = open("$/user/xserver/new_window", O_RDWR, 0);

    write(fd, "new", 3);
    char buf[32];
    ssize_t r = read(fd, buf, sizeof(buf));
    if (r < 0) {
        printf("read failed");
        return 1;
    }

    buf[r] = '\0'; // null-terminate
    int window_id = atoi(buf);
    printf("New window ID: %d\n", window_id);

    close(fd);
    return 0;
}