//fd.c - Per-process file descriptor tables (with HVFS)
//
// Each process_t owns a table of pointers to refcounted open file
// descriptions (fd_file_t). dup/dup2/fork share a description, so file
// offsets, pipe ends and write buffers behave as in POSIX, and a description
// is torn down exactly once when its last descriptor is closed.
#include "moduos/fs/fd.h"
#include "moduos/fs/fs.h"
#include "moduos/fs/hvfs.h"
#include "moduos/fs/hvfs_cache.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/process/process_new.h"
#include "moduos/drivers/Drive/vDrive.h"
#include "moduos/fs/path.h"
#include "moduos/fs/devfs.h"
#include "moduos/fs/userfs.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/fs/MDFS/mdfs.h"
#include "moduos/kernel/errno.h"

/* Pipe ring buffer shared between the read and write descriptions. */
#define PIPE_BUF_SIZE 4096

typedef struct {
    char    buf[PIPE_BUF_SIZE];
    int     read_pos;
    int     write_pos;
    int     count;
    int     write_end_open;  /* 0 when write end is closed -> EOF on read */
    int     read_end_open;   /* 0 when read end is closed -> EPIPE on write */
    int     ends_alive;      /* buffer is freed when this reaches 0 */
} pipe_buf_t;

/* FD_DEBUG traces to COM1; far too slow to leave on outside of debugging. */
#ifndef FD_DEBUG
#define FD_DEBUG 0
#endif

#if FD_DEBUG
#define FD_LOG(s) com_write_string(COM1_PORT, s)
#else
#define FD_LOG(s) do { (void)(s); } while (0)
#endif

/* Open file description. The leading members mirror file_descriptor_t so
 * fd_get() can hand the same object out under the public type. */
struct fd_file {
    int mount_slot;
    char path[256];
    size_t position;
    size_t file_size;
    int flags;                /* FD_FLAG_* */
    int in_use;
    int pid;                  /* process that opened it */
    int type;                 /* FD_TYPE_* */
    void *pipe_buf;
    int   is_read_end;
    uint32_t cached_inode;    /* MDFS inode resolved at open() for writes */
    uint8_t  cached_type;
    uint8_t  cache_valid;
    uint8_t  _pad_cache;

    /* Private to this file. */
    int refcount;
    void *cached_data;        /* HVFS file image, or devfs/userfs handle */
    int is_directory;
    void *dir_handle;         /* fs_dir_t* or devvfs_dir_t* */
    int is_devvfs;
    int is_devfs;
    int is_userfs;

    /* Write coalescing buffer (large sequential writes). */
    uint8_t *wbuf;
    size_t   wbuf_cap;
    size_t   wbuf_len;
    size_t   wbuf_file_off;
};
typedef struct fd_file fd_file_t;

_Static_assert(offsetof(fd_file_t, pipe_buf) == offsetof(file_descriptor_t, pipe_buf),
               "fd_file_t must start with the file_descriptor_t layout");
_Static_assert(offsetof(fd_file_t, cached_inode) == offsetof(file_descriptor_t, cached_inode),
               "fd_file_t must start with the file_descriptor_t layout");
_Static_assert(MAX_FDS == PROCESS_MAX_FDS, "MAX_FDS must match PROCESS_MAX_FDS");

#define FD_WRITEBUF_DEFAULT_CAP (256u * 1024u) /* 256KiB */
#define FD_WRITEBUF_MAX_CAP     (4u * 1024u * 1024u) /* 4MiB */

typedef struct {
    int kind;   /* 0=$/, 1=$/mnt, 2=$/dev, 3=$/user */
    int index;
    int cookie; /* devfs/userfs listing cursor */
    char dev_path[128];
    char user_path[128];
} devvfs_dir_t;

/* ------------------------------------------------------------------ */
/* Description lifetime                                                */
/* ------------------------------------------------------------------ */

static fd_file_t *file_alloc(void) {
    fd_file_t *f = (fd_file_t*)kzalloc(sizeof(fd_file_t));
    if (!f) return NULL;
    f->refcount = 1;
    f->in_use = 1;
    f->mount_slot = -1;
    return f;
}

static void file_get(fd_file_t *f) {
    __atomic_add_fetch(&f->refcount, 1, __ATOMIC_ACQ_REL);
}

static int file_flush_wbuf(fd_file_t *f) {
    if (f->wbuf_len == 0) return 0;

    fs_mount_t *mount = fs_get_mount(f->mount_slot);
    if (!mount || !mount->valid) return -3;

    int rc;
    if (mount->type == FS_TYPE_MDFS && f->cache_valid && f->cached_type == 1 && f->cached_inode != 0) {
        rc = mdfs_write_file_at_by_inode(mount->handle, f->cached_inode,
                                         f->wbuf, f->wbuf_len, f->wbuf_file_off);
    } else {
        rc = fs_write_file_at(mount, f->path, f->wbuf, f->wbuf_len, f->wbuf_file_off);
    }
    if (rc != 0) return (rc < 0) ? rc : -3;

    f->wbuf_len = 0;
    return 0;
}

/* Runs once, when the last descriptor referencing f goes away. */
static void file_release(fd_file_t *f) {
    (void)file_flush_wbuf(f);

    if (f->type != FD_TYPE_PIPE && f->mount_slot >= 0) {
        fs_mount_t *m = fs_get_mount(f->mount_slot);
        if (m && m->valid && m->type == FS_TYPE_MDFS && f->cache_valid &&
            f->cached_type == 1 && f->cached_inode != 0) {
            (void)mdfs_flush_inode(m->handle, f->cached_inode);
        }
    }

    if (f->wbuf) kfree(f->wbuf);

    if (f->type == FD_TYPE_PIPE && f->pipe_buf) {
        pipe_buf_t *pb = (pipe_buf_t*)f->pipe_buf;
        if (f->is_read_end) pb->read_end_open = 0;
        else                pb->write_end_open = 0;
        if (__atomic_sub_fetch(&pb->ends_alive, 1, __ATOMIC_ACQ_REL) == 0)
            kfree(pb);
    }

    if (f->is_directory && f->dir_handle) {
        if (f->is_devvfs) kfree(f->dir_handle);
        else              fs_closedir((fs_dir_t*)f->dir_handle);
    } else if (f->cached_data) {
        if (f->is_devfs) {
            devfs_close(f->cached_data);
        } else if (f->is_userfs) {
            userfs_close(f->cached_data);
        } else if (!hvfs_cache_release(f->mount_slot, f->path, f->cached_data)) {
            kfree(f->cached_data);
        }
    }

    kfree(f);
}

static void file_put(fd_file_t *f) {
    if (__atomic_sub_fetch(&f->refcount, 1, __ATOMIC_ACQ_REL) == 0)
        file_release(f);
}

/* ------------------------------------------------------------------ */
/* Per-process table                                                   */
/* ------------------------------------------------------------------ */

/* Returns p's table, installing console stdio on first use. Lazy so every
 * process-creation path (fork, kernel spawn, idle) gets stdio without each
 * having to know about it. */
static fd_file_t **fd_proc_table(process_t *p) {
    if (!p) return NULL;
    if (!p->fd_ready) {
        p->fd_ready = 1;
        for (int i = STDIN_FILENO; i <= STDERR_FILENO; i++) {
            if (p->fd_table[i]) continue;
            fd_file_t *f = file_alloc();
            if (!f) break;
            f->type = FD_TYPE_CONSOLE;
            f->flags = (i == STDIN_FILENO) ? FD_FLAG_READ : FD_FLAG_WRITE;
            f->pid = (int)p->pid;
            p->fd_table[i] = f;
        }
    }
    return p->fd_table;
}

static fd_file_t *fd_lookup(int fd) {
    if (fd < 0 || fd >= PROCESS_MAX_FDS) return NULL;
    fd_file_t **t = fd_proc_table(process_get_current());
    return t ? t[fd] : NULL;
}

static int fd_find_free_from(fd_file_t **t, int min_fd) {
    if (!t) return -1;
    for (int i = min_fd; i < PROCESS_MAX_FDS; i++)
        if (!t[i]) return i;
    return -1;
}

/* Takes ownership of the caller's reference to f. */
static int fd_install(fd_file_t *f) {
    fd_file_t **t = fd_proc_table(process_get_current());
    int fd = fd_find_free_from(t, 0);
    if (fd < 0) {
        file_put(f);
        return -EMFILE;
    }
    t[fd] = f;
    return fd;
}

static int fd_has_free_slot(void) {
    return fd_find_free_from(fd_proc_table(process_get_current()), 0) >= 0;
}

static int fd_cur_pid(void) {
    process_t *p = process_get_current();
    return p ? (int)p->pid : 0;
}

static int oflags_to_fd_flags(int flags) {
    int fd_flags;
    if ((flags & O_RDWR) == O_RDWR)  fd_flags = FD_FLAG_READ | FD_FLAG_WRITE;
    else if (flags & O_WRONLY)       fd_flags = FD_FLAG_WRITE;
    else                             fd_flags = FD_FLAG_READ;
    return fd_flags;
}

void fd_init(void) {
    FD_LOG("[FD] per-process descriptor tables ready\n");
}

/* ------------------------------------------------------------------ */
/* Open                                                                */
/* ------------------------------------------------------------------ */

static int fd_open_node(const char *node, int flags, int want_devfs) {
    if (!node || !*node) return -1;
    if (!process_get_current()) return -1;
    if (!fd_has_free_slot()) return -EMFILE;

    void *h;
    if (want_devfs) {
        /* Prefer tree-based open (supports subpaths like input/kbd0). */
        h = devfs_open_path(node, flags);
        if (!h) h = devfs_open(node, flags);
    } else {
        h = userfs_open_path(node, flags);
    }
    if (!h) return -1;

    fd_file_t *f = file_alloc();
    if (!f) {
        if (want_devfs) devfs_close(h); else userfs_close(h);
        return -ENOMEM;
    }

    strncpy(f->path, node, sizeof(f->path) - 1);
    f->flags = oflags_to_fd_flags(flags);
    f->pid = fd_cur_pid();
    f->cached_data = h;
    f->is_devfs = want_devfs ? 1 : 0;
    f->is_userfs = want_devfs ? 0 : 1;
    f->type = want_devfs ? FD_TYPE_DEVFS : FD_TYPE_USERFS;

    return fd_install(f);
}

int fd_open_devfs(const char *node, int flags) {
    return fd_open_node(node, flags, 1);
}

int fd_open_userfs(const char *node, int flags) {
    return fd_open_node(node, flags, 0);
}

/* Open file - uses HVFS to cache file contents */
int fd_open(int mount_slot, const char* path, int flags, int mode) {
    (void)mode;

    if (!path || mount_slot < 0 || mount_slot >= 26) return -1;
    if (!process_get_current()) return -1;
    if (!fd_has_free_slot()) return -EMFILE;

    int fd_flags = oflags_to_fd_flags(flags);
    if (flags & O_APPEND) fd_flags |= FD_FLAG_APPEND;

    void* file_buffer = NULL;
    size_t file_size = 0;

    if (fd_flags & FD_FLAG_READ) {
        int hvfs_result = hvfs_read(mount_slot, path, &file_buffer, &file_size);
        if (hvfs_result != 0) {
            switch (hvfs_result) {
                case 1:  return -3; /* File not found */
                case 2:  return -5; /* Is directory */
                case -1: return -4; /* Stat failed */
                case -3: return -6; /* Malloc failed */
                default: return -7; /* Other error */
            }
        }
    }

    fd_file_t *f = file_alloc();
    if (!f) {
        if (file_buffer && !hvfs_cache_release(mount_slot, path, file_buffer))
            kfree(file_buffer);
        return -ENOMEM;
    }

    f->mount_slot = mount_slot;
    strncpy(f->path, path, sizeof(f->path) - 1);
    f->position = (flags & O_APPEND) ? file_size : 0;
    f->file_size = file_size;
    f->flags = fd_flags;
    f->pid = fd_cur_pid();
    f->cached_data = file_buffer;
    f->type = FD_TYPE_FILE;

    /* Resolve/create the MDFS inode once so write() avoids a path lookup and
     * directory scan per call. */
    fs_mount_t *m = fs_get_mount(mount_slot);
    if (m && m->valid && m->type == FS_TYPE_MDFS && (fd_flags & FD_FLAG_WRITE)) {
        uint32_t ino_n = 0;
        int tr = (flags & O_TRUNC) ? 1 : 0;
        if (mdfs_create_file_trunc(m->handle, path, tr, &ino_n) == 0) {
            f->cache_valid = 1;
            f->cached_inode = ino_n;
            f->cached_type = 1;
        }
    }

    return fd_install(f);
}

/* ------------------------------------------------------------------ */
/* Close / dup / fork                                                  */
/* ------------------------------------------------------------------ */

int fd_close(int fd) {
    fd_file_t *f = fd_lookup(fd);
    if (!f) return -1;

    /* Clear the slot before tearing down so a blocked/preempted release can
     * never be re-entered through the same descriptor number. */
    process_get_current()->fd_table[fd] = NULL;
    file_put(f);
    return 0;
}

int fd_dup(int oldfd) {
    fd_file_t *f = fd_lookup(oldfd);
    if (!f) return -EBADF;

    fd_file_t **t = fd_proc_table(process_get_current());
    int newfd = fd_find_free_from(t, 0);
    if (newfd < 0) return -EMFILE;

    file_get(f);
    t[newfd] = f;
    return newfd;
}

int fd_dup2(int oldfd, int newfd) {
    fd_file_t *f = fd_lookup(oldfd);
    if (!f) return -EBADF;
    if (newfd < 0 || newfd >= PROCESS_MAX_FDS) return -EBADF;
    if (oldfd == newfd) return newfd;

    fd_file_t **t = fd_proc_table(process_get_current());
    fd_file_t *old = t[newfd];

    file_get(f);
    t[newfd] = f;
    if (old) file_put(old);
    return newfd;
}

void fd_clone_for_fork(int parent_pid, int child_pid) {
    process_t *parent = process_find((uint32_t)parent_pid);
    process_t *child  = process_find((uint32_t)child_pid);
    if (!parent || !child || parent == child) return;

    fd_file_t **pt = fd_proc_table(parent);
    if (!pt) return;

    for (int i = 0; i < PROCESS_MAX_FDS; i++) {
        fd_file_t *f = pt[i];
        if (f) file_get(f);
        child->fd_table[i] = f;
    }
    child->fd_ready = 1;
}

void fd_close_all(int pid) {
    process_t *p;
    if (pid == 0) p = process_get_current();
    else          p = process_find((uint32_t)pid);
    if (!p) return;

    for (int i = 0; i < PROCESS_MAX_FDS; i++) {
        fd_file_t *f = p->fd_table[i];
        if (!f) continue;
        p->fd_table[i] = NULL;
        file_put(f);
    }
    /* A dying process must not get fresh stdio from a late fd call. */
    p->fd_ready = 1;
}

int fd_inject_dup(uint32_t pid, int target_fd, int src_fd) {
    if (target_fd < 0 || target_fd >= PROCESS_MAX_FDS) return -EBADF;

    fd_file_t *src = fd_lookup(src_fd);
    if (!src) return -EBADF;

    process_t *target = process_find(pid);
    if (!target) return -ESRCH;

    fd_file_t **t = fd_proc_table(target);
    if (!t) return -ESRCH;
    if (t[target_fd]) return -EBUSY;

    file_get(src);
    t[target_fd] = src;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Read / write                                                        */
/* ------------------------------------------------------------------ */

ssize_t fd_read(int fd, void* buffer, size_t count) {
    fd_file_t *f = fd_lookup(fd);
    if (!f) return -1;

    if (!(f->flags & FD_FLAG_READ)) return -2;

    /* Console stdin is serviced by the tty layer in the syscall path. */
    if (f->type == FD_TYPE_CONSOLE) return -3;

    if (f->type == FD_TYPE_PIPE) {
        pipe_buf_t *pb = (pipe_buf_t*)f->pipe_buf;
        if (!pb || !f->is_read_end) return -1;
        /* Spin-wait for data (no sleep/wakeup yet). */
        while (pb->count == 0) {
            if (!pb->write_end_open) return 0; /* EOF */
            process_yield();
        }
        size_t n = (count < (size_t)pb->count) ? count : (size_t)pb->count;
        char *dst = (char*)buffer;
        for (size_t i = 0; i < n; i++) {
            dst[i] = pb->buf[pb->read_pos];
            pb->read_pos = (pb->read_pos + 1) % PIPE_BUF_SIZE;
        }
        pb->count -= (int)n;
        return (ssize_t)n;
    }

    if (f->is_devfs) {
        if (!f->cached_data) return -4;
        return devfs_read(f->cached_data, buffer, count);
    }

    if (f->is_userfs) {
        if (!f->cached_data) return -4;
        return userfs_read(f->cached_data, buffer, count);
    }

    if (!f->cached_data) return -4;
    if (f->position >= f->file_size) return 0; /* EOF */

    size_t remaining = f->file_size - f->position;
    size_t to_read = (count < remaining) ? count : remaining;

    memcpy(buffer, (uint8_t*)f->cached_data + f->position, to_read);
    f->position += to_read;
    return (ssize_t)to_read;
}

ssize_t fd_write(int fd, const void* buffer, size_t count) {
    fd_file_t *f = fd_lookup(fd);
    if (!f) return -1;

    if (!buffer && count != 0) return -1;
    if (!(f->flags & FD_FLAG_WRITE)) return -2;

    if (f->type == FD_TYPE_CONSOLE) {
        if (buffer && count) VGA_WriteN((const char*)buffer, count);
        return (ssize_t)count;
    }

    if (f->type == FD_TYPE_PIPE) {
        pipe_buf_t *pb = (pipe_buf_t*)f->pipe_buf;
        if (!pb || f->is_read_end) return -1;
        const char *src = (const char*)buffer;
        size_t written = 0;
        while (written < count) {
            if (!pb->read_end_open)
                return written ? (ssize_t)written : -EPIPE;
            while (pb->count == PIPE_BUF_SIZE) {
                if (!pb->read_end_open)
                    return written ? (ssize_t)written : -EPIPE;
                process_yield();
            }
            pb->buf[pb->write_pos] = src[written++];
            pb->write_pos = (pb->write_pos + 1) % PIPE_BUF_SIZE;
            pb->count++;
        }
        return (ssize_t)count;
    }

    if (f->is_devfs) {
        if (!f->cached_data) return -4;
        return devfs_write(f->cached_data, buffer, count);
    }

    if (f->is_userfs) {
        if (!f->cached_data) return -4;
        return userfs_write(f->cached_data, buffer, count);
    }

    /* Regular file: coalesce small sequential writes into larger I/O. Without
     * this, userland writing in 4KiB chunks causes hundreds of thousands of
     * vdrive_write() calls. */
    if (count == 0) return 0;

    if (!f->wbuf) {
        f->wbuf_cap = FD_WRITEBUF_DEFAULT_CAP;
        f->wbuf = (uint8_t*)kmalloc(f->wbuf_cap);
        f->wbuf_len = 0;
        f->wbuf_file_off = f->position;
        if (!f->wbuf) {
            f->wbuf_cap = 0;
            fs_mount_t *mount = fs_get_mount(f->mount_slot);
            if (!mount || !mount->valid) return -3;
            int rc;
            if (mount->type == FS_TYPE_MDFS && f->cache_valid && f->cached_type == 1 && f->cached_inode != 0) {
                rc = mdfs_write_file_at_by_inode(mount->handle, f->cached_inode, buffer, count, f->position);
            } else {
                rc = fs_write_file_at(mount, f->path, buffer, count, f->position);
            }
            if (rc != 0) return (rc < 0) ? (ssize_t)rc : -3;
            f->position += count;
            if (f->position > f->file_size) f->file_size = f->position;
            return (ssize_t)count;
        }
    }

    /* Non-contiguous with buffered data: flush first. */
    size_t expected_off = f->wbuf_file_off + f->wbuf_len;
    if (f->position != expected_off) {
        int frc = file_flush_wbuf(f);
        if (frc != 0) return (ssize_t)frc;
        f->wbuf_file_off = f->position;
    }

    const uint8_t *src = (const uint8_t*)buffer;
    size_t remaining = count;
    while (remaining > 0) {
        size_t space = f->wbuf_cap - f->wbuf_len;
        if (space == 0) {
            /* Prefer growing up to a cap over flushing too often. */
            if (f->wbuf_cap < FD_WRITEBUF_MAX_CAP) {
                size_t new_cap = f->wbuf_cap * 2u;
                if (new_cap > FD_WRITEBUF_MAX_CAP) new_cap = FD_WRITEBUF_MAX_CAP;

                uint8_t *nb = (uint8_t*)kmalloc(new_cap);
                if (nb) {
                    memcpy(nb, f->wbuf, f->wbuf_len);
                    kfree(f->wbuf);
                    f->wbuf = nb;
                    f->wbuf_cap = new_cap;
                    space = f->wbuf_cap - f->wbuf_len;
                } else {
                    int frc = file_flush_wbuf(f);
                    if (frc != 0) return (ssize_t)frc;
                    f->wbuf_file_off = f->position;
                    space = f->wbuf_cap;
                }
            } else {
                int frc = file_flush_wbuf(f);
                if (frc != 0) return (ssize_t)frc;
                f->wbuf_file_off = f->position;
                space = f->wbuf_cap;
            }
        }

        size_t take = (remaining < space) ? remaining : space;
        memcpy(f->wbuf + f->wbuf_len, src, take);
        f->wbuf_len += take;
        f->position += take;
        src += take;
        remaining -= take;

        if (f->wbuf_len == f->wbuf_cap) {
            int frc = file_flush_wbuf(f);
            if (frc != 0) return (ssize_t)frc;
            f->wbuf_file_off = f->position;
        }
    }

    if (f->position > f->file_size) f->file_size = f->position;
    return (ssize_t)count;
}

/* Invoke (ioctl-style RPC) on a device-backed descriptor. */
ssize_t fd_invoke(int fd,
                  const void *in_buf,  size_t in_size,
                  void *out_buf, size_t out_size)
{
    fd_file_t *f = fd_lookup(fd);
    if (!f) return -1;      /* EBADF */

    if (!f->is_userfs && !f->is_devfs) return -2;
    if (!(f->flags & (FD_FLAG_READ | FD_FLAG_WRITE))) return -3;
    if (!f->cached_data) return -4;

    if (f->is_userfs)
        return userfs_invoke(f->cached_data, in_buf, in_size, out_buf, out_size);
    return devfs_invoke(f->cached_data, in_buf, in_size, out_buf, out_size);
}

/* mmap a devfs-backed descriptor (e.g. $/dev/mvc/mvi0 framebuffer).
 * Returns the mapped VA, or (void*)-1 on error. */
void *fd_mmap(int fd, void *hint, size_t length, int prot, int flags, uint64_t offset) {
    fd_file_t *f = fd_lookup(fd);
    if (!f || length == 0) return (void*)-1;
    if (!f->is_devfs || !f->cached_data) return (void*)-1;

    return devfs_mmap(f->cached_data, hint, length, prot, flags, offset);
}

off_t fd_lseek(int fd, off_t offset, int whence) {
    fd_file_t *f = fd_lookup(fd);
    if (!f) return -1;

    if (f->type == FD_TYPE_CONSOLE || f->type == FD_TYPE_PIPE) return -2;

    off_t new_pos;

    /* Flush buffered writes first to preserve ordering and file offsets. */
    if ((f->flags & FD_FLAG_WRITE) && f->wbuf_len) {
        int frc = file_flush_wbuf(f);
        if (frc != 0) return frc;
        f->wbuf_file_off = f->position;
    }

    switch (whence) {
        case SEEK_SET: new_pos = offset; break;
        case SEEK_CUR: new_pos = f->position + offset; break;
        case SEEK_END: new_pos = f->file_size + offset; break;
        default:       return -3;
    }

    if (new_pos < 0) new_pos = 0;
    else if ((size_t)new_pos > f->file_size) new_pos = f->file_size;

    f->position = new_pos;
    return new_pos;
}

file_descriptor_t* fd_get(int fd) {
    return (file_descriptor_t*)fd_lookup(fd);
}

int fd_is_valid(int fd) {
    return fd_lookup(fd) ? 1 : 0;
}

int fd_is_tty_routed(int fd) {
    fd_file_t *f = fd_lookup(fd);
    if (!f) return 0;
    return f->type == FD_TYPE_CONSOLE || f->is_devfs || f->is_userfs;
}

off_t fd_tell(int fd) {
    fd_file_t *f = fd_lookup(fd);
    return f ? (off_t)f->position : -1;
}

/* ------------------------------------------------------------------ */
/* Pipes / fcntl                                                       */
/* ------------------------------------------------------------------ */

int fd_pipe(int fds[2]) {
    if (!process_get_current()) return -1;

    pipe_buf_t *pb = (pipe_buf_t*)kzalloc(sizeof(pipe_buf_t));
    if (!pb) return -1;
    pb->write_end_open = 1;
    pb->read_end_open  = 1;
    pb->ends_alive     = 2;

    fd_file_t *rf = file_alloc();
    fd_file_t *wf = file_alloc();
    if (!rf || !wf) {
        if (rf) kfree(rf);
        if (wf) kfree(wf);
        kfree(pb);
        return -1;
    }

    int pid = fd_cur_pid();
    rf->type = FD_TYPE_PIPE; rf->flags = FD_FLAG_READ;  rf->pipe_buf = pb;
    rf->is_read_end = 1;     rf->pid = pid;
    wf->type = FD_TYPE_PIPE; wf->flags = FD_FLAG_WRITE; wf->pipe_buf = pb;
    wf->is_read_end = 0;     wf->pid = pid;

    fd_file_t **t = fd_proc_table(process_get_current());
    int rfd = fd_find_free_from(t, 0);
    int wfd = (rfd >= 0) ? fd_find_free_from(t, rfd + 1) : -1;
    if (rfd < 0 || wfd < 0) {
        /* Release through the normal path so ends_alive reaches zero. */
        file_put(rf);
        file_put(wf);
        return -EMFILE;
    }

    t[rfd] = rf;
    t[wfd] = wf;
    fds[0] = rfd;
    fds[1] = wfd;
    return 0;
}

/* F_GETFL / F_SETFL. Only O_NONBLOCK is acted on by F_SETFL. */
int fd_fcntl(int fd, int cmd, int arg) {
    fd_file_t *f = fd_lookup(fd);
    if (!f) return -EBADF;

    switch (cmd) {
        case F_GETFL: {
            int oflags = 0;
            if ((f->flags & FD_FLAG_READ) && (f->flags & FD_FLAG_WRITE))
                oflags |= O_RDWR;
            else if (f->flags & FD_FLAG_WRITE)
                oflags |= O_WRONLY;
            if (f->flags & FD_FLAG_APPEND)   oflags |= O_APPEND;
            if (f->flags & FD_FLAG_NONBLOCK) oflags |= O_NONBLOCK;
            return oflags;
        }

        case F_SETFL:
            if (arg & O_NONBLOCK) f->flags |= FD_FLAG_NONBLOCK;
            else                  f->flags &= ~FD_FLAG_NONBLOCK;
            return 0;

        default:
            return -EINVAL;
    }
}

/* ------------------------------------------------------------------ */
/* Directories                                                         */
/* ------------------------------------------------------------------ */

int fd_opendir(int mount_slot, const char* path) {
    if (!path || mount_slot < 0) return -1;
    if (!process_get_current()) return -1;
    if (!fd_has_free_slot()) return -EMFILE;

    fs_mount_t* mount = fs_get_mount(mount_slot);
    if (!mount || !mount->valid) return -1;

    fs_dir_t* dir = fs_opendir(mount, path);
    if (!dir) return -1;

    fd_file_t *f = file_alloc();
    if (!f) {
        fs_closedir(dir);
        return -1;
    }

    f->mount_slot = mount_slot;
    strncpy(f->path, path, sizeof(f->path) - 1);
    f->flags = FD_FLAG_READ;
    f->pid = fd_cur_pid();
    f->type = FD_TYPE_DIR;
    f->is_directory = 1;
    f->dir_handle = dir;

    return fd_install(f);
}

static int fd_devvfs_open(int kind, const char *dev_sub, const char *user_sub) {
    if (!process_get_current()) return -1;
    if (!fd_has_free_slot()) return -EMFILE;

    devvfs_dir_t *h = (devvfs_dir_t*)kzalloc(sizeof(devvfs_dir_t));
    if (!h) return -1;
    h->kind = kind;
    if (dev_sub) {
        strncpy(h->dev_path, dev_sub, sizeof(h->dev_path) - 1);
    }
    if (user_sub) {
        strncpy(h->user_path, user_sub, sizeof(h->user_path) - 1);
    }

    fd_file_t *f = file_alloc();
    if (!f) {
        kfree(h);
        return -1;
    }

    f->flags = FD_FLAG_READ;
    f->pid = fd_cur_pid();
    f->type = FD_TYPE_DIR;
    f->is_directory = 1;
    f->is_devvfs = 1;
    f->dir_handle = h;

    return fd_install(f);
}

int fd_devvfs_opendir_dev(const char *dev_subdir) {
    return fd_devvfs_open(2, dev_subdir, NULL);
}

int fd_devvfs_opendir_user(const char *user_subpath) {
    return fd_devvfs_open(3, NULL, user_subpath);
}

int fd_devvfs_opendir(int kind) {
    if (kind != 0 && kind != 1 && kind != 3) return -1;
    return fd_devvfs_open(kind, NULL, NULL);
}

int fd_readdir(int fd, char* name_buf, size_t buf_size, int* is_dir, uint32_t* size) {
    fd_file_t *f = fd_lookup(fd);
    if (!f) return -1;

    if (!f->is_directory || !f->dir_handle) return -1;
    if (!name_buf || buf_size == 0) return -1;

    if (f->is_devvfs) {
        devvfs_dir_t *h = (devvfs_dir_t*)f->dir_handle;

        if (h->kind == 0) {
            // $/: DEVVFS root
            const char *names[] = {"dev", "mnt", "user"};
            const int n_names = 3;
            if (h->index >= n_names) return 0;
            strncpy(name_buf, names[h->index], buf_size - 1);
            name_buf[buf_size - 1] = 0;
            if (is_dir) *is_dir = 1;
            if (size) *size = 0;
            h->index++;
            return 1;
        }

        if (h->kind == 1) {
            // $/mnt: one entry per mount slot. Names must be stable and
            // non-colliding even for same-model drives and multi-partition mounts.
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
            // $/dev or $/dev/<subdir>. Every present vdrive has a real node
            // registered by vdrive_register_devfs_nodes(), so this walk is
            // authoritative; synthesizing names as well would list drives twice.
            int d_is_dir = 0;
            int rc = devfs_list_dir_next(h->dev_path, &h->cookie, name_buf, buf_size, &d_is_dir);
            if (rc == 1) {
                if (is_dir) *is_dir = d_is_dir;
                if (size) *size = 0;
                return 1;
            }
            return 0;
        }

        if (h->kind == 3) {
            int d_is_dir = 0;
            int rc = userfs_list_dir_next(h->user_path, &h->cookie, name_buf, buf_size, &d_is_dir);
            if (rc == 1) {
                if (is_dir) *is_dir = d_is_dir;
                if (size) *size = 0;
                return 1;
            }
            return 0;
        }

        return 0;
    }

    fs_dir_t* dir = (fs_dir_t*)f->dir_handle;
    fs_dirent_t entry;

    int result = fs_readdir(dir, &entry);
    if (result <= 0) return result; /* 0 = end of directory, -1 = error */

    strncpy(name_buf, entry.name, buf_size - 1);
    name_buf[buf_size - 1] = '\0';

    if (is_dir) *is_dir = entry.is_directory;
    if (size) *size = entry.size;

    f->position++;
    return 1;
}

int fd_closedir(int fd) {
    fd_file_t *f = fd_lookup(fd);
    if (!f || !f->is_directory) return -1;
    return fd_close(fd);
}
