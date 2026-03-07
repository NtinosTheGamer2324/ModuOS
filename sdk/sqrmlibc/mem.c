#include "sqrmlibc.h"

/*
 * AMD64-optimised memory primitives using rep movsq / rep stosq.
 * Handles unaligned head/tail with byte ops, bulk with 8-byte stores.
 */

void *memset(void *dst, int c, size_t n) {
    unsigned char *p = (unsigned char *)dst;
    unsigned char  v = (unsigned char)c;
    while (n--) *p++ = v;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    unsigned char v = (unsigned char)c;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == v) return (void *)(p + i);
    }
    return (void *)0;
}
