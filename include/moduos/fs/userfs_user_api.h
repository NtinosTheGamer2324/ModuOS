#ifndef MODUOS_FS_USERFS_USER_API_H
#define MODUOS_FS_USERFS_USER_API_H

#include <stdint.h>
#include <stddef.h>

typedef int64_t ssize_t;

#ifdef __cplusplus
extern "C" {
#endif

typedef ssize_t (*userfs_user_read_fn)(void *ctx, void *buf, size_t count);
typedef ssize_t (*userfs_user_write_fn)(void *ctx, const void *buf, size_t count);
typedef ssize_t (*userfs_user_invoke_fn)(void *ctx, const void *in_buf,  size_t in_size, void *out_buf, size_t out_size);

typedef struct {
    userfs_user_read_fn read;
    userfs_user_write_fn write;
    userfs_user_invoke_fn invoke;
} userfs_user_ops_t;

typedef enum {
    USERFS_PERM_READ_ONLY  = 0x1,
    USERFS_PERM_WRITE_ONLY = 0x2,
    USERFS_PERM_READ_WRITE = 0x3,
    USERFS_PERM_INVOKE     = 0x4,
} userfs_perm_t;

typedef struct {
    const char *path;         /* path relative to $/user */
    const char *owner_id;     /* owner identity string */
    uint32_t perms;           /* USERFS_PERM_* */
    userfs_user_ops_t ops;    /* user callbacks (unused in-kernel) */
    void *ctx;                /* user context pointer */
} userfs_user_node_t;

#ifdef __cplusplus
}
#endif

#endif
