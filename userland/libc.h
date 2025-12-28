// libc.h

#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Local string helpers (strlen, strcmp, etc.) */
#include "string.h"

// Syscall numbers (shared with kernel)
#include "../include/moduos/kernel/syscall/syscall_numbers.h"
// MD64API (userland-visible kernel interfaces)
#include "../include/moduos/kernel/md64api_grp.h"
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
        "int $0x80\n"
        "mov %%rax, %0"
        : "=r"(ret)
        : "r"(num), "r"(arg1), "r"(arg2), "r"(arg3)
        : "rax", "rdi", "rsi", "rdx", "memory"
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
        "int $0x80\n"
        "mov %%rax, %0"
        : "=r"(ret)
        : "r"(num), "r"(arg1), "r"(arg2), "r"(arg3), "r"(arg4)
        : "rax", "rdi", "rsi", "rdx", "r10", "memory"
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
    if (!s) s = " ";
    syscall(SYS_WRITE, (long)s, 0, 0);
}

// Write string + newline
static inline void puts(const char *s) {
    puts_raw(s);
    putc('\n');
}

static inline ssize_t input_read(char *buf, size_t max_len) {
    if (!buf || max_len == 0) return -1;
    return (ssize_t)syscall(SYS_INPUT, (long)buf, (long)max_len, 0);
}

/* Forward declarations (input() uses file I/O wrappers declared later) */
static inline int open(const char *pathname, int flags, int mode);
static inline ssize_t read(int fd, void *buf, size_t count);
static inline int close(int fd);

// Drain the structured input queue ($/dev/input/event0).
// Useful because the kernel shell and some apps consume event0, while libc's input() reads kbd0.
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

// Convenience: returns pointer to a static buffer (blocking line read from kbd0)
static inline char* input() {
    static char input_buf[256];

    int fd = open("$/dev/input/kbd0", O_RDONLY, 0);
    if (fd < 0) {
        input_buf[0] = 0;
        return input_buf;
    }

    int n = 0;
    for (;;) {
        char c;
        if (read(fd, &c, 1) != 1) continue;
        if (c == '\r') continue;
        if (c == '\n') {
            input_buf[n] = 0;
            close(fd);
            /* Prevent typed keys from being replayed later by event0 consumers. */
            input_flush_events();
            return input_buf;
        }
        if ((c == '\b' || c == 127) && n > 0) {
            n--;
            input_buf[n] = 0;
            /* echo erase */
            puts_raw("\b");
            continue;
        }
        if (n < (int)sizeof(input_buf) - 1 && c >= 32 && c < 127) {
            input_buf[n++] = c;
            input_buf[n] = 0;
            char tmp[2] = {c, 0};
            puts_raw(tmp);
        }
    }
}

/* ============================================================
   SYSTEM INFO
   ============================================================ */

static inline int get_system_info_u(md64api_sysinfo_data_u *out) {
    return (int)syscall(SYS_SSTATS2, (long)out, (long)sizeof(*out), 0);
}

/* Time API: milliseconds since boot */
static inline uint64_t time_ms(void) {
    return (uint64_t)syscall(SYS_TIME, 0, 0, 0);
}

/* ============================================================
   VGA / CONSOLE COLOR (Text Mode)
   ============================================================ */

/* Set VGA text colors: fg/bg are 0-15 (VGA attribute nibble) */
static inline void vga_set_color(uint8_t fg, uint8_t bg) {
    syscall(SYS_VGA_SET_COLOR, (long)fg, (long)bg, 0);
}

/* Get current color attribute: returns (bg<<4)|fg */
static inline uint8_t vga_get_color(void) {
    return (uint8_t)syscall(SYS_VGA_GET_COLOR, 0, 0, 0);
}

/* Reset to default 0x07 on 0x00 */
static inline void vga_reset_color(void) {
    syscall(SYS_VGA_RESET_COLOR, 0, 0, 0);
}

/* ANSI helpers (SGR). Works because kernel VGA driver parses ESC[...m. */
#define ANSI_ESC "\x1b"
#define ANSI_RESET ANSI_ESC "[0m"

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

static inline void uheap_insert_free(uheap_hdr_t *b) {
    b->free = 1;
    b->next = g_uheap_free;
    g_uheap_free = b;
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

static inline uheap_hdr_t* uheap_request_from_kernel(size_t payload) {
    size_t total = sizeof(uheap_hdr_t) + payload;
    total = uheap_align(total);

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
    /* split only if we can create a useful remainder */
    size_t remain = (h->size > need) ? (h->size - need) : 0;
    if (remain < (sizeof(uheap_hdr_t) + 32)) return;

    /* New block starts after allocated payload */
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
            /* detach */
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

    /* mark free and add to free list */
    uheap_insert_free(h);

    /* NOTE: simple allocator: no coalescing yet */
}

static inline void* calloc(size_t nmemb, size_t size) {
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
    if (h->size >= new_sz) return ptr;

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

static inline int getpid(void) {
    return (int)syscall(SYS_GETPID, 0, 0, 0);
}

static inline int getppid(void) {
    return (int)syscall(SYS_GETPPID, 0, 0, 0);
}

static inline int getuid(void) {
    return (int)syscall(SYS_GETUID, 0, 0, 0);
}

static inline int setuid(int uid) {
    return (int)syscall(SYS_SETUID, (long)uid, 0, 0);
}

/*
 * Graphics blit: copy a user backbuffer into the framebuffer.
 * fmt must match the current framebuffer format (no conversion).
 */
static inline int gfx_blit(const void *src, uint16_t w, uint16_t h,
                           uint16_t dst_x, uint16_t dst_y,
                           uint16_t src_pitch_bytes,
                           uint16_t fmt) {
    uint32_t wh = ((uint32_t)w << 16) | (uint32_t)h;
    uint32_t xy = ((uint32_t)dst_x << 16) | (uint32_t)dst_y;
    uint32_t pf = ((uint32_t)src_pitch_bytes << 16) | (uint32_t)fmt;
    return (int)syscall4(SYS_GFX_BLIT, (long)src, (long)wh, (long)xy, (long)pf);
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
        "int $0x80\n"
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

#include "../include/moduos/fs/mkfs.h"
#include "../include/moduos/fs/part.h"

static inline int vfs_mkfs(const vfs_mkfs_req_t *req) {
    return (int)syscall(SYS_VFS_MKFS, (uint64_t)req, 0, 0);
}

static inline int vfs_getpart(const vfs_part_req_t *req, vfs_part_info_t *out) {
    return (int)syscall(SYS_VFS_GETPART, (uint64_t)req, (uint64_t)out, 0);
}

int md_main(long argc, char** argv);

__attribute__((noreturn)) void _start(long argc, char** argv)
{
    // ModuOS start wrapper / ABI
    int mdm = md_main(argc, argv);

    if (mdm) {
        exit(mdm);
    } else {
        exit(0);
    }
}
