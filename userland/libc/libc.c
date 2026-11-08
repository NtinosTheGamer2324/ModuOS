/* libc.c - ModuOS C library (ISO C + a POSIX subset), built as libc.sqrl.
 *
 * Single translation unit. Every ISO function is a real (non-inline) symbol
 * so it can live in the library. ModuOS-only calls are declared in moduos.h
 * and implemented at the bottom of this file. There is no _start here: every
 * program gets one from crt0.c, which calls md_main().
 * The library exports functions only (no data), so it needs no copy relocs.
 *
 * Not provided yet: floating point (printf/scanf %f %e %g, strtod, math.h),
 * locale, wide chars, rename(), tmpfile().
 */

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>
#include <stdbool.h>

#undef NDEBUG
#include "errno.h"
#include "ctype.h"
#include "string.h"
#include "stdlib.h"
#include "stdio.h"
#include "time.h"
#include "signal.h"
#include "setjmp.h"
#include "assert.h"
#include "unistd.h"
#include "fcntl.h"
#include "sys/types.h"
#include "sys/wait.h"
#include "moduos.h"

#if defined(__GNUC__) && !defined(__clang__)
#define NO_LOOP_PATTERNS __attribute__((optimize("no-tree-loop-distribute-patterns")))
#else
#define NO_LOOP_PATTERNS
#endif

/* ============================================================
   errno + syscall result helper
   ============================================================ */

static int g_errno = 0;
int *__errno_location(void) { return &g_errno; }

/* Kernel convention: negative return = -errno. Convert to -1 + errno. */
static long sysret(long r) {
    if (r < 0) { errno = (int)-r; return -1; }
    return r;
}

/* ============================================================
   <ctype.h>  (C locale)
   ============================================================ */

int isdigit(int c)  { return c >= '0' && c <= '9'; }
int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int islower(int c)  { return c >= 'a' && c <= 'z'; }
int isalpha(int c)  { return isupper(c) || islower(c); }
int isalnum(int c)  { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isblank(int c)  { return c == ' ' || c == '\t'; }
int isspace(int c)  { return c == ' ' || (c >= '\t' && c <= '\r'); }
int iscntrl(int c)  { return (c >= 0 && c < 32) || c == 127; }
int isprint(int c)  { return c >= 32 && c < 127; }
int isgraph(int c)  { return c > 32 && c < 127; }
int ispunct(int c)  { return isgraph(c) && !isalnum(c); }
int tolower(int c)  { return isupper(c) ? c + 32 : c; }
int toupper(int c)  { return islower(c) ? c - 32 : c; }

/* ============================================================
   <string.h>
   ============================================================ */

/* rep movsb / stosb: immune to the compiler turning these loops into
 * calls to themselves. */
void *memcpy(void *__restrict dest, const void *__restrict src, size_t n) {
    void *d = dest;
    __asm__ volatile ("rep movsb" : "+D"(d), "+S"(src), "+c"(n) :: "memory");
    return dest;
}

void *memset(void *dest, int val, size_t n) {
    void *d = dest;
    __asm__ volatile ("rep stosb" : "+D"(d), "+c"(n) : "a"(val) : "memory");
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    if (d == s || n == 0) return dest;
    if (d < s || d >= s + n) {
        __asm__ volatile ("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    } else {
        d += n - 1; s += n - 1;
        __asm__ volatile ("std\n\trep movsb\n\tcld" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    }
    return dest;
}

NO_LOOP_PATTERNS
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *p = a, *q = b;
    while (n--) { if (*p != *q) return *p - *q; p++; q++; }
    return 0;
}

NO_LOOP_PATTERNS
void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    while (n--) { if (*p == (unsigned char)c) return (void *)p; p++; }
    return NULL;
}

NO_LOOP_PATTERNS
size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t max) {
    size_t i = 0;
    while (i < max && s[i]) i++;
    return i;
}

char *strcpy(char *__restrict dest, const char *__restrict src) {
    char *d = dest;
    while ((*d++ = *src++) != '\0') {}
    return dest;
}

char *strncpy(char *__restrict dest, const char *__restrict src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dest[i] = src[i];
    for (; i < n; i++) dest[i] = '\0';
    return dest;
}

char *strcat(char *__restrict dest, const char *__restrict src) {
    char *d = dest + strlen(dest);
    while ((*d++ = *src++) != '\0') {}
    return dest;
}

char *strncat(char *__restrict dest, const char *__restrict src, size_t n) {
    char *d = dest + strlen(dest);
    size_t i = 0;
    for (; i < n && src[i]; i++) d[i] = src[i];
    d[i] = '\0';
    return dest;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *(const unsigned char *)a - *(const unsigned char *)b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return *(const unsigned char *)a - *(const unsigned char *)b;
}

int strcoll(const char *a, const char *b) { return strcmp(a, b); }

size_t strxfrm(char *__restrict dest, const char *__restrict src, size_t n) {
    size_t len = strlen(src);
    if (n) {
        size_t c = len < n - 1 ? len : n - 1;
        memcpy(dest, src, c);
        dest[c] = '\0';
    }
    return len;
}

int strcasecmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int c1 = tolower((unsigned char)*a), c2 = tolower((unsigned char)*b);
        if (c1 != c2) return c1 - c2;
        if (!c1) return 0;
    }
}

int strncasecmp(const char *a, const char *b, size_t n) {
    for (; n; a++, b++, n--) {
        int c1 = tolower((unsigned char)*a), c2 = tolower((unsigned char)*b);
        if (c1 != c2) return c1 - c2;
        if (!c1) return 0;
    }
    return 0;
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c) last = s;
        if (!*s) return (char *)last;
    }
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while (s[n] && strchr(accept, s[n])) n++;
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n])) n++;
    return n;
}

char *strpbrk(const char *s, const char *accept) {
    for (; *s; s++) if (strchr(accept, *s)) return (char *)s;
    return NULL;
}

char *strstr(const char *hay, const char *needle) {
    if (!*needle) return (char *)hay;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)hay;
    }
    return NULL;
}

char *strtok(char *__restrict s, const char *__restrict delim) {
    static char *last = NULL;
    if (s) last = s;
    if (!last) return NULL;
    last += strspn(last, delim);
    if (!*last) { last = NULL; return NULL; }
    char *tok = last;
    last += strcspn(last, delim);
    if (*last) *last++ = '\0'; else last = NULL;
    return tok;
}

size_t strlcpy(char *dest, const char *src, size_t size) {
    size_t len = strlen(src);
    if (size) {
        size_t c = len < size - 1 ? len : size - 1;
        memcpy(dest, src, c);
        dest[c] = '\0';
    }
    return len;
}

size_t strlcat(char *dest, const char *src, size_t size) {
    size_t dlen = strnlen(dest, size);
    size_t slen = strlen(src);
    if (dlen == size) return size + slen;
    strlcpy(dest + dlen, src, size - dlen);
    return dlen + slen;
}

size_t safe_strcpy(char *dest, size_t dest_sz, const char *src) {
    if (!dest || !src || dest_sz == 0) return 0;
    size_t i = 0;
    for (; i + 1 < dest_sz && src[i]; i++) dest[i] = src[i];
    dest[i] = '\0';
    return i;
}

size_t safe_strcat(char *dest, size_t dest_sz, const char *src) {
    return strlcat(dest, src, dest_sz);
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (!p) return NULL;
    return memcpy(p, s, n);
}

char *strndup(const char *s, size_t max) {
    size_t n = strnlen(s, max);
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

char *strerror(int e) {
    switch (e) {
    case 0:         return "Success";
    case EPERM:     return "Operation not permitted";
    case ENOENT:    return "No such file or directory";
    case ESRCH:     return "No such process";
    case EINTR:     return "Interrupted";
    case EIO:       return "I/O error";
    case ENXIO:     return "No such device or address";
    case E2BIG:     return "Argument list too long";
    case ENOEXEC:   return "Exec format error";
    case EBADF:     return "Bad file descriptor";
    case ECHILD:    return "No child processes";
    case EAGAIN:    return "Try again";
    case ENOMEM:    return "Out of memory";
    case EACCES:    return "Permission denied";
    case EFAULT:    return "Bad address";
    case EBUSY:     return "Device or resource busy";
    case EEXIST:    return "File exists";
    case EXDEV:     return "Cross-device link";
    case ENODEV:    return "No such device";
    case ENOTDIR:   return "Not a directory";
    case EISDIR:    return "Is a directory";
    case EINVAL:    return "Invalid argument";
    case ENFILE:    return "File table overflow";
    case EMFILE:    return "Too many open files";
    case ENOTTY:    return "Not a typewriter";
    case EFBIG:     return "File too large";
    case ENOSPC:    return "No space left on device";
    case ESPIPE:    return "Illegal seek";
    case EROFS:     return "Read-only file system";
    case EMLINK:    return "Too many links";
    case EPIPE:     return "Broken pipe";
    case EDOM:      return "Math argument out of domain";
    case ERANGE:    return "Result not representable";
    case ENOSYS:    return "Function not implemented";
    case ENOTEMPTY: return "Directory not empty";
    case EILSEQ:    return "Illegal byte sequence";
    default:        return "Unknown error";
    }
}

/* ============================================================
   Heap: address-ordered free list, coalescing, 16-byte aligned payloads.
   Not thread-safe (single-threaded userland).
   ============================================================ */

typedef struct uheap_hdr {
    size_t size;                /* payload size */
    struct uheap_hdr *next;     /* next free block */
    uint32_t magic;
    uint32_t free;
} __attribute__((aligned(16))) uheap_hdr_t;   /* sizeof == 32, so payloads stay 16-aligned */

#define UHEAP_MAGIC 0xC0FFEE55u

static uheap_hdr_t *g_uheap_free = NULL;
static uint8_t     *g_uheap_end  = NULL;      /* one past the last byte we got from sbrk */
static int          g_uheap_init = 0;

static size_t uheap_align(size_t n) { return (n + 15) & ~(size_t)15; }

static void uheap_insert_free(uheap_hdr_t *b) {
    b->free = 1;
    uheap_hdr_t **pp = &g_uheap_free;
    while (*pp && *pp < b) pp = &(*pp)->next;
    b->next = *pp;
    *pp = b;
}

static void uheap_remove_free(uheap_hdr_t *b) {
    for (uheap_hdr_t **pp = &g_uheap_free; *pp; pp = &(*pp)->next) {
        if (*pp == b) { *pp = b->next; b->next = NULL; return; }
    }
}

/* Merge physically adjacent free blocks (list is address ordered). */
static void uheap_coalesce(void) {
    uheap_hdr_t *cur = g_uheap_free;
    while (cur && cur->next) {
        uheap_hdr_t *next = cur->next;
        if ((uint8_t *)(cur + 1) + cur->size == (uint8_t *)next) {
            cur->size += sizeof(uheap_hdr_t) + next->size;
            cur->next = next->next;
            next->magic = 0;
        } else {
            cur = cur->next;
        }
    }
}

static uheap_hdr_t *uheap_request_from_kernel(size_t payload) {
    if (payload > SIZE_MAX / 2) return NULL;

    if (!g_uheap_init) {
        g_uheap_init = 1;
        void *cur = sbrk(0);
        if ((intptr_t)cur > 0) {
            size_t pad = (size_t)(-(uintptr_t)cur) & 15;
            if (pad) sbrk((intptr_t)pad);
        }
    }

    size_t total = uheap_align(sizeof(uheap_hdr_t) + payload);
    void *mem = sbrk((intptr_t)total);
    if (!mem || (intptr_t)mem < 0) return NULL;

    uheap_hdr_t *h = mem;
    h->size  = total - sizeof(uheap_hdr_t);
    h->next  = NULL;
    h->magic = UHEAP_MAGIC;
    h->free  = 0;
    if ((uint8_t *)mem + total > g_uheap_end) g_uheap_end = (uint8_t *)mem + total;
    return h;
}

static void uheap_split_if_needed(uheap_hdr_t *h, size_t need) {
    size_t remain = (h->size > need) ? (h->size - need) : 0;
    if (remain < sizeof(uheap_hdr_t) + 32) return;

    uheap_hdr_t *nh = (uheap_hdr_t *)((uint8_t *)(h + 1) + need);
    nh->size  = remain - sizeof(uheap_hdr_t);
    nh->next  = NULL;
    nh->magic = UHEAP_MAGIC;
    nh->free  = 1;
    h->size   = need;
    uheap_insert_free(nh);
}

void *malloc(size_t size) {
    if (size == 0) return NULL;
    if (size > SIZE_MAX / 2) { errno = ENOMEM; return NULL; }
    size = uheap_align(size);

    uheap_hdr_t *prev = NULL, *cur = g_uheap_free;
    while (cur) {
        if (cur->magic != UHEAP_MAGIC) { errno = ENOMEM; return NULL; }
        if (cur->free && cur->size >= size) {
            if (prev) prev->next = cur->next; else g_uheap_free = cur->next;
            cur->next = NULL;
            cur->free = 0;
            uheap_split_if_needed(cur, size);
            return cur + 1;
        }
        prev = cur;
        cur = cur->next;
    }

    uheap_hdr_t *h = uheap_request_from_kernel(size);
    if (!h) { errno = ENOMEM; return NULL; }
    uheap_split_if_needed(h, size);
    return h + 1;
}

void free(void *ptr) {
    if (!ptr) return;
    uheap_hdr_t *h = (uheap_hdr_t *)ptr - 1;
    if (h->magic != UHEAP_MAGIC || h->free) return;
    uheap_insert_free(h);
    uheap_coalesce();
}

void *calloc(size_t nmemb, size_t size) {
    if (nmemb > 0 && size > SIZE_MAX / nmemb) { errno = ENOMEM; return NULL; }
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    if (size > SIZE_MAX / 2) { errno = ENOMEM; return NULL; }

    uheap_hdr_t *h = (uheap_hdr_t *)ptr - 1;
    if (h->magic != UHEAP_MAGIC) return NULL;

    size_t new_sz = uheap_align(size);
    if (h->size >= new_sz) {
        uheap_split_if_needed(h, new_sz);
        return ptr;
    }

    /* Try to grow in place into a free neighbour (only look at memory we own). */
    uint8_t *cur_end = (uint8_t *)(h + 1) + h->size;
    if (cur_end + sizeof(uheap_hdr_t) <= g_uheap_end) {
        uheap_hdr_t *next = (uheap_hdr_t *)cur_end;
        if (next->magic == UHEAP_MAGIC && next->free == 1) {
            bool in_list = false;
            for (uheap_hdr_t *f = g_uheap_free; f; f = f->next) {
                if (f == next) { in_list = true; break; }
            }
            if (in_list) {
                size_t combined = h->size + sizeof(uheap_hdr_t) + next->size;
                if (combined >= new_sz) {
                    uheap_remove_free(next);
                    next->magic = 0;
                    h->size = combined;
                    uheap_split_if_needed(h, new_sz);
                    return ptr;
                }
            }
        }
    }

    void *n = malloc(new_sz);
    if (!n) return NULL;
    memcpy(n, ptr, h->size);
    free(ptr);
    return n;
}

/* ============================================================
   <stdlib.h>
   ============================================================ */

static void (*g_atexit[32])(void);
static int g_atexit_n = 0;

int atexit(void (*fn)(void)) {
    if (g_atexit_n >= 32) return -1;
    g_atexit[g_atexit_n++] = fn;
    return 0;
}

__attribute__((noreturn)) void _exit(int status) {
    syscall(SYS_EXIT, status, 0, 0);
    for (;;) syscall(SYS_YIELD, 0, 0, 0);   /* SYS_EXIT must not return */
}

__attribute__((noreturn)) void _Exit(int status) { _exit(status); }

__attribute__((noreturn)) void exit(int status) {
    while (g_atexit_n > 0) g_atexit[--g_atexit_n]();
    fflush(NULL);
    _exit(status);
}

__attribute__((noreturn)) void abort(void) {
    raise(SIGABRT);
    _exit(134);
}

int abs(int x)              { return x < 0 ? -x : x; }
long labs(long x)           { return x < 0 ? -x : x; }
long long llabs(long long x){ return x < 0 ? -x : x; }

div_t div(int a, int b)             { div_t r;   r.quot = a / b; r.rem = a % b; return r; }
ldiv_t ldiv(long a, long b)         { ldiv_t r;  r.quot = a / b; r.rem = a % b; return r; }
lldiv_t lldiv(long long a, long long b) { lldiv_t r; r.quot = a / b; r.rem = a % b; return r; }

static uint32_t g_rand_seed = 1;
void srand(unsigned int seed) { g_rand_seed = seed; }
int rand(void) {
    g_rand_seed = g_rand_seed * 1103515245u + 12345u;
    return (int)((g_rand_seed / 65536u) % 32768u);
}

/* strto* core: returns magnitude; sets *neg and *ovf. */
static unsigned long long strtox(const char *s, char **endp, int base, int *neg, int *ovf) {
    const char *p = s;
    *neg = 0; *ovf = 0;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '-') { *neg = 1; p++; } else if (*p == '+') p++;

    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
        isxdigit((unsigned char)p[2])) {
        p += 2; base = 16;
    } else if (base == 0) {
        base = (p[0] == '0') ? 8 : 10;
    }
    if (base < 2 || base > 36) {
        errno = EINVAL;
        if (endp) *endp = (char *)s;
        return 0;
    }

    unsigned long long v = 0;
    const char *start = p;
    for (;; p++) {
        int c = (unsigned char)*p, d;
        if (c >= '0' && c <= '9')      d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        if (v > (ULLONG_MAX - (unsigned)d) / (unsigned)base) *ovf = 1;
        else v = v * (unsigned)base + (unsigned)d;
    }
    if (p == start) { if (endp) *endp = (char *)s; return 0; }
    if (endp) *endp = (char *)p;
    return v;
}

unsigned long long strtoull(const char *s, char **end, int base) {
    int neg, ovf;
    unsigned long long v = strtox(s, end, base, &neg, &ovf);
    if (ovf) { errno = ERANGE; return ULLONG_MAX; }
    return neg ? 0ULL - v : v;
}

unsigned long strtoul(const char *s, char **end, int base) {
    int neg, ovf;
    unsigned long long v = strtox(s, end, base, &neg, &ovf);
    if (ovf || v > ULONG_MAX) { errno = ERANGE; return ULONG_MAX; }
    return neg ? 0UL - (unsigned long)v : (unsigned long)v;
}

long long strtoll(const char *s, char **end, int base) {
    int neg, ovf;
    unsigned long long v = strtox(s, end, base, &neg, &ovf);
    if (ovf || (!neg && v > (unsigned long long)LLONG_MAX) ||
        (neg && v > (unsigned long long)LLONG_MAX + 1ULL)) {
        errno = ERANGE;
        return neg ? LLONG_MIN : LLONG_MAX;
    }
    return neg ? (long long)(0ULL - v) : (long long)v;
}

long strtol(const char *s, char **end, int base) {
    int neg, ovf;
    unsigned long long v = strtox(s, end, base, &neg, &ovf);
    if (ovf || (!neg && v > (unsigned long long)LONG_MAX) ||
        (neg && v > (unsigned long long)LONG_MAX + 1ULL)) {
        errno = ERANGE;
        return neg ? LONG_MIN : LONG_MAX;
    }
    return neg ? (long)(0ULL - v) : (long)v;
}

int       atoi(const char *s)  { return (int)strtol(s, NULL, 10); }
long      atol(const char *s)  { return strtol(s, NULL, 10); }
long long atoll(const char *s) { return strtoll(s, NULL, 10); }

void ulltoa(unsigned long long value, char *str, int base, int upper) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char *p = str;
    if (base < 2 || base > 16) base = 10;
    if (value == 0) { *p++ = '0'; *p = '\0'; return; }
    while (value) { *p++ = digits[value % (unsigned)base]; value /= (unsigned)base; }
    *p = '\0';
    for (char *a = str, *b = p - 1; a < b; a++, b--) { char t = *a; *a = *b; *b = t; }
}

void itoa(int value, char *str, int base) {
    if (value < 0 && base == 10) {
        *str++ = '-';
        ulltoa(0u - (unsigned)value, str, 10, 0);
    } else {
        ulltoa((unsigned)value, str, base, 0);
    }
}

void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *)) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const char *p = (const char *)base + mid * size;
        int c = cmp(key, p);
        if (c == 0) return (void *)p;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

static void swap_bytes(char *a, char *b, size_t n) {
    while (n--) { char t = *a; *a++ = *b; *b++ = t; }
}

static void heap_sift(char *base, size_t size, size_t root, size_t end,
                      int (*cmp)(const void *, const void *)) {
    while (root * 2 + 1 <= end) {
        size_t child = root * 2 + 1, sw = root;
        if (cmp(base + sw * size, base + child * size) < 0) sw = child;
        if (child + 1 <= end && cmp(base + sw * size, base + (child + 1) * size) < 0) sw = child + 1;
        if (sw == root) return;
        swap_bytes(base + root * size, base + sw * size, size);
        root = sw;
    }
}

/* In-place heapsort: O(n log n), no recursion, no allocation. Not stable. */
void qsort(void *vbase, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    char *base = vbase;
    if (n < 2 || size == 0) return;
    for (size_t start = (n - 2) / 2; ; start--) {
        heap_sift(base, size, start, n - 1, cmp);
        if (start == 0) break;
    }
    for (size_t end = n - 1; end > 0; end--) {
        swap_bytes(base, base + end * size, size);
        heap_sift(base, size, 0, end - 1, cmp);
    }
}

/* ---- environment (kernel-managed per process) ---- */

int putenv(const char *kv) { return (int)sysret(syscall(SYS_PUTENV, (long)kv, 0, 0)) < 0 ? -1 : 0; }

/* Returns a pointer to a static buffer (overwritten per call), or NULL. */
char *getenv(const char *key) {
    static char buf[256];
    long r = syscall(SYS_GETENV, (long)key, (long)buf, (long)sizeof(buf));
    return r < 0 ? NULL : buf;
}

int unsetenv(const char *key) { return (int)sysret(syscall(SYS_UNSETENV, (long)key, 0, 0)) < 0 ? -1 : 0; }

int setenv(const char *key, const char *val, int overwrite) {
    if (!key || !*key || strchr(key, '=') || !val) { errno = EINVAL; return -1; }
    if (!overwrite && getenv(key)) return 0;
    char kv[512];
    size_t klen = strlen(key), vlen = strlen(val);
    if (klen + 1 + vlen + 1 > sizeof(kv)) { errno = E2BIG; return -1; }
    memcpy(kv, key, klen);
    kv[klen] = '=';
    memcpy(kv + klen + 1, val, vlen + 1);
    return putenv(kv);
}

/* No shell known to libc: hands the line to the kernel's exec. */
int system(const char *cmd) {
    if (!cmd) return 1;
    return (int)syscall(SYS_EXEC, (long)cmd, 0, 0);
}

/* ============================================================
   POSIX-ish syscall wrappers (<unistd.h>, <fcntl.h>, <sys/wait.h>, <signal.h>)
   ============================================================ */

int open(const char *path, int flags, ...) {
    unsigned mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, unsigned);
        va_end(ap);
    }
    return (int)sysret(syscall(SYS_OPEN, (long)path, flags, (long)mode));
}

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    return (int)sysret(syscall(SYS_FCNTL, fd, cmd, arg));
}

int     close(int fd)                          { return (int)sysret(syscall(SYS_CLOSE, fd, 0, 0)); }
ssize_t read(int fd, void *buf, size_t count)  { return (ssize_t)sysret(syscall(SYS_READ, fd, (long)buf, (long)count)); }
ssize_t write(int fd, const void *buf, size_t count) { return (ssize_t)sysret(syscall(SYS_WRITEFILE, fd, (long)buf, (long)count)); }
off_t   lseek(int fd, off_t off, int whence)   { return (off_t)sysret(syscall(SYS_LSEEK, fd, off, whence)); }
int     dup(int fd)                            { return (int)sysret(syscall(SYS_DUP, fd, 0, 0)); }
int     dup2(int oldfd, int newfd)             { return (int)sysret(syscall(SYS_DUP2, oldfd, newfd, 0)); }
int     pipe(int fds[2])                       { return (int)sysret(syscall(SYS_PIPE, (long)fds, 0, 0)); }

pid_t fork(void)    { return (pid_t)sysret(syscall(SYS_FORK, 0, 0, 0)); }
pid_t getpid(void)  { return (pid_t)syscall(SYS_GETPID, 0, 0, 0); }
pid_t getppid(void) { return (pid_t)syscall(SYS_GETPPID, 0, 0, 0); }

int execve(const char *path, char *const argv[], char *const envp[]) {
    return (int)sysret(syscall(SYS_EXECVE, (long)path, (long)argv, (long)envp));
}

uid_t getuid(void)  { return (uid_t)syscall(SYS_GETUID, 0, 0, 0); }
uid_t geteuid(void) { return (uid_t)syscall(SYS_GETEUID, 0, 0, 0); }
gid_t getgid(void)  { return (gid_t)syscall(SYS_GETGID, 0, 0, 0); }
gid_t getegid(void) { return (gid_t)syscall(SYS_GETEGID, 0, 0, 0); }
int   setuid(uid_t uid) { return (int)sysret(syscall(SYS_SETUID, uid, 0, 0)); }
int   setgid(gid_t gid) { return (int)sysret(syscall(SYS_SETGID, gid, 0, 0)); }

unsigned int sleep(unsigned int sec) { syscall(SYS_SLEEP, sec, 0, 0); return 0; }

int chdir(const char *path)  { return (int)sysret(syscall(SYS_CHDIR, (long)path, 0, 0)); }
int rmdir(const char *path)  { return (int)sysret(syscall(SYS_RMDIR, (long)path, 0, 0)); }
int unlink(const char *path) { return (int)sysret(syscall(SYS_UNLINK, (long)path, 0, 0)); }

char *getcwd(char *buf, size_t size) {
    long r = syscall(SYS_GETCWD, (long)buf, (long)size, 0);
    if (r < 0 && r > -4096) { errno = (int)-r; return NULL; }
    return (char *)r;
}

pid_t waitpid(pid_t pid, int *status, int options) {
    long kopt = (options & WNOHANG) ? 1 : 0;      /* WAITX_WNOHANG */
    return (pid_t)sysret(syscall(SYS_WAITX, pid, (long)status, kopt));
}

pid_t wait(int *status) { return (pid_t)sysret(syscall(SYS_WAIT, (long)status, 0, 0)); }

#define LIBC_STRINGIFY_INNER(value) #value
#define LIBC_STRINGIFY(value) LIBC_STRINGIFY_INNER(value)

static __attribute__((naked, noreturn)) void signal_restorer(void) {
    __asm__ volatile (
        "movl $" LIBC_STRINGIFY(SYS_RT_SIGRETURN) ", %eax\n"
        "syscall\n"
        "ud2\n");
}

#undef LIBC_STRINGIFY
#undef LIBC_STRINGIFY_INNER

int sigaction(int sig, const struct sigaction *action,
              struct sigaction *old_action) {
    struct sigaction kernel_action;
    if (action) {
        kernel_action = *action;
        kernel_action.sa_restorer = signal_restorer;
        action = &kernel_action;
    }

    long result = syscall(SYS_SIGACTION, sig, (long)action,
                          (long)old_action);
    if (result < 0 && result > -4096) {
        errno = (int)-result;
        return -1;
    }
    return (int)result;
}

sighandler_t signal(int sig, sighandler_t handler) {
    struct sigaction action = {0};
    struct sigaction old_action;
    action.sa_handler = handler;
    if (sigaction(sig, &action, &old_action) < 0) return SIG_ERR;
    return old_action.sa_handler;
}
int raise(int sig)           { return (int)sysret(syscall(SYS_RAISE, sig, 0, 0)) < 0 ? -1 : 0; }
int kill(int pid, int sig)   { return (int)sysret(syscall(SYS_KILL, pid, sig, 0)); }

pid_t setsid(void)                    { return (pid_t)sysret(syscall(SYS_SETSID, 0, 0, 0)); }
int   setpgid(pid_t pid, pid_t pgid)  { return (int)sysret(syscall(SYS_SETPGID, pid, pgid, 0)); }
pid_t getpgid(pid_t pid)              { return (pid_t)sysret(syscall(SYS_GETPGID, pid, 0, 0)); }
pid_t getsid(pid_t pid)               { return (pid_t)sysret(syscall(SYS_GETSID, pid, 0, 0)); }

/* ============================================================
   <assert.h>
   ============================================================ */

__attribute__((noreturn))
void __assert_fail(const char *expr, const char *file, int line, const char *func) {
    fprintf(stderr, "Assertion failed: %s (%s: %s: %d)\n", expr, file, func, line);
    abort();
}

/* ============================================================
   Formatted output engine (printf family)
   One engine, driven by an emit callback, so vprintf/fprintf are unbounded
   and snprintf is bounded without duplicated code.
   Supports: d i u o x X p c s %, flags - 0 + space #, width, .precision,
   '*' for either, length hh h l ll z j t. No floating point yet.
   ============================================================ */

typedef void (*emit_fn)(void *ctx, char c);

static int vformat(emit_fn emit, void *ctx, const char *fmt, va_list ap) {
    int total = 0;
#define EMIT(ch) do { emit(ctx, (char)(ch)); total++; } while (0)

    for (const char *p = fmt; *p; ) {
        if (*p != '%') { EMIT(*p++); continue; }
        p++;
        if (!*p) break;

        int flag_left = 0, flag_zero = 0, flag_plus = 0, flag_space = 0, flag_hash = 0;
        for (;;) {
            if      (*p == '-') { flag_left  = 1; p++; }
            else if (*p == '0') { flag_zero  = 1; p++; }
            else if (*p == '+') { flag_plus  = 1; p++; }
            else if (*p == ' ') { flag_space = 1; p++; }
            else if (*p == '#') { flag_hash  = 1; p++; }
            else break;
        }

        int width = 0;
        if (*p == '*') {
            width = va_arg(ap, int);
            if (width < 0) { flag_left = 1; width = -width; }
            p++;
        } else {
            while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        }

        int prec = -1;
        if (*p == '.') {
            p++; prec = 0;
            if (*p == '*') {
                prec = va_arg(ap, int);
                if (prec < 0) prec = -1;
                p++;
            } else {
                while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
            }
        }

        /* -2=hh -1=h 0=int 1=long 2=long long 3=size_t */
        int lmod = 0;
        if (*p == 'h')      { p++; lmod = -1; if (*p == 'h') { p++; lmod = -2; } }
        else if (*p == 'l') { p++; lmod = 1;  if (*p == 'l') { p++; lmod = 2; } }
        else if (*p == 'z') { p++; lmod = 3; }
        else if (*p == 'j') { p++; lmod = 2; }
        else if (*p == 't') { p++; lmod = 1; }

        char spec = *p;
        if (!spec) break;
        p++;

        if (spec == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int slen = (prec >= 0) ? (int)strnlen(s, (size_t)prec) : (int)strlen(s);
            int pad = (width > slen) ? width - slen : 0;
            if (!flag_left) for (int i = 0; i < pad; i++) EMIT(' ');
            for (int i = 0; i < slen; i++) EMIT(s[i]);
            if (flag_left)  for (int i = 0; i < pad; i++) EMIT(' ');
            continue;
        }
        if (spec == 'c') {
            char c = (char)va_arg(ap, int);
            int pad = (width > 1) ? width - 1 : 0;
            if (!flag_left) for (int i = 0; i < pad; i++) EMIT(' ');
            EMIT(c);
            if (flag_left)  for (int i = 0; i < pad; i++) EMIT(' ');
            continue;
        }
        if (spec == '%') { EMIT('%'); continue; }

        char tmp[72];
        int  val_len = 0;
        char sign_ch = 0;
        char prefix[3] = {0, 0, 0};

        switch (spec) {
        case 'd': case 'i': {
            long long n;
            switch (lmod) {
            case 2:  n = va_arg(ap, long long); break;
            case 1:  n = va_arg(ap, long);      break;
            case 3:  n = va_arg(ap, ssize_t);   break;
            case -1: n = (short)va_arg(ap, int);       break;
            case -2: n = (signed char)va_arg(ap, int); break;
            default: n = va_arg(ap, int);       break;
            }
            unsigned long long mag = n < 0 ? 0ULL - (unsigned long long)n : (unsigned long long)n;
            if (n < 0) sign_ch = '-'; else if (flag_plus) sign_ch = '+'; else if (flag_space) sign_ch = ' ';
            ulltoa(mag, tmp, 10, 0);
            val_len = (int)strlen(tmp);
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            unsigned long long n;
            switch (lmod) {
            case 2:  n = va_arg(ap, unsigned long long); break;
            case 1:  n = va_arg(ap, unsigned long);     break;
            case 3:  n = va_arg(ap, size_t);            break;
            case -1: n = (unsigned short)va_arg(ap, unsigned int);  break;
            case -2: n = (unsigned char)va_arg(ap, unsigned int);   break;
            default: n = va_arg(ap, unsigned int);      break;
            }
            int base = (spec == 'u') ? 10 : (spec == 'o') ? 8 : 16;
            ulltoa(n, tmp, base, spec == 'X');
            val_len = (int)strlen(tmp);
            if (flag_hash && spec == 'o' && tmp[0] != '0') { prefix[0] = '0'; }
            if (flag_hash && base == 16 && n != 0) { prefix[0] = '0'; prefix[1] = (spec == 'X') ? 'X' : 'x'; }
            break;
        }
        case 'p': {
            unsigned long long n = (unsigned long long)(uintptr_t)va_arg(ap, void *);
            ulltoa(n, tmp, 16, 0);
            val_len = (int)strlen(tmp);
            prefix[0] = '0'; prefix[1] = 'x';
            break;
        }
        default:
            EMIT('%'); EMIT(spec);
            continue;
        }

        int sign_w  = sign_ch ? 1 : 0;
        int pre_w   = (int)strlen(prefix);
        int zpad    = (prec >= 0 && prec > val_len) ? prec - val_len : 0;
        int content = sign_w + pre_w + zpad + val_len;
        int pad     = (width > content) ? width - content : 0;
        int use_zero = flag_zero && !flag_left && prec < 0;

        if (!flag_left && !use_zero) for (int i = 0; i < pad; i++) EMIT(' ');
        if (sign_ch) EMIT(sign_ch);
        for (int i = 0; prefix[i]; i++) EMIT(prefix[i]);
        if (use_zero) for (int i = 0; i < pad; i++) EMIT('0');
        for (int i = 0; i < zpad; i++) EMIT('0');
        for (int i = 0; i < val_len; i++) EMIT(tmp[i]);
        if (flag_left) for (int i = 0; i < pad; i++) EMIT(' ');
    }
#undef EMIT
    return total;
}

/* ---- snprintf family ---- */

struct sn_ctx { char *out; size_t rem; };

static void sn_emit(void *c, char ch) {
    struct sn_ctx *s = c;
    if (s->rem > 0) { *s->out++ = ch; s->rem--; }
}

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap) {
    struct sn_ctx s = { str, size ? size - 1 : 0 };
    int n = vformat(sn_emit, &s, fmt, ap);
    if (size) *s.out = '\0';
    return n;
}

int snprintf(char *str, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(str, size, fmt, ap);
    va_end(ap);
    return r;
}

int vsprintf(char *str, const char *fmt, va_list ap) { return vsnprintf(str, (size_t)INT_MAX, fmt, ap); }

int sprintf(char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(str, (size_t)INT_MAX, fmt, ap);
    va_end(ap);
    return r;
}

/* ============================================================
   <stdio.h>
   FILE = fd + optional buffer. One buffer serves reading or writing
   (never both at once). stdout/stderr are unbuffered (the console is
   the kernel's); files opened with fopen are fully buffered.
   stdin reads a whole edited line from $/dev/input/kbd0 at a time.
   ============================================================ */

#define F_READ   1
#define F_WRITE  2
#define F_STDIN  4    /* keyboard line editor, not a real fd read */

struct _FILE {
    int            fd;
    unsigned       flags;
    int            err, eof;
    int            ungot;           /* pushed-back char or -1 */
    int            bmode;           /* _IOFBF / _IOLBF / _IONBF */
    unsigned char *buf;
    size_t         bsize;
    int            ownbuf;
    size_t         rpos, rlen;      /* read buffer window */
    size_t         wlen;            /* pending bytes to write */
    struct _FILE  *next;            /* open-file list for fflush(NULL) */
};

static FILE g_stderr = { 2, F_WRITE, 0, 0, -1, _IONBF, NULL, 0, 0, 0, 0, 0, NULL };
static FILE g_stdout = { 1, F_WRITE, 0, 0, -1, _IONBF, NULL, 0, 0, 0, 0, 0, &g_stderr };
static FILE g_stdin  = { 0, F_READ | F_STDIN, 0, 0, -1, _IOFBF, NULL, 0, 0, 0, 0, 0, &g_stdout };

FILE *__libc_stdin(void)  { return &g_stdin; }
FILE *__libc_stdout(void) { return &g_stdout; }
FILE *__libc_stderr(void) { return &g_stderr; }

static FILE *g_files = &g_stdin;

static int write_all_fd(int fd, const void *b, size_t n) {
    const char *p = b;
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

static int alloc_buf(FILE *f) {
    if (f->buf) return 1;
    f->buf = malloc(BUFSIZ);
    if (!f->buf) return 0;
    f->bsize = BUFSIZ;
    f->ownbuf = 1;
    return 1;
}

static int flushw(FILE *f) {
    if (f->wlen) {
        size_t n = f->wlen;
        f->wlen = 0;
        if (write_all_fd(f->fd, f->buf, n) < 0) { f->err = 1; return EOF; }
    }
    return 0;
}

/* Get ready to write: drop any read-ahead (and give the bytes back to the fd). */
static int prepw(FILE *f) {
    if (!(f->flags & F_WRITE)) { errno = EBADF; f->err = 1; return -1; }
    if (f->rlen) {
        if (!(f->flags & F_STDIN) && f->rlen > f->rpos) lseek(f->fd, -(off_t)(f->rlen - f->rpos), SEEK_CUR);
        f->rpos = f->rlen = 0;
    }
    return 0;
}

static int putbyte(FILE *f, int c) {
    unsigned char ch = (unsigned char)c;
    if (prepw(f) < 0) return EOF;
    if (f->bmode == _IONBF || !alloc_buf(f)) {
        if (write_all_fd(f->fd, &ch, 1) < 0) { f->err = 1; return EOF; }
        return ch;
    }
    if (f->wlen >= f->bsize && flushw(f) < 0) return EOF;
    f->buf[f->wlen++] = ch;
    if (f->bmode == _IOLBF && ch == '\n' && flushw(f) < 0) return EOF;
    return ch;
}

static ssize_t tty_readline(char *buf, size_t maxlen, int echo_nl);

/* Refill the read buffer. Returns bytes available, 0 on EOF, -1 on error. */
static ssize_t fillr(FILE *f) {
    if (!(f->flags & F_READ)) { errno = EBADF; f->err = 1; return -1; }
    if (f->wlen && flushw(f) < 0) return -1;
    if (!alloc_buf(f)) { errno = ENOMEM; f->err = 1; return -1; }
    f->rpos = f->rlen = 0;

    if (f->flags & F_STDIN) {
        ssize_t n = tty_readline((char *)f->buf, f->bsize - 1, 1);
        if (n < 0) { f->err = 1; return -1; }
        f->buf[n] = '\n';
        f->rlen = (size_t)n + 1;
        return (ssize_t)f->rlen;
    }

    ssize_t r = read(f->fd, f->buf, f->bsize);
    if (r < 0) { f->err = 1; return -1; }
    if (r == 0) { f->eof = 1; return 0; }
    f->rlen = (size_t)r;
    return r;
}

FILE *fopen(const char *path, const char *mode) {
    if (!path || !mode) { errno = EINVAL; return NULL; }
    int plus = strchr(mode, '+') != NULL;
    int flags; unsigned fl;
    switch (mode[0]) {
    case 'r': flags = plus ? O_RDWR : O_RDONLY;                          fl = plus ? (F_READ | F_WRITE) : F_READ; break;
    case 'w': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;    fl = plus ? (F_READ | F_WRITE) : F_WRITE; break;
    case 'a': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;   fl = plus ? (F_READ | F_WRITE) : F_WRITE; break;
    default:  errno = EINVAL; return NULL;
    }

    int fd = open(path, flags, 0666);
    if (fd < 0) return NULL;

    FILE *f = malloc(sizeof *f);
    if (!f) { close(fd); errno = ENOMEM; return NULL; }
    memset(f, 0, sizeof *f);
    f->fd = fd; f->flags = fl; f->ungot = -1; f->bmode = _IOFBF;
    f->next = g_files; g_files = f;
    return f;
}

int fflush(FILE *f) {
    if (!f) {
        int r = 0;
        for (FILE *p = g_files; p; p = p->next) if (flushw(p) < 0) r = EOF;
        return r;
    }
    return flushw(f);
}

int fclose(FILE *f) {
    if (!f) return EOF;
    int r = fflush(f);
    if (close(f->fd) < 0) r = EOF;
    for (FILE **pp = &g_files; *pp; pp = &(*pp)->next) {
        if (*pp == f) { *pp = f->next; break; }
    }
    if (f->ownbuf) free(f->buf);
    if (f != &g_stdin && f != &g_stdout && f != &g_stderr) free(f);
    return r;
}

FILE *freopen(const char *path, const char *mode, FILE *f) {
    if (!path) { errno = ENOSYS; return NULL; }   /* changing the mode of an open stream is not supported */
    FILE *tmp = fopen(path, mode);
    if (!tmp) return NULL;
    fflush(f);
    if (f->fd != tmp->fd) {
        dup2(tmp->fd, f->fd);
        close(tmp->fd);
    }
    f->flags = tmp->flags;           /* also clears F_STDIN */
    f->err = f->eof = 0; f->ungot = -1;
    f->rpos = f->rlen = f->wlen = 0;
    /* drop the temporary FILE without closing the fd we just duplicated */
    for (FILE **pp = &g_files; *pp; pp = &(*pp)->next) if (*pp == tmp) { *pp = tmp->next; break; }
    free(tmp);
    return f;
}

int setvbuf(FILE *f, char *buf, int mode, size_t size) {
    if (mode != _IOFBF && mode != _IOLBF && mode != _IONBF) return -1;
    if (flushw(f) < 0) return -1;
    if (f->ownbuf) { free(f->buf); f->ownbuf = 0; }
    f->buf = NULL; f->bsize = 0; f->rpos = f->rlen = 0;
    f->bmode = mode;
    if (buf && size >= 2) { f->buf = (unsigned char *)buf; f->bsize = size; }
    return 0;
}

void setbuf(FILE *f, char *buf) { setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ); }

int remove(const char *path) {
    if (unlink(path) == 0) return 0;
    return rmdir(path);
}

int rename(const char *from, const char *to) { (void)from; (void)to; errno = ENOSYS; return -1; }
FILE *tmpfile(void) { errno = ENOSYS; return NULL; }
char *tmpnam(char *s) { (void)s; errno = ENOSYS; return NULL; }

/* ---- character I/O ---- */

int fgetc(FILE *f) {
    if (f->ungot >= 0) { int c = f->ungot; f->ungot = -1; return c; }
    if (f->rpos >= f->rlen && fillr(f) <= 0) return EOF;
    return f->buf[f->rpos++];
}

int ungetc(int c, FILE *f) {
    if (c == EOF || f->ungot >= 0) return EOF;
    f->ungot = (unsigned char)c;
    f->eof = 0;
    return c;
}

int fputc(int c, FILE *f) { return putbyte(f, c); }

int getc(FILE *f)          { return fgetc(f); }
int putc(int c, FILE *f)   { return putbyte(f, c); }
int getchar(void)          { return fgetc(stdin); }
int putchar(int c)         { return putbyte(stdout, c); }

char *fgets(char *s, int n, FILE *f) {
    if (n <= 0) return NULL;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) { if (i == 0) return NULL; break; }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    return s;
}

/* ---- block I/O ---- */

size_t fread(void *ptr, size_t size, size_t n, FILE *f) {
    if (!size || !n) return 0;
    if (n > SIZE_MAX / size) { errno = EINVAL; return 0; }
    size_t total = size * n, got = 0;
    unsigned char *out = ptr;

    if (f->ungot >= 0) { out[got++] = (unsigned char)f->ungot; f->ungot = -1; }
    while (got < total) {
        if (f->rpos >= f->rlen && fillr(f) <= 0) break;
        size_t take = f->rlen - f->rpos;
        if (take > total - got) take = total - got;
        memcpy(out + got, f->buf + f->rpos, take);
        f->rpos += take; got += take;
    }
    return got / size;
}

size_t fwrite(const void *ptr, size_t size, size_t n, FILE *f) {
    if (!size || !n) return 0;
    if (n > SIZE_MAX / size) { errno = EINVAL; return 0; }
    size_t total = size * n;
    const unsigned char *s = ptr;
    if (prepw(f) < 0) return 0;

    if (f->bmode == _IONBF || total >= BUFSIZ) {
        if (flushw(f) < 0) return 0;
        if (write_all_fd(f->fd, s, total) < 0) { f->err = 1; return 0; }
        return n;
    }
    for (size_t i = 0; i < total; i++)
        if (putbyte(f, s[i]) == EOF) return i / size;
    return n;
}

int fputs(const char *s, FILE *f) {
    size_t len = strlen(s);
    return fwrite(s, 1, len, f) == len ? 0 : EOF;
}

int puts(const char *s) {
    if (fputs(s, stdout) == EOF) return EOF;
    return putbyte(stdout, '\n') == EOF ? EOF : 0;
}

/* ---- positioning ---- */

int fseek(FILE *f, long off, int whence) {
    if (f->flags & F_STDIN) { errno = ESPIPE; return -1; }
    if (flushw(f) < 0) return -1;
    if (whence == SEEK_CUR) {
        off -= (long)(f->rlen - f->rpos);
        if (f->ungot >= 0) off--;
    }
    f->rpos = f->rlen = 0;
    f->ungot = -1;
    f->eof = 0;
    return lseek(f->fd, off, whence) < 0 ? -1 : 0;
}

long ftell(FILE *f) {
    if (f->flags & F_STDIN) { errno = ESPIPE; return -1; }
    long pos = lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0) return -1;
    pos -= (long)(f->rlen - f->rpos);
    pos += (long)f->wlen;
    if (f->ungot >= 0) pos--;
    return pos;
}

void rewind(FILE *f)                    { fseek(f, 0, SEEK_SET); f->err = 0; }
int  fgetpos(FILE *f, fpos_t *pos)      { long p = ftell(f); if (p < 0) return -1; *pos = p; return 0; }
int  fsetpos(FILE *f, const fpos_t *pos){ return fseek(f, *pos, SEEK_SET); }

void clearerr(FILE *f) { f->err = 0; f->eof = 0; }
int  feof(FILE *f)     { return f->eof; }
int  ferror(FILE *f)   { return f->err; }
int  fileno(FILE *f)   { return f->fd; }

void perror(const char *s) {
    if (s && *s) fprintf(stderr, "%s: %s\n", s, strerror(errno));
    else         fprintf(stderr, "%s\n", strerror(errno));
}

/* ---- printf to streams ---- */

struct f_ctx { FILE *f; size_t n; char buf[256]; };

static void f_emit(void *c, char ch) {
    struct f_ctx *x = c;
    if (x->n == sizeof x->buf) { fwrite(x->buf, 1, x->n, x->f); x->n = 0; }
    x->buf[x->n++] = ch;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    struct f_ctx x = { f, 0, {0} };
    int n = vformat(f_emit, &x, fmt, ap);
    if (x.n) fwrite(x.buf, 1, x.n, f);
    return f->err ? -1 : n;
}

int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

/* ============================================================
   scanf family
   Supports: d i u o x X p c s [set] n %, '*' suppression, width,
   length hh h l ll z j t. No floating point yet.
   ============================================================ */

typedef struct { const unsigned char *s; FILE *f; long nread; } scan_src;

static int sget(scan_src *in) {
    int c;
    if (in->f) c = fgetc(in->f);
    else       c = *in->s ? *in->s++ : EOF;
    if (c != EOF) in->nread++;
    return c;
}

static void sunget(scan_src *in, int c) {
    if (c == EOF) return;
    in->nread--;
    if (in->f) ungetc(c, in->f); else in->s--;
}

static void skip_ws(scan_src *in) {
    int c;
    do { c = sget(in); } while (c != EOF && isspace(c));
    sunget(in, c);
}

static void store_int(void *dst, int lmod, unsigned long long v) {
    switch (lmod) {
    case -2: *(unsigned char *)dst      = (unsigned char)v;      break;
    case -1: *(unsigned short *)dst     = (unsigned short)v;     break;
    case 1:  *(unsigned long *)dst      = (unsigned long)v;      break;
    case 2:  *(unsigned long long *)dst = v;                     break;
    case 3:  *(size_t *)dst             = (size_t)v;             break;
    default: *(unsigned int *)dst       = (unsigned int)v;       break;
    }
}

static int vscan(scan_src *in, const char *fmt, va_list ap) {
    int count = 0, c;
#define FAIL_INPUT() return count ? count : EOF

    for (const char *p = fmt; *p; p++) {
        if (isspace((unsigned char)*p)) { skip_ws(in); continue; }

        if (*p != '%') {
            c = sget(in);
            if (c == EOF) FAIL_INPUT();
            if (c != (unsigned char)*p) { sunget(in, c); return count; }
            continue;
        }
        p++;

        int suppress = 0;
        if (*p == '*') { suppress = 1; p++; }

        int width = 0;
        while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        if (width == 0) width = INT_MAX;

        int lmod = 0;
        if (*p == 'h')      { p++; lmod = -1; if (*p == 'h') { p++; lmod = -2; } }
        else if (*p == 'l') { p++; lmod = 1;  if (*p == 'l') { p++; lmod = 2; } }
        else if (*p == 'z') { p++; lmod = 3; }
        else if (*p == 'j') { p++; lmod = 2; }
        else if (*p == 't') { p++; lmod = 1; }

        char spec = *p;
        if (!spec) break;

        if (spec == '%') {
            skip_ws(in);
            c = sget(in);
            if (c == EOF) FAIL_INPUT();
            if (c != '%') { sunget(in, c); return count; }
            continue;
        }

        if (spec == 'n') {
            if (!suppress) store_int(va_arg(ap, void *), lmod, (unsigned long long)in->nread);
            continue;
        }

        if (spec == 'c') {
            int n = (width == INT_MAX) ? 1 : width;
            char *dst = suppress ? NULL : va_arg(ap, char *);
            for (int i = 0; i < n; i++) {
                c = sget(in);
                if (c == EOF) { if (i == 0) FAIL_INPUT(); break; }
                if (dst) dst[i] = (char)c;
            }
            if (dst) count++;
            continue;
        }

        if (spec == '[') {
            unsigned char tab[256];
            memset(tab, 0, sizeof tab);
            int neg = 0;
            p++;
            if (*p == '^') { neg = 1; p++; }
            const char *set_start = p;
            for (; *p && (*p != ']' || p == set_start); p++) {
                if (p[1] == '-' && p[2] && p[2] != ']') {
                    for (int ch = (unsigned char)p[0]; ch <= (unsigned char)p[2]; ch++) tab[ch] = 1;
                    p += 2;
                } else {
                    tab[(unsigned char)*p] = 1;
                }
            }
            char *dst = suppress ? NULL : va_arg(ap, char *);
            int n = 0;
            c = sget(in);
            while (c != EOF && n < width && (tab[c] ^ neg)) {
                if (dst) dst[n] = (char)c;
                n++;
                c = sget(in);
            }
            sunget(in, c);
            if (n == 0) { if (c == EOF) FAIL_INPUT(); return count; }
            if (dst) { dst[n] = '\0'; count++; }
            if (!*p) break;
            continue;
        }

        skip_ws(in);

        if (spec == 's') {
            c = sget(in);
            if (c == EOF) FAIL_INPUT();
            char *dst = suppress ? NULL : va_arg(ap, char *);
            int n = 0;
            while (c != EOF && !isspace(c) && n < width) {
                if (dst) dst[n] = (char)c;
                n++;
                c = sget(in);
            }
            sunget(in, c);
            if (dst) { dst[n] = '\0'; count++; }
            continue;
        }

        int is_signed = (spec == 'd' || spec == 'i');
        if (!(is_signed || spec == 'u' || spec == 'o' || spec == 'x' || spec == 'X' || spec == 'p'))
            return count;   /* unsupported (floats, ...) */

        int base = (spec == 'd' || spec == 'u') ? 10 : (spec == 'o') ? 8 : (spec == 'i') ? 10 : 16;
        int w = width, neg = 0;

        c = sget(in);
        if (c == '-' || c == '+') { neg = (c == '-'); w--; c = sget(in); }
        if (c == EOF) FAIL_INPUT();

        if ((spec == 'i' || base == 16) && c == '0' && w > 1) {
            int c2 = sget(in);
            if (c2 == 'x' || c2 == 'X') { base = 16; w -= 2; c = sget(in); }
            else { sunget(in, c2); if (spec == 'i') base = 8; }
        } else if (spec == 'i' && c == '0') {
            base = 8;
        }

        unsigned long long v = 0;
        int nd = 0;
        while (c != EOF && w > 0) {
            int d;
            if (c >= '0' && c <= '9')      d = c - '0';
            else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
            else break;
            if (d >= base) break;
            v = v * (unsigned)base + (unsigned)d;
            nd++; w--;
            c = sget(in);
        }
        sunget(in, c);
        if (nd == 0) { if (c == EOF) FAIL_INPUT(); return count; }

        if (!suppress) {
            void *dst = va_arg(ap, void *);
            if (neg) v = 0ULL - v;
            if (spec == 'p') *(void **)dst = (void *)(uintptr_t)v;
            else store_int(dst, lmod, v);
            count++;
        }
    }
#undef FAIL_INPUT
    return count;
}

int vsscanf(const char *s, const char *fmt, va_list ap) {
    scan_src in = { (const unsigned char *)s, NULL, 0 };
    return vscan(&in, fmt, ap);
}

int vfscanf(FILE *f, const char *fmt, va_list ap) {
    scan_src in = { NULL, f, 0 };
    return vscan(&in, fmt, ap);
}

int vscanf(const char *fmt, va_list ap) { return vfscanf(stdin, fmt, ap); }

int sscanf(const char *s, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}

int fscanf(FILE *f, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfscanf(f, fmt, ap);
    va_end(ap);
    return r;
}

int scanf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfscanf(stdin, fmt, ap);
    va_end(ap);
    return r;
}

/* ============================================================
   <time.h>
   ModuOS has no RTC syscall yet: time() is seconds since boot.
   gmtime/mktime/strftime are real calendar math, so they work as soon as
   time() returns a real epoch value.
   ============================================================ */

clock_t clock(void) { return (clock_t)time_ms(); }

time_t time(time_t *out) {
    time_t t = (time_t)(time_ms() / 1000);
    if (out) *out = t;
    return t;
}

static long long floordiv(long long a, long long b) {
    long long q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

static long long days_from_civil(long long y, int m, int d) {
    y -= m <= 2;
    long long era = floordiv(y, 400);
    long long yoe = y - era * 400;
    long long doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

struct tm *gmtime(const time_t *tp) {
    static struct tm r;
    long long t = *tp;
    long long days = floordiv(t, 86400);
    long long secs = t - days * 86400;

    r.tm_hour = (int)(secs / 3600);
    r.tm_min  = (int)((secs % 3600) / 60);
    r.tm_sec  = (int)(secs % 60);
    r.tm_wday = (int)(((days % 7) + 11) % 7);       /* 1970-01-01 was a Thursday */

    long long z   = days + 719468;
    long long era = floordiv(z, 146097);
    long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y   = yoe + era * 400;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp  = (5 * doy + 2) / 153;
    int d = (int)(doy - (153 * mp + 2) / 5 + 1);
    int m = (int)(mp < 10 ? mp + 3 : mp - 9);
    if (m <= 2) y++;

    static const short cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;

    r.tm_year  = (int)(y - 1900);
    r.tm_mon   = m - 1;
    r.tm_mday  = d;
    r.tm_yday  = cum[m - 1] + d - 1 + (leap && m > 2);
    r.tm_isdst = 0;
    return &r;
}

struct tm *localtime(const time_t *t) { return gmtime(t); }   /* no time zones */

time_t mktime(struct tm *tm) {
    long long y   = tm->tm_year + 1900LL;
    long long mon = tm->tm_mon;
    long long carry = floordiv(mon, 12);
    y += carry; mon -= carry * 12;
    long long days = days_from_civil(y, (int)mon + 1, 1) + (tm->tm_mday - 1);
    long long t = days * 86400 + tm->tm_hour * 3600LL + tm->tm_min * 60LL + tm->tm_sec;
    time_t tt = (time_t)t;
    *tm = *gmtime(&tt);
    return tt;
}

static const char *const wday_names[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const mon_names[12] = {"January", "February", "March", "April", "May", "June", "July",
                                          "August", "September", "October", "November", "December"};

char *asctime(const struct tm *tm) {
    static char buf[40];
    int wd = (tm->tm_wday >= 0 && tm->tm_wday < 7) ? tm->tm_wday : 0;
    int mo = (tm->tm_mon  >= 0 && tm->tm_mon  < 12) ? tm->tm_mon : 0;
    snprintf(buf, sizeof buf, "%.3s %.3s%3d %02d:%02d:%02d %d\n",
             wday_names[wd], mon_names[mo], tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900);
    return buf;
}

char *ctime(const time_t *t) { return asctime(localtime(t)); }

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm) {
    if (max == 0) return 0;
    size_t n = 0;
    char tmp[40];
    int wd = (tm->tm_wday >= 0 && tm->tm_wday < 7) ? tm->tm_wday : 0;
    int mo = (tm->tm_mon  >= 0 && tm->tm_mon  < 12) ? tm->tm_mon : 0;

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            if (n + 1 >= max) return 0;
            s[n++] = *fmt;
            continue;
        }
        fmt++;
        if (!*fmt) break;
        const char *piece = tmp;
        int h12 = tm->tm_hour % 12 ? tm->tm_hour % 12 : 12;
        switch (*fmt) {
        case 'Y': snprintf(tmp, sizeof tmp, "%d", tm->tm_year + 1900); break;
        case 'y': snprintf(tmp, sizeof tmp, "%02d", (tm->tm_year + 1900) % 100); break;
        case 'm': snprintf(tmp, sizeof tmp, "%02d", tm->tm_mon + 1); break;
        case 'd': snprintf(tmp, sizeof tmp, "%02d", tm->tm_mday); break;
        case 'e': snprintf(tmp, sizeof tmp, "%2d", tm->tm_mday); break;
        case 'H': snprintf(tmp, sizeof tmp, "%02d", tm->tm_hour); break;
        case 'I': snprintf(tmp, sizeof tmp, "%02d", h12); break;
        case 'M': snprintf(tmp, sizeof tmp, "%02d", tm->tm_min); break;
        case 'S': snprintf(tmp, sizeof tmp, "%02d", tm->tm_sec); break;
        case 'j': snprintf(tmp, sizeof tmp, "%03d", tm->tm_yday + 1); break;
        case 'w': snprintf(tmp, sizeof tmp, "%d", wd); break;
        case 'u': snprintf(tmp, sizeof tmp, "%d", wd ? wd : 7); break;
        case 'p': piece = tm->tm_hour < 12 ? "AM" : "PM"; break;
        case 'a': snprintf(tmp, sizeof tmp, "%.3s", wday_names[wd]); break;
        case 'A': piece = wday_names[wd]; break;
        case 'b': case 'h': snprintf(tmp, sizeof tmp, "%.3s", mon_names[mo]); break;
        case 'B': piece = mon_names[mo]; break;
        case 'F': snprintf(tmp, sizeof tmp, "%d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday); break;
        case 'T': snprintf(tmp, sizeof tmp, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
        case 'R': snprintf(tmp, sizeof tmp, "%02d:%02d", tm->tm_hour, tm->tm_min); break;
        case 'Z': piece = "UTC"; break;
        case 'z': piece = "+0000"; break;
        case 'n': piece = "\n"; break;
        case 't': piece = "\t"; break;
        case '%': piece = "%"; break;
        default:  tmp[0] = '%'; tmp[1] = *fmt; tmp[2] = '\0'; break;
        }
        size_t l = strlen(piece);
        if (n + l + 1 > max) return 0;
        memcpy(s + n, piece, l);
        n += l;
    }
    if (n >= max) return 0;
    s[n] = '\0';
    return n;
}

/* ============================================================
   <setjmp.h>  (x86-64: rbx rbp r12-r15 rsp rip)
   ============================================================ */

__asm__(
    ".pushsection .text\n"
    ".globl setjmp\n"
    ".type setjmp,@function\n"
    "setjmp:\n"
    "    movq %rbx, 0(%rdi)\n"
    "    movq %rbp, 8(%rdi)\n"
    "    movq %r12, 16(%rdi)\n"
    "    movq %r13, 24(%rdi)\n"
    "    movq %r14, 32(%rdi)\n"
    "    movq %r15, 40(%rdi)\n"
    "    leaq 8(%rsp), %rdx\n"
    "    movq %rdx, 48(%rdi)\n"
    "    movq (%rsp), %rdx\n"
    "    movq %rdx, 56(%rdi)\n"
    "    xorl %eax, %eax\n"
    "    ret\n"
    ".size setjmp, .-setjmp\n"
    ".globl longjmp\n"
    ".type longjmp,@function\n"
    "longjmp:\n"
    "    movl %esi, %eax\n"
    "    testl %eax, %eax\n"
    "    jnz 1f\n"
    "    movl $1, %eax\n"
    "1:\n"
    "    movq 0(%rdi), %rbx\n"
    "    movq 8(%rdi), %rbp\n"
    "    movq 16(%rdi), %r12\n"
    "    movq 24(%rdi), %r13\n"
    "    movq 32(%rdi), %r14\n"
    "    movq 40(%rdi), %r15\n"
    "    movq 48(%rdi), %rsp\n"
    "    jmp *56(%rdi)\n"
    ".size longjmp, .-longjmp\n"
    ".popsection\n"
);

/* ============================================================
   ModuOS extensions (declared in moduos.h)
   ============================================================ */

void yield(void) { syscall(SYS_YIELD, 0, 0, 0); }

void puts_raw(const char *s) {
    if (!s) s = " ";
    write_all_fd(STDOUT_FILENO, s, strlen(s));
}

void putc_raw(char c) { write_all_fd(STDOUT_FILENO, &c, 1); }

ssize_t input_read(char *buf, size_t max_len) {
    if (!buf || max_len == 0 || max_len > INT_MAX) return -1;
    return syscall(SYS_INPUT, (long)buf, (long)max_len, 0);
}

void input_flush_events(void) {
    int efd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    if (efd >= 0) {
        unsigned char buf[64];
        for (;;) {
            ssize_t n = read(efd, buf, sizeof buf);
            if (n <= 0) break;
        }
        close(efd);
    }
}

static int have_ctty_cached = -1;

void input_flush(void) {
    if (have_ctty_cached != 0) {
        int tfd = open("$/dev/tty", O_RDWR, 0);
        if (tfd >= 0) {
            unsigned char io[8 + 96] = {0};
            io[0] = 0x0B; io[1] = 0x54;   /* TTY_IOC_FLUSH_IN = 0x540B (LE) */
            invoke(tfd, io, sizeof io, io, sizeof io);
            close(tfd);
        }
    }
    int kfd = open("$/dev/input/kbd0", O_RDONLY | O_NONBLOCK, 0);
    if (kfd >= 0) {
        char c;
        while (read(kfd, &c, 1) == 1) {}
        close(kfd);
    }
    input_flush_events();
}

/* Line input. If the caller has a controlling TTY, the kernel line discipline
 * does editing + echo and we just read(0). Otherwise (early boot, no TTY layer)
 * fall back to the old kbd0 editor. Returns length (no newline), or -1. */
static int have_ctty(void) {
    if (have_ctty_cached < 0) {
        int fd = open("$/dev/tty", O_RDWR, 0);
        have_ctty_cached = (fd >= 0);
        if (fd >= 0) close(fd);
    }
    return have_ctty_cached;
}

static ssize_t tty_readline_kbd0(char *buf, size_t maxlen, int echo_nl);

static ssize_t tty_readline(char *buf, size_t maxlen, int echo_nl) {
    if (!buf || maxlen == 0 || maxlen > 0x7FFFFFFF) return -1;
    if (!have_ctty()) return tty_readline_kbd0(buf, maxlen, echo_nl);

    size_t n = 0;
    for (;;) {
        char c;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r == 0) break;               /* EOF (^D) */
        if (r < 0) { if (n) break; buf[0] = 0; return -1; }
        if (c == '\n') break;           /* canonical mode delivers whole lines */
        if (n + 1 < maxlen) buf[n++] = c;
    }
    buf[n] = 0;
    (void)echo_nl;                       /* the tty already echoed the newline */
    return (ssize_t)n;
}

static ssize_t tty_readline_kbd0(char *buf, size_t maxlen, int echo_nl) {
    int fd = open("$/dev/input/kbd0", O_RDONLY, 0);
    if (fd < 0) { buf[0] = 0; return -1; }

    size_t n = 0, safe_max = maxlen - 1;
    for (;;) {
        char c;
        if (read(fd, &c, 1) != 1) { yield(); continue; }
        if (c == '\r') continue;

        if (c == '\n') {
            buf[n] = 0;
            close(fd);
            input_flush_events();
            if (echo_nl) puts_raw("\n");
            return (ssize_t)n;
        }
        if ((c == '\b' || c == 127) && n > 0) {
            n--;
            buf[n] = 0;
            puts_raw("\b");
            continue;
        }
        if (n < safe_max && c >= 32 && c < 127) {
            buf[n++] = c;
            buf[n] = 0;
            char tmp[2] = { c, 0 };
            puts_raw(tmp);
        }
    }
}

ssize_t input_line_to_buffer(char *buf, size_t maxlen) { return tty_readline(buf, maxlen, 0); }

char *input(void) {
    static char input_buf[256];
    input_line_to_buffer(input_buf, sizeof input_buf);
    return input_buf;   /* empty string on error */
}

/* SYS_SSTATS2 is deprecated in the kernel; read the DevFS node instead. */
int get_system_info_u(md64api_sysinfo_data_u *out) {
    if (!out) return -1;
    int fd = open("$/dev/md64api/sysinfo", O_RDONLY, 0);
    if (fd < 0) return -2;
    ssize_t r = read(fd, out, sizeof(*out));
    close(fd);
    return (r == (ssize_t)sizeof(*out)) ? 0 : -3;
}

int get_process_list(md_proclist_entry_u *out, size_t out_count) {
    return (int)syscall(SYS_PROCLIST, (long)out, (long)(out_count * sizeof(*out)), 0);
}

int md64api_get_pid_info(uint32_t pid, md64api_pid_info_u *out) {
    return (int)syscall(SYS_PIDINFO, (long)pid, (long)out, (long)sizeof(*out));
}

int md64api_grp_get_video0_info(md64api_grp_video_info_t *out) {
    if (!out) return -1;
    int fd = open(MD64API_GRP_DEFAULT_DEVICE, O_RDONLY, 0);
    if (fd < 0) return -2;
    ssize_t r = read(fd, out, sizeof(*out));
    close(fd);
    return (r == (ssize_t)sizeof(*out)) ? 0 : -3;
}

uint64_t time_ms(void) { return (uint64_t)syscall(SYS_TIME, 0, 0, 0); }
int fs_trace_set(int enabled) { return (int)syscall(SYS_FS_TRACE, (long)enabled, 0, 0); }

void *sbrk(intptr_t inc) { return (void *)syscall(SYS_SBRK, inc, 0, 0); }

void *mmap(void *addr, size_t length, int prot, int flags, int fd) {
    return (void *)syscall5(SYS_MMAP, (long)addr, (long)length, (long)prot, (long)flags, (long)fd);
}
int munmap(void *addr, size_t length) { return (int)syscall(SYS_MUNMAP, (long)addr, (long)length, 0); }

int shm_open(const char *name, int oflags, uint32_t mode, uint64_t size) {
    return (int)syscall4(SYS_SHM_OPEN, (long)name, (long)oflags, (long)mode, (long)size);
}
int shm_unlink(const char *name) { return (int)syscall(SYS_SHM_UNLINK, (long)name, 0, 0); }

void *dev_mmap(int fd, void *hint, size_t length, int prot, int flags, uint64_t offset) {
    long ret = syscall6(SYS_DEV_MMAP, fd, (long)hint, (long)length, prot, flags, (long)offset);
    if (ret == -1L) { errno = ENOMEM; return MAP_FAILED; }
    return (void *)(uintptr_t)ret;
}

int exec(const char *cmdline) { return (int)syscall(SYS_EXEC, (long)cmdline, 0, 0); }

int envlist(char *buf, size_t buflen) {
    return (int)sysret(syscall(SYS_ENVLIST, (long)buf, (long)buflen, 0));
}
int envlist2(size_t *offset, char *buf, size_t buflen) {
    return (int)sysret(syscall(SYS_ENVLIST2, (long)offset, (long)buf, (long)buflen));
}

int setuid_of_other_process(int uid, uint32_t target) {
    return (int)syscall(SYS_SETUID_OF_OTHER_PROCESS, (long)target, (long)uid, 0);
}
int getuid_of_other_process(uint32_t target) {
    return (int)syscall(SYS_GETUID_OF_OTHER_PROCESS, (long)target, 0, 0);
}
int fd_inject(int pid, int fd, void *fd_obj) {
    return (int)syscall(SYS_FD_INJECT, (long)pid, (long)fd, (long)fd_obj);
}

ssize_t invoke(int fd, const void *in_buf, size_t in_size, void *out_buf, size_t out_size) {
    long ret = syscall5(SYS_INVOKE, fd, (long)in_buf, (long)in_size, (long)out_buf, (long)out_size);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (ssize_t)ret;
}

int opendir(const char *path) { return (int)syscall(SYS_OPENDIR, (long)path, 0, 0); }
int readdir(int fd, char *name_buf, size_t buf_size, int *is_dir, uint32_t *size) {
    return (int)syscall5(SYS_READDIR, fd, (long)name_buf, (long)buf_size, (long)is_dir, (long)size);
}
int closedir(int fd) { return (int)syscall(SYS_CLOSEDIR, fd, 0, 0); }
int mkdir(const char *path) { return (int)syscall(SYS_MKDIR, (long)path, 0, 0); }
int stat(const char *path, fs_file_info_t *out_info) {
    return (int)syscall(SYS_STAT, (long)path, (long)out_info, (long)sizeof(*out_info));
}

int vfs_mkfs(const vfs_mkfs_req_t *req)  { return (int)syscall(SYS_VFS_MKFS, (long)req, 0, 0); }
int vfs_getpart(const vfs_part_req_t *req, vfs_part_info_t *out) {
    return (int)syscall(SYS_VFS_GETPART, (long)req, (long)out, 0);
}
int vfs_mbrinit(const vfs_mbrinit_req_t *req) { return (int)syscall(SYS_VFS_MBRINIT, (long)req, 0, 0); }
int userfs_register(const userfs_user_node_t *node) { return (int)syscall(SYS_USERFS_REGISTER, (long)node, 0, 0); }

int userfs_register_path(const char *path, uint32_t perms) {
    userfs_user_node_t node;
    memset(&node, 0, sizeof node);
    node.path = path;
    node.owner_id = "userland";
    node.perms = perms;
    return userfs_register(&node);
}

int vdrive_write(uint8_t id, uint64_t lba, uint32_t count, const void *buffer) {
    return (int)syscall4(SYS_VDRIVE_WRITE, id, (long)lba, count, (long)buffer);
}
int vdrive_write_sector(uint8_t id, uint64_t lba, const void *buffer) {
    return (int)syscall(SYS_VDRIVE_WRITE_SECTOR, id, (long)lba, (long)buffer);
}
int vdrive_read(uint8_t id, uint64_t lba, uint32_t count, void *buffer) {
    return (int)syscall4(SYS_VDRIVE_READ, id, (long)lba, count, (long)buffer);
}
int vdrive_read_sector(uint8_t id, uint64_t lba, void *buffer) {
    return (int)syscall(SYS_VDRIVE_READ_SECTOR, id, (long)lba, (long)buffer);
}

int mount_drive(int vdrive_id, uint32_t partition_lba, int fs_type) {
    return (int)syscall(SYS_MOUNT, vdrive_id, (long)partition_lba, fs_type);
}
int unmount_slot(int slot) { return (int)syscall(SYS_UNMOUNT, slot, 0, 0); }
int list_mounts(fs_mount_info_t *buf, size_t max_entries) {
    return (int)syscall(SYS_MOUNTS, (long)buf, (long)max_entries, 0);
}
