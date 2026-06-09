// mv.c — move (rename) a file
// Cross-filesystem moves are handled as copy + unlink since the kernel
// has no dedicated rename/mv syscall.

#include "libc.h"

int md_main(long argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: %s <src> <dst>\n", argv[0]);
        return 1;
    }

    const char *src = argv[1];
    const char *dst = argv[2];

    int in = open(src, O_RDONLY, 0);
    if (in < 0) {
        printf("mv: cannot open '%s' (rc=%d)\n", src, in);
        return 2;
    }

    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        printf("mv: cannot open '%s' for write (rc=%d)\n", dst, out);
        close(in);
        return 3;
    }

    size_t buf_sz = 256 * 1024;
    char *buf = (char *)malloc(buf_sz);
    if (!buf) { buf_sz = 64 * 1024; buf = (char *)malloc(buf_sz); }
    if (!buf) { buf_sz = 16 * 1024; buf = (char *)malloc(buf_sz); }
    if (!buf) {
        puts("mv: out of memory");
        close(in); close(out);
        return 6;
    }

    for (;;) {
        ssize_t rd = read(in, buf, buf_sz);
        if (rd == 0) break;
        if (rd < 0) {
            printf("mv: read error on '%s' (rc=%ld)\n", src, (long)rd);
            free(buf); close(in); close(out);
            return 4;
        }
        size_t off = 0;
        while (off < (size_t)rd) {
            ssize_t wr = write(out, buf + off, (size_t)rd - off);
            if (wr < 0) {
                printf("mv: write error on '%s' (rc=%ld)\n", dst, (long)wr);
                free(buf); close(in); close(out);
                return 5;
            }
            off += (size_t)wr;
        }
    }

    free(buf);
    close(in);
    close(out);

    int rc = unlink(src);
    if (rc < 0) {
        printf("mv: copied but failed to remove '%s' (rc=%d)\n", src, rc);
        return 7;
    }

    return 0;
}