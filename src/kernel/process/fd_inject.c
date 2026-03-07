// fd_inject.c - File descriptor injection into process fd_table
//
// Allows privileged kernel components (TTY manager, pipe subsystem, etc.) to
// inject an opaque fd object into a target process's fd_table[fd] slot before
// the process begins executing. The fd_table stores void* so the kernel's vfs
// layer can cast to the appropriate type at the call site.

#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/spinlock.h"
#include "moduos/kernel/COM/com.h"

extern char *itoa(int value, char *str, int base);

// A single coarse lock is sufficient: fd injection is a setup-time operation,
// not a hot path. Concurrent injection into different processes is serialised
// but that is acceptable for the intended use case.
static spinlock_t fd_inject_lock;
static int fd_inject_lock_inited = 0;

static void ensure_lock(void) {
    if (!fd_inject_lock_inited) {
        spinlock_init(&fd_inject_lock);
        fd_inject_lock_inited = 1;
    }
}

// Inject fd_obj into process pid's fd_table at slot fd.
//
// Rules:
//   - fd must be in [0, PROCESS_MAX_FDS).
//   - The slot must be currently empty (NULL). Overwriting a live FD is
//     refused to prevent accidental resource leaks; use process_close_fd
//     first if replacement is intended.
//   - The calling context must have verified that pid is a trusted target
//     (e.g. a child process being set up before it runs).
//
// Returns 0 on success, -1 on error.
int process_inject_fd(uint32_t pid, int fd, void *fd_obj) {
    if (fd < 0 || fd >= PROCESS_MAX_FDS) {
        com_write_string(COM1_PORT, "[FD_INJ] fd out of range\n");
        return -1;
    }
    if (!fd_obj) {
        com_write_string(COM1_PORT, "[FD_INJ] NULL fd_obj\n");
        return -1;
    }

    ensure_lock();
    spinlock_lock(&fd_inject_lock);

    process_t *p = process_find(pid);
    if (!p) {
        spinlock_unlock(&fd_inject_lock);
        com_write_string(COM1_PORT, "[FD_INJ] Target process not found\n");
        return -1;
    }

    if (p->fd_table[fd] != NULL) {
        spinlock_unlock(&fd_inject_lock);
        com_write_string(COM1_PORT, "[FD_INJ] Slot already occupied - close first\n");
        return -1;
    }

    p->fd_table[fd] = fd_obj;

    spinlock_unlock(&fd_inject_lock);

    com_write_string(COM1_PORT, "[FD_INJ] Injected fd ");
    char buf[16];
    itoa(fd, buf, 10);
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, " into PID ");
    itoa((int)pid, buf, 10);
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, "\n");

    return 0;
}

// Retrieve the fd object at slot fd in process pid's fd_table.
// Returns NULL if the process is not found or the slot is empty.
void *process_get_fd(uint32_t pid, int fd) {
    if (fd < 0 || fd >= PROCESS_MAX_FDS) return NULL;

    ensure_lock();
    spinlock_lock(&fd_inject_lock);
    process_t *p = process_find(pid);
    void *obj = p ? p->fd_table[fd] : NULL;
    spinlock_unlock(&fd_inject_lock);
    return obj;
}

// Clear fd slot fd in process pid's fd_table (does not call any close
// callback - that is the caller's responsibility).
//
// Returns 0 on success, -1 if the process is not found or fd is out of range.
int process_close_fd(uint32_t pid, int fd) {
    if (fd < 0 || fd >= PROCESS_MAX_FDS) return -1;

    ensure_lock();
    spinlock_lock(&fd_inject_lock);
    process_t *p = process_find(pid);
    if (!p) {
        spinlock_unlock(&fd_inject_lock);
        return -1;
    }
    p->fd_table[fd] = NULL;
    spinlock_unlock(&fd_inject_lock);
    return 0;
}
