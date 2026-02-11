#include "libc.h"
#include "string.h"

static void write_zterm(const char *s, int nul_term) {
    if (!s) return;
    if (!nul_term) {
        printf("%s\n", s);
        return;
    }
    // write raw bytes plus NUL
    size_t n = strlen(s);
    (void)write(1, s, n);
    char z = 0;
    (void)write(1, &z, 1);
}

int md_main(long argc, char **argv) {
    int nul = 0;
    long argi = 1;
    if (argc > 1 && strcmp(argv[1], "-0") == 0) {
        nul = 1;
        argi = 2;
    }

    if (argc > argi) {
        int rc = 0;
        for (long i = argi; i < argc; i++) {
            const char *v = getenv(argv[i]);
            if (!v) {
                rc = 1;
                continue;
            }
            write_zterm(v, nul);
        }
        return rc;
    }

    // No keys: print entire env.
    size_t off = 0;
    char buf[512];
    for (;;) {
        int n = envlist2(&off, buf, sizeof(buf));
        if (n < 0) {
            printf("printenv: envlist2 failed (errno=%d)\n", errno);
            return 1;
        }
        if (n == 0) break;

        if (!nul) {
            printf("%s", buf);
        } else {
            // Convert newline-separated stream to NUL-separated.
            for (int i = 0; buf[i]; i++) {
                if (buf[i] == '\n') buf[i] = 0;
            }
            (void)write(1, buf, (size_t)n);
        }
    }
    return 0;
}
