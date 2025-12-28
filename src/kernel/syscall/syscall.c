#include "moduos/kernel/syscall/syscall.h"
#include "moduos/fs/mkfs.h"
#include "moduos/fs/part.h"
#include "moduos/fs/devfs.h"
#include "moduos/kernel/syscall/syscall_numbers.h"
#include "moduos/kernel/md64api.h"
#include "moduos/kernel/process/process.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/interrupts/idt.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/debug.h"
#include "moduos/kernel/process/process.h"
#include "moduos/kernel/macros.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/md64api_user.h"
#include "moduos/fs/fs.h"
#include "moduos/fs/fd.h"
#include "moduos/fs/path.h"
#include "moduos/fs/path_norm.h"
#include "moduos/kernel/exec.h"
#include "moduos/drivers/input/input.h"

#include "moduos/kernel/memory/usercopy.h"

/* Helper: Copy string from userspace to kernel buffer */
static int copy_string_from_user(const char *user_str, char *kernel_buf, size_t max_len) {
    int rc = usercopy_string_from_user(kernel_buf, user_str, max_len);
    if (rc != 0) return -1;

    /* Strip trailing whitespace/newlines (common when userland passes raw tokens) */
    size_t n = strlen(kernel_buf);
    while (n > 0) {
        char c = kernel_buf[n - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            kernel_buf[n - 1] = 0;
            n--;
        } else {
            break;
        }
    }

    return 0;
}

extern void syscall_entry(void);

static int sys_vfs_mkfs(const vfs_mkfs_req_t *user_req);
static int sys_vfs_getpart(const vfs_part_req_t *user_req, vfs_part_info_t *user_out);

void syscall_init(void) {
    COM_LOG_INFO(COM1_PORT, "Initializing system calls");

    /* Use INT 0x80 for now (SYSCALL/SYSRET needs more debugging) */
    idt_set_entry(0x80, syscall_entry, 0xEF);

    fd_init();
    COM_LOG_OK(COM1_PORT, "System calls initialized (INT 0x80)");
}

uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    // DEBUG: Log every syscall
    char buf[32];
    
    switch (syscall_num) {
        case SYS_EXIT:    return sys_exit((int)arg1);
        case SYS_FORK:    return sys_fork();
        case SYS_READ:    return sys_read((int)arg1, (void*)arg2, (size_t)arg3);
        case SYS_WRITEFILE: return sys_writefile((int)arg1, (const char*)arg2, (size_t)arg3);
        case SYS_WRITE:   return sys_write((const char*)arg1);
        case SYS_OPEN:    return sys_open((const char*)arg1, (int)arg2, (int)arg3);
        case SYS_CLOSE:   return sys_close((int)arg1);
        case SYS_WAIT:    return sys_wait((int*)arg1);
        case SYS_GETPID:  return sys_getpid();
        case SYS_GETPPID: return sys_getppid();
        case SYS_GETUID: {
            process_t *p = process_get_current();
            return p ? (long)p->uid : 0;
        }
        case SYS_SETUID: {
            process_t *p = process_get_current();
            if (!p) return -1;
            if (p->uid != 0) return -2; /* EPERM */
            p->uid = (uint32_t)arg1;
            return 0;
        }

        case SYS_GFX_BLIT: {
            /* args:
             *  arg1 = src_ptr
             *  arg2 = packed (src_w<<16 | src_h)
             *  arg3 = packed (dst_x<<16 | dst_y)
             *  arg4 = packed (src_pitch_bytes<<16 | fmt)
             */
            const uint8_t *src = (const uint8_t*)arg1;
            uint32_t wh = (uint32_t)arg2;
            uint32_t xy = (uint32_t)arg3;
            uint32_t pf = (uint32_t)arg4;

            uint32_t src_w = (wh >> 16) & 0xFFFFu;
            uint32_t src_h = (wh) & 0xFFFFu;
            uint32_t dst_x = (xy >> 16) & 0xFFFFu;
            uint32_t dst_y = (xy) & 0xFFFFu;
            uint32_t src_pitch = (pf >> 16) & 0xFFFFu;
            framebuffer_format_t fmt = (framebuffer_format_t)(pf & 0xFFFFu);

            if (!src || src_w == 0 || src_h == 0) return -1;

            framebuffer_t fb;
            if (VGA_GetFrameBufferMode() != FB_MODE_GRAPHICS) return -2;
            if (VGA_GetFrameBuffer(&fb) != 0 || !fb.addr) return -2;

            /*
             * Format negotiation:
             * The blit is fundamentally determined by bytes-per-pixel.
             * Some code paths may not initialize fb.fmt reliably, so validate against fb.bpp.
             */
            framebuffer_format_t expected_fmt = FB_FMT_UNKNOWN;
            if (fb.bpp == 32) expected_fmt = FB_FMT_XRGB8888;
            else if (fb.bpp == 16) expected_fmt = FB_FMT_RGB565;

            if (expected_fmt == FB_FMT_UNKNOWN || fmt != expected_fmt) {
                if (kernel_debug_is_on()) {
                    /* Debug to COM1 to help diagnose real hardware mismatches */
                    com_write_string(COM1_PORT, "[SYS_GFX_BLIT] fmt mismatch: user=");
                    itoa((int)fmt, buf, 10); com_write_string(COM1_PORT, buf);
                    com_write_string(COM1_PORT, " fb.bpp=");
                    itoa((int)fb.bpp, buf, 10); com_write_string(COM1_PORT, buf);
                    com_write_string(COM1_PORT, " fb.fmt=");
                    itoa((int)fb.fmt, buf, 10); com_write_string(COM1_PORT, buf);
                    com_write_string(COM1_PORT, " expected=");
                    itoa((int)expected_fmt, buf, 10); com_write_string(COM1_PORT, buf);
                    com_write_string(COM1_PORT, "\n");
                }
                return -3; /* format mismatch */
            }

            uint32_t bpp = (fb.bpp / 8u);
            if (bpp != 2 && bpp != 4) return -4;

            if (src_pitch == 0) {
                src_pitch = src_w * bpp;
            }

            if (dst_x >= fb.width || dst_y >= fb.height) return -5;
            if (dst_x + src_w > fb.width) src_w = fb.width - dst_x;
            if (dst_y + src_h > fb.height) src_h = fb.height - dst_y;

            uint8_t *dst_base = (uint8_t*)fb.addr + (uint64_t)dst_y * fb.pitch + (uint64_t)dst_x * bpp;
            size_t row_bytes = (size_t)src_w * bpp;

            for (uint32_t y = 0; y < src_h; y++) {
                const uint8_t *srow = src + (uint64_t)y * src_pitch;
                uint8_t *drow = dst_base + (uint64_t)y * fb.pitch;
                memcpy(drow, srow, row_bytes);
            }

            return 0;
        }
        case SYS_SLEEP:   return sys_sleep((unsigned int)arg1);
        case SYS_YIELD:   sys_yield(); return 0;
        case SYS_MALLOC:  return (uint64_t)sys_malloc((size_t)arg1);
        case SYS_FREE:    sys_free((void*)arg1); return 0;
        case SYS_SBRK:    return (uint64_t)sys_sbrk((intptr_t)arg1);
        case SYS_KILL:    return sys_kill((int)arg1, (int)arg2);
        case SYS_TIME:    return sys_time();
        case SYS_EXEC:    return sys_exec((const char*)arg1);
        case SYS_CHDIR:   return sys_chdir((const char*)arg1);
        case SYS_GETCWD:  return (uint64_t)sys_getcwd((char*)arg1, (size_t)arg2);
        case SYS_STAT:    return sys_stat((const char*)arg1, (void*)arg2, (size_t)arg3);
        case SYS_LSEEK:   return (uint64_t)sys_lseek((int)arg1, (off_t)arg2, (int)arg3);
        case SYS_MKDIR:   return sys_mkdir((const char*)arg1);
        case SYS_RMDIR:   return sys_rmdir((const char*)arg1);
        case SYS_UNLINK:  return sys_unlink((const char*)arg1);
        case SYS_OPENDIR: return sys_opendir((const char*)arg1);
        case SYS_READDIR: return sys_readdir((int)arg1, (char*)arg2, (size_t)arg3, (int*)arg4, (uint32_t*)arg5);
        case SYS_CLOSEDIR: return sys_closedir((int)arg1);
        case SYS_INPUT:   return (uint64_t)sys_input((char*)arg1, (size_t)arg2);
        case SYS_SSTATS:
            return (uint64_t)sys_get_sysinfo();
        case SYS_SSTATS2:
            return (uint64_t)sys_get_sysinfo2((md64api_sysinfo_data_u*)arg1, (size_t)arg2);

        case SYS_MMAP:
            return (uint64_t)sys_mmap((void*)arg1, (size_t)arg2, (int)arg3, (int)arg4);
        case SYS_MUNMAP:
            return (uint64_t)sys_munmap((void*)arg1, (size_t)arg2);

        case SYS_VGA_SET_COLOR:
            VGA_SetTextColor((uint8_t)arg1, (uint8_t)arg2);
            return 0;
        case SYS_VGA_GET_COLOR:
            return (uint64_t)VGA_GetTextColor();
        case SYS_VGA_RESET_COLOR:
            VGA_ResetTextColor();
            return 0;

        case SYS_VFS_MKFS:
            return (uint64_t)sys_vfs_mkfs((const vfs_mkfs_req_t*)arg1);
        case SYS_VFS_GETPART:
            return (uint64_t)sys_vfs_getpart((const vfs_part_req_t*)arg1, (vfs_part_info_t*)arg2);

        default:
            if (kernel_debug_is_med()) {
                com_write_string(COM1_PORT, "[SYSCALL] Unknown syscall: ");
                itoa(syscall_num, buf, 10);
                com_write_string(COM1_PORT, buf);
                com_write_string(COM1_PORT, "\n");
            }
            return (uint64_t)-1;
    }
}

/* ============================================================
   SYSTEM CALL IMPLEMENTATIONS
   ============================================================ */

int sys_exit(int status) {
    process_t* proc = process_get_current();
    if (proc) {
        fd_close_all(proc->pid);
    }

    /*
     * This syscall must never return to userspace.
     * process_exit() switches away and halts.
     */
    process_exit(status);

    /* Fallback: if process_exit ever returns, halt. */
    for (;;) { __asm__ volatile("hlt"); }
}

int sys_fork(void) {
    COM_LOG_WARN(COM1_PORT, "fork() not yet implemented");
    return -1;
}

ssize_t sys_read(int fd, void *buf, size_t count) {
    if (!buf) return -1;
    if (count == 0) return 0;

    /* Read into a kernel buffer, then copy_to_user.
     * This prevents drivers/filesystems from accidentally treating user pointers as kernel pointers.
     */
    size_t tmp_sz = (count > 4096) ? 4096 : count;
    char tmp[4096];

    ssize_t result = fd_read(fd, tmp, tmp_sz);
    if (result > 0) {
        if (usercopy_to_user(buf, tmp, (size_t)result) != 0) return -1;
    }
    
    // DEBUG
    if (kernel_debug_is_on()) {
        com_write_string(COM1_PORT, "[SYS_READ] fd=");
        char dbuf[32];
        itoa(fd, dbuf, 10);
        com_write_string(COM1_PORT, dbuf);
        com_write_string(COM1_PORT, " count=");
        itoa(count, dbuf, 10);
        com_write_string(COM1_PORT, dbuf);
        com_write_string(COM1_PORT, " result=");
        itoa(result, dbuf, 10);
        com_write_string(COM1_PORT, dbuf);
        com_write_string(COM1_PORT, "\n");
    }
    
    return result;
}

md64api_sysinfo_data* sys_get_sysinfo(void) {
    /* Legacy: returns a pointer to a kernel struct containing kernel pointers.
     * Unsafe for ring3 userland. Use SYS_SSTATS2 instead.
     */
    static md64api_sysinfo_data info;
    info = get_system_info();
    return &info;
}

static void safe_strcpy(char *dst, size_t dst_sz, const char *src) {
    if (!dst || dst_sz == 0) return;
    if (!src) src = "";
    size_t i = 0;
    for (; i + 1 < dst_sz && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

int sys_get_sysinfo2(md64api_sysinfo_data_u *out, size_t out_size) {
    if (!out) return -1;
    if (out_size < sizeof(*out)) return -1;

    md64api_sysinfo_data k = get_system_info();

    /* Copy scalars */
    out->sys_available_ram = k.sys_available_ram;
    out->sys_total_ram = k.sys_total_ram;
    /* Version strings (user-safe). */
    safe_strcpy(out->SystemVersion, sizeof(out->SystemVersion), k.SystemVersion);
    safe_strcpy(out->KernelVersion, sizeof(out->KernelVersion), k.KernelVersion);
    out->cpu_cores = k.cpu_cores;
    out->cpu_threads = k.cpu_threads;
    out->cpu_hyperthreading_enabled = k.cpu_hyperthreading_enabled;
    out->cpu_base_mhz = k.cpu_base_mhz;
    out->cpu_max_mhz = k.cpu_max_mhz;
    out->cpu_cache_l1_kb = k.cpu_cache_l1_kb;
    out->cpu_cache_l2_kb = k.cpu_cache_l2_kb;
    out->cpu_cache_l3_kb = k.cpu_cache_l3_kb;
    out->is_virtual_machine = k.is_virtual_machine;
    out->gpu_vram_mb = k.gpu_vram_mb;
    out->storage_total_mb = k.storage_total_mb;
    out->storage_free_mb = k.storage_free_mb;
    out->secure_boot_enabled = k.secure_boot_enabled;
    out->tpm_version = k.tpm_version;

    /* Copy strings from kernel pointers into user buffer */
    safe_strcpy(out->KernelVendor, sizeof(out->KernelVendor), k.KernelVendor);
    safe_strcpy(out->os_name, sizeof(out->os_name), k.os_name);
    safe_strcpy(out->os_arch, sizeof(out->os_arch), k.os_arch);
    safe_strcpy(out->pcname, sizeof(out->pcname), k.pcname);
    safe_strcpy(out->username, sizeof(out->username), k.username);
    safe_strcpy(out->domain, sizeof(out->domain), k.domain);
    safe_strcpy(out->kconsole, sizeof(out->kconsole), k.kconsole);
    safe_strcpy(out->cpu, sizeof(out->cpu), k.cpu);
    safe_strcpy(out->cpu_manufacturer, sizeof(out->cpu_manufacturer), k.cpu_manufacturer);
    safe_strcpy(out->cpu_model, sizeof(out->cpu_model), k.cpu_model);
    safe_strcpy(out->cpu_flags, sizeof(out->cpu_flags), k.cpu_flags);
    safe_strcpy(out->virtualization_vendor, sizeof(out->virtualization_vendor), k.virtualization_vendor);
    safe_strcpy(out->gpu_name, sizeof(out->gpu_name), k.gpu_name);
    safe_strcpy(out->primary_disk_model, sizeof(out->primary_disk_model), k.primary_disk_model);
    safe_strcpy(out->bios_vendor, sizeof(out->bios_vendor), k.bios_vendor);
    safe_strcpy(out->bios_version, sizeof(out->bios_version), k.bios_version);
    safe_strcpy(out->motherboard_model, sizeof(out->motherboard_model), k.motherboard_model);

    return 0;
}

ssize_t sys_writefile(int fd, const char *str, size_t count) {
    if (!str) return -1;

    /* Copy from user once, then write from kernel buffer */
    if (count > 4096) count = 4096;
    char tmp[4096];
    if (count && usercopy_from_user(tmp, str, count) != 0) return -1;

    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        VGA_WriteN(tmp, count);
        return (ssize_t)count;
    }

    return fd_write(fd, tmp, count);
}

int sys_write(const char *str) {
    if (!str) {
        if (kernel_debug_is_med()) {
            com_write_string(COM1_PORT, "[SYS_WRITE] NULL pointer!\n");
        }
        return -1;
    }

    char tmp[512];
    if (usercopy_string_from_user(tmp, str, sizeof(tmp)) != 0) return -1;
    VGA_Write(tmp);
    return 0;
}

ssize_t sys_input(char *user_buf, size_t max_len) {
    if (!user_buf || max_len == 0) return -1;

    char *line = input();
    if (!line) {
        char z = 0;
        usercopy_to_user(user_buf, &z, 1);
        return 0;
    }

    size_t n = strlen(line);
    if (n >= max_len) n = max_len - 1;
    if (usercopy_to_user(user_buf, line, n) != 0) return -1;
    char z = 0;
    if (usercopy_to_user(user_buf + n, &z, 1) != 0) return -1;
    return (ssize_t)n;
}

int sys_exec(const char *str) {
    if (!str) return -1;
    
    /* Copy command from userspace */
    char kernel_cmd[256];
    if (copy_string_from_user(str, kernel_cmd, sizeof(kernel_cmd)) != 0) {
        return -1;
    }
    
    exec(kernel_cmd);
    return 0;
}

int sys_open(const char *pathname, int flags, int mode) {
    if (!pathname) return -1;

    process_t *proc = process_get_current();
    if (!proc) return -1;

    // Resolve relative paths against process CWD (supports both / and $/ namespaces)
    char full_path[256];
    const char *p = pathname;
    if (!(p[0] == '/' || (p[0] == '$' && p[1] == '/'))) {
        const char *cwd = (proc->cwd[0] ? proc->cwd : "/");
        strncpy(full_path, cwd, sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = 0;
        size_t len = strlen(full_path);
        if (len == 0 || full_path[len - 1] != '/') {
            strncat(full_path, "/", sizeof(full_path) - strlen(full_path) - 1);
        }
        strncat(full_path, p, sizeof(full_path) - strlen(full_path) - 1);
        p = full_path;
    }

    fs_path_resolved_t r;
    if (fs_resolve_path(proc, p, &r) != 0) return -1;

    // DEVVFS: allow opening $/dev/<node> and $/dev/input/<node> as character devices
    if (r.route == FS_ROUTE_DEVVFS) {
        if (r.devvfs_kind != 2) return -1;
        const char *node = r.rel_path;
        while (*node == '/') node++;
        if (!*node) return -1;

        // DEVFS is hierarchical; node may contain slashes.
        return fd_open_devfs(node, flags);
    }

    int slot = (r.route == FS_ROUTE_MOUNT) ? r.mount_slot : proc->current_slot;
    if (slot < 0) return -1;

    return fd_open(slot, r.rel_path, flags, mode);
}

int sys_close(int fd) { return fd_close(fd); }

int sys_wait(int *status) { (void)status; return -1; }

int sys_getpid(void) {
    process_t *proc = process_get_current();
    return proc ? proc->pid : -1;
}

int sys_getppid(void) {
    process_t *proc = process_get_current();
    return proc ? proc->parent_pid : -1;
}

int sys_sleep(unsigned int seconds) {
    process_sleep(seconds * 1000);
    return 0;
}

void sys_yield(void) { process_yield(); }

void* sys_malloc(size_t size) {
    process_t *p = process_get_current();
    if (p && p->is_user) {
        /* Returning kernel pointers to ring3 userland will crash.
         * Userland must use sbrk()/mmap() based allocators.
         */
        return (void*)0;
    }
    return kmalloc(size);
}

void sys_free(void *ptr) {
    process_t *p = process_get_current();
    if (p && p->is_user) {
        /* Ignore: userland must not free kernel allocations. */
        return;
    }
    kfree(ptr);
}

void* sys_sbrk(intptr_t increment) {
    process_t *p = process_get_current();
    if (!p) return (void*)-1;
    if (!p->is_user) return (void*)-1;

    /* Only support growing for now. */
    if (increment < 0) return (void*)-1;

    uint64_t old = p->user_heap_end;
    uint64_t new_end = old + (uint64_t)increment;
    if (p->user_heap_limit && new_end > p->user_heap_limit) return (void*)-1;

    /* page-align mapping */
    uint64_t map_start = (old + 0xFFFULL) & ~0xFFFULL;
    uint64_t map_end = (new_end + 0xFFFULL) & ~0xFFFULL;

    if (map_end > map_start) {
        size_t pages = (size_t)((map_end - map_start) / 0x1000ULL);
        uint64_t phys = phys_alloc_contiguous(pages);
        if (!phys) return (void*)-1;
        if (paging_map_range(map_start, phys, (uint64_t)pages * 0x1000ULL, PFLAG_PRESENT | PFLAG_WRITABLE | PFLAG_USER) != 0) {
            for (size_t i = 0; i < pages; i++) phys_free_frame(phys + (uint64_t)i * 0x1000ULL);
            return (void*)-1;
        }
    }

    p->user_heap_end = new_end;
    return (void*)(uintptr_t)old;
}

/* VM mapping (MVP). prot: bit0=R bit1=W bit2=X (X currently ignored)
 * flags: bit0=FIXED, bit1=ANON (only anon supported)
 */
void* sys_mmap(void *addr, size_t size, int prot, int flags) {
    (void)flags;
    process_t *p = process_get_current();
    if (!p || !p->is_user) return (void*)-1;
    if (size == 0) return (void*)-1;

    uint64_t sz = ((uint64_t)size + 0xFFFULL) & ~0xFFFULL;

    uint64_t v = (uint64_t)(uintptr_t)addr;
    int fixed = (flags & 1) != 0;

    if (fixed) {
        if (v == 0 || (v & 0xFFFULL)) return (void*)-1;
    } else {
        v = (p->user_mmap_end + 0xFFFULL) & ~0xFFFULL;
        if (v < p->user_mmap_base) v = p->user_mmap_base;
        if (v + sz > p->user_mmap_limit) return (void*)-1;
    }

    size_t pages = (size_t)(sz / 0x1000ULL);
    uint64_t phys = phys_alloc_contiguous(pages);
    if (!phys) return (void*)-1;

    uint64_t pflags = PFLAG_PRESENT | PFLAG_USER;
    if (prot & 2) pflags |= PFLAG_WRITABLE;

    if (paging_map_range(v, phys, sz, pflags) != 0) {
        for (size_t i = 0; i < pages; i++) phys_free_frame(phys + (uint64_t)i * 0x1000ULL);
        return (void*)-1;
    }

    memset((void*)(uintptr_t)v, 0, (size_t)sz);

    if (!fixed) {
        if (v + sz > p->user_mmap_end) p->user_mmap_end = v + sz;
    }

    return (void*)(uintptr_t)v;
}

int sys_munmap(void *addr, size_t size) {
    process_t *p = process_get_current();
    if (!p || !p->is_user) return -1;
    if (!addr || size == 0) return -1;

    uint64_t v = (uint64_t)(uintptr_t)addr;
    if (v & 0xFFFULL) return -1;

    uint64_t sz = ((uint64_t)size + 0xFFFULL) & ~0xFFFULL;
    uint64_t end = v + sz;

    for (uint64_t cur = v; cur < end; cur += 0x1000ULL) {
        if (paging_virt_to_phys(cur) != 0) {
            paging_unmap_page(cur);
        }
    }

    /* NOTE: physical pages are currently leaked because we don't track phys per mapping.
     * This is acceptable for an early ld.so MVP.
     */
    return 0;
}

int sys_kill(int pid, int sig) {
    process_kill(pid);
    (void)sig;
    return 0;
}

#include "moduos/arch/AMD64/interrupts/timer.h"

// Time API: milliseconds since boot (PIT ticks). PIT runs at 100Hz by default.
uint64_t sys_time(void) {
    const uint64_t ticks = get_system_ticks();
    return ticks * 10ULL; // 100Hz => 10ms per tick
}
int sys_stat(const char *path, void *out_info, size_t out_size) {
    if (!path || !out_info || out_size < sizeof(fs_file_info_t)) return -1;

    process_t *proc = process_get_current();
    if (!proc) return -1;

    char kpath[256];
    if (copy_string_from_user(path, kpath, sizeof(kpath)) != 0) return -1;

    // Resolve relative paths against CWD
    const char *p = kpath;
    char full_path[256];
    if (!(p[0] == '/' || (p[0] == '$' && p[1] == '/'))) {
        const char *cwd = (proc->cwd[0] ? proc->cwd : "/");
        strncpy(full_path, cwd, sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = 0;
        size_t len = strlen(full_path);
        if (len == 0 || full_path[len - 1] != '/') {
            strncat(full_path, "/", sizeof(full_path) - strlen(full_path) - 1);
        }
        strncat(full_path, p, sizeof(full_path) - strlen(full_path) - 1);
        p = full_path;
    }

    fs_path_resolved_t r;
    if (fs_resolve_path(proc, p, &r) != 0) return -1;
    if (r.route == FS_ROUTE_DEVVFS) {
        return -1;
    }

    int slot = (r.route == FS_ROUTE_MOUNT) ? r.mount_slot : proc->current_slot;
    if (slot < 0) return -1;

    fs_mount_t *mount = fs_get_mount(slot);
    if (!mount || !mount->valid) return -1;

    fs_file_info_t info;
    int rc = fs_stat(mount, r.rel_path, &info);
    if (rc != 0) return -1;

    if (usercopy_to_user(out_info, &info, sizeof(info)) != 0) return -1;
    return 0;
}

off_t sys_lseek(int fd, off_t offset, int whence) {
    return fd_lseek(fd, offset, whence);
}

int sys_mkdir(const char *path) {
    (void)path;
    // TODO: Implement via FAT32 write support
    return -1;
}

int sys_rmdir(const char *path) {
    (void)path;
    // TODO: Implement via FAT32 write support
    return -1;
}

int sys_unlink(const char *path) {
    (void)path;
    // TODO: Implement via FAT32 write support
    return -1;
}

int sys_chdir(const char *path) {
    if (!path) return -1;

    process_t *proc = process_get_current();
    if (!proc) return -1;

    char kpath[256];
    if (copy_string_from_user(path, kpath, sizeof(kpath)) != 0) return -1;

    // Resolve relative paths against CWD for both / and $/ namespaces
    const char *p = kpath;
    char full_path[256];
    if (!(p[0] == '/' || (p[0] == '$' && p[1] == '/'))) {
        const char *cwd = (proc->cwd[0] ? proc->cwd : "/");
        strncpy(full_path, cwd, sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = 0;
        size_t len = strlen(full_path);
        if (len == 0 || full_path[len - 1] != '/') {
            strncat(full_path, "/", sizeof(full_path) - strlen(full_path) - 1);
        }
        strncat(full_path, p, sizeof(full_path) - strlen(full_path) - 1);
        p = full_path;
    }

    // DEVVFS routing: $/mnt/<drive>/... maps to a real mount
    if (p[0] == '$' && p[1] == '/') {
        // Normalize $/ paths so proc->cwd never contains /.. or /.
        char norm[256];
        strncpy(norm, p, sizeof(norm) - 1);
        norm[sizeof(norm) - 1] = 0;
        path_normalize_inplace(norm);
        p = norm;
        fs_path_resolved_t r;
        if (fs_resolve_path(proc, p, &r) != 0) return -1;
        if (r.route != FS_ROUTE_MOUNT) return -1;
        fs_mount_t *mount = fs_get_mount(r.mount_slot);
        if (!mount || !mount->valid) return -1;
        if (!fs_directory_exists(mount, r.rel_path)) return -1;
        strncpy(proc->cwd, p, sizeof(proc->cwd) - 1);
        proc->cwd[sizeof(proc->cwd) - 1] = 0;
        return 0;
    }

    // Normal FS chdir for / paths
    // If no filesystem is mounted, can't change directory
    if (proc->current_slot < 0) return -1;

    // Get the mount
    fs_mount_t* mount = fs_get_mount(proc->current_slot);
    if (!mount || !mount->valid) return -1;

    // Build absolute path
    char new_path[256];
    if (p[0] == '/') {
        // Absolute path
        strncpy(new_path, p, sizeof(new_path) - 1);
        new_path[sizeof(new_path) - 1] = '\0';
    } else {
        // Relative path - join with current directory
        if (strcmp(proc->cwd, "/") == 0) {
            strcpy(new_path, "/");
            strncat(new_path, p, sizeof(new_path) - strlen(new_path) - 1);
        } else {
            strcpy(new_path, proc->cwd);
            strcat(new_path, "/");
            strncat(new_path, p, sizeof(new_path) - strlen(new_path) - 1);
        }
    }
    
    // Handle special cases
    if (strcmp(path, "..") == 0) {
        // Go to parent directory
        char *last_slash = NULL;
        for (char *p = proc->cwd; *p; p++) {
            if (*p == '/') last_slash = p;
        }
        if (last_slash == proc->cwd) {
            strcpy(new_path, "/");
        } else if (last_slash) {
            strncpy(new_path, proc->cwd, last_slash - proc->cwd);
            new_path[last_slash - proc->cwd] = '\0';
        } else {
            strcpy(new_path, "/");
        }
    } else if (strcmp(path, ".") == 0) {
        // Stay in current directory
        return 0;
    }
    
    // Normalize path (remove trailing slashes)
    size_t len = strlen(new_path);
    if (len > 1 && new_path[len - 1] == '/') {
        new_path[len - 1] = '\0';
    }
    
    // Verify the directory exists
    if (!fs_directory_exists(mount, new_path)) {
        return -1;
    }
    
    // Update process CWD
    strncpy(proc->cwd, new_path, sizeof(proc->cwd) - 1);
    proc->cwd[sizeof(proc->cwd) - 1] = '\0';
    
    return 0;
}

char* sys_getcwd(char *buf, size_t size) {
    process_t *proc = process_get_current();
    if (!proc) return NULL;

    /* Never return a kernel pointer to userland.
     * With proper ring3, user code cannot dereference kernel addresses.
     */
    if (!buf || size == 0) return NULL;

    /* Copy into kernel temporary then into user buffer safely. */
    char tmp[256];
    strncpy(tmp, proc->cwd, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;

    size_t n = strlen(tmp);
    if (n + 1 > size) n = size - 1;
    tmp[n] = 0;

    if (usercopy_to_user(buf, tmp, n + 1) != 0) return NULL;
    return buf;
}
int sys_opendir(const char *path) {
    if (!path) return -1;

    /* Copy path from userspace */
    char kernel_path[256];
    if (copy_string_from_user(path, kernel_path, sizeof(kernel_path)) != 0) {
        return -1;
    }

    process_t *proc = process_get_current();
    if (!proc) return -1;

    // Resolve relative paths against CWD
    const char *p = kernel_path;
    char full_path[256];
    if (!(p[0] == '/' || (p[0] == '$' && p[1] == '/'))) {
        const char *cwd = (proc->cwd[0] ? proc->cwd : "/");
        strncpy(full_path, cwd, sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = 0;
        size_t len = strlen(full_path);
        if (len == 0 || full_path[len - 1] != '/') {
            strncat(full_path, "/", sizeof(full_path) - strlen(full_path) - 1);
        }
        strncat(full_path, p, sizeof(full_path) - strlen(full_path) - 1);
        p = full_path;
    }

    // Normalize both / and $/ paths so opendir(".") works reliably.
    char norm[256];
    if ((p[0] == '/') || (p[0] == '$' && p[1] == '/')) {
        strncpy(norm, p, sizeof(norm) - 1);
        norm[sizeof(norm) - 1] = 0;
        path_normalize_inplace(norm);
        p = norm;
    }

    fs_path_resolved_t r;
    if (fs_resolve_path(proc, p, &r) != 0) return -1;
    if (r.route == FS_ROUTE_DEVVFS) {
        if (r.devvfs_kind == 0 || r.devvfs_kind == 1) {
            return fd_devvfs_opendir(r.devvfs_kind);
        }
        if (r.devvfs_kind == 2) {
            const char *sub = r.rel_path;
            while (*sub == '/') sub++;
            return fd_devvfs_opendir_dev(sub);
        }
        return -1;
    }

    int slot = (r.route == FS_ROUTE_MOUNT) ? r.mount_slot : proc->current_slot;
    if (slot < 0) return -1;

    return fd_opendir(slot, r.rel_path);
}

int sys_readdir(int fd, char *name_buf, size_t buf_size, int *is_dir, uint32_t *size) {
    if (!name_buf || buf_size == 0) return -1;

    /* Read into kernel temporaries, then copy to user */
    if (buf_size > 256) buf_size = 256;
    char kname[256];
    int k_is_dir = 0;
    uint32_t k_size = 0;

    int rc = fd_readdir(fd, kname, buf_size, &k_is_dir, &k_size);
    if (rc <= 0) {
        /* 0=end of dir, <0=error */
        return rc;
    }

    /* rc==1: we have a valid entry in kname/k_is_dir/k_size */
    if (usercopy_to_user(name_buf, kname, buf_size) != 0) return -1;
    if (is_dir && usercopy_to_user(is_dir, &k_is_dir, sizeof(k_is_dir)) != 0) return -1;
    if (size && usercopy_to_user(size, &k_size, sizeof(k_size)) != 0) return -1;
    return 1;
}

static int sys_vfs_mkfs(const vfs_mkfs_req_t *user_req) {
    if (!user_req) return -1;

    vfs_mkfs_req_t req;
    if (usercopy_from_user(&req, user_req, sizeof(req)) != 0) return -1;
    req.fs_name[sizeof(req.fs_name) - 1] = 0;
    req.label[sizeof(req.label) - 1] = 0;

    if (req.vdrive_id < 0) return -2;
    if (req.sectors == 0) return -3;

    const char *label = req.label[0] ? req.label : NULL;

    if (strcmp(req.fs_name, "fat32") == 0) {
        // Match Windows' typical FAT32 formatter limit (~32GiB): refuse unless forced.
        if (req.sectors > 67108864u && (req.flags & VFS_MKFS_FLAG_FORCE) == 0) {
            return -10;
        }

        uint32_t spc = req.fat32_sectors_per_cluster;
        if (spc == 0) {
            // Auto-pick sectors-per-cluster based on volume size (in 512B sectors).
            uint32_t s = req.sectors;
            if (s <= 532480u) spc = 1;            // <= ~260MiB => 512B clusters
            else if (s <= 16777216u) spc = 8;     // <= 8GiB   => 4KiB clusters
            else if (s <= 33554432u) spc = 16;    // <= 16GiB  => 8KiB clusters
            else if (s <= 67108864u) spc = 32;    // <= 32GiB  => 16KiB clusters
            else spc = 64;                        // > 32GiB   => 32KiB clusters
        }

        return fs_format(req.vdrive_id, req.start_lba, req.sectors, label, spc);
    }

    return fs_ext_mkfs(req.fs_name, req.vdrive_id, req.start_lba, req.sectors, label);
}

static int sys_vfs_getpart(const vfs_part_req_t *user_req, vfs_part_info_t *user_out) {
    if (!user_req || !user_out) return -1;

    vfs_part_req_t req;
    if (usercopy_from_user(&req, user_req, sizeof(req)) != 0) return -1;

    if (req.vdrive_id < 0) return -2;
    if (req.part_no < 1 || req.part_no > 4) return -3;

    vfs_part_info_t out;
    memset(&out, 0, sizeof(out));

    uint32_t lba = 0, secs = 0;
    uint8_t type = 0;
    int rc = fs_mbr_get_partition(req.vdrive_id, req.part_no, &lba, &secs, &type);
    if (rc != 0) return -4;

    out.start_lba = lba;
    out.sectors = secs;
    out.type = type;

    if (usercopy_to_user(user_out, &out, sizeof(out)) != 0) return -1;
    return 0;
}

int sys_closedir(int fd) {
    return fd_closedir(fd);
}