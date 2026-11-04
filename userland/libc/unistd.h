#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>
#include <stdint.h>
#include "sys/types.h"

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

/* All of these return -1 and set errno on failure. */
int     close(int fd);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
off_t   lseek(int fd, off_t offset, int whence);
int     dup(int oldfd);
int     dup2(int oldfd, int newfd);
int     pipe(int fds[2]);

pid_t   fork(void);
pid_t   getpid(void);
pid_t   getppid(void);
int     execve(const char *path, char *const argv[], char *const envp[]);
__attribute__((noreturn)) void _exit(int status);

uid_t   getuid(void);
uid_t   geteuid(void);
gid_t   getgid(void);
gid_t   getegid(void);
int     setuid(uid_t uid);
int     setgid(gid_t gid);

pid_t   setsid(void);
int     setpgid(pid_t pid, pid_t pgid);
pid_t   getpgid(pid_t pid);
pid_t   getsid(pid_t pid);

unsigned int sleep(unsigned int seconds);

int     chdir(const char *path);
char   *getcwd(char *buf, size_t size);
int     rmdir(const char *path);
int     unlink(const char *path);

#endif /* _UNISTD_H */
