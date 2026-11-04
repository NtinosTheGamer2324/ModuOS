#ifndef _FCNTL_H
#define _FCNTL_H

#include "sys/types.h"

#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040   /* was 0x0100 in the old libc.h; 0x0040 is what the kernel checks */
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_NONBLOCK  0x0800

int open(const char *path, int flags, ...);   /* mode is read only if O_CREAT is set */
int fcntl(int fd, int cmd, ...);

#endif /* _FCNTL_H */
