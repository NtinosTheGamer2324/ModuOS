#pragma once
/*
 * sqrm_blockdev.h — SQRM minimal block-device ABI.
 *
 * Available to: FS, GENERIC modules (via block_get_info / block_read /
 * block_write / block_register in sqrm_kernel_api_t).
 *
 * All other module types receive NULL pointers for block operations.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Handle                                                             */
/* ------------------------------------------------------------------ */

typedef uint32_t blockdev_handle_t;
#define BLOCKDEV_INVALID_HANDLE 0u

/* ------------------------------------------------------------------ */
/*  Flags                                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    BLOCKDEV_F_READONLY  = 1u << 0,
    BLOCKDEV_F_REMOVABLE = 1u << 1,
} blockdev_flags_t;

/* ------------------------------------------------------------------ */
/*  Device info                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t sector_size;
    uint64_t sector_count;
    uint32_t flags;        /* bitfield of blockdev_flags_t */
    char     model[64];
} blockdev_info_t;

/* ------------------------------------------------------------------ */
/*  Operations (used when registering a block device)                 */
/* ------------------------------------------------------------------ */

typedef struct {
    int (*get_info)(void *ctx, blockdev_info_t *out);
    int (*read) (void *ctx, uint64_t lba, uint32_t count, void *buf,       size_t buf_sz);
    int (*write)(void *ctx, uint64_t lba, uint32_t count, const void *buf, size_t buf_sz);
} blockdev_ops_t;

#ifdef __cplusplus
}
#endif