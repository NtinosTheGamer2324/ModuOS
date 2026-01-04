#ifndef MODUOS_FS_MDFS_DIR_H
#define MODUOS_FS_MDFS_DIR_H

#include "moduos/fs/MDFS/mdfs.h"

int mdfs_v2_dir_list(const mdfs_fs_t *fs, uint32_t dir_ino, mdfs_dirent_t *out, int max_entries);
int mdfs_v2_dir_lookup(const mdfs_fs_t *fs, uint32_t dir_ino, const char *name, uint32_t *out_ino, uint8_t *out_type);
int mdfs_v2_dir_add(const mdfs_fs_t *fs, uint32_t dir_ino, const char *name, uint32_t ino, uint8_t type);
int mdfs_v2_dir_remove(const mdfs_fs_t *fs, uint32_t dir_ino, const char *name);

// Convenience wrappers for root
static inline int mdfs_v2_root_list(const mdfs_fs_t *fs, mdfs_dirent_t *out, int max_entries) {
    return mdfs_v2_dir_list(fs, (uint32_t)fs->sb.root_inode, out, max_entries);
}
static inline int mdfs_v2_root_lookup_export(const mdfs_fs_t *fs, const char *name, uint32_t *out_ino, uint8_t *out_type) {
    return mdfs_v2_dir_lookup(fs, (uint32_t)fs->sb.root_inode, name, out_ino, out_type);
}
static inline int mdfs_v2_root_add_export(const mdfs_fs_t *fs, const char *name, uint32_t ino, uint8_t type) {
    return mdfs_v2_dir_add(fs, (uint32_t)fs->sb.root_inode, name, ino, type);
}

#endif
