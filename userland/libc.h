// libc.h

#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Syscall numbers
#define SYS_EXIT        0
#define SYS_FORK        1
#define SYS_READ        2
#define SYS_WRITE       3       // NEW: write(str)
#define SYS_OPEN        4
#define SYS_CLOSE       5
#define SYS_WAIT        6
#define SYS_EXEC        7
#define SYS_GETPID      8
#define SYS_GETPPID     9
#define SYS_SLEEP       10
#define SYS_YIELD       11
#define SYS_MALLOC      12
#define SYS_FREE        13
#define SYS_SBRK        14
#define SYS_KILL        15
#define SYS_TIME        16
#define SYS_CHDIR       17 
#define SYS_GETCWD      18
#define SYS_STAT        19
#define SYS_MKDIR       20
#define SYS_RMDIR       21
#define SYS_UNLINK      22
#define SYS_INPUT       23
#define SYS_WRITEFILE   30     // NEW: fd-based writing

// File descriptor constants
#define STDIN_FILENO    0
#define STDOUT_FILENO   1
#define STDERR_FILENO   2

// Open flags
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0100
#define O_TRUNC     0x0200
#define O_APPEND    0x0400

// Type definitions
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef long ssize_t;
#endif

// Syscall wrapper (3 args max)
static inline long syscall(long num, long arg1, long arg2, long arg3) {
    long ret;
    __asm__ volatile (
        "mov %1, %%rax\n"
        "mov %2, %%rbx\n"
        "mov %3, %%rcx\n"
        "mov %4, %%rdx\n"
        "int $0x80\n"
        "mov %%rax, %0"
        : "=r"(ret)
        : "r"(num), "r"(arg1), "r"(arg2), "r"(arg3)
        : "rax", "rbx", "rcx", "rdx", "memory"
    );
    return ret;
}

/* ============================================================
   BASIC OUTPUT (now matches new SYS_WRITE)
   ============================================================ */

// Writes a single character to VGA
static inline void putc(char c) {
    char tmp[2] = {c, 0};
    syscall(SYS_WRITE, (long)tmp, 0, 0);
}

// Writes a null-terminated string to VGA
static inline void puts_raw(const char *s) {
    syscall(SYS_WRITE, (long)s, 0, 0);
}

// Write string + newline
static inline void puts(const char *s) {
    puts_raw(s);
    putc('\n');
}

static inline char* input() {
    return syscall(SYS_INPUT, 0, 0, 0);
}

/* ============================================================
   FILE I/O OPERATIONS
   ============================================================ */

// Open a file - returns file descriptor or -1 on error
static inline int open(const char *pathname, int flags, int mode) {
    return (int)syscall(SYS_OPEN, (long)pathname, flags, mode);
}

// Close a file descriptor
static inline int close(int fd) {
    return (int)syscall(SYS_CLOSE, fd, 0, 0);
}

// Read from a file descriptor
static inline ssize_t read(int fd, void *buf, size_t count) {
    return (ssize_t)syscall(SYS_READ, fd, (long)buf, count);
}

// Write to a file descriptor (binary safe)
static inline ssize_t write(int fd, const void *buf, size_t count) {
    return (ssize_t)syscall(SYS_WRITEFILE, fd, (long)buf, count);
}

/* ============================================================
   PRINTING UTILITIES
   ============================================================ */

static void print_uint(unsigned int n, int base, int upper) {
    char buf[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (n == 0) {
        putc('0');
        return;
    }

    int i = 0;
    while (n > 0) {
        buf[i++] = digits[n % base];
        n /= base;
    }

    while (i > 0)
        putc(buf[--i]);
}

static void print_int(int n) {
    if (n < 0) {
        putc('-');
        n = -n;
    }
    print_uint((unsigned)n, 10, 0);
}

static void print_ulong(unsigned long n, int base, int upper) {
    char buf[64];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (n == 0) {
        putc('0');
        return;
    }

    int i = 0;
    while (n > 0) {
        buf[i++] = digits[n % base];
        n /= base;
    }

    while (i > 0)
        putc(buf[--i]);
}

static void print_long(long n) {
    if (n < 0) {
        putc('-');
        n = -n;
    }
    print_ulong((unsigned long)n, 10, 0);
}

/* ============================================================
   printf()
   ============================================================ */

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    while (*fmt) {
        if (*fmt == '%') {
            fmt++;

            int longmod = 0;
            if (*fmt == 'l') {   // handle 'l' modifier
                longmod = 1;
                fmt++;
            }

            switch (*fmt) {
                case 'c':
                    putc((char)va_arg(ap, int));
                    break;

                case 's':
                    puts_raw(va_arg(ap, const char*));
                    break;

                case 'd':
                case 'i':
                    if (longmod)
                        print_long(va_arg(ap, long));
                    else
                        print_int(va_arg(ap, int));
                    break;

                case 'u':
                    if (longmod)
                        print_ulong(va_arg(ap, unsigned long), 10, 0);
                    else
                        print_uint(va_arg(ap, unsigned int), 10, 0);
                    break;

                case 'x':
                    if (longmod)
                        print_ulong(va_arg(ap, unsigned long), 16, 0);
                    else
                        print_uint(va_arg(ap, unsigned int), 16, 0);
                    break;

                case 'X':
                    if (longmod)
                        print_ulong(va_arg(ap, unsigned long), 16, 1);
                    else
                        print_uint(va_arg(ap, unsigned int), 16, 1);
                    break;

                case '%':
                    putc('%');
                    break;

                default:
                    putc('%');
                    putc(*fmt);
            }

            fmt++;
        } else {
            putc(*fmt++);
        }
    }

    va_end(ap);
    return 0;
}


/* ============================================================
   MEMORY FUNCTIONS
   ============================================================ */

static inline void* malloc(size_t size) {
    return (void*)syscall(SYS_MALLOC, size, 0, 0);
}

static inline void free(void *ptr) {
    syscall(SYS_FREE, (long)ptr, 0, 0);
}

static inline void* sbrk(intptr_t inc) {
    return (void*)syscall(SYS_SBRK, inc, 0, 0);
}

/* ============================================================
   PROCESS FUNCTIONS
   ============================================================ */

static inline void exit(int status) {
    syscall(SYS_EXIT, status, 0, 0);
}

static inline void exec(const char *str) {
    syscall(SYS_EXEC, str, 0 , 0);
}

static inline int getpid(void) {
    return (int)syscall(SYS_GETPID, 0, 0, 0);
}

static inline int getppid(void) {
    return (int)syscall(SYS_GETPPID, 0, 0, 0);
}

static inline void sleep(unsigned int sec) {
    syscall(SYS_SLEEP, sec, 0, 0);
}

static inline void yield(void) {
    syscall(SYS_YIELD, 0, 0, 0);
}

static inline int kill(int pid, int sig) {
    return (int)syscall(SYS_KILL, pid, sig, 0);
}

int md_main(long argc, char** argv);

void _start(long argc, char** argv)
{
    // ModuOS start wrapper / ABI
    int mdm = md_main(argc, argv);
    exit(mdm);
}