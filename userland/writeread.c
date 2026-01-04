#include "libc.h"

static void usage(const char *argv0) {
    printf("Usage: %s <path> <text>\n", argv0);
    printf("Writes text then reads back and prints it.\n");
}

int md_main(long argc, char **argv) {
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    const char *path = argv[1];
    const char *text = argv[2];

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("writeread: open(w) failed rc=%d\n", fd);
        return 2;
    }

    size_t len = strlen(text);
    if (write(fd, text, len) != (ssize_t)len) {
        printf("writeread: write failed\n");
        close(fd);
        return 3;
    }
    (void)write(fd, "\n", 1);
    close(fd);

    fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("writeread: open(r) failed rc=%d\n", fd);
        return 4;
    }

    char buf[256];
    ssize_t rd = read(fd, buf, sizeof(buf) - 1);
    if (rd < 0) {
        printf("writeread: read failed\n");
        close(fd);
        return 5;
    }
    buf[rd] = 0;
    close(fd);

    printf("writeread: read back: %s", buf);
    return 0;
}
