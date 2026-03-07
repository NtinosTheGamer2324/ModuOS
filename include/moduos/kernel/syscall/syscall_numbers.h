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
/* SYS_MALLOC/FREE removed - handled by userland libc */
#define SYS_SBRK        14  /* Low-level heap expansion only */
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
/* Graphics/VGA - REMOVED: Use $/dev/graphics/video0 or $/dev/console instead */
/* Virtual memory mapping (userland dynamic linker support) */
#define SYS_MMAP        39
#define SYS_MUNMAP      40
/* Networking - REMOVED: Use $/user/network/* (NetMan service) instead */
/* FS tracing (timing) */
#define SYS_FS_TRACE        55 /* arg1=0/1 set, returns previous state */
#define SYS_OPENDIR         56 /* opendir(path) -> dirfd */
#define SYS_READDIR         57 /* readdir(dirfd, namebuf, bufsz, is_dir*, size*) */
#define SYS_CLOSEDIR        58 /* closedir(dirfd) */

/* Userland USERFS nodes */
#define SYS_USERFS_REGISTER 64

/* Filesystem mount/unmount */
#define SYS_MOUNT       65  /* mount(vdrive_id, partition_lba, fs_type) -> slot or -errno */
#define SYS_UNMOUNT     66  /* unmount(slot) -> 0 or -errno */
#define SYS_MOUNTS      67  /* mounts(buf, buflen) -> count or -errno */

/* POSIX fd operations */
#define SYS_DUP         68  /* dup(oldfd) -> newfd or -errno */
#define SYS_DUP2        69  /* dup2(oldfd, newfd) -> newfd or -errno */
#define SYS_PIPE        70  /* pipe(fds[2]) -> 0 or -errno */
#define SYS_FCNTL       71  /* fcntl(fd, cmd, arg) -> result or -errno */

/* POSIX process identity */
#define SYS_GETGID      72  /* getgid() -> gid */
#define SYS_SETGID      73  /* setgid(gid) -> 0 or -errno */
#define SYS_GETEUID     74  /* geteuid() -> euid */
#define SYS_GETEGID     75  /* getegid() -> egid */

/* GPU Core - REMOVED: Use $/dev/graphics/video0 (DevFS) instead */

/* POSIX Signals */
#define SYS_SIGNAL             87  /* signal(signum, handler) -> old_handler or -errno */
#define SYS_RAISE              88  /* raise(signum) -> 0 or -errno */
#define SYS_SIGACTION          89  /* sigaction(signum, act*, oldact*) -> 0 or -errno */

/* File Descriptor Injection (for TTY manager) */
#define SYS_FD_INJECT          90  /* fd_inject(pid, fd, fd_obj) -> 0 or -errno */

/* POSIX Sessions and Process Groups */
#define SYS_SETSID             91  /* setsid() -> new SID or -errno */
#define SYS_SETPGID            92  /* setpgid(pid, pgid) -> 0 or -errno */
#define SYS_GETPGID            93  /* getpgid(pid) -> pgid or -errno */
#define SYS_GETSID             94  /* getsid(pid) -> sid or -errno */

/* ioctl commands for controlling terminal */
#define TIOCSCTTY              0x540E  /* Set controlling terminal */
#define TIOCNOTTY              0x5422  /* Give up controlling terminal */

/* Signal numbers (POSIX-compatible) */
#define SIGHUP     1   /* Hangup */
#define SIGINT     2   /* Interrupt (Ctrl+C) */
#define SIGQUIT    3   /* Quit */
#define SIGILL     4   /* Illegal instruction */
#define SIGTRAP    5   /* Trace trap */
#define SIGABRT    6   /* Abort */
#define SIGBUS     7   /* Bus error */
#define SIGFPE     8   /* Floating point exception */
#define SIGKILL    9   /* Kill (uncatchable) */
#define SIGUSR1    10  /* User-defined 1 */
#define SIGSEGV    11  /* Segmentation fault */
#define SIGUSR2    12  /* User-defined 2 */
#define SIGPIPE    13  /* Broken pipe */
#define SIGALRM    14  /* Alarm */
#define SIGTERM    15  /* Termination */
#define SIGCHLD    17  /* Child process status */
#define SIGCONT    18  /* Continue */
#define SIGSTOP    19  /* Stop (uncatchable) */
#define SIGTSTP    20  /* Terminal stop (Ctrl+Z) */

/* Signal handler types */
#define SIG_DFL    ((void (*)(int))0)  /* Default action */
#define SIG_IGN    ((void (*)(int))1)  /* Ignore signal */
#define SIG_ERR    ((void (*)(int))-1) /* Error return */

#endif

