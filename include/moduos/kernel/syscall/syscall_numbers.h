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
