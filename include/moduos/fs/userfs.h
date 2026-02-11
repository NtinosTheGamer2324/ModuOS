#ifndef MODUOS_FS_USERFS_H
#define MODUOS_FS_USERFS_H

#include <stddef.h>
#include <stdint.h>
#include "moduos/fs/userfs_user_api.h"

#ifdef __cplusplus
extern "C" {
#endif

int userfs_register_user_path(const char *path, const char *owner_id);
void *userfs_open_path(const char *path, int flags);
ssize_t userfs_read(void *handle, void *buf, size_t count);
ssize_t userfs_write(void *handle, const void *buf, size_t count);
void userfs_close(void *handle);
int userfs_list_dir_next(const char *path, int *cookie, char *name_buf, size_t buf_size, int *is_dir);

#ifdef __cplusplus
}
#endif

#endif
