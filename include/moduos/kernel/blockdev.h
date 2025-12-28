#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t blockdev_handle_t;

#define BLOCKDEV_INVALID_HANDLE 0

typedef enum {
    BLOCKDEV_F_READONLY  = 1u << 0,
    BLOCKDEV_F_REMOVABLE = 1u << 1,
} blockdev_flags_t;

typedef struct {
    uint32_t sector_size;
    uint64_t sector_count;
    uint32_t flags;
    char model[64];
} blockdev_info_t;

typedef struct {
    int (*get_info)(void *ctx, blockdev_info_t *out);
    int (*read)(void *ctx, uint64_t lba, uint32_t count, void *buf, size_t buf_sz);
    int (*write)(void *ctx, uint64_t lba, uint32_t count, const void *buf, size_t buf_sz);
} blockdev_ops_t;

// Register a block device and return an opaque handle.
blockdev_handle_t blockdev_register(const blockdev_ops_t *ops, void *ctx);

// First-party helper: register all existing vDrive devices as blockdev handles.
void blockdev_register_vdrives(void);

// If vDrive blockdevs were registered, return the corresponding handle (or BLOCKDEV_INVALID_HANDLE).
blockdev_handle_t blockdev_get_vdrive_handle(int vdrive_id);

int blockdev_get_info(blockdev_handle_t h, blockdev_info_t *out);
int blockdev_read(blockdev_handle_t h, uint64_t lba, uint32_t count, void *buf, size_t buf_sz);
int blockdev_write(blockdev_handle_t h, uint64_t lba, uint32_t count, const void *buf, size_t buf_sz);

#ifdef __cplusplus
}
#endif
