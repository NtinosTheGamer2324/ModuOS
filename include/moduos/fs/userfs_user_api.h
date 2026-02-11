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

typedef struct {
    userfs_user_read_fn read;
    userfs_user_write_fn write;
} userfs_user_ops_t;

typedef struct {
    const char *path;         /* path relative to $/userland */
    const char *owner_id;     /* owner identity string */
    userfs_user_ops_t ops;    /* user callbacks */
    void *ctx;                /* user context pointer */
} userfs_user_node_t;

#ifdef __cplusplus
}
#endif

#endif
