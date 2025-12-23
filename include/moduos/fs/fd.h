//fd.h - File descriptor management
#ifndef FD_H
#define FD_H

#include <stdint.h>
#include <stddef.h>
typedef int64_t off_t;  // 64-bit signed offset
typedef int64_t ssize_t;

/* Standard file descriptors */
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* Maximum number of file descriptors per process */
#define MAX_FDS 256

/* File descriptor flags */
#define FD_FLAG_READ   0x01
#define FD_FLAG_WRITE  0x02
#define FD_FLAG_APPEND 0x04
#define FD_FLAG_CREATE 0x08

/* Open flags (similar to POSIX) */
#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_RDWR    0x0002
#define O_CREAT   0x0040
#define O_APPEND  0x0400
#define O_TRUNC    0x0200
#define O_NONBLOCK 0x0800

/* Seek positions */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* File descriptor structure */
typedef struct {
    int mount_slot;           /* Which filesystem mount (0-25) */
    char path[256];           /* Full file path */
    size_t position;          /* Current read/write position */
    size_t file_size;         /* Total file size (cached) */
    int flags;                /* FD_FLAG_* flags */
    int in_use;               /* Is this FD active? */
    int pid;                  /* Owner process ID (0 for kernel) */
} file_descriptor_t;

/**
 * Initialize file descriptor table
 * Called once during kernel init
 */
void fd_init(void);

/**
 * Open a file and get a file descriptor
 * @param mount_slot: Filesystem mount slot (0-25)
 * @param path: File path
 * @param flags: O_RDONLY, O_WRONLY, O_RDWR, etc.
 * @param mode: Permissions (unused for now)
 * @return: File descriptor number (>=0) on success, negative on error
 */
int fd_open(int mount_slot, const char* path, int flags, int mode);

// Open a DEVFS character device (e.g. "$\/dev\/kbd0")
int fd_open_devfs(const char *node, int flags);


/**
 * Close a file descriptor
 * @param fd: File descriptor number
 * @return: 0 on success, -1 on error
 */
int fd_close(int fd);

/**
 * Read from a file descriptor
 * @param fd: File descriptor number
 * @param buffer: Output buffer
 * @param count: Number of bytes to read
 * @return: Number of bytes read, 0 on EOF, -1 on error
 */
ssize_t fd_read(int fd, void* buffer, size_t count);

/**
 * Write to a file descriptor (not implemented yet)
 * @param fd: File descriptor number
 * @param buffer: Data to write
 * @param count: Number of bytes to write
 * @return: Number of bytes written, -1 on error
 */
ssize_t fd_write(int fd, const void* buffer, size_t count);

/**
 * Seek to a position in file
 * @param fd: File descriptor number
 * @param offset: Offset value
 * @param whence: SEEK_SET, SEEK_CUR, or SEEK_END
 * @return: New position on success, -1 on error
 */
off_t fd_lseek(int fd, off_t offset, int whence);

/**
 * Get file descriptor structure (for kernel use)
 * @param fd: File descriptor number
 * @return: Pointer to fd structure, or NULL if invalid
 */
file_descriptor_t* fd_get(int fd);

/**
 * Check if file descriptor is valid
 * @param fd: File descriptor number
 * @return: 1 if valid, 0 if not
 */
int fd_is_valid(int fd);

/**
 * Duplicate a file descriptor
 * @param oldfd: Existing file descriptor
 * @return: New file descriptor, or -1 on error
 */
int fd_dup(int oldfd);

/**
 * Close all file descriptors for a process
 * @param pid: Process ID (0 for current process)
 */
void fd_close_all(int pid);

/**
 * Get current position in file
 * @param fd: File descriptor number
 * @return: Current position, or -1 on error
 */
off_t fd_tell(int fd);

/**
 * Open directory for reading
 * @param mount_slot: Filesystem mount slot (0-25)
 * @param path: Directory path (NULL or "/" for root)
 * @return: File descriptor number (>=0) on success, negative on error
 */
int fd_opendir(int mount_slot, const char* path);

/**
 * Read next directory entry
 * @param fd: Directory file descriptor
 * @param name_buf: Output buffer for entry name
 * @param buf_size: Size of name buffer
 * @param is_dir: Output - 1 if directory, 0 if file (can be NULL)
 * @param size: Output - file size (can be NULL)
 * @return: 1 on success, 0 at end of directory, -1 on error
 */
int fd_readdir(int fd, char* name_buf, size_t buf_size, int* is_dir, uint32_t* size);

/**
 * Close directory file descriptor
 * @param fd: Directory file descriptor
 * @return: 0 on success, -1 on error
 */
int fd_closedir(int fd);

/* Open DEVVFS pseudo directories (kind: 1=$/mnt, 2=$/dev) */
int fd_devvfs_opendir(int kind);

#endif /* FD_H */