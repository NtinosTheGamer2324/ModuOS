// argtest.c - debug argv passing into userland
#include "libc.h"
#include "string.h"
#include <stdint.h>

static void print_hex_byte(uint8_t b) {
    const char *digits = "0123456789abcdef";
    char out[3];
    out[0] = digits[(b >> 4) & 0xF];
    out[1] = digits[b & 0xF];
    out[2] = 0;
    puts_raw(out);
}

static void print_ptr(const void *p) {
    uintptr_t v = (uintptr_t)p;
    puts_raw("0x");
    for (int i = (int)(sizeof(uintptr_t) * 2) - 1; i >= 0; i--) {
        uint8_t nib = (v >> (i * 4)) & 0xF;
        char c = (nib < 10) ? ('0' + nib) : ('a' + (nib - 10));
        putc(c);
    }
}

static void hexdump_prefix(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) {
        print_hex_byte(b[i]);
        if (i + 1 < n) putc(' ');
    }
}

int md_main(long argc, char **argv) {
    printf("argtest: argc=%ld\n", argc);
    printf("argtest: argv ptr=");
    print_ptr(argv);
    printf("\n");

    if (!argv) {
        printf("argtest: argv is NULL\n");
        return 0;
    }

    for (long i = 0; i < argc; i++) {
        char *s = argv[i];
        printf("argv[%ld] ptr=", i);
        print_ptr(s);

        if (!s) {
            printf(" (NULL)\n");
            continue;
        }

        // Try to print string safely-ish (bounded)
        printf(" str='");
        for (int j = 0; j < 128; j++) {
            char c = s[j];
            if (c == 0) break;
            if ((unsigned char)c < 32 || (unsigned char)c > 126) {
                putc('.');
            } else {
                putc(c);
            }
        }
        printf("'\n");

        // Show first bytes to catch corruption / missing NUL
        printf("          hex[0..31]=");
        hexdump_prefix(s, 32);
        printf("\n");
    }

    // Also show argv[argc] should be NULL
    printf("argv[%ld] (terminator) ptr=", argc);
    print_ptr((argc >= 0) ? argv[argc] : (char*)0);
    printf("\n");

    return 0;
}
