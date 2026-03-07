#include "moduos/kernel/syscall/syscall.h"
#include "moduos/fs/mkfs.h"
#include "moduos/fs/part.h"
#include "moduos/fs/MDFS/mdfs.h"
#include "moduos/drivers/Drive/vDrive.h"
#include "moduos/fs/devfs.h"
#include "moduos/fs/userfs_user_api.h"
#include "moduos/fs/userfs.h"
#include "moduos/kernel/syscall/userfs_user.h"
#include "moduos/kernel/syscall/syscall_numbers.h"
#include "moduos/kernel/md64api.h"
#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/user_identity.h"
#include "moduos/fs/userfs.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/interrupts/idt.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/debug.h"
#include "moduos/kernel/macros.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/md64api_user.h"
#include "moduos/kernel/md64api_pidinfo_user.h"
#include "moduos/kernel/process/proclist_user.h"
#include "moduos/fs/fs.h"
#include "moduos/fs/fd.h"
#include "moduos/fs/path.h"
#include "moduos/fs/path_norm.h"
#include "moduos/kernel/exec.h"
#include "moduos/kernel/syscall/execve_impl.h"
#include "moduos/drivers/input/input.h"
#include "moduos/kernel/memory/usercopy.h"
#include "moduos/kernel/errno.h"
#include "moduos/arch/AMD64/syscall/syscall64.h"

/* Forward declarations */
uint64_t sys_signal(int sig, uint64_t handler);
int sys_raise(int sig);
int sys_fd_inject(uint32_t pid, int fd, void *arg);

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

volatile uint64_t g_last_syscall_num = 0;
volatile uint64_t g_last_syscall_args[5] = {0,0,0,0,0};
volatile uint64_t g_syscall_entry_rbp = 0;

static int sys_vfs_mkfs(const vfs_mkfs_req_t *user_req);
static int sys_vfs_getpart(const vfs_part_req_t *user_req, vfs_part_info_t *user_out);
static int sys_vfs_mbrinit(const vfs_mbrinit_req_t *user_req);
static int sys_pidinfo(uint32_t pid, md64api_pid_info_u *out, size_t out_size);
static int sys_proclist(md_proclist_entry_u *out, size_t out_bytes);
static int sys_mount(int vdrive_id, uint32_t partition_lba, int fs_type);
static int sys_unmount(int slot);
static int sys_mounts(char *user_buf, size_t buflen);

void syscall_init(void) {
    COM_LOG_INFO(COM1_PORT, "Initializing system calls");

    /* AMD64 SYSCALL/SYSRET (faster ~3-10x than INT 0x80) */
    amd64_syscall_init();
    idt_set_entry(0x80, syscall_entry, 0xEF);

    fd_init();
    COM_LOG_OK(COM1_PORT, "System calls initialized (SYSCALL/SYSRET) plus 0x80");
}

/* Forward declaration — defined in signals_new.c */
extern void check_signals(void);

uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    g_last_syscall_num = syscall_num;
    g_last_syscall_args[0] = arg1;
    g_last_syscall_args[1] = arg2;
    g_last_syscall_args[2] = arg3;
    g_last_syscall_args[3] = arg4;
    g_last_syscall_args[4] = arg5;
    
    // DEBUG: Check PID 2 before each syscall from PID 4
    extern process_t *process_get_by_pid(uint32_t);
    extern volatile process_t *current;
    if (current && current->pid == 4) {
        process_t *pid2 = process_get_by_pid(2);
        if (pid2 && pid2->page_table == 0) {
            com_write_string(COM1_PORT, "[SYSCALL] PID 2 corrupted BEFORE syscall ");
            char sbuf[16];
            itoa((int)syscall_num, sbuf, 10);
            com_write_string(COM1_PORT, sbuf);
            com_write_string(COM1_PORT, " from PID 4\n");
            for(;;) __asm__("hlt");
        }
    }
    /* Deliver any pending signals from the previous syscall or IRQ before
     * dispatching the new syscall.  This is the earliest safe point where
     * we have a valid kernel stack and can call do_exit() if needed. */
    if (syscall_num != SYS_EXIT)
        check_signals();

    char buf[32];
    switch (syscall_num) {
        case SYS_EXIT:    
            // com_printf(COM1_PORT, "[EXIT] PID %d exiting with status %d\n", current->pid, (int)arg1);
            return sys_exit((int)arg1);
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
            if (!p) return (uint64_t)-(int64_t)EPERM;
            if (p->uid == KERNEL_UID) return (uint64_t)-(int64_t)EPERM;
            if ((uint32_t)arg1 == KERNEL_UID) return (uint64_t)-(int64_t)EPERM;
            if (p->uid != 0) return (uint64_t)-(int64_t)EPERM;
            p->uid = (uint32_t)arg1;
            p->euid = (uint32_t)arg1;
            return 0;
        }

        case SYS_SLEEP:   return sys_sleep((unsigned int)arg1);
        case SYS_YIELD:   sys_yield(); return 0;
        case SYS_SBRK:    return (uint64_t)sys_sbrk((intptr_t)arg1);
        case SYS_KILL:    return sys_kill((int)arg1, (int)arg2);
        case SYS_SIGNAL:  return sys_signal((int)arg1, (uint64_t)arg2);
        case SYS_RAISE:   return sys_raise((int)arg1);
        case SYS_FD_INJECT: return sys_fd_inject((uint32_t)arg1, (int)arg2, (void*)arg3);
        case SYS_TIME:    return sys_time();
        case SYS_EXEC:    return sys_exec((const char*)arg1);
        case SYS_EXECVE:  
            // com_printf(COM1_PORT, "[SYSCALL] PID %d calling SYS_EXECVE path=%s\n", current->pid, (const char*)arg1);
            return sys_execve_impl((const char*)arg1, (char *const*)arg2, (char *const*)arg3);
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
        /* SYS_SSTATS (29) removed — use $/dev/md64api/sysinfo via DevFS instead. */

        case SYS_MMAP:
            return (uint64_t)sys_mmap((void*)arg1, (size_t)arg2, (int)arg3, (int)arg4);
        case SYS_MUNMAP:
            return (uint64_t)sys_munmap((void*)arg1, (size_t)arg2);

        case SYS_VFS_MKFS:
            return (uint64_t)sys_vfs_mkfs((const vfs_mkfs_req_t*)arg1);
        case SYS_VFS_GETPART:
            return (uint64_t)sys_vfs_getpart((const vfs_part_req_t*)arg1, (vfs_part_info_t*)arg2);
        case SYS_VFS_MBRINIT:
            return (uint64_t)sys_vfs_mbrinit((const vfs_mbrinit_req_t*)arg1);
        case SYS_USERFS_REGISTER:
            return (uint64_t)sys_userfs_register((const userfs_user_node_t*)arg1);
        case SYS_PROCLIST:
            return (uint64_t)sys_proclist((md_proclist_entry_u*)arg1, (size_t)arg2);
        case SYS_PIDINFO:
            return (uint64_t)sys_pidinfo((uint32_t)arg1, (md64api_pid_info_u*)arg2, (size_t)arg3);
        case SYS_MOUNT:
            return (uint64_t)sys_mount((int)arg1, (uint32_t)arg2, (int)arg3);
        case SYS_UNMOUNT:
            return (uint64_t)sys_unmount((int)arg1);
        case SYS_MOUNTS:
            return (uint64_t)sys_mounts((char*)arg1, (size_t)arg2);

        case SYS_DUP: {
            extern int fd_dup(int);
            return (uint64_t)fd_dup((int)arg1);
        }

        case SYS_DUP2: {
            extern int fd_dup2(int, int);
            return (uint64_t)fd_dup2((int)arg1, (int)arg2);
        }

        case SYS_PIPE: {
            extern int fd_pipe(int[2]);
            int k_fds[2] = {-1, -1};
            int rc = fd_pipe(k_fds);
            if (rc == 0 && arg1)
                usercopy_to_user((void*)arg1, k_fds, sizeof(k_fds));
            return (uint64_t)(int64_t)rc;
        }

        case SYS_GETGID: {
            process_t *p = process_get_current();
            return p ? p->gid : 0;
        }

        case SYS_SETGID: {
            process_t *p = process_get_current();
            if (!p) return (uint64_t)-(int64_t)EPERM;
            if (p->uid != 0) return (uint64_t)-(int64_t)EPERM;
            p->gid = (uint32_t)arg1;
            p->egid = (uint32_t)arg1;
            return 0;
        }

        case SYS_GETEUID: {
            process_t *p = process_get_current();
            return p ? p->euid : 0;
        }

        case SYS_GETEGID: {
            process_t *p = process_get_current();
            return p ? p->egid : 0;
        }

        case SYS_WAITX: {
            com_write_string(COM1_PORT, "[SYSCALL] SYS_WAITX entered, status ptr=0x");
            com_write_hex64(COM1_PORT, arg2);
            com_write_string(COM1_PORT, "\n");
            int wstatus = 0;
            pid_t r = do_waitpid((int32_t)arg1, &wstatus, (int)arg3);
            com_write_string(COM1_PORT, "[SYSCALL] do_waitpid returned, about to copy to userspace\n");
            if (arg2 && r > 0)
                usercopy_to_user((void*)arg2, &wstatus, sizeof(wstatus));
            com_write_string(COM1_PORT, "[SYSCALL] SYS_WAITX about to return\n");
            return (uint64_t)(int64_t)r;
        }

        // GPU Core syscalls (like Linux DRM) - Simple primitives, driver-agnostic
        // TODO: Implement basic GPU memory/command syscalls
        
        default:
            if (kernel_debug_is_med()) {
                com_write_string(COM1_PORT, "[SYSCALL] Unknown syscall: ");
                itoa(syscall_num, buf, 10);
                com_write_string(COM1_PORT, buf);
                com_write_string(COM1_PORT, "\n");
            }
            return (uint64_t)(-(int64_t)ENOSYS);
    }
    
    // NOTE: Code never reaches here - all cases return directly
}

/* ============================================================
   SYSTEM CALL IMPLEMENTATIONS
   ============================================================ */

int sys_exit(int status) {
    process_t* proc = process_get_current();
    if (proc) {
        const char *owner = user_identity_get(proc);
        if (owner && owner[0]) {
            userfs_owner_exited(owner);
        }
        fd_close_all(proc->pid);
        com_write_string(COM1_PORT, "[SYS_EXIT] pid=");
        char pbuf[12];
        itoa((int)proc->pid, pbuf, 10);
        com_write_string(COM1_PORT, pbuf);
        com_write_string(COM1_PORT, " status=");
        char sbuf[16];
        itoa(status, sbuf, 10);
        com_write_string(COM1_PORT, sbuf);
        com_write_string(COM1_PORT, "\n");
    }

    // Use new POSIX-compliant exit
    do_exit(status);

    /* Fallback: if do_exit ever returns, halt. */
    for (;;) { __asm__ volatile("hlt"); }
}

int sys_fork(void) {
    // sys_fork_impl() copies the kernel stack and resumes the child at
    // syscall_entry_return with rax=0 — the correct POSIX fork() semantics.
    extern int sys_fork_impl(void);
    return sys_fork_impl();
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


static void safe_strcpy(char *dst, size_t dst_sz, const char *src) {
    if (!dst || dst_sz == 0) return;
    if (!src) src = "";
    size_t i = 0;
    for (; i + 1 < dst_sz && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}


ssize_t sys_writefile(int fd, const char *user_buf, size_t count) {
    if (!user_buf) return -1;

    // Write to console: still copy from user, but chunk it.
    // Write to files: MUST use kmalloc() buffer (DMA-safe) and chunked copyin.
    const size_t CHUNK = 4096;
    char *kbuf = (char*)kmalloc(CHUNK);
    if (!kbuf) return -1;

    size_t total = 0;
    while (total < count) {
        size_t n = count - total;
        if (n > CHUNK) n = CHUNK;

        if (n && usercopy_from_user(kbuf, user_buf + total, n) != 0) {
            kfree(kbuf);
            return -1;
        }

        if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
            VGA_WriteN(kbuf, n);
            total += n;
            continue;
        }

        ssize_t wr = fd_write(fd, kbuf, n);
        if (wr < 0) {
            kfree(kbuf);
            return wr;
        }
        if ((size_t)wr == 0) break;
        total += (size_t)wr;
        if ((size_t)wr < n) break;
    }

    kfree(kbuf);
    return (ssize_t)total;
}

int sys_write(const char *str) {
    if (!str) {
        if (kernel_debug_is_med()) {
            com_write_string(COM1_PORT, "[SYS_WRITE] NULL pointer!\n");
        }
        return -1;
    }

    /* LEGACY SYSCALL: sys_write(str) is deprecated!
     * New code should use: write(fd, buf, count) via fd_write()
     * This legacy path writes to stdout (FD 1) for backwards compatibility.
     */
    
    char tmp[512];
    if (usercopy_string_from_user(tmp, str, sizeof(tmp)) != 0) return -1;
    
    size_t len = strlen(tmp);
    
    /* Write to stdout (FD 1) using proper FD routing.
     * This will route to $/dev/console, $/dev/graphics/video0, or $/user/ttyman/tty1/stdin
     * depending on what the process has FD 1 pointed to. */
    ssize_t written = fd_write(1, tmp, len);
    
    return (written >= 0) ? 0 : -1;
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

    if (r.route == FS_ROUTE_USERLAND) {
        const char *node = r.rel_path;
        while (*node == '/') node++;
        if (!*node) return -1;

        return fd_open_userfs(node, flags);
    }

    int slot = (r.route == FS_ROUTE_MOUNT) ? r.mount_slot : proc->current_slot;
    if (slot < 0) return -1;

    return fd_open(slot, r.rel_path, flags, mode);
}

int sys_close(int fd) { return fd_close(fd); }

int sys_wait(int *status) {
    // Use new POSIX-compliant wait
    int exit_status = 0;
    pid_t child_pid = do_wait(&exit_status);
    if (child_pid > 0 && status) {
        if (usercopy_to_user(status, &exit_status, sizeof(exit_status)) != 0) {
            return -1;
        }
    }
    return child_pid;
}

int sys_getpid(void) {
    process_t *proc = process_get_current();
    return proc ? (int)proc->pid : -1;
}

int sys_getppid(void) {
    process_t *proc = process_get_current();
    return proc ? (int)proc->parent_pid : -1;
}

int sys_sleep(unsigned int seconds) {
    process_sleep(seconds * 1000);
    return 0;
}

void sys_yield(void) {
    /* Request a reschedule after returning to userspace.
     * Avoid context switching on the syscall stack.
     */
    process_yield();
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
        
        /* Get current process's page table */
        uint64_t proc_cr3 = p->page_table;
        uint64_t *proc_pml4 = (uint64_t*)phys_to_virt_kernel(proc_cr3 & 0xFFFFFFFFFFFFF000ULL);
        if (!proc_pml4) {
            for (size_t i = 0; i < pages; i++) phys_free_frame(phys + (uint64_t)i * 0x1000ULL);
            return (void*)-1;
        }
        
        if (paging_map_range_to_pml4(proc_pml4, map_start, phys, (uint64_t)pages * 0x1000ULL, PFLAG_PRESENT | PFLAG_WRITABLE | PFLAG_USER) != 0) {
            for (size_t i = 0; i < pages; i++) phys_free_frame(phys + (uint64_t)i * 0x1000ULL);
            return (void*)-1;
        }
        
        /* Zero the allocated pages */
        memset((void*)(uintptr_t)map_start, 0, (size_t)((map_end - map_start)));
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
        uint64_t phys = paging_virt_to_phys(cur);
        if (phys != 0) {
            paging_unmap_page(cur);
            phys_free_frame(phys & ~0xFFFULL);
        }
    }

    return 0;
}

int sys_kill(int pid, int sig) {
    return send_signal((uint32_t)pid, sig);
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
    if (!path) return -1;

    process_t *proc = process_get_current();
    if (!proc) return -1;

    char kpath[256];
    if (copy_string_from_user(path, kpath, sizeof(kpath)) != 0) return -1;

    // Resolve relative paths against CWD (supports both / and $/ namespaces)
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

    // Normalize for stable behavior.
    char norm[256];
    strncpy(norm, p, sizeof(norm) - 1);
    norm[sizeof(norm) - 1] = 0;
    path_normalize_inplace(norm);
    p = norm;

    fs_path_resolved_t r;
    if (fs_resolve_path(proc, p, &r) != 0) return -1;

    // RULE: userland must not create DEVFS directories
    if (r.route == FS_ROUTE_DEVVFS) {
        return -1;
    }

    if (r.route == FS_ROUTE_USERLAND) {
        return -30; // userfs has no mkdir support
    }

    int slot = (r.route == FS_ROUTE_MOUNT) ? r.mount_slot : proc->current_slot;
    if (slot < 0) return -1;

    fs_mount_t *mount = fs_get_mount(slot);
    if (!mount || !mount->valid) return -1;

    return fs_mkdir(mount, r.rel_path);
}

int sys_rmdir(const char *path) {
    if (!path) return -1;

    process_t *proc = process_get_current();
    if (!proc) return -1;

    char kpath[256];
    if (copy_string_from_user(path, kpath, sizeof(kpath)) != 0) return -1;

    // Resolve relative paths against CWD (supports both / and $/ namespaces)
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

    // Normalize for stable behavior.
    char norm[256];
    strncpy(norm, p, sizeof(norm) - 1);
    norm[sizeof(norm) - 1] = 0;
    path_normalize_inplace(norm);
    p = norm;

    fs_path_resolved_t r;
    if (fs_resolve_path(proc, p, &r) != 0) return -1;

    // RULE: userland must not remove DEVFS directories
    if (r.route == FS_ROUTE_DEVVFS) {
        return -1;
    }

    int slot = (r.route == FS_ROUTE_MOUNT) ? r.mount_slot : proc->current_slot;
    if (slot < 0) return -1;

    fs_mount_t *mount = fs_get_mount(slot);
    if (!mount || !mount->valid) return -1;

    return fs_rmdir(mount, r.rel_path);
}

int sys_unlink(const char *path) {
    if (!path) return -1;

    process_t *proc = process_get_current();
    if (!proc) return -1;

    char kpath[256];
    if (copy_string_from_user(path, kpath, sizeof(kpath)) != 0) return -1;

    // Resolve relative paths against CWD (supports both / and $/ namespaces)
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

    // Normalize for stable behavior.
    char norm[256];
    strncpy(norm, p, sizeof(norm) - 1);
    norm[sizeof(norm) - 1] = 0;
    path_normalize_inplace(norm);
    p = norm;

    fs_path_resolved_t r;
    if (fs_resolve_path(proc, p, &r) != 0) return -1;

    // RULE: userland must not delete DEVFS entries
    if (r.route == FS_ROUTE_DEVVFS) {
        return -1;
    }

    int slot = (r.route == FS_ROUTE_MOUNT) ? r.mount_slot : proc->current_slot;
    if (slot < 0) return -1;

    fs_mount_t *mount = fs_get_mount(slot);
    if (!mount || !mount->valid) return -1;

    return fs_unlink(mount, r.rel_path);
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
        if (r.route == FS_ROUTE_USERLAND) {
            const char *node = r.rel_path;
            while (*node == '/') node++;
            if (strncmp(node, "user", 4) == 0 && (node[4] == 0 || node[4] == '/')) {
                node += 4;
                while (*node == '/') node++;
            }
            if (!userfs_directory_exists(node)) return -1;
            strncpy(proc->cwd, p, sizeof(proc->cwd) - 1);
            proc->cwd[sizeof(proc->cwd) - 1] = 0;
            return 0;
        }
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
    
    // Handle special cases (use kernel copy kpath, not the user pointer path)
    if (strcmp(kpath, "..") == 0) {
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
    } else if (strcmp(kpath, ".") == 0) {
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

    if (r.route == FS_ROUTE_USERLAND) {
        return fd_devvfs_opendir(3);
    }

    int slot = (r.route == FS_ROUTE_MOUNT) ? r.mount_slot : proc->current_slot;
    if (slot < 0) return -1;

    return fd_opendir(slot, r.rel_path);
}

int sys_readdir(int fd, char *name_buf, size_t buf_size, int *is_dir, uint32_t *size) {
    if (!name_buf || buf_size == 0) return -1;

    if (buf_size > 256) buf_size = 256;
    char kname[256];
    int k_is_dir = 0;
    uint32_t k_size = 0;

    int rc = fd_readdir(fd, kname, buf_size, &k_is_dir, &k_size);
    if (rc <= 0) {
        return rc; /* 0=end of dir, <0=error */
    }

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

    int rc = 0;
    uint8_t mbr_type = 0;

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

        rc = fs_format(req.vdrive_id, req.start_lba, req.sectors, label, spc);
        if (rc == 0) mbr_type = 0x0C; // FAT32 LBA
    } else if (strcmp(req.fs_name, "mdfs") == 0) {
        rc = mdfs_mkfs(req.vdrive_id, req.start_lba, req.sectors, label);
        if (rc == 0) mbr_type = 0xE1; // ModularFS
    } else {
        rc = fs_ext_mkfs(req.fs_name, req.vdrive_id, req.start_lba, req.sectors, label);
        if (rc == 0 && strcmp(req.fs_name, "ext2") == 0) mbr_type = 0x83; // Linux
    }

    // Best-effort MBR type update: if the formatted region starts at an MBR partition,
    // update the partition type to match the filesystem.
    if (rc == 0 && mbr_type) {
        (void)fs_mbr_set_type_for_lba(req.vdrive_id, req.start_lba, mbr_type);
    }
    return rc;
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

static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static int sys_vfs_mbrinit(const vfs_mbrinit_req_t *user_req) {
    if (!user_req) return -1;

    vfs_mbrinit_req_t req;
    if (usercopy_from_user(&req, user_req, sizeof(req)) != 0) return -1;

    if (req.vdrive_id < 0) return -2;

    vdrive_t *d = vdrive_get((uint8_t)req.vdrive_id);
    if (!d || !d->present) return -3;
    if (d->read_only) return -4;
    if (d->sector_size != 512) return -5;

    uint8_t *mbr = (uint8_t*)kmalloc(512);
    if (!mbr) return -6;

    memset(mbr, 0, 512);

    int force = (req.flags & 1) != 0;
    if (!force) {
        if (vdrive_read_sector((uint8_t)req.vdrive_id, 0, mbr) == VDRIVE_SUCCESS) {
            if (mbr[510] == 0x55 && mbr[511] == 0xAA) {
                kfree(mbr);
                return -7;
            }
        }
        memset(mbr, 0, 512);
    }

    uint32_t start_lba = req.start_lba;
    uint32_t sectors = req.sectors;
    if (sectors == 0) {
        uint64_t total_sectors = d->total_sectors;
        if (total_sectors > start_lba) {
            uint64_t avail = total_sectors - (uint64_t)start_lba;
            sectors = (avail > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)avail;
        } else {
            kfree(mbr);
            return -8;
        }
    }

    uint8_t type = req.type ? req.type : 0x83;
    uint8_t bootable = req.bootable ? 0x80 : 0x00;

    uint8_t *ent = mbr + 0x1BE;
    ent[0] = bootable;
    ent[1] = 0x00;
    ent[2] = 0x02;
    ent[3] = 0x00;
    ent[4] = type;
    ent[5] = 0xFF;
    ent[6] = 0xFF;
    ent[7] = 0xFF;
    write_le32(ent + 8, start_lba);
    write_le32(ent + 12, sectors);

    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    int rc = 0;
    if (vdrive_write_sector((uint8_t)req.vdrive_id, 0, mbr) != VDRIVE_SUCCESS) {
        rc = -9;
    }

    kfree(mbr);
    return rc;
}

int sys_closedir(int fd) {
    return fd_closedir(fd);
}

// Signal syscalls
extern uint64_t do_signal(int sig, uint64_t handler);

uint64_t sys_signal(int sig, uint64_t handler) {
    return do_signal(sig, handler);
}

int sys_raise(int sig) {
    process_t *p = process_get_current();
    if (!p) return -1;
    return sys_kill((int)p->pid, sig);
}

// FD injection syscall (for privileged processes like TTY manager)
extern int process_inject_fd(uint32_t pid, int fd, void *fd_obj);

int sys_fd_inject(uint32_t pid, int fd, void *fd_obj) {
    // TODO: Add permission check - only allow root or TTY manager
    return process_inject_fd(pid, fd, fd_obj);
}

int sys_userfs_register(const userfs_user_node_t *user_node) {
    if (!user_node) return -1;

    process_t *p = process_get_current();
    if (!p) return -1;
    if (p->uid != 0) return -2; /* root only */

    userfs_user_node_t req;
    if (usercopy_from_user(&req, user_node, sizeof(req)) != 0) return -1;

    if (!req.path || !req.owner_id) return -1;

    char kpath[128];
    char kowner[64];
    if (usercopy_string_from_user(kpath, req.path, sizeof(kpath)) != 0) return -1;
    if (usercopy_string_from_user(kowner, req.owner_id, sizeof(kowner)) != 0) return -1;

    const char *path_in = kpath;

    com_write_string(COM1_PORT, "[USERFS] register ");
    com_write_string(COM1_PORT, kpath);
    com_write_string(COM1_PORT, " owner=");
    com_write_string(COM1_PORT, kowner);
    com_write_string(COM1_PORT, "\n");

    size_t owner_len = strlen(kowner) + 1;
    char *owner_copy = (char*)kmalloc(owner_len);
    if (!owner_copy) return -12;
    memcpy(owner_copy, kowner, owner_len);

    uint32_t perms = req.perms;
    int rc = userfs_register_user_path(path_in, owner_copy, perms);
    if (rc != 0) {
        kfree(owner_copy);
    }
    com_write_string(COM1_PORT, "[USERFS] register rc=");
    char rbuf[16];
    itoa(rc, rbuf, 10);
    com_write_string(COM1_PORT, rbuf);
    com_write_string(COM1_PORT, "\n");
    return rc;
}

static int sys_proclist(md_proclist_entry_u *out, size_t out_bytes) {
    if (!out || out_bytes == 0) return -1;

    size_t max_entries = out_bytes / sizeof(md_proclist_entry_u);
    if (max_entries == 0) return -1;
    if (max_entries > MAX_PROCESSES) max_entries = MAX_PROCESSES;

    md_proclist_entry_u *kbuf = (md_proclist_entry_u*)kmalloc(max_entries * sizeof(md_proclist_entry_u));
    if (!kbuf) return -1;

    size_t count = 0;
    for (size_t i = 0; i < MAX_PROCESSES && count < max_entries; i++) {
        process_t *p = process_get_by_pid((uint32_t)i);
        if (!p || p->pid == 0) continue;
        md_proclist_entry_u e;
        memset(&e, 0, sizeof(e));
        e.pid = p->pid;
        e.ppid = p->parent_pid;
        e.state = (uint32_t)p->state;
        e.total_time = p->total_time;
        safe_strcpy(e.name, sizeof(e.name), p->name);
        kbuf[count++] = e;
    }

    size_t out_size = count * sizeof(md_proclist_entry_u);
    int rc = usercopy_to_user(out, kbuf, out_size);
    kfree(kbuf);
    if (rc != 0) return -1;
    return (int)count;
}

static int sys_pidinfo(uint32_t pid, md64api_pid_info_u *out, size_t out_size) {
    if (!out || out_size < sizeof(md64api_pid_info_u)) return -1;
    if (pid >= MAX_PROCESSES) return -1;

    process_t *p = process_get_by_pid(pid);
    if (!p || p->pid == 0) return -1;

    md64api_pid_info_u info;
    memset(&info, 0, sizeof(info));
    info.pid = p->pid;
    info.ppid = p->parent_pid;
    info.uid = p->uid;
    info.gid = p->gid;
    info.state = (uint32_t)p->state;
    info.priority = (uint32_t)p->priority;
    info.total_time = p->total_time;
    info.user_rip = p->user_rip;
    info.user_rsp = p->user_rsp;
    info.is_user = (uint8_t)(p->is_user ? 1 : 0);

    if (p->user_image_end > p->user_image_base) {
        info.mem_image_bytes = p->user_image_end - p->user_image_base;
    }
    if (p->user_heap_end > p->user_heap_base) {
        info.mem_heap_bytes = p->user_heap_end - p->user_heap_base;
    }
    if (p->user_mmap_end > p->user_mmap_base) {
        info.mem_mmap_bytes = p->user_mmap_end - p->user_mmap_base;
    }
    if (p->user_stack_top > p->user_stack_low) {
        info.mem_stack_bytes = p->user_stack_top - p->user_stack_low;
    }
    info.mem_total_bytes = info.mem_image_bytes + info.mem_heap_bytes + info.mem_mmap_bytes + info.mem_stack_bytes;

    safe_strcpy(info.name, sizeof(info.name), p->name);
    safe_strcpy(info.cwd, sizeof(info.cwd), p->cwd);

    if (usercopy_to_user(out, &info, sizeof(info)) != 0) return -1;
    return 0;
}

/* Mount a filesystem on a drive/partition */
static int sys_mount(int vdrive_id, uint32_t partition_lba, int fs_type) {
    /* Basic validation */
    if (vdrive_id < 0 || vdrive_id >= 16) return -1;
    if (fs_type < 0 || fs_type > 10) return -1;
    
    /* Call kernel fs_mount_drive */
    int slot = fs_mount_drive(vdrive_id, partition_lba, (fs_type_t)fs_type);
    return slot;
}

/* Unmount a filesystem slot */
static int sys_unmount(int slot) {
    if (slot < 0) return -1;
    return fs_unmount_slot(slot);
}

/* List mounted filesystems */
static int sys_mounts(char *user_buf, size_t buflen) {
    if (!user_buf || buflen == 0) return -1;
    
    /* Build mount list string in kernel buffer */
    char kbuf[2048];
    int pos = 0;
    int count = fs_get_mount_count();
    
    for (int slot = 0; slot < 16 && pos < (int)sizeof(kbuf) - 128; slot++) {
        int vdrive_id = -1;
        uint32_t partition_lba = 0;
        fs_type_t type = FS_TYPE_UNKNOWN;
        
        if (fs_get_mount_info(slot, &vdrive_id, &partition_lba, &type) == 0) {
            char label[64] = "";
            fs_get_mount_label(slot, label, sizeof(label));
            
            /* Format: "slot=X vdrive=Y lba=Z type=N label=L\n" */
            pos += snprintf(kbuf + pos, sizeof(kbuf) - pos,
                           "slot=%d vdrive=%d lba=%u type=%d label=%s\n",
                           slot, vdrive_id, partition_lba, (int)type, label);
        }
    }
    
    /* Copy to userspace */
    size_t copy_len = (size_t)pos;
    if (copy_len > buflen - 1) copy_len = buflen - 1;
    
    if (usercopy_to_user(user_buf, kbuf, copy_len) != 0) return -1;
    
    /* Null terminate */
    char nul = 0;
    if (usercopy_to_user(user_buf + copy_len, &nul, 1) != 0) return -1;
    
    return count;
}
