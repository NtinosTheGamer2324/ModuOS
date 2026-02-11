#ifndef MODUOS_KERNEL_SYSCALL_USERFS_USER_H
#define MODUOS_KERNEL_SYSCALL_USERFS_USER_H

#include "moduos/fs/userfs_user_api.h"

int sys_userfs_register(const userfs_user_node_t *user_node);

#endif
