#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include "moduos/kernel/md64api.h"

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef long ssize_t;
#endif

// System call numbers
#define SYS_EXIT        0
#define SYS_FORK        1
#define SYS_READ        2
#define SYS_WRITE       3
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
#define SYS_SSTATS      24
#define SYS_WRITEFILE   30


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
void* sys_malloc(size_t size);
void sys_free(void *ptr);
void* sys_sbrk(intptr_t increment);
int sys_kill(int pid, int sig);
uint64_t sys_time(void);
char* sys_input(void);

md64api_sysinfo_data* sys_get_sysinfo(void);  // Changed to return pointer

#endif