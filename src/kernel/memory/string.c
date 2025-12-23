#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/drivers/graphics/VGA.h"
#include <stdarg.h>

static inline void ulltoa_pad(unsigned long long value, char *buf, int base, int upper, int width, int zero_pad) {
    /* Convert to string first */
    char tmp[64];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    int i = 0;
    if (value == 0) {
        tmp[i++] = '0';
    } else {
        while (value > 0 && i < (int)sizeof(tmp)) {
            tmp[i++] = digits[value % (unsigned long long)base];
            value /= (unsigned long long)base;
        }
    }

    int len = i;
    int pad = (width > len) ? (width - len) : 0;

    int out = 0;
    char pad_ch = zero_pad ? '0' : ' ';
    while (pad-- > 0) buf[out++] = pad_ch;

    while (i-- > 0) buf[out++] = tmp[i];
    buf[out] = 0;
}

static inline void lltoa_dec(long long value, char *buf) {
    if (value < 0) {
        *buf++ = '-';
        unsigned long long u = (unsigned long long)(-(value + 1)) + 1ULL;
        ulltoa_pad(u, buf, 10, 0, 0, 0);
    } else {
        ulltoa_pad((unsigned long long)value, buf, 10, 0, 0, 0);
    }
}

int snprintf(char *str, size_t size, const char *format, ...) {
    if (!str || !format || size == 0) {
        return 0;
    }

    va_list args;
    va_start(args, format);

    char *out = str;
    const char *p = format;
    size_t remaining = size - 1;  // Leave space for null terminator
    int written = 0;

    while (*p && remaining > 0) {
        if (*p == '%') {
            p++;

            /* length modifiers: l / ll */
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

            /* Minimal width + zero padding support: e.g. %04X */
            int zero_pad = 0;
            int width = 0;
            if (*p == '0') {
                zero_pad = 1;
                p++;
            }
            while (*p >= '0' && *p <= '9') {
                width = (width * 10) + (*p - '0');
                p++;
            }

            switch (*p) {
                case 's': {
                    const char *s = va_arg(args, const char *);
                    if (!s) s = "(null)";
                    while (*s && remaining > 0) {
                        *out++ = *s++;
                        written++;
                        remaining--;
                    }
                    break;
                }

                case 'd':
                case 'i': {
                    char buf[64];
                    if (longlongmod) {
                        long long val = va_arg(args, long long);
                        lltoa_dec(val, buf);
                    } else if (longmod) {
                        long val = va_arg(args, long);
                        lltoa_dec((long long)val, buf);
                    } else {
                        int val = va_arg(args, int);
                        itoa(val, buf, 10);
                    }

                    const char *s = buf;
                    while (*s && remaining > 0) {
                        *out++ = *s++;
                        written++;
                        remaining--;
                    }
                    break;
                }

                case 'u': {
                    char buf[64];
                    unsigned long long val;
                    if (longlongmod) {
                        val = va_arg(args, unsigned long long);
                    } else if (longmod) {
                        val = (unsigned long long)va_arg(args, unsigned long);
                    } else {
                        val = (unsigned long long)va_arg(args, unsigned int);
                    }
                    ulltoa_pad(val, buf, 10, 0, width, zero_pad);

                    const char *s = buf;
                    while (*s && remaining > 0) {
                        *out++ = *s++;
                        written++;
                        remaining--;
                    }
                    break;
                }

                case 'x':
                case 'X': {
                    char buf[64];
                    unsigned long long val;
                    if (longlongmod) {
                        val = va_arg(args, unsigned long long);
                    } else if (longmod) {
                        val = (unsigned long long)va_arg(args, unsigned long);
                    } else {
                        val = (unsigned long long)va_arg(args, unsigned int);
                    }
                    ulltoa_pad(val, buf, 16, (*p == 'X'), width, zero_pad);

                    const char *s = buf;
                    while (*s && remaining > 0) {
                        *out++ = *s++;
                        written++;
                        remaining--;
                    }
                    break;
                }

                case 'c': {
                    char c = (char)va_arg(args, int);
                    if (remaining > 0) {
                        *out++ = c;
                        written++;
                        remaining--;
                    }
                    break;
                }

                case '%': {
                    if (remaining > 0) {
                        *out++ = '%';
                        written++;
                        remaining--;
                    }
                    break;
                }

                default: {
                    /* Unsupported format specifier — print literally */
                    if (remaining > 0) {
                        *out++ = '%';
                        written++;
                        remaining--;
                    }
                    if (remaining > 0) {
                        *out++ = *p;
                        written++;
                        remaining--;
                    }
                    break;
                }
            }
        } else {
            *out++ = *p;
            written++;
            remaining--;
        }
        p++;
    }

    *out = '\0';
    va_end(args);
    return written;
}

// Optimized memset function using word-sized accesses if possible
void *memset(void *dest, int val, size_t len) {
    unsigned char *ptr = dest;
    while (len--) {
        *ptr++ = (unsigned char)val;
    }
    return dest;
}

// Optimized memcpy function
void *memcpy(void *dest, const void *src, size_t len) {
    const unsigned char *s = src;
    unsigned char *d = dest;
    while (len--) {
        *d++ = *s++;
    }
    return dest;
}

// Optimized strlen function using pointer arithmetic
size_t strlen(const char *str) {
    const char *s = str;
    while (*s) {
        ++s;
    }
    return s - str;
}

// Optimized strcmp function to avoid extra dereferencing
int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        ++s1;
        ++s2;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

// Optimized strcpy function using pointer arithmetic
char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++)) {
        // Do nothing here except assignment
    }
    return dest;
}

// Optimized strncpy function - copies up to n characters from src to dest
char *strncpy(char *dest, const char *src, size_t n) {
    char *d = dest;
    size_t i;
    
    // Copy characters from src to dest, up to n characters
    for (i = 0; i < n && src[i] != '\0'; i++) {
        d[i] = src[i];
    }
    
    // Pad remaining bytes with null characters if src is shorter than n
    for (; i < n; i++) {
        d[i] = '\0';
    }
    
    return dest;
}

// Optimized strcat function using pointer arithmetic
char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) {
        ++d;
    }
    while ((*d++ = *src++)) {
        // Do nothing here except assignment
    }
    return dest;
}

// Optimized strncat function - concatenates up to n characters
char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest;
    
    // Find end of dest string
    while (*d) {
        ++d;
    }
    
    // Append up to n characters from src
    while (n-- && *src) {
        *d++ = *src++;
    }
    
    // Always null-terminate
    *d = '\0';
    
    return dest;
}

// Optimized strncmp function
int strncmp(const char *s1, const char *s2, size_t n) {
    while (n-- && *s1 && (*s1 == *s2)) {
        ++s1;
        ++s2;
    }
    return n ? *(unsigned char *)s1 - *(unsigned char *)s2 : 0;
}

// Optimized memcmp function
int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = s1;
    const uint8_t *p2 = s2;
    
    while (n--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        ++p1;
        ++p2;
    }
    return 0;
}

// memmove - handles overlapping memory regions
//
// NOTE: This is performance-critical for framebuffer scrolling (multi-megabyte moves).
// The simple byte loop can appear like a "hang" under emulation.
void *memmove(void *dest, const void *src, size_t n) {
    if (dest == src || n == 0) return dest;

#if defined(__x86_64__) || defined(__amd64__)
    // Use rep movsb (handles any alignment; fast on modern CPUs and in QEMU).
    // We must handle overlap direction correctly.
    if ((uintptr_t)dest < (uintptr_t)src) {
        __asm__ volatile(
            "cld\n\t"
            "rep movsb"
            : "+D"(dest), "+S"(src), "+c"(n)
            :
            : "memory"
        );
        return dest;
    }

    // Backward copy: start from end, set DF, rep movsb, then clear DF.
    const uint8_t *s = (const uint8_t*)src + n - 1;
    uint8_t *d = (uint8_t*)dest + n - 1;
    __asm__ volatile(
        "std\n\t"
        "rep movsb\n\t"
        "cld"
        : "+D"(d), "+S"(s), "+c"(n)
        :
        : "memory"
    );
    return dest;
#else
    unsigned char *d = (unsigned char*)dest;
    const unsigned char *s = (const unsigned char*)src;

    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dest;
#endif
}

// strchr - find first occurrence of character in string
char *strchr(const char *str, int c) {
    while (*str) {
        if (*str == (char)c) {
            return (char *)str;
        }
        ++str;
    }
    return (*str == (char)c) ? (char *)str : NULL;
}

// strrchr - find last occurrence of character in string
char *strrchr(const char *str, int c) {
    const char *last = NULL;
    while (*str) {
        if (*str == (char)c) {
            last = str;
        }
        ++str;
    }
    if (c == '\0') {
        return (char *)str;
    }
    return (char *)last;
}

// strstr - find first occurrence of substring in string
char *strstr(const char *haystack, const char *needle) {
    if (!*needle) {
        return (char *)haystack;
    }
    
    while (*haystack) {
        const char *h = haystack;
        const char *n = needle;
        
        while (*h && *n && (*h == *n)) {
            ++h;
            ++n;
        }
        
        if (!*n) {
            return (char *)haystack;
        }
        
        ++haystack;
    }
    
    return NULL;
}

// Optimized itoa function with a precomputed digits array
char *itoa(int value, char *str, int base) {
    if (base < 2 || base > 36) {
        *str = '\0';
        return str;
    }

    const char *digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    char *ptr = str, *ptr1 = str, tmp_char;
    int sign = 0;

    // Handle negative sign for base 10
    if (value < 0 && base == 10) {
        sign = 1;
    }

    // Use unsigned int for safe handling of INT_MIN
    unsigned int uvalue = (sign) ? -value : value;

    do {
        int remainder = uvalue % base;
        *ptr++ = digits[remainder];
        uvalue /= base;
    } while (uvalue);

    if (sign) {
        *ptr++ = '-';
    }

    *ptr-- = '\0';

    // Reverse the string
    while (ptr1 < ptr) {
        tmp_char = *ptr;
        *ptr-- = *ptr1;
        *ptr1++ = tmp_char;
    }

    return str;
}

// Optimized atoi function that avoids extra loops
int atoi(const char *str) {
    int result = 0;
    int sign = 1;

    while (*str == ' ' || *str == '\t') {
        ++str;
    }

    if (*str == '-') {
        sign = -1;
        ++str;
    } else if (*str == '+') {
        ++str;
    }

    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        ++str;
    }

    return sign * result;
}

char* str_append(int count, ...) {
    va_list args;
    va_start(args, count);

    // First pass — measure total length
    size_t total_length = 1; // for '\0'
    for (int i = 0; i < count; i++) {
        const char *s = va_arg(args, const char*);
        total_length += strlen(s);
    }

    va_end(args);

    // Allocate memory using kmalloc
    char *result = (char*)kmalloc(total_length);
    if (!result)
        return NULL;

    result[0] = '\0';

    // Second pass — build final string
    va_start(args, count);
    for (int i = 0; i < count; i++) {
        const char *s = va_arg(args, const char*);
        strcat(result, s);
    }
    va_end(args);

    return result;
}


// Helper function to convert string