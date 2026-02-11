#ifndef MODUOS_FS_MDFS_DISK_H
#define MODUOS_FS_MDFS_DISK_H

#include "moduos/fs/MDFS/mdfs.h"

/* Fast CRC32 with lookup table */
uint32_t mdfs_crc32_fast(const void *data, size_t len);

int mdfs_disk_read_block(int vdrive_id, uint32_t start_lba, uint64_t block_no, void *out);
int mdfs_disk_write_block(int vdrive_id, uint32_t start_lba, uint64_t block_no, const void *in);
int mdfs_disk_write_blocks(int vdrive_id, uint32_t start_lba, uint64_t first_block_no, uint32_t block_count, const void *in);


int mdfs_disk_read_inode(int vdrive_id, uint32_t start_lba, const mdfs_superblock_t *sb, uint32_t ino, mdfs_inode_t *out);
int mdfs_disk_write_inode(int vdrive_id, uint32_t start_lba, const mdfs_superblock_t *sb, uint32_t ino, const mdfs_inode_t *in);

uint32_t mdfs_crc32_buf(const void *data, size_t len);

#endif
