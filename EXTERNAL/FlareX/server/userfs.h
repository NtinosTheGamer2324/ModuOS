#ifndef USERFS_H
#define USERFS_H
#include "libc.h"

// All userfs types are defined in libc.h

static inline int userfs_register_node(const char *path) {
    userfs_user_node_t node;
    memset(&node, 0, sizeof(node));
    node.path = path;
    node.owner_id = "xserver64";
    node.perms = USERFS_PERM_READ_WRITE;
    return userfs_register(&node);
}

#endif