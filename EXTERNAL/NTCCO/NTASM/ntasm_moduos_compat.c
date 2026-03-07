/*
 * ntasm_moduos_compat.c
 * Compatibility shim providing POSIX/C99 functions that NTASM uses
 * but ModuOS libc.h does not expose, plus the md_main entry wrapper.
 *
 * Build with the other ntasm sources using x86_64-elf-gcc and link
 * against userland/user.ld to produce ntasm.sqr.
 */

/*
 * Pull in the ModuOS libc — must come first so _start and syscalls
 * are defined before anything else.
 */
#include "../../userland/libc.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* =========================================================
 * Forward declarations for NTASM's public API
 * ========================================================= */
int ntasm_main(int argc, char **argv);   /* defined in ntasm.c */

/* =========================================================
 * md_main — ModuOS entry convention
 * ========================================================= */
int md_main(long argc, char **argv) {
    return ntasm_main((int)argc, argv);
}

/* =========================================================
 * FILE* stdio emulation
 * We map FILE* to a small struct wrapping a raw fd.
 * This is intentionally minimal: NTASM only needs fopen/fwrite/fclose
 * plus stdout/stderr (fputs/fprintf).
 * ========================================================= */
typedef struct _MFILE {
    int  fd;
    int  is_err;   /* 1 = stderr, suppress in release */
    char buf[256]; /* tiny write buffer — flushed on newline / fclose */
    int  buflen;
} MFILE;

static MFILE _stdout_f = { 1, 0, {0}, 0 };
static MFILE _stderr_f = { 2, 1, {0}, 0 };

FILE *stdout = (FILE *)&_stdout_f;
FILE *stderr = (FILE *)&_stderr_f;

static void mfile_flush(MFILE *mf) {
    if (mf->buflen > 0) {
        sys_writefile_raw(mf->fd, mf->buf, mf->buflen);
        mf->buflen = 0;
    }
}

FILE *fopen(const char *path, const char *mode) {
    int flags = 0;
    int creat = 0;
    if (mode[0] == 'r') flags = O_RDONLY;
    if (mode[0] == 'w') { flags = O_WRONLY | O_CREAT; creat = 1; }
    if (mode[0] == 'a') { flags = O_WRONLY | O_CREAT; creat = 1; }
    (void)creat;
    int fd = open(path, flags, 0644);
    if (fd < 0) return NULL;
    MFILE *mf = malloc(sizeof(MFILE));
    if (!mf) { close(fd); return NULL; }
    mf->fd     = fd;
    mf->is_err = 0;
    mf->buflen = 0;
    return (FILE *)mf;
}

int fclose(FILE *f) {
    MFILE *mf = (MFILE *)f;
    mfile_flush(mf);
    if (mf != &_stdout_f && mf != &_stderr_f) {
        close(mf->fd);
        free(mf);
    }
    return 0;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f) {
    MFILE *mf = (MFILE *)f;
    size_t total = size * nmemb;
    mfile_flush(mf);
    sys_writefile_raw(mf->fd, (const char *)ptr, (int)total);
    return nmemb;
}

int fputc(int c, FILE *f) {
    MFILE *mf = (MFILE *)f;
    char ch = (char)c;
    sys_writefile_raw(mf->fd, &ch, 1);
    return (unsigned char)c;
}

int fputs(const char *s, FILE *f) {
    MFILE *mf = (MFILE *)f;
    mfile_flush(mf);
    sys_writefile_raw(mf->fd, s, strlen(s));
    return 0;
}

/* Minimal vfprintf — delegates to our printf infrastructure */
static void _write_fd(int fd, const char *s, int len) {
    sys_writefile_raw(fd, s, len);
}

static void _fmt_uint(int fd, unsigned long long v, int base, int upper) {
    char buf[32]; int i = 31;
    buf[i] = 0;
    if (v == 0) { buf[--i] = '0'; }
    while (v && i > 0) {
        int d = v % base;
        buf[--i] = d < 10 ? '0'+d : (upper ? 'A' : 'a') + d - 10;
        v /= base;
    }
    _write_fd(fd, buf+i, 31-i);
}

int fprintf(FILE *f, const char *fmt, ...) {
    MFILE *mf = (MFILE *)f;
    va_list ap; va_start(ap, fmt);
    const char *p = fmt;
    while (*p) {
        if (*p != '%') {
            const char *s = p;
            while (*p && *p != '%') p++;
            _write_fd(mf->fd, s, (int)(p-s));
            continue;
        }
        p++; /* skip % */
        int lng = 0;
        if (*p == 'l') { lng++; p++; }
        if (*p == 'l') { lng++; p++; }
        switch (*p++) {
        case 'd': case 'i': {
            long long v = lng >= 2 ? va_arg(ap, long long) :
                          lng == 1 ? va_arg(ap, long)      :
                                     va_arg(ap, int);
            if (v < 0) { _write_fd(mf->fd, "-", 1); v = -v; }
            _fmt_uint(mf->fd, (unsigned long long)v, 10, 0);
            break;
        }
        case 'u': {
            unsigned long long v = lng >= 2 ? va_arg(ap, unsigned long long) :
                                   lng == 1 ? va_arg(ap, unsigned long)      :
                                              va_arg(ap, unsigned int);
            _fmt_uint(mf->fd, v, 10, 0);
            break;
        }
        case 'x': { unsigned long long v = va_arg(ap, unsigned long long); _fmt_uint(mf->fd,v,16,0); break; }
        case 'X': { unsigned long long v = va_arg(ap, unsigned long long); _fmt_uint(mf->fd,v,16,1); break; }
        case 's': { const char *s = va_arg(ap, const char *); if (!s) s="(null)"; _write_fd(mf->fd,s,strlen(s)); break; }
        case 'c': { char c = (char)va_arg(ap, int); _write_fd(mf->fd,&c,1); break; }
        case '%': _write_fd(mf->fd,"%",1); break;
        default:  break;
        }
    }
    va_end(ap);
    return 0;
}

/* =========================================================
 * strcasecmp / strncasecmp
 * ========================================================= */
int strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncasecmp(const char *a, const char *b, size_t n) {
    while (n-- && *a && *b) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    if (n == (size_t)-1) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

/* =========================================================
 * strtoll / strtoull / strtol
 * ========================================================= */
long long strtoll(const char *s, char **end, int base) {
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    if (base == 0) {
        if (s[0]=='0' && (s[1]=='x'||s[1]=='X')) { base=16; s+=2; }
        else if (s[0]=='0') base=8;
        else base=10;
    } else if (base==16 && s[0]=='0' && (s[1]=='x'||s[1]=='X')) s+=2;
    unsigned long long v = 0;
    while (*s) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        s++;
    }
    if (end) *end = (char *)s;
    return neg ? -(long long)v : (long long)v;
}

unsigned long long strtoull(const char *s, char **end, int base) {
    return (unsigned long long)strtoll(s, end, base);
}

long strtol(const char *s, char **end, int base) {
    return (long)strtoll(s, end, base);
}

/* =========================================================
 * rename ntasm's main so it doesn't clash with md_main
 * We use a #define in the Makefile to redirect:
 *   -Dmain=ntasm_main
 * ========================================================= */
