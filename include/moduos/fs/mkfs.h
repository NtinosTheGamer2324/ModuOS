#ifndef MODUOS_FS_MKFS_H
#define MODUOS_FS_MKFS_H

#include <stdint.h>

// mkfs request for SYS_VFS_MKFS.
// Strings are NUL-terminated.
typedef struct {
    char fs_name[16];        // e.g. "fat32", "ext2"
    char label[16];          // volume label (optional)

    int32_t vdrive_id;
    uint32_t start_lba;
    uint32_t sectors;        // partition length in 512-byte sectors

    uint32_t flags;          // vfs_mkfs_req_t flags

    // flags bits
    //  - allows fat32 mkfs on volumes >32GiB when auto-picking cluster size
    //    (Windows formatter typically refuses without special tooling)
    #define VFS_MKFS_FLAG_FORCE  (1u << 0)

    // FAT32-specific (0 => kernel decides default)
    uint32_t fat32_sectors_per_cluster;
} vfs_mkfs_req_t;

#endif
