#include "libc.h"

static void usage(const char *argv0) {
    printf("Usage: %s <src> <dst>\n", argv0);
    printf("Copies a file from <src> to <dst>.\n");
    printf("Example: %s /ModuOS/System64/mdsys.sqr $/vDrive3-P1/NTDev/ModuOS/dist/mdsys.sqr\n", argv0);
}

int md_main(long argc, char **argv) {
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    const char *src = argv[1];
    const char *dst = argv[2];

    int in = open(src, O_RDONLY, 0);
    if (in < 0) {
        printf("cp: cannot open '%s' (rc=%d)\n", src, in);
        return 2;
    }

    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        printf("cp: cannot open '%s' for write (rc=%d)\n", dst, out);
        close(in);
        return 3;
    }

    /* Use a large buffer to reduce expensive vDrive write calls.
     * Try 256KiB, then 64KiB, then 16KiB.
     */
    size_t buf_sz = 256 * 1024;
    char *buf = (char*)malloc(buf_sz);
    if (!buf) {
        buf_sz = 64 * 1024;
        buf = (char*)malloc(buf_sz);
    }
    if (!buf) {
        buf_sz = 16 * 1024;
        buf = (char*)malloc(buf_sz);
    }
    if (!buf) {
        printf("cp: out of memory\n");
        close(in);
        close(out);
        return 6;
    }

    long total = 0;

    for (;;) {
        ssize_t rd = read(in, buf, buf_sz);
        if (rd == 0) break; // EOF
        if (rd < 0) {
            printf("cp: read error on '%s' (rc=%ld)\n", src, (long)rd);
            close(in);
            close(out);
            return 4;
        }

        size_t off = 0;
        while (off < (size_t)rd) {
            ssize_t wr = write(out, buf + off, (size_t)rd - off);
            if (wr < 0) {
                printf("cp: write error on '%s' (rc=%ld)\n", dst, (long)wr);
                close(in);
                close(out);
                return 5;
            }
            off += (size_t)wr;
        }

        total += rd;
    }

    free(buf);
    close(in);
    close(out);

    printf("cp: OK (%ld bytes)\n", total);
    return 0;
}
