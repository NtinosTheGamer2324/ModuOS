#ifndef USERFS_H
#define USERFS_H
#include <stdint.h>
#include "string.h"

typedef enum {
    USERFS_PERM_READ_ONLY  = 0x1,
    USERFS_PERM_WRITE_ONLY = 0x2,
    USERFS_PERM_READ_WRITE = 0x3,
} userfs_perm_t;

typedef struct {
    userfs_user_read_fn read;
    userfs_user_write_fn write;
} userfs_user_ops_t;

typedef struct {
    const char *path;         /* path relative to $/user */
    const char *owner_id;     /* owner identity string */
    uint32_t perms;           /* USERFS_PERM_* */
    userfs_user_ops_t ops;    /* user callbacks (unused in-kernel) */
    void *ctx;                /* user context pointer */
} userfs_user_node_t;


static int userfs_register_node(const char *path) {
    userfs_user_node_t node;
    memset(&node, 0, sizeof(node));
    node.path = path;
    node.owner_id = "xserver64";
    node.perms = USERFS_PERM_READ_WRITE;
    return userfs_register(&node);
}

#endif