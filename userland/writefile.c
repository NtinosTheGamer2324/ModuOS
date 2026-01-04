#include "libc.h"

static void usage(const char *argv0) {
    printf("Usage: %s <path> <text>\n", argv0);
    printf("Example: %s /hello.txt \"Hello from ext2\"\n", argv0);
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
        printf("writefile: open failed rc=%d path='%s'\n", fd, path);
        return 2;
    }

    size_t len = strlen(text);
    ssize_t wr = write(fd, text, len);
    if (wr < 0 || (size_t)wr != len) {
        printf("writefile: write failed rc=%ld (wanted=%u)\n", (long)wr, (unsigned)len);
        close(fd);
        return 3;
    }

    // newline for convenience
    (void)write(fd, "\n", 1);

    close(fd);
    printf("writefile: OK (%u bytes) -> %s\n", (unsigned)len, path);
    return 0;
}
