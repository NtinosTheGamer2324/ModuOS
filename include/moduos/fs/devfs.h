#ifndef MODUOS_FS_DEVFS_H
#define MODUOS_FS_DEVFS_H

#include <stddef.h>
#include <stdint.h>
#include "moduos/fs/fd.h"
#include "moduos/kernel/events/events.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DevFS character/block device interface (kernel-only).
 *
 * mmap support
 * ────────────
 * If a device wants to support mmap (e.g. a framebuffer or DMA buffer),
 * it provides a devfs_mmap_fn in its ops table.  The kernel calls it from
 * devfs_mmap() after validating the request.
 *
 * Signature:
 *   void *mmap(void *ctx, void *hint, size_t length, int prot, int flags,
 *              uint64_t offset)
 *
 * Returns: the userspace virtual address of the mapping on success,
 *          or MAP_FAILED ((void*)-1) on failure.
 *
 * The device is responsible for calling paging_map_range() (or equivalent)
 * to wire physical pages into the calling process's page table and for
 * returning the resulting user VA.  The kernel does not touch paging itself;
 * it simply routes the call and validates arguments.
 *
 * `hint`   — preferred user VA (may be NULL; device may ignore).
 * `offset` — byte offset into the device (e.g. into a framebuffer region).
 * `prot`   — PROT_READ / PROT_WRITE / PROT_EXEC bitmask (POSIX semantics).
 * `flags`  — MAP_SHARED / MAP_PRIVATE / MAP_FIXED (POSIX semantics).
 */

typedef void* (*devfs_open_fn)(void *ctx, int flags);
typedef ssize_t (*devfs_read_fn)(void *ctx, void *buf, size_t count);
typedef ssize_t (*devfs_write_fn)(void *ctx, const void *buf, size_t count);
typedef int (*devfs_close_fn)(void *ctx);
typedef void* (*devfs_mmap_fn)(void *ctx, void *hint, size_t length,
                               int prot, int flags, uint64_t offset);

typedef enum {
    DEVFS_OWNER_KERNEL = 0,
    DEVFS_OWNER_SQRM   = 1,
    DEVFS_OWNER_USER   = 2,
} devfs_owner_kind_t;

typedef enum {
    DEVFS_REPLACE_DENY  = 0,
    DEVFS_REPLACE_ALLOW = 1,
} devfs_replace_decision_t;

typedef devfs_replace_decision_t (*devfs_can_replace_fn)(
    void *existing_ctx,
    const char *path,
    const char *new_owner_id
);

typedef struct {
    const char *name;           /* basename, e.g. "kbd0"                         */
    devfs_open_fn        open;  /* optional; if NULL ctx is shared device ctx     */
    devfs_read_fn        read;
    devfs_write_fn       write;
    devfs_close_fn       close;
    devfs_mmap_fn        mmap;  /* optional; NULL → device does not support mmap  */
    devfs_can_replace_fn can_replace; /* optional; consulted for 3rd-party overwrite */
} devfs_device_ops_t;

typedef struct {
    devfs_owner_kind_t kind;
    const char *id; /* e.g. "kernel" or module name */
} devfs_owner_t;

/* ── Registration ─────────────────────────────────────────────────── */

/* Register a device node under $/dev (flat) - legacy helper.
 * Equivalent to devfs_register_path(ops->name, ...) */
int devfs_register(const devfs_device_ops_t *ops, void *ctx);

/* Create directories recursively (like mkdir -p) under $/dev. */
int devfs_mkdir_p(const char *path, devfs_owner_t owner);

/* Register a device node at an arbitrary DEVFS path relative to $/dev.
 * Example: "mvc/mvi0" => $/dev/mvc/mvi0
 * Intermediate directories are created automatically. */
int devfs_register_path(const char *path, const devfs_device_ops_t *ops,
                        void *ctx, devfs_owner_t owner);

/* ── I/O ──────────────────────────────────────────────────────────── */

/* Look up a device by name; returns opaque handle. */
void* devfs_open(const char *name, int flags);

/* Tree-based open: path relative to $/dev, e.g. "input/kbd0" */
void* devfs_open_path(const char *path, int flags);

/* List children in a DEVFS directory path (relative to $/dev).
 * Cookie starts at 0.
 * Returns 1 if an entry was written, 0 if end, <0 on error. */
int devfs_list_dir_next(const char *dir_path, int *cookie,
                        char *name_buf, size_t buf_size, int *is_dir);

/* IO ops on an opened device handle. */
ssize_t devfs_read(void *handle, void *buf, size_t count);
ssize_t devfs_write(void *handle, const void *buf, size_t count);
int     devfs_close(void *handle);

/*
 * devfs_mmap — map a device region into the calling process's address space.
 *
 * `handle`  — opaque handle returned by devfs_open / devfs_open_path.
 * `hint`    — preferred VA hint (may be NULL).
 * `length`  — mapping length in bytes (must be > 0).
 * `prot`    — protection flags (PROT_READ=1, PROT_WRITE=2, PROT_EXEC=4).
 * `flags`   — mapping flags (MAP_SHARED=1, MAP_PRIVATE=2, MAP_FIXED=16).
 * `offset`  — byte offset into the device.
 *
 * Returns the mapped user VA on success, MAP_FAILED ((void*)-1) on failure.
 * Returns MAP_FAILED immediately if the device's ops table has no mmap hook.
 */
void* devfs_mmap(void *handle, void *hint, size_t length,
                 int prot, int flags, uint64_t offset);

/* List devices at $/dev root (for directory listing). */
int devfs_list_next(int *cookie, char *name_buf, size_t buf_size);

/* ── Built-in subsystem init ──────────────────────────────────────── */

/* $/dev/input/kbd0 and $/dev/input/event0 */
int devfs_input_init(void);

/* $/dev/gui0 — GUI IPC device (stub if no GUI server present) */
int devfs_gui_init(void);

/* Return current GUI server pid (0 if none). Kernel-only. */
uint32_t devfs_gui_server_pid(void);

/* Inject an input event (called by PS/2 and USB HID). */
void devfs_input_push_event(const Event *e);

/*
 * devfs_mmap_region — map an existing kernel buffer or MMIO region into
 * the calling process's user address space.
 *
 * phys_or_virt — physical address (is_phys=1) or kernel VA (is_phys=0).
 * size         — mapping size in bytes (rounded up to page boundary).
 * prot         — PROT_READ=1, PROT_WRITE=2.
 * is_phys      — 1 for MMIO/framebuffer PA, 0 for kmalloc'd kernel VA.
 *
 * Returns user VA on success, (void*)-1 on failure.
 * Called by SQRM modules via sqrm_kernel_api.devfs_mmap_region.
 */
void *devfs_mmap_region(uint64_t phys_or_virt, size_t size, int prot, int is_phys);

/*
 * NOTE: devfs_graphics_init() has been removed.
 * $/dev/graphics/video0 (the old software VIDEOCTL2 path) is no longer
 * registered by DevFS.  Graphics are now handled entirely by the MVC3
 * kernel module ($/dev/mvc/mvi0) and the NodGL userland library.
 */

#ifdef __cplusplus
}
#endif

#endif /* MODUOS_FS_DEVFS_H */