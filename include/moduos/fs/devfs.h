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

typedef struct {
    const char *name;      // e.g. "kbd0" (listed under $/dev)
    devfs_read_fn read;
    devfs_write_fn write;
    devfs_close_fn close;
} devfs_device_ops_t;

// Register a device node under $/dev
int devfs_register(const devfs_device_ops_t *ops, void *ctx);

// Look up a device by name; returns opaque handle (internal)
void* devfs_open(const char *name, int flags);

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
