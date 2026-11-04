/* moduos.h - ModuOS-specific userland API (everything that is NOT ISO C / POSIX).
 *
 * Everything the old monolithic libc.h exposed that has no ISO/POSIX
 * equivalent lives here: raw syscall wrappers, MD64API, DevFS/VFS helpers,
 * shared memory, device mmap, kbd0 line input, etc.
 *
 * Implementations are in libc.c (-> libc.sqrl) unless marked static inline.
 */
#ifndef _MODUOS_H
#define _MODUOS_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "sys/types.h"

/* Kernel-shared headers (this file lives in userland/libc/) */
#include "../../include/moduos/kernel/syscall/syscall_numbers.h"
#include "../../include/moduos/fs/fs.h"
#include "../../include/moduos/kernel/md64api_grp.h"
#include "../../include/moduos/kernel/md64api_user.h"
#include "../../include/moduos/fs/userfs_user_api.h"
#include "../../include/moduos/kernel/process/proclist_user.h"
#include "../../include/moduos/kernel/md64api_pidinfo_user.h"
#include "../../include/moduos/fs/mkfs.h"
#include "../../include/moduos/fs/part.h"

typedef const char *string;

/* ---------------- Legacy sysinfo struct (unsafe in ring3) ----------------
 * Kept for older apps. Prefer md64api_sysinfo_data_u + get_system_info_u().
 */
typedef struct md64api_sysinfo_data
{
    uint64_t sys_available_ram;     /* MB */
    uint64_t sys_total_ram;         /* MB */

    int SystemVersion;
    int KernelVersion;
    string KernelVendor;
    string os_name;
    string os_arch;

    string pcname;
    string username;
    string domain;
    string kconsole;

    string cpu;
    string cpu_manufacturer;
    string cpu_model;
    int cpu_cores;
    int cpu_threads;
    int cpu_hyperthreading_enabled;
    int cpu_base_mhz;
    int cpu_max_mhz;
    int cpu_cache_l1_kb;
    int cpu_cache_l2_kb;
    int cpu_cache_l3_kb;
    string cpu_flags;

    int is_virtual_machine;
    string virtualization_vendor;

    string gpu_name;
    int gpu_vram_mb;

    uint64_t storage_total_mb;
    uint64_t storage_free_mb;
    string primary_disk_model;

    string bios_vendor;
    string bios_version;
    string motherboard_model;

    int secure_boot_enabled;
    int tpm_version;
} md64api_sysinfo_data;

/* ---------------- Raw syscalls ----------------
 * ABI: rax=num, rdi=a1, rsi=a2, rdx=a3, r10=a4, r8=a5, r9=a6.
 * Returns the raw kernel value (negative = -errno for most calls).
 */
static inline long syscall6(long num, long a1, long a2, long a3,
                            long a4, long a5, long a6) {
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return ret;
}
static inline long syscall(long num, long a1, long a2, long a3) {
    return syscall6(num, a1, a2, a3, 0, 0, 0);
}
static inline long syscall4(long num, long a1, long a2, long a3, long a4) {
    return syscall6(num, a1, a2, a3, a4, 0, 0);
}
static inline long syscall5(long num, long a1, long a2, long a3, long a4, long a5) {
    return syscall6(num, a1, a2, a3, a4, a5, 0);
}

static inline void rt_sigreturn(void) {
    __asm__ volatile (
        "mov %0, %%rax\n"
        "syscall\n"
        :: "i"(SYS_RT_SIGRETURN)
        : "rax", "rcx", "r11", "memory");
}

/* ---------------- Program entry ----------------
 * crt0.c provides _start(), which calls md_main(argc, argv), flushes input,
 * and calls exit(); build.sh links it into every app. Define md_main in your
 * app. -DLIBC_NO_START is no longer needed (and is harmless).
 */
int md_main(long argc, char **argv);

/* ---------------- Raw console output ----------------
 * ISO putc() takes a FILE*, so the old putc(char) is now putc_raw(). */
void putc_raw(char c);
void puts_raw(const char *s);

/* ---------------- Keyboard / input (DevFS) ---------------- */
/* SYS_INPUT is marked "DO NOT USE - Deprecated" in the kernel. Use input_line_to_buffer(). */
ssize_t input_read(char *buf, size_t max_len) __attribute__((deprecated("SYS_INPUT is deprecated; use input_line_to_buffer")));
void    input_flush_events(void);                           /* drain $/dev/input/event0 */
void    input_flush(void);                                  /* drain kbd0 + event0 */
ssize_t input_line_to_buffer(char *buf, size_t maxlen);     /* blocking line read from kbd0 (no trailing newline) */
char   *input(void);                                        /* static buffer; NOT thread-safe */
void    yield(void);

/* ---------------- System info / processes ---------------- */
int      get_system_info_u(md64api_sysinfo_data_u *out);            /* reads $/dev/md64api/sysinfo (SYS_SSTATS2 is deprecated) */
int      get_process_list(md_proclist_entry_u *out, size_t out_count);
int      md64api_get_pid_info(uint32_t pid, md64api_pid_info_u *out);
int      md64api_grp_get_video0_info(md64api_grp_video_info_t *out);
uint64_t time_ms(void);                                     /* ms since boot */
int      fs_trace_set(int enabled);

/* ---------------- Deprecated VGA stubs ---------------- */
static inline void    vga_set_color(uint8_t fg, uint8_t bg) { (void)fg; (void)bg; }
static inline uint8_t vga_get_color(void)                   { return 0x07; }
static inline void    vga_reset_color(void)                 { }

#define ANSI_ESC   "\x1b"
#define ANSI_RESET ANSI_ESC "[0m"

/* ---------------- Memory mapping / shared memory ---------------- */
void *sbrk(intptr_t inc);

#define MAP_FIXED   0x1
#define MAP_ANON    0x2
#define MAP_SHARED  0x4   /* fd must be a shm_open() handle */
#define MAP_FAILED  ((void *)-1)

#define PROT_R 0x1
#define PROT_W 0x2
#define PROT_X 0x4

/* Note: ModuOS mmap has no offset argument. */
void *mmap(void *addr, size_t length, int prot, int flags, int fd);
int   munmap(void *addr, size_t length);

/* shm_open handles are valid for exactly ONE mmap(MAP_SHARED, handle) call.
 * SHM_O_CREAT/SHM_O_EXCL match the kernel's real bit values (the old libc.h
 * O_CREAT did not). See the comment history in the old libc.h. */
#define SHM_O_CREAT 0x0040
#define SHM_O_EXCL  0x1000
int shm_open(const char *name, int oflags, uint32_t mode, uint64_t size);
int shm_unlink(const char *name);

/* mmap a device fd (e.g. $/dev/mvc/mvi0). hint=NULL lets the kernel choose.
 * prot: 1=R 2=W 3=RW. Returns MAP_FAILED on error. */
void *dev_mmap(int fd, void *hint, size_t length, int prot, int flags, uint64_t offset);

/* ---------------- Process / environment extras ---------------- */
int  exec(const char *cmdline);
int  envlist(char *buf, size_t buflen);                     /* "KEY=VALUE\n..." */
int  envlist2(size_t *offset, char *buf, size_t buflen);    /* streaming; offset starts at 0 */
int  setuid_of_other_process(int uid, uint32_t target);
int  getuid_of_other_process(uint32_t target);
int  fd_inject(int pid, int fd, void *fd_obj);
ssize_t invoke(int fd, const void *in_buf, size_t in_size, void *out_buf, size_t out_size);

/* ---------------- Directory / filesystem (ModuOS flavour) ----------------
 * These differ from POSIX: opendir() returns an int fd, mkdir() has no mode.
 * Kernel rejects DEVFS paths for mkdir/rmdir/unlink. */
int opendir(const char *path);
int readdir(int fd, char *name_buf, size_t buf_size, int *is_dir, uint32_t *size);
int closedir(int fd);
int mkdir(const char *path);
int stat(const char *path, fs_file_info_t *out_info);

int vfs_mkfs(const vfs_mkfs_req_t *req);
/* vfs_getpart / vfs_mbrinit: kernel marks these deprecated - use vdrive_* instead. */
int vfs_getpart(const vfs_part_req_t *req, vfs_part_info_t *out) __attribute__((deprecated("use vdrive_* userland I/O")));
int vfs_mbrinit(const vfs_mbrinit_req_t *req) __attribute__((deprecated("use vdrive_* userland I/O")));
int userfs_register(const userfs_user_node_t *node);
int userfs_register_path(const char *path, uint32_t perms);

/* UNSAFE raw disk access (caller must be mdman, UID 0) */
int vdrive_write(uint8_t vdrive_id, uint64_t lba, uint32_t count, const void *buffer);
int vdrive_write_sector(uint8_t vdrive_id, uint64_t lba, const void *buffer);
int vdrive_read(uint8_t vdrive_id, uint64_t lba, uint32_t count, void *buffer);
int vdrive_read_sector(uint8_t vdrive_id, uint64_t lba, void *buffer);

int mount_drive(int vdrive_id, uint32_t partition_lba, int fs_type);
int unmount_slot(int slot);
int list_mounts(fs_mount_info_t *buf, size_t max_entries);

#endif /* _MODUOS_H */
