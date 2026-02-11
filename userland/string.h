#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* --- 1. SEARCH & COMPARISON --- */

static inline size_t strlen(const char *str) {
    const char *s = str;
    while (*s) ++s;
    return s - str;
}

static inline int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) { ++s1; ++s2; }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

static inline int strncmp(const char *s1, const char *s2, size_t n) {
    if (n == 0) return 0;
    while (n-- > 0 && *s1 && (*s1 == *s2)) {
        if (n == 0) break;
        ++s1; ++s2;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

static inline int strcasecmp(const char *s1, const char *s2) {
    while (*s1) {
        int c1 = (*s1 >= 'A' && *s1 <= 'Z') ? *s1 + 32 : *s1;
        int c2 = (*s2 >= 'A' && *s2 <= 'Z') ? *s2 + 32 : *s2;
        if (c1 != c2) return c1 - c2;
        s1++; s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

static inline char *strchr(const char *str, int c) {
    while (*str) { if (*str == (char)c) return (char *)str; ++str; }
    return (c == '\0') ? (char *)str : NULL;
}

static inline char *strrchr(const char *str, int c) {
    const char *last = NULL;
    while (*str) { if (*str == (char)c) last = str; ++str; }
    return (c == '\0') ? (char *)str : (char *)last;
}

static inline char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && (*h == *n)) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return NULL;
}

/* --- 2. THE TOKENIZER (Crucial for DB parsing) --- */

static inline char *strtok(char *str, const char *delim) {
    static char *last = NULL;
    if (str) last = str;
    if (!last || *last == '\0') return NULL;

    // Skip leading delimiters
    while (*last && strchr(delim, *last)) last++;
    if (*last == '\0') return NULL;

    char *token = last;
    // Find end of token
    while (*last && !strchr(delim, *last)) last++;
    
    if (*last != '\0') {
        *last = '\0';
        last++;
    }
    return token;
}

/* --- 3. SAFE COPY & CONCAT (BSD Style) --- */

static inline size_t strlcpy(char *dest, const char *src, size_t size) {
    size_t i = 0;
    size_t src_len = strlen(src);
    if (size > 0) {
        for (i = 0; i < size - 1 && src[i] != '\0'; i++) dest[i] = src[i];
        dest[i] = '\0';
    }
    return src_len;
}

static inline size_t strlcat(char *dest, const char *src, size_t size) {
    size_t dest_len = strlen(dest);
    size_t src_len = strlen(src);
    if (dest_len >= size) return size + src_len;
    strlcpy(dest + dest_len, src, size - dest_len);
    return dest_len + src_len;
}

static inline char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++) != '\0');
    return dest;
}

static inline char *strncpy(char *dest, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dest[i] = src[i];
    for (; i < n; i++) dest[i] = '\0';
    return dest;
}

static inline char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++) != '\0');
    return dest;
}

static inline char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (*d) d++;
    size_t i = 0;
    for (; i < n && src[i]; i++) d[i] = src[i];
    d[i] = '\0';
    return dest;
}

static inline size_t safe_strcpy(char *dest, size_t dest_sz, const char *src) {
    if (!dest || !src || dest_sz == 0) return 0;
    size_t i = 0;
    for (; i + 1 < dest_sz && src[i]; i++) dest[i] = src[i];
    dest[i] = '\0';
    return i;
}

// Your requested implementation
static inline size_t safe_strcat(char *dest, size_t dest_sz, const char *src) {
    return strlcat(dest, src, dest_sz);
}

/* --- 4. MEMORY OPERATIONS --- */

static inline void *memset(void *dest, int val, size_t len) {
    unsigned char *ptr = dest;
    while (len--) *ptr++ = (unsigned char)val;
    return dest;
}

static inline void *memcpy(void *dest, const void *src, size_t len) {
    unsigned char *d = dest; const unsigned char *s = src;
    while (len--) *d++ = *s++;
    return dest;
}

static inline void *memmove(void *dest, const void *src, size_t n) {
    unsigned char *d = dest; const unsigned char *s = src;
    if (d < s) { while (n--) *d++ = *s++; }
    else if (d > s) { d += n; s += n; while (n--) *--d = *--s; }
    return dest;
}

static inline int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = s1, *p2 = s2;
    while (n--) { if (*p1 != *p2) return *p1 - *p2; p1++; p2++; }
    return 0;
}

/* --- 5. NUMERIC CONVERSION --- */

static inline int atoi(const char *str) {
    int res = 0, sign = 1;
    while (*str == ' ' || *str == '\t') str++;
    if (*str == '-') { sign = -1; str++; }
    while (*str >= '0' && *str <= '9') { res = res * 10 + (*str - '0'); str++; }
    return res * sign;
}

// Optimized itoa/ulltoa helpers
static inline void ulltoa(unsigned long long value, char *str, int base, int upper) {
    char *p = str;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (value == 0) { *p++ = '0'; *p = '\0'; return; }
    while (value > 0) { *p++ = digits[value % base]; value /= base; }
    *p = '\0';
    // Reverse
    char *start = str, *end = p - 1;
    while (start < end) { char t = *start; *start++ = *end; *end-- = t; }
}

static inline void itoa(int value, char *str, int base) {
    if (value < 0 && base == 10) { *str++ = '-'; ulltoa((unsigned int)-value, str, 10, 0); }
    else { ulltoa((unsigned int)value, str, base, 0); }
}

/* --- 6. THE PRINTF ENGINE --- */

static inline int snprintf(char *str, size_t size, const char *fmt, ...) {
    if (!str || size == 0) return 0;
    va_list args; va_start(args, fmt);
    char *out = str;
    size_t rem = size - 1;
    
    for (const char *p = fmt; *p && rem > 0; p++) {
        if (*p == '%' && *(p+1)) {
            p++;
            char buf[64]; memset(buf, 0, 64);
            if (*p == 's') {
                const char *s = va_arg(args, const char*);
                while (s && *s && rem > 0) { *out++ = *s++; rem--; }
            } else if (*p == 'd' || *p == 'i') {
                itoa(va_arg(args, int), buf, 10);
                for (int i=0; buf[i] && rem > 0; i++) { *out++ = buf[i]; rem--; }
            } else if (*p == 'x' || *p == 'X') {
                ulltoa(va_arg(args, unsigned int), buf, 16, (*p == 'X'));
                for (int i=0; buf[i] && rem > 0; i++) { *out++ = buf[i]; rem--; }
            } else if (*p == 'c') {
                *out++ = (char)va_arg(args, int); rem--;
            } else if (*p == '%') {
                *out++ = '%'; rem--;
            }
        } else {
            *out++ = *p; rem--;
        }
    }
    *out = '\0';
    va_end(args);
    return (int)(out - str);
}

