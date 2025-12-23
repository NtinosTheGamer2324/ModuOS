//fd.c - File descriptor management implementation (with HVFS)
#include "moduos/fs/fd.h"
#include "moduos/fs/fs.h"
#include "moduos/fs/hvfs.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/process/process.h"
#include "moduos/drivers/Drive/vDrive.h"
#include "moduos/fs/path.h"
#include "moduos/fs/devfs.h"

/* Enhanced file descriptor structure with cached data */
typedef struct {
    int mount_slot;           /* Which filesystem mount (0-25) */
    char path[256];           /* Full file path */
    size_t position;          /* Current read/write position */
    size_t file_size;         /* Total file size */
    int flags;                /* FD_FLAG_* flags */
    int in_use;               /* Is this FD active? */
    int pid;                  /* Owner process ID (0 for kernel) */
    void* cached_data;        /* Cached file contents (for reading) OR devfs handle */
    int is_directory;         /* 1 if this is a directory descriptor */
    void* dir_handle;         /* Directory handle (fs_dir_t*) or DEVVFS handle */
    int is_devvfs;            /* 1 if dir_handle is a DEVVFS pseudo dir */
    int is_devfs;             /* 1 if cached_data is a devfs handle */
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
        fd_table[i].is_directory = 0;
        fd_table[i].dir_handle = NULL;
        fd_table[i].is_devvfs = 0;
        fd_table[i].is_devfs = 0;
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

static int fd_open_devfs_internal(const char *node, int flags) {
    fd_init();
    if (!node || !*node) return -1;

    void *h = devfs_open(node, flags);
    if (!h) return -1;

    int fd = find_free_fd();
    if (fd < 0) {
        devfs_close(h);
        return -6;
    }

    // Convert flags
    int fd_flags = 0;
    if ((flags & O_RDWR) == O_RDWR) {
        fd_flags = FD_FLAG_READ | FD_FLAG_WRITE;
    } else if (flags & O_WRONLY) {
        fd_flags = FD_FLAG_WRITE;
    } else {
        fd_flags = FD_FLAG_READ;
    }

    process_t* proc = process_get_current();
    int pid = proc ? proc->pid : 0;

    fd_table[fd].mount_slot = -1;
    strncpy(fd_table[fd].path, node, sizeof(fd_table[fd].path) - 1);
    fd_table[fd].path[sizeof(fd_table[fd].path) - 1] = 0;
    fd_table[fd].position = 0;
    fd_table[fd].file_size = 0;
    fd_table[fd].flags = fd_flags;
    fd_table[fd].pid = pid;
    fd_table[fd].cached_data = h;
    fd_table[fd].in_use = 1;
    fd_table[fd].is_directory = 0;
    fd_table[fd].dir_handle = NULL;
    fd_table[fd].is_devvfs = 0;
    fd_table[fd].is_devfs = 1;

    return fd;
}

int fd_open_devfs(const char *node, int flags) {
    return fd_open_devfs_internal(node, flags);
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
    
    /* Close devfs handle OR free cached file data */
    if (fd_table[fd].cached_data) {
        if (fd_table[fd].is_devfs) {
            devfs_close(fd_table[fd].cached_data);
        } else {
            kfree(fd_table[fd].cached_data);
        }
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
    fd_table[fd].is_directory = 0;
    fd_table[fd].dir_handle = NULL;
    fd_table[fd].is_devvfs = 0;
    fd_table[fd].is_devfs = 0;
    
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

    /* devfs-backed FD */
    if (fd_table[fd].is_devfs) {
        if (!fd_table[fd].cached_data) return -4;
        return devfs_read(fd_table[fd].cached_data, buffer, count);
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

/* --- DIRECTORY OPERATIONS --- */

int fd_opendir(int mount_slot, const char* path) {
    fd_init();
    
    if (!path || mount_slot < 0) {
        return -1;
    }
    
    /* Get mount */
    fs_mount_t* mount = fs_get_mount(mount_slot);
    if (!mount || !mount->valid) {
        return -1;
    }
    
    /* Open directory using filesystem layer */
    fs_dir_t* dir = fs_opendir(mount, path);
    if (!dir) {
        return -1;
    }
    
    /* Find free FD slot */
    int fd = -1;
    for (int i = 3; i < MAX_FDS; i++) {
        if (!fd_table[i].in_use) {
            fd = i;
            break;
        }
    }
    
    if (fd < 0) {
        fs_closedir(dir);
        return -1;
    }
    
    /* Get current process PID */
    process_t* proc = process_get_current();
    int pid = proc ? proc->pid : 0;
    
    /* Set up FD entry */
    fd_table[fd].in_use = 1;
    fd_table[fd].mount_slot = mount_slot;
    strncpy(fd_table[fd].path, path, sizeof(fd_table[fd].path) - 1);
    fd_table[fd].path[sizeof(fd_table[fd].path) - 1] = '\0';
    fd_table[fd].position = 0;
    fd_table[fd].file_size = 0;
    fd_table[fd].flags = FD_FLAG_READ;
    fd_table[fd].pid = pid;
    fd_table[fd].cached_data = NULL;
    fd_table[fd].is_directory = 1;
    fd_table[fd].dir_handle = dir;
    
    return fd;
}

typedef struct {
    int kind;   /* 1=$/mnt, 2=$/dev, 3=$/dev/input, 4=$/dev/graphics */
    int index;  /* current index */
    int cookie; /* for devfs listing */
} devvfs_dir_t;

static void devvfs_sanitize(const char *in, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    size_t j = 0;
    for (size_t i = 0; in && in[i] && j + 1 < out_sz; i++) {
        char c = in[i];
        if (c == ' ' || c == '\t') {
            if (j == 0 || out[j - 1] == '-') continue;
            out[j++] = '-';
        } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            out[j++] = c;
        } else {
            if (j == 0 || out[j - 1] == '-') continue;
            out[j++] = '-';
        }
    }
    while (j > 0 && out[j - 1] == '-') j--;
    out[j] = 0;
}

int fd_devvfs_opendir(int kind) {
    fd_init();

    if (kind != 0 && kind != 1 && kind != 2 && kind != 3 && kind != 4) return -1;

    int fd = find_free_fd();
    if (fd < 0) return -1;

    devvfs_dir_t *h = (devvfs_dir_t*)kmalloc(sizeof(devvfs_dir_t));
    if (!h) return -1;
    h->kind = kind;
    // kind=0 ($/) emits top-level dirs; kind=2 ($/dev) emits "input" dir first
    if (kind == 0) h->index = 0;
    else h->index = (kind == 2) ? -1 : 0;
    h->cookie = 0;

    process_t* proc = process_get_current();
    int pid = proc ? proc->pid : 0;

    fd_table[fd].in_use = 1;
    fd_table[fd].mount_slot = -1;
    fd_table[fd].path[0] = '\0';
    fd_table[fd].position = 0;
    fd_table[fd].file_size = 0;
    fd_table[fd].flags = FD_FLAG_READ;
    fd_table[fd].pid = pid;
    fd_table[fd].cached_data = NULL;
    fd_table[fd].is_directory = 1;
    fd_table[fd].is_devvfs = 1;
    fd_table[fd].dir_handle = h;

    return fd;
}

int fd_readdir(int fd, char* name_buf, size_t buf_size, int* is_dir, uint32_t* size) {
    fd_init();

    if (fd < 0 || fd >= MAX_FDS || !fd_table[fd].in_use) {
        return -1;
    }

    if (!fd_table[fd].is_directory || !fd_table[fd].dir_handle) {
        return -1;
    }

    if (!name_buf || buf_size == 0) {
        return -1;
    }

    if (fd_table[fd].is_devvfs) {
        devvfs_dir_t *h = (devvfs_dir_t*)fd_table[fd].dir_handle;

        if (h->kind == 0) {
            // $/: DEVVFS root
            const char *names[] = {"dev", "mnt"};
            const int n_names = 2;
            if (h->index >= n_names) return 0;
            strncpy(name_buf, names[h->index], buf_size - 1);
            name_buf[buf_size - 1] = 0;
            if (is_dir) *is_dir = 1;
            if (size) *size = 0;
            h->index++;
            return 1;
        }

        if (h->kind == 1) {
            // $/mnt: list mounted filesystems (one entry per mount slot).
            // Names must be stable and non-colliding even for same-model drives and multi-partition mounts.
            int produced = 0;
            int seen = 0;

            for (int slot = 0; slot < 26; slot++) {
                int vdrive_id = -1;
                uint32_t lba = 0;
                fs_type_t type;
                if (fs_get_mount_info(slot, &vdrive_id, &lba, &type) != 0) continue;
                if (vdrive_id < 0) continue;

                if (seen++ < h->index) continue;

                char label[64];
                if (fs_get_mount_label(slot, label, sizeof(label)) != 0) {
                    strcpy(label, "vDrive");
                    { char nbuf[16]; itoa(vdrive_id, nbuf, 10); strncat(label, nbuf, sizeof(label) - strlen(label) - 1); }
                }

                strncpy(name_buf, label, buf_size - 1);
                name_buf[buf_size - 1] = 0;
                if (is_dir) *is_dir = 1;
                if (size) *size = 0;
                h->index++;
                produced = 1;
                break;
            }

            return produced ? 1 : 0;
        }

        if (h->kind == 2) {
            // $/dev: expose "input" + "graphics" directories, plus non-input devfs devices (e.g. vDrives)
            if (h->index == -1) {
                strncpy(name_buf, "input", buf_size - 1);
                name_buf[buf_size - 1] = 0;
                if (is_dir) *is_dir = 1;
                if (size) *size = 0;
                h->index = -2;
                return 1;
            }
            if (h->index == -2) {
                strncpy(name_buf, "graphics", buf_size - 1);
                name_buf[buf_size - 1] = 0;
                if (is_dir) *is_dir = 1;
                if (size) *size = 0;
                h->index = 0;
                return 1;
            }

            // devfs devices (filter out input devices; they live under $/dev/input)
            {
                extern int devfs_list_next(int *cookie, char *name_buf, size_t buf_size);
                for (;;) {
                    int rc = devfs_list_next(&h->cookie, name_buf, buf_size);
                    if (rc != 1) break;
                    if (strcmp(name_buf, "event0") == 0) continue;
                    if (strcmp(name_buf, "kbd0") == 0) continue;
                    if (strcmp(name_buf, "video0") == 0) continue; // listed under $/dev/graphics
                    if (is_dir) *is_dir = 0;
                    if (size) *size = 0;
                    return 1;
                }
            }

            // then vDrives
            int count = vdrive_get_count();
            if (h->index >= count) return 0;
            int vdrive_id = h->index;
            vdrive_t *d = vdrive_get((uint8_t)vdrive_id);
            char name[96];
            strcpy(name, "vDrive");
            char nbuf[8];
            itoa(vdrive_id, nbuf, 10);
            strncat(name, nbuf, sizeof(name) - strlen(name) - 1);
            if (d) {
                char san[64];
                devvfs_sanitize(d->model, san, sizeof(san));
                if (san[0]) {
                    strncat(name, "-", sizeof(name) - strlen(name) - 1);
                    strncat(name, san, sizeof(name) - strlen(name) - 1);
                }
            }

            strncpy(name_buf, name, buf_size - 1);
            name_buf[buf_size - 1] = 0;
            if (is_dir) *is_dir = 0;
            if (size) *size = 0;
            h->index++;
            return 1;
        }

        if (h->kind == 3) {
            // $/dev/input: linux-like input directory
            const char *names[] = {"event0", "kbd0"};
            const int n_names = 2;
            if (h->index >= n_names) return 0;
            strncpy(name_buf, names[h->index], buf_size - 1);
            name_buf[buf_size - 1] = 0;
            if (is_dir) *is_dir = 0;
            if (size) *size = 0;
            h->index++;
            return 1;
        }

        if (h->kind == 4) {
            // $/dev/graphics: graphics directory
            const char *names[] = {"video0"};
            const int n_names = 1;
            if (h->index >= n_names) return 0;
            strncpy(name_buf, names[h->index], buf_size - 1);
            name_buf[buf_size - 1] = 0;
            if (is_dir) *is_dir = 0;
            if (size) *size = 0;
            h->index++;
            return 1;
        }

        return 0;
    }

    fs_dir_t* dir = (fs_dir_t*)fd_table[fd].dir_handle;
    fs_dirent_t entry;

    int result = fs_readdir(dir, &entry);
    if (result <= 0) {
        return result; /* 0 = end of directory, -1 = error */
    }

    /* Copy data to output buffers */
    strncpy(name_buf, entry.name, buf_size - 1);
    name_buf[buf_size - 1] = '\0';

    if (is_dir) {
        *is_dir = entry.is_directory;
    }

    if (size) {
        *size = entry.size;
    }

    fd_table[fd].position++;
    return 1; /* Successfully read entry */
}

int fd_closedir(int fd) {
    fd_init();
    
    if (fd < 0 || fd >= MAX_FDS || !fd_table[fd].in_use) {
        return -1;
    }
    
    if (!fd_table[fd].is_directory) {
        return -1;
    }

    if (fd_table[fd].dir_handle) {
        if (fd_table[fd].is_devvfs) {
            kfree(fd_table[fd].dir_handle);
        } else {
            fs_closedir((fs_dir_t*)fd_table[fd].dir_handle);
        }
        fd_table[fd].dir_handle = NULL;
    }

    fd_table[fd].in_use = 0;
    fd_table[fd].is_directory = 0;
    fd_table[fd].is_devvfs = 0;

    return 0;
}