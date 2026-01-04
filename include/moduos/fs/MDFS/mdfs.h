#ifndef MODUOS_FS_MDFS_H
#define MODUOS_FS_MDFS_H

#include <stdint.h>
#include <stddef.h>

#define MDFS_MAGIC 0x5346444Du /* 'MDFS' little-endian */
#define MDFS_VERSION 2
#define MDFS_BLOCK_SIZE 4096u

#define MDFS_INODE_SIZE 256u
#define MDFS_MAX_DIRECT 12u

#define MDFS_MAX_NAME 255u

// Directory record format (exFAT-style entry sets): 32-byte records
#define MDFS_DIR_REC_SIZE 32u
#define MDFS_DIRREC_PRIMARY 1u
#define MDFS_DIRREC_NAME    2u

#define MDFS_DIRFLAG_VALID   0x01u
#define MDFS_DIRFLAG_DELETED 0x02u

typedef struct __attribute__((packed)) {
    uint16_t mode;        // 0x4000 dir, 0x8000 file
    uint16_t _pad0;
    uint32_t uid;
    uint32_t gid;
    uint64_t size_bytes;
    uint32_t link_count;
    uint32_t flags;
    uint64_t direct[MDFS_MAX_DIRECT];
    uint64_t indirect1;
    uint8_t  _pad[MDFS_INODE_SIZE - 2 - 2 - 4 - 4 - 8 - 4 - 4 - (8*MDFS_MAX_DIRECT) - 8];
} mdfs_inode_t;

// Primary directory record (32 bytes)
typedef struct __attribute__((packed)) {
    uint8_t  rec_type;      // MDFS_DIRREC_PRIMARY
    uint8_t  flags;         // MDFS_DIRFLAG_*
    uint8_t  entry_type;    // 1=file,2=dir
    uint8_t  record_count;  // total records in entry set (including this primary)
    uint32_t inode;
    uint16_t name_len;      // UTF-8 bytes
    uint16_t _rsv0;
    uint32_t checksum;      // CRC32 over entry set with this field zero
    uint8_t  _pad[32 - 1 - 1 - 1 - 1 - 4 - 2 - 2 - 4];
} mdfs_dir_primary_t;

// Name record (32 bytes) stores 31 bytes of UTF-8 payload
typedef struct __attribute__((packed)) {
    uint8_t rec_type; // MDFS_DIRREC_NAME
    uint8_t name_bytes[31];
} mdfs_dir_name_t;

// High-level extracted dirent (for VFS listings)
typedef struct {
    uint32_t inode;
    uint8_t  type; // 1=file,2=dir
    char     name[MDFS_MAX_NAME + 1];
} mdfs_dirent_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t _reserved0;

    uint64_t total_blocks;
    uint64_t free_blocks;
    uint64_t total_inodes;
    uint64_t free_inodes;

    uint64_t block_bitmap_start;
    uint64_t block_bitmap_blocks;
    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;

    uint64_t root_inode;

    uint8_t  uuid[16];
    uint32_t features;
    uint32_t checksum; /* CRC32 over superblock with this field zero */

    uint8_t  pad[MDFS_BLOCK_SIZE - (4*4) - (6*8) - (1*8) - 16 - 4 - 4];
} mdfs_superblock_t;

// Minimal mount record (kept inside kernel mount table via handle)
typedef struct {
    int in_use;
    int vdrive_id;
    uint32_t start_lba;
    uint32_t sectors;

    mdfs_superblock_t sb;
} mdfs_fs_t;

int mdfs_mount(int vdrive_id, uint32_t start_lba);
int mdfs_mkfs(int vdrive_id, uint32_t start_lba, uint32_t sectors, const char *label);
const mdfs_fs_t *mdfs_get_fs(int handle);

// VFS integration helpers (v1: flat root directory only)
int mdfs_read_file_by_path(int handle, const char *path, void *buffer, size_t buffer_size, size_t *bytes_read);
int mdfs_write_file_by_path(int handle, const char *path, const void *buffer, size_t size);
int mdfs_stat_by_path(int handle, const char *path, uint32_t *out_size, int *out_is_dir);
int mdfs_read_dir(int handle, const char *path, mdfs_dirent_t *out, int max_entries);
int mdfs_read_root_dir(int handle, mdfs_dirent_t *out, int max_entries);
int mdfs_mkdir_by_path(int handle, const char *path);
int mdfs_rmdir_by_path(int handle, const char *path);
int mdfs_unlink_by_path(int handle, const char *path);

#endif
