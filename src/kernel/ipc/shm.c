// shm.c — POSIX-style shared memory core (see ipc/shm.h for the design
// writeup). Only touches: phys.c's existing per-frame refcounts and
// kmalloc/kfree.

#include "moduos/kernel/ipc/shm.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/spinlock.h"
#include "moduos/kernel/errno.h"
#include "moduos/fs/fd.h"   /* O_CREAT / O_EXCL / O_RDONLY / O_RDWR */

typedef struct {
    int       in_use;
    char      name[SHM_NAME_MAX];   /* name[0] == 0 means "unlinked" */
    uint64_t  size;                 /* bytes, page-aligned */
    size_t    frame_count;
    uint64_t *frames;               /* kmalloc'd, frame_count entries */
    uint32_t  refcount;             /* metadata refs: 1 while named +
                                      * 1 per outstanding un-mmap'd handle */
} shm_object_t;

typedef struct {
    int in_use;
    int object_index;
    int can_write;
} shm_handle_t;

static shm_object_t g_shm_objects[SHM_MAX_OBJECTS];
static shm_handle_t g_shm_handles[SHM_MAX_HANDLES];
static spinlock_t   g_shm_lock;
static int          g_shm_inited = 0;

void shm_init(void) {
    memset(g_shm_objects, 0, sizeof(g_shm_objects));
    memset(g_shm_handles, 0, sizeof(g_shm_handles));
    spinlock_init(&g_shm_lock);
    g_shm_inited = 1;
}

/* Caller must hold g_shm_lock. Returns index or -1. Ignores unlinked
 * (name[0] == 0) objects, so a freshly shm_unlink()'d name is immediately
 * reusable by a new shm_open(O_CREAT), exactly like Linux. */
static int shm_find_locked(const char *name) {
    for (int i = 0; i < SHM_MAX_OBJECTS; i++) {
        if (g_shm_objects[i].in_use && g_shm_objects[i].name[0] &&
            strncmp(g_shm_objects[i].name, name, SHM_NAME_MAX) == 0) {
            return i;
        }
    }
    return -1;
}

static int shm_alloc_object_locked(void) {
    for (int i = 0; i < SHM_MAX_OBJECTS; i++)
        if (!g_shm_objects[i].in_use) return i;
    return -1;
}

static int shm_alloc_handle_locked(int object_index, int can_write) {
    for (int i = 0; i < SHM_MAX_HANDLES; i++) {
        if (!g_shm_handles[i].in_use) {
            g_shm_handles[i].in_use       = 1;
            g_shm_handles[i].object_index = object_index;
            g_shm_handles[i].can_write    = can_write;
            return i;
        }
    }
    return -1;
}

/* Drop `count` already-allocated frames on a failed creation. */
static void shm_rollback_frames(uint64_t *frames, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (frames[i]) phys_free_frame(frames[i]);
}

int shm_open(const char *name, int oflags, uint32_t mode, uint64_t size) {
    (void)mode; /* not enforced yet — see shm.h */
    if (!g_shm_inited) return -ENOSYS;
    if (!name || !name[0] || strlen(name) >= SHM_NAME_MAX) return -EINVAL;

    int want_write = ((oflags & O_RDWR) != 0);

    spinlock_lock(&g_shm_lock);

    int idx = shm_find_locked(name);

    if (idx >= 0) {
        /* Opening an existing object. */
        if ((oflags & O_CREAT) && (oflags & O_EXCL)) {
            spinlock_unlock(&g_shm_lock);
            return -EEXIST;
        }
        if (size != 0 && size != g_shm_objects[idx].size) {
            spinlock_unlock(&g_shm_lock);
            return -EINVAL;
        }
        int h = shm_alloc_handle_locked(idx, want_write);
        if (h < 0) { spinlock_unlock(&g_shm_lock); return -ENOMEM; }
        g_shm_objects[idx].refcount++;
        spinlock_unlock(&g_shm_lock);
        return h;
    }

    /* Not found. */
    if (!(oflags & O_CREAT)) {
        spinlock_unlock(&g_shm_lock);
        return -ENOENT;
    }
    if (size == 0) {
        spinlock_unlock(&g_shm_lock);
        return -EINVAL;
    }

    int new_idx = shm_alloc_object_locked();
    if (new_idx < 0) { spinlock_unlock(&g_shm_lock); return -ENOMEM; }

    /* Reserve the slot before dropping the lock to allocate frames — a
     * concurrent shm_open() of the same name must not race us. */
    g_shm_objects[new_idx].in_use = 1;
    strncpy(g_shm_objects[new_idx].name, name, SHM_NAME_MAX - 1);
    g_shm_objects[new_idx].name[SHM_NAME_MAX - 1] = 0;

    uint64_t page_sz_size = (size + 0xFFFULL) & ~0xFFFULL;
    size_t   frame_count  = (size_t)(page_sz_size / 0x1000ULL);

    spinlock_unlock(&g_shm_lock);

    uint64_t *frames = (uint64_t *)kmalloc(frame_count * sizeof(uint64_t));
    if (!frames) {
        spinlock_lock(&g_shm_lock);
        g_shm_objects[new_idx].in_use = 0;
        spinlock_unlock(&g_shm_lock);
        return -ENOMEM;
    }
    memset(frames, 0, frame_count * sizeof(uint64_t));

    for (size_t i = 0; i < frame_count; i++) {
        uint64_t f = phys_alloc_frame();
        if (!f) {
            shm_rollback_frames(frames, i);
            kfree(frames);
            spinlock_lock(&g_shm_lock);
            g_shm_objects[new_idx].in_use = 0;
            spinlock_unlock(&g_shm_lock);
            return -ENOMEM;
        }
        /* Zero it — mirrors sys_mmap's anon-page zeroing. */
        void *va = phys_to_virt_kernel(f);
        if (va) memset(va, 0, 0x1000);
        frames[i] = f;
    }

    spinlock_lock(&g_shm_lock);
    g_shm_objects[new_idx].size        = page_sz_size;
    g_shm_objects[new_idx].frame_count = frame_count;
    g_shm_objects[new_idx].frames      = frames;
    g_shm_objects[new_idx].refcount    = 1; /* the namespace's own hold */

    int h = shm_alloc_handle_locked(new_idx, want_write);
    if (h < 0) {
        /* Handle table exhausted — undo the whole creation. */
        for (size_t i = 0; i < frame_count; i++) phys_free_frame(frames[i]);
        kfree(frames);
        g_shm_objects[new_idx].in_use = 0;
        spinlock_unlock(&g_shm_lock);
        return -ENOMEM;
    }
    g_shm_objects[new_idx].refcount++; /* +1 for the handle we're issuing */
    spinlock_unlock(&g_shm_lock);

    return h;
}

int shm_unlink(const char *name) {
    if (!name || !name[0]) return -EINVAL;

    spinlock_lock(&g_shm_lock);
    int idx = shm_find_locked(name);
    if (idx < 0) {
        spinlock_unlock(&g_shm_lock);
        return -ENOENT;
    }

    g_shm_objects[idx].name[0] = 0; /* remove from namespace */
    g_shm_objects[idx].refcount--;
    int should_free   = (g_shm_objects[idx].refcount == 0);
    uint64_t *frames  = g_shm_objects[idx].frames;
    size_t    fcount  = g_shm_objects[idx].frame_count;
    spinlock_unlock(&g_shm_lock);

    if (should_free) {
        /* Drop this object's own hold on each page. Pages still mapped
         * somewhere have refcount > 1 (bumped in sys_mmap), so this only
         * actually frees a page once its last mapper has gone too — same
         * "marked for destruction, freed on last detach" semantics as
         * System V shmctl(IPC_RMID). */
        for (size_t i = 0; i < fcount; i++) phys_free_frame(frames[i]);
        kfree(frames);

        spinlock_lock(&g_shm_lock);
        g_shm_objects[idx].in_use      = 0;
        g_shm_objects[idx].frames      = NULL;
        g_shm_objects[idx].frame_count = 0;
        spinlock_unlock(&g_shm_lock);
    }

    return 0;
}

int shm_handle_take(int handle, uint64_t **out_frames, size_t *out_frame_count,
                    int *out_can_write) {
    if (handle < 0 || handle >= SHM_MAX_HANDLES) return -EBADF;

    spinlock_lock(&g_shm_lock);
    if (!g_shm_handles[handle].in_use) {
        spinlock_unlock(&g_shm_lock);
        return -EBADF;
    }

    int obj_idx = g_shm_handles[handle].object_index;
    if (obj_idx < 0 || obj_idx >= SHM_MAX_OBJECTS || !g_shm_objects[obj_idx].in_use) {
        /* Shouldn't happen — object metadata can't be freed while a handle
         * referencing it is outstanding — but stay defensive. */
        g_shm_handles[handle].in_use = 0;
        spinlock_unlock(&g_shm_lock);
        return -EBADF;
    }

    *out_frames      = g_shm_objects[obj_idx].frames;
    *out_frame_count = g_shm_objects[obj_idx].frame_count;
    *out_can_write   = g_shm_handles[handle].can_write;

    /* Consume the handle. */
    g_shm_handles[handle].in_use = 0;
    g_shm_objects[obj_idx].refcount--;
    int should_free = (g_shm_objects[obj_idx].refcount == 0);
    uint64_t *frames_to_free = should_free ? g_shm_objects[obj_idx].frames : NULL;
    size_t    fcount_to_free = should_free ? g_shm_objects[obj_idx].frame_count : 0;
    if (should_free) {
        g_shm_objects[obj_idx].in_use      = 0;
        g_shm_objects[obj_idx].frames      = NULL;
        g_shm_objects[obj_idx].frame_count = 0;
    }
    spinlock_unlock(&g_shm_lock);

    /* This can only happen if shm_unlink() dropped the namespace ref while
     * this was the last outstanding handle, in the narrow window before
     * mmap() got to it. Free the pages this handle was the last owner of. */
    if (should_free) {
        for (size_t i = 0; i < fcount_to_free; i++) phys_free_frame(frames_to_free[i]);
        kfree(frames_to_free);
        return -EBADF;
    }

    return 0;
}
