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

/* Local string helpers (strlen, strcmp, etc.) */
#include "string.h"

// Syscall numbers (shared with kernel)
#ifndef SYSCALL_NUMBERS_H
#define SYSCALL_NUMBERS_H
// This file can be included by both kernel and userspace programs
#define SYS_EXIT        0
#define SYS_FORK        1
#define SYS_READ        2
#define SYS_WRITE       3
#define SYS_OPEN        4
#define SYS_CLOSE       5
#define SYS_WAIT        6
#define SYS_EXEC        7
#define SYS_WAITX       42 /* waitpid-like: (pid, status*, options) */
#define SYS_EXECVE      43 /* execve(path, argv, envp) */
#define SYS_PUTENV      44 /* putenv("KEY=VALUE") */
#define SYS_GETENV      45 /* getenv(key, buf, buflen) -> len or -errno */
#define SYS_ENVLIST     46 /* envlist(buf, buflen) -> bytes_written or -errno */
#define SYS_ENVLIST2    47 /* envlist2(offset_inout, buf, buflen) -> bytes_written or -errno */
#define SYS_UNSETENV    48 /* unsetenv(key) -> 0 or -errno */
#define SYS_PROCLIST    49 /* procs(buf, buflen) -> count or -errno */
#define SYS_PIDINFO     50 /* md64api_get_pid_info(pid, out, out_size) -> 0 or -errno */
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
#define SYS_LSEEK       23
#define SYS_WRITEFILE   24
#define SYS_INPUT       28
#define SYS_SSTATS      29
#define SYS_SSTATS2     38 /* fill user buffer with md64api_sysinfo_data_u */
// VFS formatting / mkfs
#define SYS_VFS_MKFS    36
#define SYS_VFS_GETPART 37
#define SYS_VFS_MBRINIT 41
/* User identity */
#define SYS_GETUID      33
#define SYS_SETUID      34
/* Graphics blit */
#define SYS_GFX_BLIT    35
// VGA / Console
#define SYS_VGA_SET_COLOR  30  // arg1=fg (0-15), arg2=bg (0-15)
#define SYS_VGA_GET_COLOR  31  // returns (bg<<4)|fg
#define SYS_VGA_RESET_COLOR 32 // reset to default (0x07 on 0x00)
/* Virtual memory mapping (userland dynamic linker support) */
#define SYS_MMAP        39
#define SYS_MUNMAP      40
/* Xenith26 shared buffers */
#define SYS_X26_SHM_CREATE  51
#define SYS_X26_SHM_MAP     52
#define SYS_X26_SHM_UNMAP   53
#define SYS_X26_SHM_DESTROY 54
/* Networking (via SQRM 'net' service; returns -ENOSYS if unavailable) */
#define SYS_NET_LINK_UP     59 /* () -> 0/1 or -errno */
#define SYS_NET_IPV4_ADDR   60 /* (out_u32_be*) -> 0 or -errno */
#define SYS_NET_IPV4_GW     61 /* (out_u32_be*) -> 0 or -errno */
#define SYS_NET_DNS_A       62 /* (hostname, out_u32_be*) -> 0 or -errno */
#define SYS_NET_HTTP_GET    63 /* (url, buf, bufsz, out_bytes*) -> 0 or -errno */
/* FS tracing (timing) */
#define SYS_FS_TRACE        55 /* arg1=0/1 set, returns previous state */
#define SYS_OPENDIR         56 /* opendir(path) -> dirfd */
#define SYS_READDIR         57 /* readdir(dirfd, namebuf, bufsz, is_dir*, size*) */
#define SYS_CLOSEDIR        58 /* closedir(dirfd) */

/* Userland USERFS nodes */
#define SYS_USERFS_REGISTER 64

#endif

// MD64API (userland-visible kernel interfaces)
#ifndef MODUOS_KERNEL_MD64API_GRP_H
#define MODUOS_KERNEL_MD64API_GRP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MD64API Graphics (GRP)
 *
 * This is a small, file-based API that targets devices under:
 *   $/dev/graphics/
 *
 * The canonical primary device is:
 *   $/dev/graphics/video0
 */

#define MD64API_GRP_DEFAULT_DEVICE "$/dev/graphics/video0"

typedef enum {
    MD64API_GRP_MODE_TEXT     = 0,
    MD64API_GRP_MODE_GRAPHICS = 1,
} md64api_grp_mode_t;

typedef enum {
    MD64API_GRP_FMT_UNKNOWN   = 0,
    MD64API_GRP_FMT_RGB565    = 1,
    MD64API_GRP_FMT_XRGB8888  = 2,
} md64api_grp_format_t;

/* Binary payload returned by reading from $/dev/graphics/video0 */
typedef struct __attribute__((packed)) {
    uint64_t fb_addr;     /* virtual address of linear framebuffer (0 in text mode) */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t  bpp;
    uint8_t  mode;        /* md64api_grp_mode_t */
    uint8_t  fmt;         /* md64api_grp_format_t */
    uint8_t  reserved;
} md64api_grp_video_info_t;

#ifdef __cplusplus
}
#endif

#endif

#ifndef MODUOS_FS_USERFS_USER_API_H
#define MODUOS_FS_USERFS_USER_API_H

#include <stdint.h>
#include <stddef.h>

typedef int64_t ssize_t;

#ifdef __cplusplus
extern "C" {
#endif

typedef ssize_t (*userfs_user_read_fn)(void *ctx, void *buf, size_t count);
typedef ssize_t (*userfs_user_write_fn)(void *ctx, const void *buf, size_t count);

typedef struct {
    userfs_user_read_fn read;
    userfs_user_write_fn write;
} userfs_user_ops_t;

typedef enum {
    USERFS_PERM_READ_ONLY  = 0x1,
    USERFS_PERM_WRITE_ONLY = 0x2,
    USERFS_PERM_READ_WRITE = 0x3,
} userfs_perm_t;

typedef struct {
    const char *path;         /* path relative to $/user */
    const char *owner_id;     /* owner identity string */
    uint32_t perms;           /* USERFS_PERM_* */
    userfs_user_ops_t ops;    /* user callbacks (unused in-kernel) */
    void *ctx;                /* user context pointer */
} userfs_user_node_t;

#ifdef __cplusplus
}
#endif

#endif

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

#ifndef MD64API_USER_H
#define MD64API_USER_H

#include <stdint.h>

/*
 * User-safe system info structure.
 * No pointers: all strings are copied into fixed-size buffers.
 */

typedef struct md64api_sysinfo_data_u {
    uint64_t sys_available_ram;
    uint64_t sys_total_ram;

    /* Version strings (copied from kernel; user-safe) */
    char SystemVersion[32];
    char KernelVersion[64];

    char KernelVendor[64];
    char os_name[32];
    char os_arch[16];

    char pcname[128];
    char username[32];
    char domain[32];
    char kconsole[64];

    char cpu[16];
    char cpu_manufacturer[16];
    char cpu_model[64];
    int cpu_cores;
    int cpu_threads;
    int cpu_hyperthreading_enabled;
    int cpu_base_mhz;
    int cpu_max_mhz;
    int cpu_cache_l1_kb;
    int cpu_cache_l2_kb;
    int cpu_cache_l3_kb;
    char cpu_flags[128];

    int is_virtual_machine;
    char virtualization_vendor[32];

    char gpu_name[128];
    char gpu_driver[64];
    int gpu_vram_mb;

    uint64_t storage_total_mb;
    uint64_t storage_free_mb;
    char primary_disk_model[128];

    char bios_vendor[64];
    char bios_version[64];
    char motherboard_model[64];

    int secure_boot_enabled;
    int tpm_version;

    /* RTC date/time snapshot (from kernel RTC). */
    uint8_t rtc_second;
    uint8_t rtc_minute;
    uint8_t rtc_hour;
    uint8_t rtc_day;
    uint8_t rtc_month;
    uint16_t rtc_year;
} md64api_sysinfo_data_u;

#endif


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

// Writes a single character to STDOUT
static inline void putc(char c);

// Writes a null-terminated string to STDOUT
static inline void puts_raw(const char *s);

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

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MD_PROCLIST_NAME_MAX 64

typedef struct md_proclist_entry_u {
    uint32_t pid;
    uint32_t ppid;
    uint32_t state;
    uint64_t total_time;     // scheduler ticks (see process_t.total_time)
    char name[MD_PROCLIST_NAME_MAX];
} md_proclist_entry_u;

#ifdef __cplusplus
}
#endif


static inline int get_process_list(md_proclist_entry_u *out, size_t out_count) {
    return (int)syscall(SYS_PROCLIST, (long)out, (long)(out_count * sizeof(*out)), 0);
}

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MD64API_PID_NAME_MAX 64
#define MD64API_CWD_MAX 256
#define MD64API_MAX_PROCESSES 256

typedef struct md64api_pid_info_u {
    uint32_t pid;
    uint32_t ppid;
    uint32_t uid;
    uint32_t gid;
    uint32_t state;        // process_state_t
    uint32_t priority;
    uint64_t total_time;   // scheduler ticks
    uint64_t user_rip;
    uint64_t user_rsp;
    uint8_t  is_user;

    // Approx memory usage (bytes), derived from tracked ranges.
    uint64_t mem_image_bytes;
    uint64_t mem_heap_bytes;
    uint64_t mem_mmap_bytes;
    uint64_t mem_stack_bytes;
    uint64_t mem_total_bytes;

    char name[MD64API_PID_NAME_MAX];
    char cwd[MD64API_CWD_MAX];
} md64api_pid_info_u;

#ifdef __cplusplus
}
#endif


static inline int md64api_get_pid_info(uint32_t pid, md64api_pid_info_u *out) {
    return (int)syscall(SYS_PIDINFO, (long)pid, (long)out, (long)sizeof(*out));
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
   FS tracing (kernel-side timing logs)
   ============================================================ */
static inline int fs_trace_set(int enabled) {
    return (int)syscall(SYS_FS_TRACE, (long)enabled, 0, 0);
}

/* ---- Networking (SQRM 'net' service) ---- */

static inline long net_link_up(void) {
    return syscall(SYS_NET_LINK_UP, 0, 0, 0);
}

static inline long net_ipv4_addr(uint32_t *out_be) {
    return syscall(SYS_NET_IPV4_ADDR, (long)out_be, 0, 0);
}

static inline long net_ipv4_gw(uint32_t *out_be) {
    return syscall(SYS_NET_IPV4_GW, (long)out_be, 0, 0);
}

static inline long net_dns_a(const char *hostname, uint32_t *out_be) {
    return syscall(SYS_NET_DNS_A, (long)hostname, (long)out_be, 0);
}

static inline long net_http_get(const char *url, void *buf, size_t bufsz, size_t *out_bytes) {
    return syscall4(SYS_NET_HTTP_GET, (long)url, (long)buf, (long)bufsz, (long)out_bytes);
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
        "int $0x80\n"
        "mov %%rax, %0"
        : "=r"(ret)
        : "r"((long)SYS_WRITEFILE), "r"((long)fd), "r"((long)buf), "r"((long)count)
        : "rax", "rdi", "rsi", "rdx", "memory"
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

/* Graphics pixel formats for gfx_blit */
#define MD64API_GRP_FMT_UNKNOWN   0
#define MD64API_GRP_FMT_XRGB8888  1
#define MD64API_GRP_FMT_RGB565    2

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

#ifndef MODUOS_FS_MKFS_H
#define MODUOS_FS_MKFS_H

#include <stdint.h>

// mkfs request for SYS_VFS_MKFS.
// Strings are NUL-terminated.
typedef struct {
    char fs_name[16];        // e.g. "fat32", "ext2"
    char label[16];          // volume label (optional)

    int32_t vdrive_id;
    uint32_t start_lba;
    uint32_t sectors;        // partition length in 512-byte sectors

    uint32_t flags;          // vfs_mkfs_req_t flags

    // flags bits
    //  - allows fat32 mkfs on volumes >32GiB when auto-picking cluster size
    //    (Windows formatter typically refuses without special tooling)
    #define VFS_MKFS_FLAG_FORCE  (1u << 0)

    // FAT32-specific (0 => kernel decides default)
    uint32_t fat32_sectors_per_cluster;
} vfs_mkfs_req_t;

#endif

#ifndef MODUOS_FS_PART_H
#define MODUOS_FS_PART_H

#include <stdint.h>

// Request/response for SYS_VFS_GETPART

typedef struct {
    int32_t vdrive_id;
    int32_t part_no; // 1..4
} vfs_part_req_t;

typedef struct {
    uint32_t start_lba;
    uint32_t sectors;
    uint8_t type;
    uint8_t _pad[3];
} vfs_part_info_t;

// Request for SYS_VFS_MBRINIT: write a minimal MBR with a single primary partition.
// sectors==0 means "use disk size - start_lba".
// flags bit0: force overwrite even if a valid MBR signature exists.
typedef struct {
    int32_t vdrive_id;
    uint32_t start_lba;   // typically 2048
    uint32_t sectors;     // 0=auto
    uint8_t type;         // MBR partition type (0 => default 0x83)
    uint8_t bootable;     // 0/1
    uint16_t flags;       // bit0=force
} vfs_mbrinit_req_t;

#endif

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

