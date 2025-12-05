//fd.c - File descriptor management implementation (with HVFS)
#include "moduos/fs/fd.h"
#include "moduos/fs/fs.h"
#include "moduos/fs/hvfs.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/process/process.h"

/* Enhanced file descriptor structure with cached data */
typedef struct {
    int mount_slot;           /* Which filesystem mount (0-25) */
    char path[256];           /* Full file path */
    size_t position;          /* Current read/write position */
    size_t file_size;         /* Total file size */
    int flags;                /* FD_FLAG_* flags */
    int in_use;               /* Is this FD active? */
    int pid;                  /* Owner process ID (0 for kernel) */
    void* cached_data;        /* Cached file contents (for reading) */
} file_descriptor_internal_t;

/* Global file descriptor table */
static file_descriptor_internal_t fd_table[MAX_FDS];
static int fd_initialized = 0;

/* Initialize FD table */
void fd_init(void) {
    if (fd_initialized) return;
    
    for (int i = 0; i < MAX_FDS; i++) {
        fd_table[i].in_use = 0;
        fd_table[i].mount_slot = -1;
        fd_table[i].path[0] = '\0';
        fd_table[i].position = 0;
        fd_table[i].file_size = 0;
        fd_table[i].flags = 0;
        fd_table[i].pid = 0;
        fd_table[i].cached_data = NULL;
    }
    
    /* Reserve standard file descriptors */
    fd_table[STDIN_FILENO].in_use = 1;
    fd_table[STDIN_FILENO].flags = FD_FLAG_READ;
    fd_table[STDIN_FILENO].pid = 0;
    fd_table[STDIN_FILENO].cached_data = NULL;
    
    fd_table[STDOUT_FILENO].in_use = 1;
    fd_table[STDOUT_FILENO].flags = FD_FLAG_WRITE;
    fd_table[STDOUT_FILENO].pid = 0;
    fd_table[STDOUT_FILENO].cached_data = NULL;
    
    fd_table[STDERR_FILENO].in_use = 1;
    fd_table[STDERR_FILENO].flags = FD_FLAG_WRITE;
    fd_table[STDERR_FILENO].pid = 0;
    fd_table[STDERR_FILENO].cached_data = NULL;
    
    fd_initialized = 1;
    com_write_string(COM1_PORT, "[FD] File descriptor table initialized\n");
}

/* Find free file descriptor */
static int find_free_fd(void) {
    /* Start from 3 (after stdin/stdout/stderr) */
    for (int i = 3; i < MAX_FDS; i++) {
        if (!fd_table[i].in_use) {
            return i;
        }
    }
    return -1;
}

/* Open file - now uses HVFS to cache file contents */
int fd_open(int mount_slot, const char* path, int flags, int mode) {
    (void)mode; /* Unused for now */
    
    fd_init();
    
    if (!path || mount_slot < 0 || mount_slot >= 26) {
        com_write_string(COM1_PORT, "[FD] Invalid parameters\n");
        return -1;
    }
    
    /* Find free FD first */
    int fd = find_free_fd();
    if (fd < 0) {
        com_write_string(COM1_PORT, "[FD] No free file descriptors\n");
        return -6;
    }
    
    /* Convert flags */
    int fd_flags = 0;
    if ((flags & O_RDWR) == O_RDWR) {
        fd_flags = FD_FLAG_READ | FD_FLAG_WRITE;
    } else if (flags & O_WRONLY) {
        fd_flags = FD_FLAG_WRITE;
    } else {
        fd_flags = FD_FLAG_READ;
    }
    
    if (flags & O_APPEND) {
        fd_flags |= FD_FLAG_APPEND;
    }
    
    /* Get current process PID */
    process_t* proc = process_get_current();
    int pid = proc ? proc->pid : 0;
    
    /* Use HVFS to read file into memory (if reading) */
    void* file_buffer = NULL;
    size_t file_size = 0;
    
    if (fd_flags & FD_FLAG_READ) {
        int hvfs_result = hvfs_read(mount_slot, path, &file_buffer, &file_size);
        
        if (hvfs_result != 0) {
            com_write_string(COM1_PORT, "[FD] HVFS read failed: ");
            char err_str[16];
            itoa(hvfs_result, err_str, 10);
            com_write_string(COM1_PORT, err_str);
            com_write_string(COM1_PORT, "\n");
            
            /* Map HVFS errors to fd_open errors */
            switch (hvfs_result) {
                case 1:  return -3; /* File not found */
                case 2:  return -5; /* Is directory */
                case -1: return -4; /* Stat failed */
                case -3: return -6; /* Malloc failed */
                default: return -7; /* Other error */
            }
        }
        
        com_write_string(COM1_PORT, "[FD] HVFS loaded file: ");
        char size_str[32];
        itoa(file_size, size_str, 10);
        com_write_string(COM1_PORT, size_str);
        com_write_string(COM1_PORT, " bytes\n");
    }
    
    /* Initialize FD */
    fd_table[fd].mount_slot = mount_slot;
    strncpy(fd_table[fd].path, path, sizeof(fd_table[fd].path) - 1);
    fd_table[fd].path[sizeof(fd_table[fd].path) - 1] = '\0';
    fd_table[fd].position = (flags & O_APPEND) ? file_size : 0;
    fd_table[fd].file_size = file_size;
    fd_table[fd].flags = fd_flags;
    fd_table[fd].pid = pid;
    fd_table[fd].cached_data = file_buffer;
    fd_table[fd].in_use = 1;
    
    com_write_string(COM1_PORT, "[FD] Opened file: ");
    com_write_string(COM1_PORT, path);
    com_write_string(COM1_PORT, " as FD ");
    char fd_str[16];
    itoa(fd, fd_str, 10);
    com_write_string(COM1_PORT, fd_str);
    com_write_string(COM1_PORT, "\n");
    
    return fd;
}

/* Close file descriptor */
int fd_close(int fd) {
    fd_init();
    
    if (fd < 0 || fd >= MAX_FDS || !fd_table[fd].in_use) {
        return -1;
    }
    
    /* Don't close stdin/stdout/stderr */
    if (fd <= 2) {
        com_write_string(COM1_PORT, "[FD] Cannot close standard fd\n");
        return -2;
    }
    
    /* Free cached data if present */
    if (fd_table[fd].cached_data) {
        kfree(fd_table[fd].cached_data);
        fd_table[fd].cached_data = NULL;
    }
    
    com_write_string(COM1_PORT, "[FD] Closed FD ");
    char fd_str[16];
    itoa(fd, fd_str, 10);
    com_write_string(COM1_PORT, fd_str);
    com_write_string(COM1_PORT, "\n");
    
    fd_table[fd].in_use = 0;
    fd_table[fd].mount_slot = -1;
    fd_table[fd].path[0] = '\0';
    fd_table[fd].position = 0;
    fd_table[fd].file_size = 0;
    fd_table[fd].flags = 0;
    
    return 0;
}

/* Read from file descriptor - now uses cached data */
ssize_t fd_read(int fd, void* buffer, size_t count) {
    fd_init();
    
    if (fd < 0 || fd >= MAX_FDS || !fd_table[fd].in_use) {
        return -1;
    }
    
    /* Check read permission */
    if (!(fd_table[fd].flags & FD_FLAG_READ)) {
        com_write_string(COM1_PORT, "[FD] No read permission\n");
        return -2;
    }
    
    /* Handle stdin specially (not implemented) */
    if (fd == STDIN_FILENO) {
        return -3;
    }
    
    /* Check if we have cached data */
    if (!fd_table[fd].cached_data) {
        com_write_string(COM1_PORT, "[FD] No cached data for reading\n");
        return -4;
    }
    
    /* Check if at EOF */
    if (fd_table[fd].position >= fd_table[fd].file_size) {
        return 0; /* EOF */
    }
    
    /* Calculate how much to read */
    size_t remaining = fd_table[fd].file_size - fd_table[fd].position;
    size_t to_read = (count < remaining) ? count : remaining;
    
    /* Copy from cached buffer at current position */
    memcpy(buffer, (uint8_t*)fd_table[fd].cached_data + fd_table[fd].position, to_read);
    
    /* Update position */
    fd_table[fd].position += to_read;
    
    return (ssize_t)to_read;
}

/* Write to file descriptor (stub) */
ssize_t fd_write(int fd, const void* buffer, size_t count) {
    (void)buffer;
    
    fd_init();
    
    if (fd < 0 || fd >= MAX_FDS || !fd_table[fd].in_use) {
        return -1;
    }
    
    /* Check write permission */
    if (!(fd_table[fd].flags & FD_FLAG_WRITE)) {
        return -2;
    }
    
    /* Handle stdout/stderr specially */
    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        /* Would write to VGA here */
        return (ssize_t)count;
    }
    
    /* File writing not implemented yet */
    com_write_string(COM1_PORT, "[FD] File writing not implemented\n");
    return -3;
}

/* Seek in file */
off_t fd_lseek(int fd, off_t offset, int whence) {
    fd_init();
    
    if (fd < 0 || fd >= MAX_FDS || !fd_table[fd].in_use) {
        return -1;
    }
    
    /* Can't seek stdin/stdout/stderr */
    if (fd <= 2) {
        return -2;
    }
    
    off_t new_pos;
    
    switch (whence) {
        case SEEK_SET:
            new_pos = offset;
            break;
            
        case SEEK_CUR:
            new_pos = fd_table[fd].position + offset;
            break;
            
        case SEEK_END:
            new_pos = fd_table[fd].file_size + offset;
            break;
            
        default:
            return -3;
    }
    
    /* Clamp to valid range */
    if (new_pos < 0) {
        new_pos = 0;
    } else if ((size_t)new_pos > fd_table[fd].file_size) {
        new_pos = fd_table[fd].file_size;
    }
    
    fd_table[fd].position = new_pos;
    
    return new_pos;
}

/* Get file descriptor structure */
file_descriptor_t* fd_get(int fd) {
    fd_init();
    
    if (fd < 0 || fd >= MAX_FDS || !fd_table[fd].in_use) {
        return NULL;
    }
    
    /* Return as public type (without cached_data pointer) */
    return (file_descriptor_t*)&fd_table[fd];
}

/* Check if FD is valid */
int fd_is_valid(int fd) {
    fd_init();
    
    return (fd >= 0 && fd < MAX_FDS && fd_table[fd].in_use) ? 1 : 0;
}

/* Duplicate file descriptor */
int fd_dup(int oldfd) {
    fd_init();
    
    if (oldfd < 0 || oldfd >= MAX_FDS || !fd_table[oldfd].in_use) {
        return -1;
    }
    
    int newfd = find_free_fd();
    if (newfd < 0) {
        return -2;
    }
    
    /* Copy FD structure */
    fd_table[newfd] = fd_table[oldfd];
    
    /* Important: Don't share cached data pointer! 
       Each FD needs its own copy if we want independent positions */
    if (fd_table[oldfd].cached_data && fd_table[oldfd].file_size > 0) {
        void* new_cache = kmalloc(fd_table[oldfd].file_size);
        if (new_cache) {
            memcpy(new_cache, fd_table[oldfd].cached_data, fd_table[oldfd].file_size);
            fd_table[newfd].cached_data = new_cache;
        } else {
            /* Failed to allocate, can't dup */
            fd_table[newfd].in_use = 0;
            return -3;
        }
    }
    
    return newfd;
}

/* Close all FDs for a process */
void fd_close_all(int pid) {
    fd_init();
    
    /* Get current process if pid is 0 */
    if (pid == 0) {
        process_t* proc = process_get_current();
        if (proc) pid = proc->pid;
    }
    
    for (int i = 3; i < MAX_FDS; i++) {
        if (fd_table[i].in_use && fd_table[i].pid == pid) {
            fd_close(i);
        }
    }
}

/* Get current position */
off_t fd_tell(int fd) {
    fd_init();
    
    if (fd < 0 || fd >= MAX_FDS || !fd_table[fd].in_use) {
        return -1;
    }
    
    return fd_table[fd].position;
}