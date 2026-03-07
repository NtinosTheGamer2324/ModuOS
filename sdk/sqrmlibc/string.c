#include "include/sqrmlibc.h"

size_t strlen(const char *s) {
    /*
     * Plain byte scan. The qword-at-a-time trick is unsafe in a freestanding
     * SQRM module context because kernel string pointers may sit at the end of
     * a mapped page — the speculative 8-byte read would fault on the next
     * (unmapped) page. Correctness over micro-optimisation here.
     */
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t maxlen) {
    size_t n = 0;
    while (n < maxlen && s[n]) n++;
    return n;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    while (i < n && src[i]) { dst[i] = src[i]; i++; }
    while (i < n) dst[i++] = '\0';
    return dst;
}

/* Safe copy: always NUL-terminates, returns number of bytes written (excl NUL) */
size_t strlcpy(char *dst, const char *src, size_t dsz) {
    if (!dsz) return strlen(src);
    size_t i = 0;
    while (i + 1 < dsz && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i + strlen(src + i);
}

char *strcat(char *dst, const char *src) {
    char *d = dst + strlen(dst);
    while ((*d++ = *src++));
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst + strlen(dst);
    size_t i = 0;
    while (i < n && src[i]) { *d++ = src[i++]; }
    *d = '\0';
    return dst;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

/* Case-insensitive compare */
static unsigned char to_lower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && to_lower((unsigned char)*a) == to_lower((unsigned char)*b)) { a++; b++; }
    return (int)to_lower((unsigned char)*a) - (int)to_lower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n) {
    while (n && *a && to_lower((unsigned char)*a) == to_lower((unsigned char)*b)) { a++; b++; n--; }
    if (!n) return 0;
    return (int)to_lower((unsigned char)*a) - (int)to_lower((unsigned char)*b);
}

char *strchr(const char *s, int c) {
    unsigned char ch = (unsigned char)c;
    while (*s) {
        if ((unsigned char)*s == ch) return (char *)s;
        s++;
    }
    return (ch == '\0') ? (char *)s : (char *)0;
}

char *strrchr(const char *s, int c) {
    unsigned char ch = (unsigned char)c;
    const char *last = (char *)0;
    while (*s) {
        if ((unsigned char)*s == ch) last = s;
        s++;
    }
    return (ch == '\0') ? (char *)s : (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    size_t nlen = strlen(needle);
    while (*haystack) {
        if ((unsigned char)*haystack == (unsigned char)*needle) {
            size_t i = 1;
            while (i < nlen && haystack[i] == needle[i]) i++;
            if (i == nlen) return (char *)haystack;
        }
        haystack++;
    }
    return (char *)0;
}

/* Minimal itoa — base 10 and 16 only */
static const char hex_digits[] = "0123456789abcdef";

char *itoa(long val, char *buf, int base) {
    char tmp[66];
    int i = 0;
    int neg = 0;

    if (base == 10 && val < 0) { neg = 1; val = -val; }

    unsigned long uval = (unsigned long)val;
    if (uval == 0) { tmp[i++] = '0'; }
    while (uval) { tmp[i++] = hex_digits[uval % (unsigned)base]; uval /= (unsigned)base; }

    int j = 0;
    if (neg) buf[j++] = '-';
    while (i--) buf[j++] = tmp[i];
    buf[j] = '\0';
    return buf;
}

char *utoa(unsigned long val, char *buf, int base) {
    char tmp[66];
    int i = 0;
    if (val == 0) { tmp[i++] = '0'; }
    while (val) { tmp[i++] = hex_digits[val % (unsigned)base]; val /= (unsigned)base; }
    int j = 0;
    while (i--) buf[j++] = tmp[i];
    buf[j] = '\0';
    return buf;
}

/* snprintf — minimal subset: %s %d %u %x %X %c %% %lu %lx */
int vsnprintf(char *buf, size_t sz, const char *fmt, __builtin_va_list ap) {
    size_t pos = 0;

#define PUT(c) do { if (pos + 1 < sz) buf[pos] = (c); pos++; } while(0)

    while (*fmt) {
        if (*fmt != '%') { PUT(*fmt++); continue; }
        fmt++;

        /* Flags */
        int zero_pad = 0, left_align = 0;
        while (*fmt == '0' || *fmt == '-') {
            if (*fmt == '0') zero_pad = 1;
            if (*fmt == '-') left_align = 1;
            fmt++;
        }

        /* Width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt++ - '0'); }

        /* Length modifier */
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; }
        if (*fmt == 'l') { fmt++; } /* ll treated as l */

        char spec = *fmt++;
        char tmp[66];

        if (spec == 's') {
            const char *sv = __builtin_va_arg(ap, const char *);
            if (!sv) sv = "(null)";
            size_t slen = strlen(sv);
            int pad = (width > (int)slen) ? width - (int)slen : 0;
            if (!left_align) while (pad--) PUT(' ');
            while (*sv) PUT(*sv++);
            if (left_align)  while (pad--) PUT(' ');
        } else if (spec == 'c') {
            char cv = (char)__builtin_va_arg(ap, int);
            PUT(cv);
        } else if (spec == 'd' || spec == 'i') {
            long iv = is_long ? __builtin_va_arg(ap, long) : (long)__builtin_va_arg(ap, int);
            itoa(iv, tmp, 10);
            size_t tlen = strlen(tmp);
            int pad = (width > (int)tlen) ? width - (int)tlen : 0;
            char pc = (zero_pad && !left_align) ? '0' : ' ';
            if (!left_align) while (pad--) PUT(pc);
            const char *tp = tmp; while (*tp) PUT(*tp++);
            if (left_align)  while (pad--) PUT(' ');
        } else if (spec == 'u') {
            unsigned long uv = is_long ? __builtin_va_arg(ap, unsigned long) : (unsigned long)__builtin_va_arg(ap, unsigned int);
            utoa(uv, tmp, 10);
            size_t tlen = strlen(tmp);
            int pad = (width > (int)tlen) ? width - (int)tlen : 0;
            char pc = (zero_pad && !left_align) ? '0' : ' ';
            if (!left_align) while (pad--) PUT(pc);
            const char *tp = tmp; while (*tp) PUT(*tp++);
            if (left_align)  while (pad--) PUT(' ');
        } else if (spec == 'x' || spec == 'X' || spec == 'p') {
            unsigned long xv;
            if (spec == 'p') xv = (unsigned long)__builtin_va_arg(ap, void *);
            else xv = is_long ? __builtin_va_arg(ap, unsigned long) : (unsigned long)__builtin_va_arg(ap, unsigned int);
            utoa(xv, tmp, 16);
            if (spec == 'X') {
                for (char *tp = tmp; *tp; tp++)
                    if (*tp >= 'a' && *tp <= 'f') *tp -= 32;
            }
            size_t tlen = strlen(tmp);
            int pad = (width > (int)tlen) ? width - (int)tlen : 0;
            char pc = (zero_pad && !left_align) ? '0' : ' ';
            if (!left_align) while (pad--) PUT(pc);
            const char *tp = tmp; while (*tp) PUT(*tp++);
            if (left_align)  while (pad--) PUT(' ');
        } else if (spec == '%') {
            PUT('%');
        }
        /* unknown specifiers silently dropped */
    }

#undef PUT
    if (sz) buf[pos < sz ? pos : sz - 1] = '\0';
    return (int)pos;
}

int snprintf(char *buf, size_t sz, const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int r = vsnprintf(buf, sz, fmt, ap);
    __builtin_va_end(ap);
    return r;
}
