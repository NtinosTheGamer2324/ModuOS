#ifndef MODUOS_FS_DEVFS_H
#define MODUOS_FS_DEVFS_H

#include <stddef.h>
#include <stdint.h>
#include "moduos/fs/fd.h" // ssize_t
#include "moduos/kernel/events/events.h"

#ifdef __cplusplus
extern "C" {
#endif

// Simple DEVVFS character device interface (kernel-only)

typedef ssize_t (*devfs_read_fn)(void *ctx, void *buf, size_t count);
typedef ssize_t (*devfs_write_fn)(void *ctx, const void *buf, size_t count);
typedef int (*devfs_close_fn)(void *ctx);

typedef enum {
    DEVFS_OWNER_KERNEL = 0,
    DEVFS_OWNER_SQRM   = 1,
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
    const char *name;      // basename, e.g. "kbd0"
    devfs_read_fn read;
    devfs_write_fn write;
    devfs_close_fn close;
    devfs_can_replace_fn can_replace; // optional; consulted for 3rd-party overwrite
} devfs_device_ops_t;

typedef struct {
    devfs_owner_kind_t kind;
    const char *id; // e.g. "kernel" or module name
} devfs_owner_t;

// Register a device node under $/dev (flat) - legacy helper.
// Equivalent to devfs_register_path(name,...)
int devfs_register(const devfs_device_ops_t *ops, void *ctx);

// Create directories recursively (like mkdir -p) under $/dev.
int devfs_mkdir_p(const char *path, devfs_owner_t owner);

// Register a device node at an arbitrary DEVFS path relative to $/dev.
// Example: "usb/ehci0" => $/dev/usb/ehci0
// This auto-creates intermediate directories.
int devfs_register_path(const char *path, const devfs_device_ops_t *ops, void *ctx, devfs_owner_t owner);

// Look up a device by name; returns opaque handle (internal)
void* devfs_open(const char *name, int flags);

// Tree-based open: path relative to $/dev, e.g. "input/kbd0"
void* devfs_open_path(const char *path, int flags);

// List children in a DEVFS directory path (relative to $/dev). Cookie starts at 0.
// Returns 1 if an entry was written, 0 if end, <0 on error.
int devfs_list_dir_next(const char *dir_path, int *cookie, char *name_buf, size_t buf_size, int *is_dir);

// IO ops on opened device handle
ssize_t devfs_read(void *handle, void *buf, size_t count);
ssize_t devfs_write(void *handle, const void *buf, size_t count);
int devfs_close(void *handle);

// List devices (for $/dev directory listing)
int devfs_list_next(int *cookie, char *name_buf, size_t buf_size);

// Built-in input devices
int devfs_input_init(void);

// Built-in graphics devices
int devfs_graphics_init(void);

// Inject an input event (called by PS/2 and USB HID)
void devfs_input_push_event(const Event *e);

#ifdef __cplusplus
}
#endif

#endif
