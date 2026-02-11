// mkbigfile.c - write a large file to stress-test MDFS indirect blocks
#include "libc.h"
#include "string.h"

static void usage(void) {
    printf("Usage: mkbigfile <size_mb>\n");
    printf("Creates ./bigfile.bin and writes <size_mb> MiB of data.\n");
    printf("Example: mkbigfile 500\n");
}

static int parse_u32(const char *s, uint32_t *out) {
    if (!s || !*s || !out) return -1;
    uint64_t v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return -2;
        v = v * 10 + (uint64_t)(*p - '0');
        if (v > 0xFFFFFFFFu) return -3;
    }
    *out = (uint32_t)v;
    return 0;
}

int md_main(long argc, char **argv) {
    if (argc != 2) { usage(); return 1; }

    uint32_t mb = 0;
    if (parse_u32(argv[1], &mb) != 0 || mb == 0) {
        printf("mkbigfile: invalid size_mb '%s'\n", argv[1]);
        return 1;
    }

    const char *path = "bigfile.bin";
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("mkbigfile: open('%s') failed (fd=%d)\n", path, fd);
        return 2;
    }

    const size_t chunk = 4 * 1024 * 1024; // 4 MiB (fewer syscalls, faster)
    uint8_t *buf = (uint8_t*)malloc(chunk);
    if (!buf) {
        printf("mkbigfile: malloc failed\n");
        close(fd);
        return 3;
    }

    // Deterministic pattern
    for (size_t i = 0; i < chunk; i++) buf[i] = (uint8_t)(i & 0xFFu);

    uint64_t total = (uint64_t)mb * 1024ULL * 1024ULL;
    uint64_t done = 0;

    printf("mkbigfile: writing %u MiB to %s...\n", (unsigned)mb, path);

    while (done < total) {
        size_t n = chunk;
        if ((total - done) < (uint64_t)chunk) n = (size_t)(total - done);

        ssize_t w = write(fd, buf, n);
        if (w < 0) {
            printf("mkbigfile: write failed at %llu bytes (w=%ld)\n", (unsigned long long)done, (long)w);
            free(buf);
            close(fd);
            return 4;
        }
        if ((size_t)w != n) {
            printf("mkbigfile: short write at %llu bytes (wanted %u got %ld)\n",
                   (unsigned long long)done, (unsigned)n, (long)w);
            free(buf);
            close(fd);
            return 5;
        }

        done += (uint64_t)w;

        // Progress every chunk (4 MiB by default)
        printf("  wrote %llu / %llu MiB\n",
               (unsigned long long)(done / (1024ULL*1024ULL)),
               (unsigned long long)mb);
    }

    free(buf);
    close(fd);

    printf("mkbigfile: done (%llu MiB).\n", (unsigned long long)(done / (1024ULL*1024ULL)));
    return 0;
}
