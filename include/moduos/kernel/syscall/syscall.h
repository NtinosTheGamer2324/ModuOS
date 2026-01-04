#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include "moduos/kernel/md64api.h"
#include "moduos/kernel/md64api_user.h"
#include "moduos/fs/fd.h"  // for off_t

// ssize_t is defined in include/moduos/fs/fd.h

// System call numbers (shared with userspace)
#include "moduos/kernel/syscall/syscall_numbers.h"


// System call handler
void syscall_init(void);
uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2, 
                         uint64_t arg3, uint64_t arg4, uint64_t arg5);

// Individual system call implementations
int sys_exit(int status);
int sys_fork(void);
ssize_t sys_read(int fd, void *buf, size_t count);
ssize_t sys_writefile(int fd, const char *str, size_t count);
int     sys_write(const char *str);
int sys_open(const char *pathname, int flags, int mode);
int sys_close(int fd);
int sys_wait(int *status);
int sys_exec(const char *str);
int sys_getpid(void);
int sys_getppid(void);
int sys_sleep(unsigned int seconds);
void sys_yield(void);
void* sys_sbrk(intptr_t increment);
int sys_kill(int pid, int sig);
uint64_t sys_time(void);
ssize_t sys_input(char *buf, size_t max_len);

// FS/syscalls
int sys_chdir(const char *path);
char* sys_getcwd(char *buf, size_t size);
int sys_stat(const char *path, void *out_info, size_t out_size);
off_t sys_lseek(int fd, off_t offset, int whence);
int sys_mkdir(const char *path);
int sys_rmdir(const char *path);
int sys_unlink(const char *path);

int sys_opendir(const char *path);
int sys_readdir(int fd, char *name_buf, size_t buf_size, int *is_dir, uint32_t *size);
int sys_closedir(int fd);

/* VM mapping (for userland ld.so) */
void* sys_mmap(void *addr, size_t size, int prot, int flags);
int   sys_munmap(void *addr, size_t size);

md64api_sysinfo_data* sys_get_sysinfo(void);
int sys_get_sysinfo2(md64api_sysinfo_data_u *out, size_t out_size);  // Changed to return pointer

// VGA / Console
// Note: currently implemented directly in syscall_handler() and VGA driver.

#endif