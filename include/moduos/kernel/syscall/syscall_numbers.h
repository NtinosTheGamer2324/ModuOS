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
#define SYS_OPENDIR     25
#define SYS_READDIR     26
#define SYS_CLOSEDIR    27
#define SYS_INPUT       28
#define SYS_SSTATS      29
#define SYS_SSTATS2     38 /* fill user buffer with md64api_sysinfo_data_u */

// VFS formatting / mkfs
#define SYS_VFS_MKFS    36
#define SYS_VFS_GETPART 37
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

#endif