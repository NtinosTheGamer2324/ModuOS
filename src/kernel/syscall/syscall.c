#include "moduos/kernel/syscall/syscall.h"
#include "moduos/kernel/md64api.h"
#include "moduos/kernel/process/process.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/interrupts/idt.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/macros.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/fs/fs.h"
#include "moduos/fs/fd.h"
#include "moduos/kernel/exec.h"
#include "moduos/drivers/input/input.h"

extern void syscall_entry(void);

void syscall_init(void) {
    COM_LOG_INFO(COM1_PORT, "Initializing system calls");
    idt_set_entry(0x80, syscall_entry, 0xEE);
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
        case SYS_SLEEP:   return sys_sleep((unsigned int)arg1);
        case SYS_YIELD:   sys_yield(); return 0;
        case SYS_MALLOC:  return (uint64_t)sys_malloc((size_t)arg1);
        case SYS_FREE:    sys_free((void*)arg1); return 0;
        case SYS_KILL:    return sys_kill((int)arg1, (int)arg2);
        case SYS_TIME:    return sys_time();
        case SYS_EXEC:    return sys_exec((const char*)arg1);
        case SYS_INPUT:   return (uint64_t)sys_input();
        case SYS_SSTATS:
            return (uint64_t)sys_get_sysinfo();
        default:
            com_write_string(COM1_PORT, "[SYSCALL] Unknown syscall: ");
            itoa(syscall_num, buf, 10);
            com_write_string(COM1_PORT, buf);
            com_write_string(COM1_PORT, "\n");
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
    process_exit(status);
    return 0;
}

int sys_fork(void) {
    COM_LOG_WARN(COM1_PORT, "fork() not yet implemented");
    return -1;
}

ssize_t sys_read(int fd, void *buf, size_t count) {
    if (!buf) return -1;
    ssize_t result = fd_read(fd, buf, count);
    
    // DEBUG
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
    
    return result;
}

md64api_sysinfo_data* sys_get_sysinfo(void) {
    static md64api_sysinfo_data info;
    info = get_system_info(); // populate the static struct
    return &info;
}

ssize_t sys_writefile(int fd, const char *str, size_t count) {
    if (!str) return -1;

    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        VGA_WriteN(str, count);
        return (ssize_t)count;
    }

    return fd_write(fd, str, count);
}

int sys_write(const char *str) {
    if (!str) {
        com_write_string(COM1_PORT, "[SYS_WRITE] NULL pointer!\n");
        return -1;
    }
    VGA_Write(str);
    
    return 0;
}

char* sys_input() {
    return input();
}

int sys_exec(const char *str) {
    exec(str);
    return 0;
}

int sys_open(const char *pathname, int flags, int mode) {
    if (!pathname) return -1;
    return fd_open(0, pathname, flags, mode);
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

void* sys_malloc(size_t size) { return kmalloc(size); }

void sys_free(void *ptr) { kfree(ptr); }

void* sys_sbrk(intptr_t increment) {
    (void)increment;
    return (void*)-1;
}

int sys_kill(int pid, int sig) {
    process_kill(pid);
    (void)sig;
    return 0;
}

uint64_t sys_time(void) { return 0; }