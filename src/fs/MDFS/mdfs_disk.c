#include "moduos/fs/MDFS/mdfs_disk.h"
#include "moduos/fs/MDFS/mdfs_cache.h"
#include "moduos/drivers/Drive/vDrive.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"

/* Use fast table-based CRC32 */
uint32_t mdfs_crc32_buf(const void *data, size_t len) {
    return mdfs_crc32_fast(data, len);
}

static uint64_t mdfs_blocks_to_lba(uint64_t block_no) {
    return block_no * (uint64_t)(MDFS_BLOCK_SIZE / 512u);
}

int mdfs_disk_read_block(int vdrive_id, uint32_t start_lba, uint64_t block_no, void *out) {
    uint64_t lba = (uint64_t)start_lba + mdfs_blocks_to_lba(block_no);
    return vdrive_read((uint8_t)vdrive_id, lba, (MDFS_BLOCK_SIZE / 512u), out);
}

int mdfs_disk_write_block(int vdrive_id, uint32_t start_lba, uint64_t block_no, const void *in) {
    uint64_t lba = (uint64_t)start_lba + mdfs_blocks_to_lba(block_no);
    return vdrive_write((uint8_t)vdrive_id, lba, (MDFS_BLOCK_SIZE / 512u), in);
}

int mdfs_disk_write_blocks(int vdrive_id, uint32_t start_lba, uint64_t first_block_no, uint32_t block_count, const void *in) {
    if (block_count == 0) return -1;
    uint64_t lba = (uint64_t)start_lba + mdfs_blocks_to_lba(first_block_no);
    uint32_t sectors = block_count * (MDFS_BLOCK_SIZE / 512u);
    return vdrive_write((uint8_t)vdrive_id, lba, sectors, in);
}

int mdfs_disk_read_inode(int vdrive_id, uint32_t start_lba, const mdfs_superblock_t *sb, uint32_t ino, mdfs_inode_t *out) {
    uint64_t byte_off = (uint64_t)ino * (uint64_t)MDFS_INODE_SIZE;
    uint64_t block = sb->inode_table_start + (byte_off / MDFS_BLOCK_SIZE);
    uint64_t off = byte_off % MDFS_BLOCK_SIZE;

    uint8_t *blk = mdfs_buffer_acquire();
    if (!blk) return -1;
    if (mdfs_disk_read_block(vdrive_id, start_lba, block, blk) != VDRIVE_SUCCESS) { mdfs_buffer_release(blk); return -2; }
    memcpy(out, blk + off, sizeof(*out));
    mdfs_buffer_release(blk);
    return 0;
}

int mdfs_disk_write_inode(int vdrive_id, uint32_t start_lba, const mdfs_superblock_t *sb, uint32_t ino, const mdfs_inode_t *in) {
    uint64_t byte_off = (uint64_t)ino * (uint64_t)MDFS_INODE_SIZE;
    uint64_t block = sb->inode_table_start + (byte_off / MDFS_BLOCK_SIZE);
    uint64_t off = byte_off % MDFS_BLOCK_SIZE;

    uint8_t *blk = mdfs_buffer_acquire();
    if (!blk) return -1;
    if (mdfs_disk_read_block(vdrive_id, start_lba, block, blk) != VDRIVE_SUCCESS) { mdfs_buffer_release(blk); return -2; }
    memcpy(blk + off, in, sizeof(*in));
    if (mdfs_disk_write_block(vdrive_id, start_lba, block, blk) != VDRIVE_SUCCESS) { mdfs_buffer_release(blk); return -3; }
    mdfs_buffer_release(blk);
    return 0;
}
