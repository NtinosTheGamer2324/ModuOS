#include "libc/libc.h"

#define URANDOM_PATH "$/dev/md64api/urandom"
#define BIG_LEN      4096

static int read_full(int fd, unsigned char *buf, unsigned long len) {
    unsigned long got = 0;

    while (got < len) {
        long r = read(fd, buf + got, len - got);
        if (r <= 0)
            return -1;
        got += (unsigned long)r;
    }
    return 0;
}

static void print_hex(const unsigned char *buf, unsigned long len) {
    for (unsigned long i = 0; i < len; i++)
        printf("%02x", buf[i]);
    printf("\n");
}

static int all_zero(const unsigned char *buf, unsigned long len) {
    for (unsigned long i = 0; i < len; i++)
        if (buf[i])
            return 0;
    return 1;
}

int md_main(long argc, char **argv) {
    (void)argc;
    (void)argv;

    static unsigned char big[BIG_LEN];
    unsigned char a[32];
    unsigned char b[32];
    int failures = 0;

    int fd = open(URANDOM_PATH, O_RDONLY, 0);
    if (fd < 0) {
        printf("urandom: cannot open %s\n", URANDOM_PATH);
        return 1;
    }

    if (read_full(fd, a, sizeof(a)) != 0 || read_full(fd, b, sizeof(b)) != 0) {
        printf("FAIL: basic read\n");
        close(fd);
        return 1;
    }
    printf("sample 1: ");
    print_hex(a, sizeof(a));
    printf("sample 2: ");
    print_hex(b, sizeof(b));

    if (all_zero(a, sizeof(a)) || memcmp(a, b, sizeof(a)) == 0) {
        printf("FAIL: consecutive reads are zero or identical\n");
        failures++;
    }

    /* Sizes straddling the 32-byte chunking in the driver. */
    static const unsigned long sizes[] = { 1, 31, 32, 33, 63, 64, 65, 1000 };
    for (unsigned long i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        if (read_full(fd, big, sizes[i]) != 0) {
            printf("FAIL: read of %lu bytes\n", sizes[i]);
            failures++;
        }
    }

    if (read_full(fd, big, BIG_LEN) != 0) {
        printf("FAIL: %d byte read\n", BIG_LEN);
        failures++;
    } else {
        unsigned long ones = 0;
        unsigned long distinct = 0;
        unsigned char seen[256];

        memset(seen, 0, sizeof(seen));

        for (unsigned long i = 0; i < BIG_LEN; i++) {
            unsigned char v = big[i];
            for (int bit = 0; bit < 8; bit++)
                ones += (v >> bit) & 1;
            if (!seen[v]) {
                seen[v] = 1;
                distinct++;
            }
        }

        /* Expected 16384 +/- ~90 (1 sigma); +-500 is more than 5 sigma. */
        printf("bit balance: %lu / %d ones\n", ones, BIG_LEN * 8);
        printf("distinct byte values: %lu / 256\n", distinct);

        if (ones < 15884 || ones > 16884) {
            printf("FAIL: bit balance out of range\n");
            failures++;
        }
        if (distinct < 240) {
            printf("FAIL: too few distinct byte values\n");
            failures++;
        }
    }

    close(fd);

    if (failures) {
        printf("urandom test: %d failure(s)\n", failures);
        return 1;
    }
    printf("urandom test: PASS\n");
    return 0;
}