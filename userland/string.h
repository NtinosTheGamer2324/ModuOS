#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

// Integer to string conversion
static inline void itoa(int value, char *str, int base) {
    char *ptr = str;
    char *ptr1 = str;
    char tmp_char;
    int tmp_value;
    
    // Handle 0 explicitly
    if (value == 0) {
        *ptr++ = '0';
        *ptr = '\0';
        return;
    }
    
    // Handle negative numbers for base 10
    int is_negative = 0;
    if (value < 0 && base == 10) {
        is_negative = 1;
        value = -value;
    }
    
    // Process individual digits
    while (value != 0) {
        tmp_value = value;
        value /= base;
        *ptr++ = "0123456789abcdef"[tmp_value - value * base];
    }
    
    // Add negative sign
    if (is_negative) {
        *ptr++ = '-';
    }
    
    *ptr-- = '\0';
    
    // Reverse the string
    while (ptr1 < ptr) {
        tmp_char = *ptr;
        *ptr-- = *ptr1;
        *ptr1++ = tmp_char;
    }
}

// Unsigned long long to string (base 10/16). Internal helper for snprintf.
static inline void ulltoa(unsigned long long value, char *str, int base, int upper) {
    char *ptr = str;
    char *ptr1 = str;
    char tmp_char;

    if (base < 2) base = 10;

    if (value == 0) {
        *ptr++ = '0';
        *ptr = '\0';
        return;
    }

    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    while (value != 0) {
        unsigned long long tmp_value = value;
        value /= (unsigned long long)base;
        *ptr++ = digits[tmp_value - value * (unsigned long long)base];
    }

    *ptr-- = '\0';

    while (ptr1 < ptr) {
        tmp_char = *ptr;
        *ptr-- = *ptr1;
        *ptr1++ = tmp_char;
    }
}

// Signed long long to string (base 10). Internal helper for snprintf.
static inline void lltoa(long long value, char *str) {
    if (value < 0) {
        *str++ = '-';
        /* careful with LLONG_MIN: cast to unsigned */
        unsigned long long u = (unsigned long long)(-(value + 1)) + 1ULL;
        ulltoa(u, str, 10, 0);
    } else {
        ulltoa((unsigned long long)value, str, 10, 0);
    }
}

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

            int longmod = 0;
            int longlongmod = 0;
            if (*p == 'l') {
                longmod = 1;
                p++;
                if (*p == 'l') {
                    longlongmod = 1;
                    p++;
                }
            }

            switch (*p) {
                case 's': {
                    const char *s = va_arg(args, const char *);
                    if (!s) s = "(null)";
                    while (*s && remaining > 0) { *out++ = *s++; written++; remaining--; }
                    break;
                }

                case 'd':
                case 'i': {
                    char buf[64];
                    if (longlongmod) {
                        long long v = va_arg(args, long long);
                        lltoa(v, buf);
                    } else if (longmod) {
                        long v = va_arg(args, long);
                        /* long may be 64-bit here; handle with lltoa */
                        lltoa((long long)v, buf);
                    } else {
                        int v = va_arg(args, int);
                        itoa(v, buf, 10);
                    }
                    const char *s = buf;
                    while (*s && remaining > 0) { *out++ = *s++; written++; remaining--; }
                    break;
                }

                case 'u': {
                    char buf[64];
                    if (longlongmod) {
                        unsigned long long v = va_arg(args, unsigned long long);
                        ulltoa(v, buf, 10, 0);
                    } else if (longmod) {
                        unsigned long v = va_arg(args, unsigned long);
                        ulltoa((unsigned long long)v, buf, 10, 0);
                    } else {
                        unsigned int v = va_arg(args, unsigned int);
                        ulltoa((unsigned long long)v, buf, 10, 0);
                    }
                    const char *s = buf;
                    while (*s && remaining > 0) { *out++ = *s++; written++; remaining--; }
                    break;
                }

                case 'x':
                case 'X': {
                    int upper = (*p == 'X');
                    char buf[64];
                    if (longlongmod) {
                        unsigned long long v = va_arg(args, unsigned long long);
                        ulltoa(v, buf, 16, upper);
                    } else if (longmod) {
                        unsigned long v = va_arg(args, unsigned long);
                        ulltoa((unsigned long long)v, buf, 16, upper);
                    } else {
                        unsigned int v = va_arg(args, unsigned int);
                        ulltoa((unsigned long long)v, buf, 16, upper);
                    }
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
                    /* Unknown specifier: print it literally */
                    if (remaining > 0) { *out++ = '%'; written++; remaining--; }
                    if (longmod && remaining > 0) { *out++ = 'l'; written++; remaining--; }
                    if (longlongmod && remaining > 0) { *out++ = 'l'; written++; remaining--; }
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
