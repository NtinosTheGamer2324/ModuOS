// libc.h
#include "errno.h"

/* Linux-ish errno variable (per-process). Since ModuOS user programs are
 * typically single translation units, a header-defined static is sufficient.
 */
static int errno;

#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

/* Local string helpers (strlen, strcmp, etc.) */
#include "string.h"

// Syscall numbers (shared with kernel)
#include "../include/moduos/kernel/syscall/syscall_numbers.h"
// MD64API (userland-visible kernel interfaces)
#include "../include/moduos/kernel/md64api_grp.h"
#include "../include/moduos/kernel/md64api_user.h"
#include "../include/moduos/fs/userfs_user_api.h"
// SYS_WRITEFILE is provided by syscall_numbers.h

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
#define O_NONBLOCK  0x0800

// Type definitions
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef long ssize_t;
#endif

typedef const char* string;

/* System Information Structure */

// ---------------- MD64API SysInfo ----------------

#include "../include/moduos/kernel/md64api_user.h"

/* Legacy pointer-based struct kept for older apps (unsafe in ring3).
 * Prefer md64api_sysinfo_data_u + SYS_SSTATS2.
 */

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

// Syscall wrapper (up to 3 args)
// ABI: rax=syscall_num, rdi=arg1, rsi=arg2, rdx=arg3
static inline long syscall(long num, long arg1, long arg2, long arg3) {
    long ret;
    __asm__ volatile (
        "mov %1, %%rax\n"
        "mov %2, %%rdi\n"
        "mov %3, %%rsi\n"
        "mov %4, %%rdx\n"
        "syscall\n"
        "mov %%rax, %0"
        : "=r"(ret)
        : "r"(num), "r"(arg1), "r"(arg2), "r"(arg3)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );
    return ret;
}
/*
 * 4-arg syscall wrapper
 * Kernel ABI (see src/arch/AMD64/syscall/syscall_entry.asm):
 *   rax=syscall_num, rdi=arg1, rsi=arg2, rdx=arg3, r10=arg4
 */
static inline long syscall4(long num, long arg1, long arg2, long arg3, long arg4) {
    long ret;
    __asm__ volatile (
        "mov %1, %%rax\n"
        "mov %2, %%rdi\n"
        "mov %3, %%rsi\n"
        "mov %4, %%rdx\n"
        "mov %5, %%r10\n"
        "syscall\n"
        "mov %%rax, %0"
        : "=r"(ret)
        : "r"(num), "r"(arg1), "r"(arg2), "r"(arg3), "r"(arg4)
        : "rax", "rdi", "rsi", "rdx", "r10", "rcx", "r11", "memory"
    );
    return ret;
}

/* ============================================================
   BASIC OUTPUT (now matches new SYS_WRITE)
   ============================================================ */

// Writes a single character to STDOUT
static inline void putc(char c);

// Writes a null-terminated string to STDOUT
static inline void puts_raw(const char *s);

// Write string + newline
static inline void puts(const char *s) {
    puts_raw(s);
    putc('\n');
}

static inline ssize_t input_read(char *buf, size_t max_len) 
{
    if (!buf || max_len == 0 || max_len > INT_MAX) {
        return -1;
    }

    return syscall(SYS_INPUT, (long)buf, (long)max_len, 0);
}

/* Forward declarations (input() uses file I/O wrappers declared later) */
static inline int open(const char *pathname, int flags, int mode);
static inline ssize_t read(int fd, void *buf, size_t count);
static inline int close(int fd);

// Drain the structured input queue ($/dev/input/event0).
// Useful because the kernel shell and some apps consume event0, while libc's input() reads kbd0.
static inline void yield(void) {
    syscall(SYS_YIELD, 0, 0, 0);
}

static inline void input_flush_events(void) {
    int efd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    if (efd >= 0) {
        /* Events are opaque to libc; we just drain bytes. */
        unsigned char buf[64];
        for (;;) {
            ssize_t n = read(efd, buf, sizeof(buf));
            if (n <= 0) break;
        }
        close(efd);
    }
}

// Flush any pending buffered input so the next line read doesn't auto-consume previous keystrokes.
// This also drains the structured input queue ($/dev/input/event0) which the shell / games may use.
static inline void input_flush(void) {
    /* Drain raw keyboard chars */
    int kfd = open("$/dev/input/kbd0", O_RDONLY | O_NONBLOCK, 0);
    if (kfd >= 0) {
        for (;;) {
            char c;
            if (read(kfd, &c, 1) != 1) break;
        }
        close(kfd);
    }

    input_flush_events();
}

/* Safer version: read input into caller-provided buffer with bounds checking.
 * Returns: bytes read (excluding null terminator), or negative on error.
 */
static inline ssize_t input_line_to_buffer(char *buf, size_t maxlen) {
    if (!buf || maxlen == 0) return -1;
    if (maxlen > 0x7FFFFFFF) return -1;

    int fd = open("$/dev/input/kbd0", O_RDONLY, 0);
    if (fd < 0) {
        if (maxlen > 0) buf[0] = 0;
        return -1;
    }

    size_t n = 0;
    size_t safe_max = (maxlen > 0) ? (maxlen - 1) : 0;
    
    for (;;) {
        char c;
        if (read(fd, &c, 1) != 1) {
            yield();
            continue;
        }
        
        if (c == '\r') continue;
        
        if (c == '\n') {
            buf[n] = 0;
            close(fd);
            input_flush_events();
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
            char tmp[2] = {c, 0};
            puts_raw(tmp);
        }
    }
}

// Convenience: returns pointer to a static buffer (blocking line read from kbd0)
// WARNING: NOT THREAD-SAFE. Use input_line_to_buffer() in new code.
static inline char* input() {
    static char input_buf[256];
    ssize_t ret = input_line_to_buffer(input_buf, sizeof(input_buf));
    return input_buf;  /* Return buffer even on error; will contain null string */
}

/* ============================================================
   SYSTEM INFO
   ============================================================ */

static inline int get_system_info_u(md64api_sysinfo_data_u *out) {
    return (int)syscall(SYS_SSTATS2, (long)out, (long)sizeof(*out), 0);
}

#include "../include/moduos/kernel/process/proclist_user.h"

static inline int get_process_list(md_proclist_entry_u *out, size_t out_count) {
    return (int)syscall(SYS_PROCLIST, (long)out, (long)(out_count * sizeof(*out)), 0);
}

#include "../include/moduos/kernel/md64api_pidinfo_user.h"

static inline int md64api_get_pid_info(uint32_t pid, md64api_pid_info_u *out) {
    return (int)syscall(SYS_PIDINFO, (long)pid, (long)out, (long)sizeof(*out));
}

/* Time API: milliseconds since boot */
static inline uint64_t time_ms(void) {
    return (uint64_t)syscall(SYS_TIME, 0, 0, 0);
}

/* VGA color functions (deprecated, kept as stubs for compatibility) */
static inline void vga_set_color(uint8_t fg, uint8_t bg) {
    /* Deprecated: VGA syscalls removed. Use ANSI codes or DevFS instead. */
    (void)fg; (void)bg;
}

static inline uint8_t vga_get_color(void) {
    /* Deprecated: VGA syscalls removed. Return default color. */
    return 0x07;
}

static inline void vga_reset_color(void) {
    /* Deprecated: VGA syscalls removed. */
}

/* ANSI helpers (SGR). Works because kernel VGA driver parses ESC[...m. */
#define ANSI_ESC "\x1b"
#define ANSI_RESET ANSI_ESC "[0m"

/* ============================================================
   FS tracing (kernel-side timing logs)
   ============================================================ */
static inline int fs_trace_set(int enabled) {
    return (int)syscall(SYS_FS_TRACE, (long)enabled, 0, 0);
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

/* SYS_STAT: fills a kernel fs_file_info_t-like struct in userland */
typedef struct {
    char name[260];
    uint32_t size;
    int is_directory;
    uint32_t cluster;
} fs_file_info_t;

static inline int stat(const char *path, fs_file_info_t *out_info) {
    return (int)syscall(SYS_STAT, (long)path, (long)out_info, (long)sizeof(*out_info));
}

static inline long lseek(int fd, long offset, int whence) {
    return syscall(SYS_LSEEK, (long)fd, (long)offset, (long)whence);
}

// Read from a file descriptor
static inline ssize_t read(int fd, void *buf, size_t count) {
    return (ssize_t)syscall(SYS_READ, fd, (long)buf, count);
}

// MD64API GRP helper (must come after open/read/close)
static inline int md64api_grp_get_video0_info(md64api_grp_video_info_t *out) {
    if (!out) return -1;
    int fd = open(MD64API_GRP_DEFAULT_DEVICE, O_RDONLY, 0);
    if (fd < 0) return -2;
    ssize_t r = read(fd, out, sizeof(*out));
    close(fd);
    return (r == (ssize_t)sizeof(*out)) ? 0 : -3;
}

// Write to a file descriptor (binary safe)
static inline ssize_t sys_writefile_raw(int fd, const void *buf, size_t count) {
    long ret;
    __asm__ volatile (
        "mov %1, %%rax\n"
        "mov %2, %%rdi\n"
        "mov %3, %%rsi\n"
        "mov %4, %%rdx\n"
        "syscall\n"
        "mov %%rax, %0"
        : "=r"(ret)
        : "r"((long)SYS_WRITEFILE), "r"((long)fd), "r"((long)buf), "r"((long)count)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );
    return (ssize_t)ret;
}

static inline void putc(char c) {
    sys_writefile_raw(STDOUT_FILENO, &c, 1);
}

static inline void puts_raw(const char *s) {
    if (!s) s = " ";
    sys_writefile_raw(STDOUT_FILENO, s, strlen(s));
}

static inline void __rovo_debug_sys_writefile(void) {
    volatile int *p = (volatile int*)0x0;
    (void)p;
}

static inline ssize_t write(int fd, const void *buf, size_t count) {
    return sys_writefile_raw(fd, buf, count);
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

/* String conversion utilities */
static inline long strtol(const char *str, char **endptr, int base) {
    if (!str) {
        if (endptr) *endptr = (char *)str;
        return 0;
    }
    
    /* Skip leading whitespace */
    while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r') str++;
    
    int negative = 0;
    if (*str == '-') {
        negative = 1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    
    /* Auto-detect base if 0 */
    if (base == 0) {
        if (*str == '0' && (str[1] == 'x' || str[1] == 'X')) {
            base = 16;
            str += 2;
        } else if (*str == '0') {
            base = 8;
        } else {
            base = 10;
        }
    } else if (base == 16 && *str == '0' && (str[1] == 'x' || str[1] == 'X')) {
        str += 2;
    }
    
    long result = 0;
    const char *start = str;
    
    while (*str) {
        int digit;
        if (*str >= '0' && *str <= '9') {
            digit = *str - '0';
        } else if (*str >= 'a' && *str <= 'z') {
            digit = *str - 'a' + 10;
        } else if (*str >= 'A' && *str <= 'Z') {
            digit = *str - 'A' + 10;
        } else {
            break;
        }
        
        if (digit >= base) break;
        result = result * base + digit;
        str++;
    }
    
    if (endptr) *endptr = (char *)str;
    return negative ? -result : result;
}

/* ============================================================
   printf()
   ============================================================ */

static int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    while (*fmt) {
        /* Fast path: write literal runs in one syscall (preserves UTF-8 bytes) */
        if (*fmt != '%') {
            const char *start = fmt;
            while (*fmt && *fmt != '%') fmt++;
            if (fmt > start) {
                write(STDOUT_FILENO, start, (size_t)(fmt - start));
                continue;
            }
        }

        /* Format handling */
        if (*fmt == '%') {
            fmt++;

            int longmod = 0;
            int longlongmod = 0;

            if (*fmt == 'l') {
                longmod = 1;
                fmt++;
                if (*fmt == 'l') {
                    longlongmod = 1;
                    fmt++;
                }
            }

            switch (*fmt) {
                case 'c': {
                    char ch = (char)va_arg(ap, int);
                    write(STDOUT_FILENO, &ch, 1);
                    break;
                }

                case 's': {
                    const char *s = va_arg(ap, const char*);
                    if (!s) s = "(null)";
                    write(STDOUT_FILENO, s, strlen(s));
                    break;
                }

                case 'd':
                case 'i':
                    if (longlongmod) {
                        long long n = va_arg(ap, long long);
                        if (n < 0) {
                            char m = '-';
                            write(STDOUT_FILENO, &m, 1);
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

                case '%': {
                    char p = '%';
                    write(STDOUT_FILENO, &p, 1);
                    break;
                }

                default: {
                    char pct = '%';
                    write(STDOUT_FILENO, &pct, 1);
                    if (*fmt) write(STDOUT_FILENO, fmt, 1);
                    break;
                }
            }

            if (*fmt) fmt++;
        }
    }

    va_end(ap);
    return 0;
}

/* sprintf - format to a string buffer */
static int sprintf(char *str, const char *fmt, ...) {
    if (!str) return -1;
    
    va_list ap;
    va_start(ap, fmt);
    
    char *out = str;
    
    while (*fmt) {
        if (*fmt != '%') {
            const char *start = fmt;
            while (*fmt && *fmt != '%') fmt++;
            size_t len = fmt - start;
            memcpy(out, start, len);
            out += len;
            continue;
        }
        
        if (*fmt == '%') {
            fmt++;
            
            int longmod = 0;
            int longlongmod = 0;
            
            if (*fmt == 'l') {
                longmod = 1;
                fmt++;
                if (*fmt == 'l') {
                    longlongmod = 1;
                    fmt++;
                }
            }
            
            switch (*fmt) {
                case 'c': {
                    *out++ = (char)va_arg(ap, int);
                    break;
                }
                
                case 's': {
                    const char *s = va_arg(ap, const char*);
                    if (!s) s = "(null)";
                    size_t len = strlen(s);
                    memcpy(out, s, len);
                    out += len;
                    break;
                }
                
                case 'd':
                case 'i': {
                    char buf[32];
                    int val;
                    if (longlongmod) {
                        long long n = va_arg(ap, long long);
                        val = (int)n; /* truncate for simplicity */
                    } else if (longmod) {
                        val = (int)va_arg(ap, long);
                    } else {
                        val = va_arg(ap, int);
                    }
                    
                    int neg = 0;
                    if (val < 0) {
                        neg = 1;
                        val = -val;
                    }
                    
                    int i = 0;
                    if (val == 0) {
                        buf[i++] = '0';
                    } else {
                        while (val > 0) {
                            buf[i++] = '0' + (val % 10);
                            val /= 10;
                        }
                    }
                    
                    if (neg) *out++ = '-';
                    while (i > 0) *out++ = buf[--i];
                    break;
                }
                
                case 'u':
                case 'x':
                case 'X': {
                    char buf[32];
                    unsigned int val;
                    int base = (*fmt == 'u') ? 10 : 16;
                    const char *digits = (*fmt == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
                    
                    if (longlongmod) {
                        val = (unsigned int)va_arg(ap, unsigned long long);
                    } else if (longmod) {
                        val = (unsigned int)va_arg(ap, unsigned long);
                    } else {
                        val = va_arg(ap, unsigned int);
                    }
                    
                    int i = 0;
                    if (val == 0) {
                        buf[i++] = '0';
                    } else {
                        while (val > 0) {
                            buf[i++] = digits[val % base];
                            val /= base;
                        }
                    }
                    
                    while (i > 0) *out++ = buf[--i];
                    break;
                }
                
                case '%':
                    *out++ = '%';
                    break;
                    
                default:
                    *out++ = '%';
                    if (*fmt) *out++ = *fmt;
                    break;
            }
            
            if (*fmt) fmt++;
        }
    }
    
    *out = '\0';
    va_end(ap);
    return (int)(out - str);
}

/* ============================================================
   MEMORY FUNCTIONS
   ============================================================ */

/* sbrk must be declared before malloc() */
static inline void* sbrk(intptr_t inc) {
    return (void*)syscall(SYS_SBRK, inc, 0, 0);
}

/*
 * Userland heap allocator (simple free-list malloc).
 *
 * This replaces the bump allocator so graphics apps and shells don't leak memory forever.
 * It is NOT thread-safe (single-threaded userland).
 */

typedef struct uheap_hdr {
    size_t size;              /* payload size */
    struct uheap_hdr *next;   /* next free block */
    uint32_t magic;
    uint32_t free;
} uheap_hdr_t;

#define UHEAP_MAGIC 0xC0FFEE55u

static uheap_hdr_t *g_uheap_free = NULL;

static inline size_t uheap_align(size_t n) {
    return (n + 15) & ~((size_t)15);
}

/* Insert block into free list in ADDRESS ORDER (required for coalescing) */
static inline void uheap_insert_free(uheap_hdr_t *b) {
    b->free = 1;

    /* Find insertion point: keep list sorted by address */
    uheap_hdr_t **pp = &g_uheap_free;
    while (*pp && *pp < b) {
        pp = &(*pp)->next;
    }
    b->next = *pp;
    *pp = b;
}

static inline void uheap_remove_free(uheap_hdr_t *b) {
    uheap_hdr_t **pp = &g_uheap_free;
    while (*pp) {
        if (*pp == b) {
            *pp = b->next;
            b->next = NULL;
            return;
        }
        pp = &(*pp)->next;
    }
}

/*
 * Coalesce adjacent free blocks in the (address-ordered) free list.
 * Two blocks A and B are adjacent if:
 *   (uint8_t*)(A+1) + A->size == (uint8_t*)B
 * i.e. A's payload ends exactly where B's header begins.
 */
static inline void uheap_coalesce(void) {
    uheap_hdr_t *cur = g_uheap_free;
    while (cur && cur->next) {
        uheap_hdr_t *next = cur->next;

        /* Check adjacency */
        uint8_t *cur_end = (uint8_t*)(cur + 1) + cur->size;
        if (cur_end == (uint8_t*)next) {
            /* Merge: absorb next into cur */
            cur->size += sizeof(uheap_hdr_t) + next->size;
            cur->next = next->next;
            /* Wipe next's magic so it can't be double-freed */
            next->magic = 0;
            /* Don't advance cur — try to merge again with new next */
        } else {
            cur = cur->next;
        }
    }
}

static inline uheap_hdr_t* uheap_request_from_kernel(size_t payload) {
    /* Prevent integer overflow: payload + header size */
    if (payload > (size_t)-1 - sizeof(uheap_hdr_t)) return NULL;
    
    size_t total = sizeof(uheap_hdr_t) + payload;
    total = uheap_align(total);
    
    /* Verify alignment didn't cause overflow */
    if (total < payload) return NULL;

    void *mem = sbrk((intptr_t)total);
    if ((intptr_t)mem == -1 || mem == NULL) return NULL;

    uheap_hdr_t *h = (uheap_hdr_t*)mem;
    h->size = total - sizeof(uheap_hdr_t);
    h->next = NULL;
    h->magic = UHEAP_MAGIC;
    h->free = 0;
    return h;
}

static inline void uheap_split_if_needed(uheap_hdr_t *h, size_t need) {
    size_t remain = (h->size > need) ? (h->size - need) : 0;
    if (remain < (sizeof(uheap_hdr_t) + 32)) return;

    uint8_t *base = (uint8_t*)(h + 1);
    uheap_hdr_t *nh = (uheap_hdr_t*)(base + need);
    nh->size = remain - sizeof(uheap_hdr_t);
    nh->next = NULL;
    nh->magic = UHEAP_MAGIC;
    nh->free = 1;

    h->size = need;

    uheap_insert_free(nh);
}

static inline void* malloc(size_t size) {
    if (size == 0) return NULL;
    size = uheap_align(size);

    /* first-fit */
    uheap_hdr_t *prev = NULL;
    uheap_hdr_t *cur = g_uheap_free;
    while (cur) {
        if (cur->magic != UHEAP_MAGIC) return NULL;
        if (cur->free && cur->size >= size) {
            if (prev) prev->next = cur->next;
            else g_uheap_free = cur->next;
            cur->next = NULL;
            cur->free = 0;

            uheap_split_if_needed(cur, size);
            return (void*)(cur + 1);
        }
        prev = cur;
        cur = cur->next;
    }

    uheap_hdr_t *h = uheap_request_from_kernel(size);
    if (!h) return NULL;

    uheap_split_if_needed(h, size);
    return (void*)(h + 1);
}

static inline void free(void *ptr) {
    if (!ptr) return;
    uheap_hdr_t *h = ((uheap_hdr_t*)ptr) - 1;
    if (h->magic != UHEAP_MAGIC) return;
    if (h->free) return;

    uheap_insert_free(h);
    uheap_coalesce();          /* <-- merge adjacent free blocks */
}

static inline void* calloc(size_t nmemb, size_t size) {
    /* Prevent integer overflow in multiplication */
    if (nmemb > 0 && size > (size_t)-1 / nmemb) return NULL;
    
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (!p) return NULL;
    memset(p, 0, total);
    return p;
}

static inline void* realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }

    uheap_hdr_t *h = ((uheap_hdr_t*)ptr) - 1;
    if (h->magic != UHEAP_MAGIC) return NULL;

    size_t new_sz = uheap_align(size);

    /* Already big enough — try to split off excess */
    if (h->size >= new_sz) {
        uheap_split_if_needed(h, new_sz);
        return ptr;
    }

    /*
     * Try in-place expansion: check if the next block in memory
     * is free and adjacent, and combined they're big enough.
     */
    uint8_t *cur_end = (uint8_t*)(h + 1) + h->size;
    uheap_hdr_t *next = (uheap_hdr_t*)cur_end;

    /* Validate next block is in free list and adjacent */
    int next_is_free = 0;
    if (next->magic == UHEAP_MAGIC && next->free == 1) {
        /* Double-check it's in free list */
        for (uheap_hdr_t *f = g_uheap_free; f; f = f->next) {
            if (f == next) {
                next_is_free = 1;
                break;
            }
        }
    }

    if (next_is_free) {
        /* Prevent overflow when combining sizes */
        if (next->size > (size_t)-1 - h->size - sizeof(uheap_hdr_t)) return NULL;
        
        size_t combined = h->size + sizeof(uheap_hdr_t) + next->size;
        if (combined >= new_sz) {
            /* Absorb next into h — expand in place, no copy needed */
            uheap_remove_free(next);
            next->magic = 0;
            h->size = combined;
            uheap_split_if_needed(h, new_sz);
            return ptr;
        }
    }

    /* Fall back: allocate new, copy, free old */
    void *n = malloc(new_sz);
    if (!n) return NULL;
    memcpy(n, ptr, h->size);
    free(ptr);
    return n;
}
/* ============================================================
   PROCESS FUNCTIONS
   ============================================================ */

__attribute__((noreturn)) static inline void exit(int status) {
    syscall(SYS_EXIT, status, 0, 0);
    /* SYS_EXIT must not return; if it does, halt here. */
    for (;;) { __asm__ volatile("hlt"); }
}

static inline void exec(const char *str) {
    syscall(SYS_EXEC, (long)str, 0 , 0);
}

/* POSIX execve wrapper. On failure returns -1 and sets errno.
 * On success does not return.
 */
static inline int execve(const char *path, char *const argv[], char *const envp[]) {
    long r = syscall(SYS_EXECVE, (long)path, (long)argv, (long)envp);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (int)r;
}

/* Environment (kernel-managed per-process) */
static inline int putenv(const char *kv) {
    long r = syscall(SYS_PUTENV, (long)kv, 0, 0);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

/* Returns pointer to a static buffer (overwritten per call), or NULL if missing. */
static inline const char *getenv(const char *key) {
    static char buf[256];
    long r = syscall(SYS_GETENV, (long)key, (long)buf, (long)sizeof(buf));
    if (r < 0) { errno = (int)(-r); return NULL; }
    return buf;
}

/* Serialize env into user-provided buffer as newline-separated KEY=VALUE lines.
 * Returns bytes written or -1 with errno set.
 */
static inline int envlist(char *buf, size_t buflen) {
    long r = syscall(SYS_ENVLIST, (long)buf, (long)buflen, 0);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (int)r;
}

/* Streaming env listing.
 * offset is an in/out cursor (start at 0). Returns bytes written this call.
 */
static inline int envlist2(size_t *offset, char *buf, size_t buflen) {
    long r = syscall(SYS_ENVLIST2, (long)offset, (long)buf, (long)buflen);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (int)r;
}

static inline int unsetenv(const char *key) {
    long r = syscall(SYS_UNSETENV, (long)key, 0, 0);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

static inline int setenv(const char *key, const char *val) {
    if (!key || !val) { errno = EINVAL; return -1; }
    char kv[512];
    size_t klen = strlen(key);
    size_t vlen = strlen(val);
    if (klen + 1 + vlen + 1 > sizeof(kv)) { errno = E2BIG; return -1; }
    memcpy(kv, key, klen);
    kv[klen] = '=';
    memcpy(kv + klen + 1, val, vlen);
    kv[klen + 1 + vlen] = 0;
    return putenv(kv);
}

static inline int dup(int oldfd) {
    return (int)syscall(SYS_DUP, (long)oldfd, 0, 0);
}

static inline int dup2(int oldfd, int newfd) {
    return (int)syscall(SYS_DUP2, (long)oldfd, (long)newfd, 0);
}

static inline int pipe(int fds[2]) {
    return (int)syscall(SYS_PIPE, (long)fds, 0, 0);
}

static inline int geteuid(void) {
    return (int)syscall(SYS_GETEUID, 0, 0, 0);
}

static inline int getgid(void) {
    return (int)syscall(SYS_GETGID, 0, 0, 0);
}

static inline int getegid(void) {
    return (int)syscall(SYS_GETEGID, 0, 0, 0);
}

static inline int setgid(int gid) {
    return (int)syscall(SYS_SETGID, (long)gid, 0, 0);
}

static inline int fork(void) {
    long r = syscall(SYS_FORK, 0, 0, 0);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (int)r;
}

static inline int getpid(void) {
    return (int)syscall(SYS_GETPID, 0, 0, 0);
}

static inline int getppid(void) {
    return (int)syscall(SYS_GETPPID, 0, 0, 0);
}

/* POSIX waitpid (Linux semantics). */
#ifndef WNOHANG
#define WNOHANG 1
#endif
static inline int waitpid(int pid, int *status, int options) {
    int kopt = 0;
    if (options & WNOHANG) kopt |= 1; /* WAITX_WNOHANG */
    long r = syscall(SYS_WAITX, (long)pid, (long)status, (long)kopt);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (int)r;
}

/* POSIX wait - wait for any child process */
static inline int wait(int *status) {
    long r = syscall(SYS_WAIT, (long)status, 0, 0);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (int)r;
}

static inline int getuid(void) {
    return (int)syscall(SYS_GETUID, 0, 0, 0);
}

static inline int setuid(int uid) {
    return (int)syscall(SYS_SETUID, (long)uid, 0, 0);
}


static inline void sleep(unsigned int sec) {
    syscall(SYS_SLEEP, sec, 0, 0);
}

static inline int kill(int pid, int sig) {
    return (int)syscall(SYS_KILL, pid, sig, 0);
}

// Signal handler type
typedef void (*sighandler_t)(int);

// Install signal handler (returns old handler or SIG_ERR on error)
static inline sighandler_t signal(int signum, sighandler_t handler) {
    return (sighandler_t)syscall(SYS_SIGNAL, (long)signum, (long)handler, 0);
}

// Raise a signal to current process
static inline int raise(int sig) {
    return (int)syscall(SYS_RAISE, (long)sig, 0, 0);
}

// File descriptor injection (for TTY manager)
static inline int fd_inject(int pid, int fd, void *fd_obj) {
    return (int)syscall(SYS_FD_INJECT, (long)pid, (long)fd, (long)fd_obj);
}

// Directory operations
static inline int opendir(const char *path) {
    return (int)syscall(SYS_OPENDIR, (uint64_t)path, 0, 0);
}

// Change current working directory
static inline int chdir(const char *path) {
    return (int)syscall(SYS_CHDIR, (uint64_t)path, 0, 0);
}

// Get current working directory
static inline char *getcwd(char *buf, size_t size) {
    return (char*)syscall(SYS_GETCWD, (uint64_t)buf, (uint64_t)size, 0);
}

static inline int readdir(int fd, char *name_buf, size_t buf_size, int *is_dir, uint32_t *size) {
    uint64_t ret;
    __asm__ volatile (
        "mov %1, %%rax\n"
        "mov %2, %%rdi\n"
        "mov %3, %%rsi\n"
        "mov %4, %%rdx\n"
        "mov %5, %%r10\n"
        "mov %6, %%r8\n"
        "syscall\n"
        "mov %%rax, %0\n"
        : "=r"(ret)
        : "r"((uint64_t)SYS_READDIR), "r"((uint64_t)fd), "r"((uint64_t)name_buf),
          "r"((uint64_t)buf_size), "r"((uint64_t)is_dir), "r"((uint64_t)size)
        : "rax", "rdi", "rsi", "rdx", "r10", "r8", "rcx", "r11", "memory"
    );
    return (int)ret;
}

static inline int closedir(int fd) {
    return (int)syscall(SYS_CLOSEDIR, (uint64_t)fd, 0, 0);
}

// Create a directory. NOTE: kernel rejects DEVFS paths.
static inline int mkdir(const char *path) {
    return (int)syscall(SYS_MKDIR, (uint64_t)path, 0, 0);
}

// Remove a directory. NOTE: kernel rejects DEVFS paths.
static inline int rmdir(const char *path) {
    return (int)syscall(SYS_RMDIR, (uint64_t)path, 0, 0);
}

// Remove a file. NOTE: kernel rejects DEVFS paths.
static inline int unlink(const char *path) {
    return (int)syscall(SYS_UNLINK, (uint64_t)path, 0, 0);
}

#include "../include/moduos/fs/mkfs.h"
#include "../include/moduos/fs/part.h"
static inline int vfs_mkfs(const vfs_mkfs_req_t *req) {
    return (int)syscall(SYS_VFS_MKFS, (uint64_t)req, 0, 0);
}

static inline int vfs_getpart(const vfs_part_req_t *req, vfs_part_info_t *out) {
    return (int)syscall(SYS_VFS_GETPART, (uint64_t)req, (uint64_t)out, 0);
}

static inline int vfs_mbrinit(const vfs_mbrinit_req_t *req) {
    return (int)syscall(SYS_VFS_MBRINIT, (uint64_t)req, 0, 0);
}

static inline int userfs_register(const userfs_user_node_t *node) {
    return (int)syscall(SYS_USERFS_REGISTER, (uint64_t)node, 0, 0);
}

static inline int userfs_register_path(const char *path, uint32_t perms) {
    userfs_user_node_t node;
    memset(&node, 0, sizeof(node));
    node.path = path;
    node.owner_id = "userland";
    node.perms = perms;
    return userfs_register(&node);
}
int md_main(long argc, char** argv);

#ifndef LIBC_NO_START
__attribute__((noreturn))
__attribute__((noinline))
__attribute__((used))
void _start(long argc, char** argv) {
    // ModuOS start wrapper / ABI
    int mdm = md_main(argc, argv);

    input_flush();

    if (mdm) {
        exit(mdm);
    } else {
        exit(0);
    }
}
#endif /* LIBC_NO_START */


// Simple sscanf for parsing integers (supports "%d" format only)
static inline int sscanf(const char *str, const char *format, ...) {
    int *args[16];
    int arg_count = 0;
    
    // Count %d in format string
    const char *f = format;
    while (*f) {
        if (*f == '%' && *(f+1) == 'd') {
            arg_count++;
            f += 2;
        } else {
            f++;
        }
    }
    
    // Get variadic arguments
    __builtin_va_list ap;
    __builtin_va_start(ap, format);
    for (int i = 0; i < arg_count; i++) {
        args[i] = __builtin_va_arg(ap, int*);
    }
    __builtin_va_end(ap);
    
    // Parse the string
    int parsed = 0;
    const char *s = str;
    
    for (int i = 0; i < arg_count && *s; i++) {
        while (*s == ' ' || *s == '\t') s++;
        
        int sign = 1;
        if (*s == '-') {
            sign = -1;
            s++;
        }
        
        int value = 0;
        int found = 0;
        while (*s >= '0' && *s <= '9') {
            value = value * 10 + (*s - '0');
            s++;
            found = 1;
        }
        
        if (found) {
            *args[i] = sign * value;
            parsed++;
        } else {
            break;
        }
    }
    
    return parsed;
}

/* Random number generation (LCG) */
static uint32_t __rand_seed = 1;

static inline void srand(unsigned int seed) {
    __rand_seed = seed;
}

static inline int rand(void) {
    __rand_seed = __rand_seed * 1103515245 + 12345;
    return (int)((__rand_seed / 65536) % 32768);
}
