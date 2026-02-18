#include "moduos/fs/MDFS/mdfs_disk.h"
#include "moduos/drivers/Drive/vDrive.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"

uint32_t mdfs_crc32_buf(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        uint32_t x = (crc ^ p[i]) & 0xFFu;
        for (int b = 0; b < 8; b++) {
            x = (x >> 1) ^ (0xEDB88320u & (-(int)(x & 1u)));
        }
        crc = (crc >> 8) ^ x;
    }
    return ~crc;
}

static uint32_t mdfs_blocks_to_lba(uint64_t block_no) {
    return (uint32_t)(block_no * (MDFS_BLOCK_SIZE / 512u));
}

int mdfs_disk_read_block(int vdrive_id, uint32_t start_lba, uint64_t block_no, void *out) {
    uint32_t lba = start_lba + mdfs_blocks_to_lba(block_no);
    return vdrive_read((uint8_t)vdrive_id, (uint64_t)lba, (MDFS_BLOCK_SIZE / 512u), out);
}

int mdfs_disk_write_block(int vdrive_id, uint32_t start_lba, uint64_t block_no, const void *in) {
    uint32_t lba = start_lba + mdfs_blocks_to_lba(block_no);
    return vdrive_write((uint8_t)vdrive_id, (uint64_t)lba, (MDFS_BLOCK_SIZE / 512u), in);
}

int mdfs_disk_read_inode(int vdrive_id, uint32_t start_lba, const mdfs_superblock_t *sb, uint32_t ino, mdfs_inode_t *out) {
    uint64_t byte_off = (uint64_t)ino * (uint64_t)MDFS_INODE_SIZE;
    uint64_t block = sb->inode_table_start + (byte_off / MDFS_BLOCK_SIZE);
    uint64_t off = byte_off % MDFS_BLOCK_SIZE;

    uint8_t *blk = (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
    if (!blk) return -1;
    if (mdfs_disk_read_block(vdrive_id, start_lba, block, blk) != VDRIVE_SUCCESS) { kfree(blk); return -2; }
    memcpy(out, blk + off, sizeof(*out));
    kfree(blk);
    return 0;
}

int mdfs_disk_write_inode(int vdrive_id, uint32_t start_lba, const mdfs_superblock_t *sb, uint32_t ino, const mdfs_inode_t *in) {
    uint64_t byte_off = (uint64_t)ino * (uint64_t)MDFS_INODE_SIZE;
    uint64_t block = sb->inode_table_start + (byte_off / MDFS_BLOCK_SIZE);
    uint64_t off = byte_off % MDFS_BLOCK_SIZE;

    uint8_t *blk = (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
    if (!blk) return -1;
    if (mdfs_disk_read_block(vdrive_id, start_lba, block, blk) != VDRIVE_SUCCESS) { kfree(blk); return -2; }
    memcpy(blk + off, in, sizeof(*in));
    if (mdfs_disk_write_block(vdrive_id, start_lba, block, blk) != VDRIVE_SUCCESS) { kfree(blk); return -3; }
    kfree(blk);
    return 0;
}
