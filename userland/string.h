#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

// Large/complex function — keep static
static int snprintf(char *str, size_t size, const char *format, ...) {
    if (!str || !format || size == 0) return 0;

    va_list args;
    va_start(args, format);

    char *out = str;
    const char *p = format;
    size_t remaining = size - 1;
    int written = 0;

    while (*p && remaining > 0) {
        if (*p == '%') {
            p++;
            switch (*p) {
                case 's': {
                    const char *s = va_arg(args, const char *);
                    if (!s) s = "(null)";
                    while (*s && remaining > 0) { *out++ = *s++; written++; remaining--; }
                    break;
                }
                case 'd':
                case 'i': {
                    int val = va_arg(args, int);
                    char buf[32]; itoa(val, buf, 10);
                    const char *s = buf;
                    while (*s && remaining > 0) { *out++ = *s++; written++; remaining--; }
                    break;
                }
                case 'x': {
                    int val = va_arg(args, int);
                    char buf[32]; itoa(val, buf, 16);
                    const char *s = buf;
                    while (*s && remaining > 0) { *out++ = *s++; written++; remaining--; }
                    break;
                }
                case 'c': {
                    char c = (char)va_arg(args, int);
                    if (remaining > 0) { *out++ = c; written++; remaining--; }
                    break;
                }
                case '%': {
                    if (remaining > 0) { *out++ = '%'; written++; remaining--; }
                    break;
                }
                default: {
                    if (remaining > 0) { *out++ = '%'; written++; remaining--; }
                    if (remaining > 0) { *out++ = *p; written++; remaining--; }
                    break;
                }
            }
        } else {
            *out++ = *p; written++; remaining--;
        }
        p++;
    }

    *out = '\0';
    va_end(args);
    return written;
}

// Small/frequently called functions — inline
static inline void *memset(void *dest, int val, size_t len) {
    unsigned char *ptr = dest;
    while (len--) *ptr++ = (unsigned char)val;
    return dest;
}

static inline void *memcpy(void *dest, const void *src, size_t len) {
    const unsigned char *s = src;
    unsigned char *d = dest;
    while (len--) *d++ = *s++;
    return dest;
}

static inline size_t strlen(const char *str) {
    const char *s = str;
    while (*s) ++s;
    return s - str;
}

static inline int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) { ++s1; ++s2; }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

static inline char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

static inline char *strncpy(char *dest, const char *src, size_t n) {
    char *d = dest;
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) d[i] = src[i];
    for (; i < n; i++) d[i] = '\0';
    return dest;
}

static inline char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) ++d;
    while ((*d++ = *src++));
    return dest;
}

static inline char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (*d) ++d;
    while (n-- && *src) *d++ = *src++;
    *d = '\0';
    return dest;
}

static inline int strncmp(const char *s1, const char *s2, size_t n) {
    while (n-- && *s1 && (*s1 == *s2)) { ++s1; ++s2; }
    return n ? *(unsigned char *)s1 - *(unsigned char *)s2 : 0;
}

static inline int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = s1, *p2 = s2;
    while (n--) if (*p1 != *p2) return *p1 - *p2; else { ++p1; ++p2; }
    return 0;
}

static void *memmove(void *dest, const void *src, size_t n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    if (d < s) { while (n--) *d++ = *s++; }
    else if (d > s) { d += n; s += n; while (n--) *--d = *--s; }
    return dest;
}

static inline char *strchr(const char *str, int c) {
    while (*str) { if (*str == (char)c) return (char *)str; ++str; }
    return (*str == (char)c) ? (char *)str : NULL;
}

static inline char *strrchr(const char *str, int c) {
    const char *last = NULL;
    while (*str) { if (*str == (char)c) last = str; ++str; }
    if (c == '\0') return (char *)str;
    return (char *)last;
}

static char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    while (*haystack) {
        const char *h = haystack, *n = needle;
        while (*h && *n && (*h == *n)) { ++h; ++n; }
        if (!*n) return (char *)haystack;
        ++haystack;
    }
    return NULL;
}

static inline int atoi(const char *str) {
    int result = 0, sign = 1;
    while (*str == ' ' || *str == '\t') ++str;
    if (*str == '-') { sign = -1; ++str; } else if (*str == '+') ++str;
    while (*str >= '0' && *str <= '9') { result = result * 10 + (*str - '0'); ++str; }
    return sign * result;
}
