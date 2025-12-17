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
#define SYS_SSTATS      24      // System statistics
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

typedef const char* string;

/* System Information Structure */
typedef struct md64api_sysinfo_data
{
    /* --- Memory Info --- */
    uint64_t sys_available_ram;     /* Available RAM in MB */
    uint64_t sys_total_ram;         /* Total system RAM in MB */

    /* --- OS / Kernel Info --- */
    int SystemVersion;              /* OS major version */
    int KernelVersion;              /* Kernel version number */
    string KernelVendor;            /* NTSoftware / New Technologies Software */
    string os_name;                 /* ModuOS */
    string os_arch;                 /* AMD64 only (ARM version not implemented) */

    /* --- System Identity --- */
    string pcname;                  /* Host / computer name */
    string username;                /* Current user */
    string domain;                  /* Domain / workgroup */
    string kconsole;                /* VGA console / VBE text console */

    /* --- CPU Info --- */
    string cpu;                     /* Vendor string: GenuineIntel, AuthenticAMD, etc */
    string cpu_manufacturer;        /* Intel / AMD / etc */
    string cpu_model;               /* Core i5-13600K, Ryzen 7 5800X, etc */
    int cpu_cores;                  /* Physical cores */
    int cpu_threads;                /* Logical processors */
    int cpu_hyperthreading_enabled; /* 1 = enabled, 0 = disabled */
    int cpu_base_mhz;               /* Base clock in MHz */
    int cpu_max_mhz;                /* Max turbo clock in MHz */
    int cpu_cache_l1_kb;            /* L1 cache size */
    int cpu_cache_l2_kb;            /* L2 cache size */
    int cpu_cache_l3_kb;            /* L3 cache size */
    string cpu_flags;               /* SSE, SSE2, AVX, AVX2, AES, etc */

    /* --- Virtualization Info --- */
    int is_virtual_machine;         /* 1 = VM detected, 0 = physical */
    string virtualization_vendor;   /* VMware, VirtualBox, KVM, Hyper-V, etc */

    /* --- GPU Info --- */
    string gpu_name;                /* GPU model */
    int gpu_vram_mb;                /* VRAM in MB */

    /* --- Storage Info --- */
    uint64_t storage_total_mb;      /* Total storage space in MB */
    uint64_t storage_free_mb;       /* Free storage space in MB */
    string primary_disk_model;      /* Disk model name */

    /* --- Firmware / BIOS --- */
    string bios_vendor;             /* AMI, Phoenix, UEFI vendor */
    string bios_version;            /* BIOS/UEFI version */
    string motherboard_model;       /* Motherboard identifier */

    /* --- Security Features --- */
    int secure_boot_enabled;        /* 1 = Secure Boot enabled */
    int tpm_version;                /* 0 = none, 1.2 = 1, 2.0 = 2 */

} md64api_sysinfo_data;

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
    return (char*)syscall(SYS_INPUT, 0, 0, 0);
}

/* ============================================================
   SYSTEM INFO
   ============================================================ */

static inline md64api_sysinfo_data* get_system_info(void) {
    return (md64api_sysinfo_data*)syscall(SYS_SSTATS, 0, 0, 0);
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

static void print_ulonglong(unsigned long long n, int base, int upper) {
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
            int longlongmod = 0;
            
            if (*fmt == 'l') {
                longmod = 1;
                fmt++;
                if (*fmt == 'l') {  // 'll' for long long
                    longlongmod = 1;
                    fmt++;
                }
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
                    if (longlongmod) {
                        long long n = va_arg(ap, long long);
                        if (n < 0) {
                            putc('-');
                            n = -n;
                        }
                        print_ulonglong((unsigned long long)n, 10, 0);
                    } else if (longmod) {
                        print_long(va_arg(ap, long));
                    } else {
                        print_int(va_arg(ap, int));
                    }
                    break;

                case 'u':
                    if (longlongmod)
                        print_ulonglong(va_arg(ap, unsigned long long), 10, 0);
                    else if (longmod)
                        print_ulong(va_arg(ap, unsigned long), 10, 0);
                    else
                        print_uint(va_arg(ap, unsigned int), 10, 0);
                    break;

                case 'x':
                    if (longlongmod)
                        print_ulonglong(va_arg(ap, unsigned long long), 16, 0);
                    else if (longmod)
                        print_ulong(va_arg(ap, unsigned long), 16, 0);
                    else
                        print_uint(va_arg(ap, unsigned int), 16, 0);
                    break;

                case 'X':
                    if (longlongmod)
                        print_ulonglong(va_arg(ap, unsigned long long), 16, 1);
                    else if (longmod)
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

    if (mdm) {
        exit(mdm);
    } else {
        exit(0);
    }
    
}