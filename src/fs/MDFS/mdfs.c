#include "moduos/fs/MDFS/mdfs.h"
#include "moduos/fs/MDFS/mdfs_cache.h"
#include "moduos/drivers/Drive/vDrive.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/fs/MDFS/mdfs_dir.h"

#define MDFS_MAX_MOUNTS 16

static mdfs_fs_t g_mdfs[MDFS_MAX_MOUNTS];
static int g_mdfs_cache_initialized = 0;

static uint32_t mdfs_crc32(const void *data, size_t len) {
    // CRC32 (IEEE 802.3 polynomial 0xEDB88320)
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

static int mdfs_read_block(int vdrive_id, uint32_t start_lba, uint64_t block_no, void *out) {
    uint64_t lba = (uint64_t)start_lba + (block_no * (uint64_t)(MDFS_BLOCK_SIZE / 512u));
    uint32_t count = MDFS_BLOCK_SIZE / 512u;
    return vdrive_read((uint8_t)vdrive_id, lba, count, out);
}

static int mdfs_write_block(int vdrive_id, uint32_t start_lba, uint64_t block_no, const void *in) {
    uint64_t lba = (uint64_t)start_lba + (block_no * (uint64_t)(MDFS_BLOCK_SIZE / 512u));
    uint32_t count = MDFS_BLOCK_SIZE / 512u;
    return vdrive_write((uint8_t)vdrive_id, lba, count, in);
}

static uint64_t mdfs_meta_end(const mdfs_superblock_t *sb) {
    return sb->inode_table_start + sb->inode_table_blocks;
}

static int mdfs_read_inode_raw(const mdfs_fs_t *fs, uint32_t ino, mdfs_inode_t *out) {
    if (!fs || !out || ino == 0) return -1;
    uint64_t idx = (uint64_t)ino;
    uint64_t byte_off = idx * (uint64_t)MDFS_INODE_SIZE;
    uint64_t block = fs->sb.inode_table_start + (byte_off / MDFS_BLOCK_SIZE);
    uint64_t off = byte_off % MDFS_BLOCK_SIZE;

    uint8_t *blk = mdfs_buffer_acquire();
    if (!blk) return -2;
    if (mdfs_read_block(fs->vdrive_id, fs->start_lba, block, blk) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)blk); return -3; }
    memcpy(out, blk + off, sizeof(mdfs_inode_t));
    mdfs_buffer_release((uint8_t*)blk);
    return 0;
}

static int mdfs_write_inode_raw(const mdfs_fs_t *fs, uint32_t ino, const mdfs_inode_t *in) {
    if (!fs || !in || ino == 0) return -1;
    uint64_t idx = (uint64_t)ino;
    uint64_t byte_off = idx * (uint64_t)MDFS_INODE_SIZE;
    uint64_t block = fs->sb.inode_table_start + (byte_off / MDFS_BLOCK_SIZE);
    uint64_t off = byte_off % MDFS_BLOCK_SIZE;

    uint8_t *blk = mdfs_buffer_acquire();
    if (!blk) return -2;
    if (mdfs_read_block(fs->vdrive_id, fs->start_lba, block, blk) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)blk); return -3; }
    memcpy(blk + off, in, sizeof(mdfs_inode_t));
    if (mdfs_write_block(fs->vdrive_id, fs->start_lba, block, blk) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)blk); return -4; }
    mdfs_buffer_release((uint8_t*)blk);
    return 0;
}

static int mdfs_bitmap_test_set(uint8_t *bm, uint64_t idx) {
    uint64_t byte = idx / 8;
    uint8_t bit = (uint8_t)(1u << (idx % 8));
    int was = (bm[byte] & bit) != 0;
    bm[byte] |= bit;
    return was;
}

static int mdfs_alloc_block(const mdfs_fs_t *fs, uint64_t *out_block) {
    if (!fs || !out_block) return -1;

    uint64_t bits_per_bitmap_block = (uint64_t)MDFS_BLOCK_SIZE * 8ULL;
    uint64_t total = fs->sb.total_blocks;
    uint64_t start = mdfs_meta_end(&fs->sb);
    if (start < 1) start = 1;
    
    /* Use allocation hint to start search from last allocated block */
    mdfs_fs_t *fs_mut = (mdfs_fs_t*)fs;  /* Safe: we only update hint */
    if (fs_mut->alloc_hint_block > start) {
        start = fs_mut->alloc_hint_block;
    }

    uint8_t *bm = mdfs_buffer_acquire();
    if (!bm) return -2;

    for (uint64_t b = start; b < total; b++) {
        uint64_t blk_index = b / bits_per_bitmap_block;
        uint64_t off_bit = b % bits_per_bitmap_block;
        if (blk_index >= fs->sb.block_bitmap_blocks) break;

        if (mdfs_read_block(fs->vdrive_id, fs->start_lba, fs->sb.block_bitmap_start + blk_index, bm) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)bm); return -3; }
        uint64_t byte = off_bit / 8;
        uint8_t bit = (uint8_t)(1u << (off_bit % 8));
        if ((bm[byte] & bit) == 0) {
            bm[byte] |= bit;
            if (mdfs_write_block(fs->vdrive_id, fs->start_lba, fs->sb.block_bitmap_start + blk_index, bm) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)bm); return -4; }
            mdfs_buffer_release((uint8_t*)bm);
            *out_block = b;
            /* Update hint for next allocation */
            fs_mut->alloc_hint_block = b + 1;
            return 0;
        }
    }

    mdfs_buffer_release((uint8_t*)bm);
    return -5;
}

static int mdfs_alloc_inode(const mdfs_fs_t *fs, uint32_t *out_ino) {
    if (!fs || !out_ino) return -1;

    uint64_t bits_per_bitmap_block = (uint64_t)MDFS_BLOCK_SIZE * 8ULL;
    uint64_t total = fs->sb.total_inodes;
    
    /* Use allocation hint */
    mdfs_fs_t *fs_mut = (mdfs_fs_t*)fs;
    uint64_t start = (fs_mut->alloc_hint_inode > 0) ? fs_mut->alloc_hint_inode : 1;

    uint8_t *bm = mdfs_buffer_acquire();
    if (!bm) return -2;

    for (uint64_t i = start; i < total; i++) {
        uint64_t blk_index = i / bits_per_bitmap_block;
        uint64_t off_bit = i % bits_per_bitmap_block;
        if (blk_index >= fs->sb.inode_bitmap_blocks) break;

        if (mdfs_read_block(fs->vdrive_id, fs->start_lba, fs->sb.inode_bitmap_start + blk_index, bm) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)bm); return -3; }
        uint64_t byte = off_bit / 8;
        uint8_t bit = (uint8_t)(1u << (off_bit % 8));
        if ((bm[byte] & bit) == 0) {
            bm[byte] |= bit;
            if (mdfs_write_block(fs->vdrive_id, fs->start_lba, fs->sb.inode_bitmap_start + blk_index, bm) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)bm); return -4; }
            mdfs_buffer_release((uint8_t*)bm);
            *out_ino = (uint32_t)i;
            /* Update hint for next allocation */
            fs_mut->alloc_hint_inode = (uint32_t)(i + 1);
            return 0;
        }
    }

    mdfs_buffer_release((uint8_t*)bm);
    return -5;
}

static int mdfs_path_is_root(const char *path) {
    return (!path || path[0] == 0 || (path[0] == '/' && path[1] == 0));
}

static const char *mdfs_basename(const char *path) {
    if (!path) return "";
    const char *bn = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' && p[1]) bn = p + 1;
    }
    return bn;
}

const mdfs_fs_t *mdfs_get_fs(int handle) {
    if (handle < 0 || handle >= MDFS_MAX_MOUNTS) return NULL;
    if (!g_mdfs[handle].in_use) return NULL;
    return &g_mdfs[handle];
}

int mdfs_mount(int vdrive_id, uint32_t start_lba) {
    /* Ensure cache is initialized */
    if (!g_mdfs_cache_initialized) {
        mdfs_cache_init();
    }
    
    uint8_t *buf = mdfs_buffer_acquire();
    if (!buf) return -1;
    memset(buf, 0, MDFS_BLOCK_SIZE);

    com_printf(COM1_PORT, "[MDFS] mount: reading superblock from block 1 (LBA %u)\n", 
               (unsigned)(start_lba + (1 * (MDFS_BLOCK_SIZE/512u))));

    if (mdfs_read_block(vdrive_id, start_lba, 1, buf) != VDRIVE_SUCCESS) {
        com_write_string(COM1_PORT, "[MDFS] mount: read_block failed\n");
        mdfs_buffer_release((uint8_t*)buf);
        return -2;
    }

    mdfs_superblock_t sb;
    memcpy(&sb, buf, sizeof(sb));
    mdfs_buffer_release((uint8_t*)buf);

    com_printf(COM1_PORT, "[MDFS] mount: magic=0x%08x version=%u block_size=%u\n", 
               sb.magic, sb.version, sb.block_size);

    if (sb.magic != MDFS_MAGIC) {
        com_printf(COM1_PORT, "[MDFS] mount: magic mismatch (expected 0x%08x)\n", MDFS_MAGIC);
        return -3;
    }
    if (sb.version < 2 || sb.version > MDFS_VERSION) {
        com_printf(COM1_PORT, "[MDFS] mount: version %u out of range (2-%u)\n", sb.version, MDFS_VERSION);
        return -4;
    }
    if (sb.block_size != MDFS_BLOCK_SIZE) {
        com_printf(COM1_PORT, "[MDFS] mount: block_size %u != %u\n", sb.block_size, MDFS_BLOCK_SIZE);
        return -5;
    }

    // Verify checksum (best-effort; placeholder hash)
    uint32_t saved = sb.checksum;
    sb.checksum = 0;
    uint32_t calc = mdfs_crc32(&sb, sizeof(sb));
    if (saved != 0 && saved != calc) {
        com_write_string(COM1_PORT, "[MDFS] superblock checksum mismatch\n");
    }

    int handle = -1;
    for (int i = 0; i < MDFS_MAX_MOUNTS; i++) {
        if (!g_mdfs[i].in_use) { handle = i; break; }
    }
    if (handle < 0) return -6;

    memset(&g_mdfs[handle], 0, sizeof(g_mdfs[handle]));
    g_mdfs[handle].in_use = 1;
    g_mdfs[handle].vdrive_id = vdrive_id;
    g_mdfs[handle].start_lba = start_lba;
    g_mdfs[handle].sb = sb;
    g_mdfs[handle].alloc_hint_block = mdfs_meta_end(&sb);
    g_mdfs[handle].alloc_hint_inode = 1;

    return handle;
}

void mdfs_unmount(int handle) {
    if (handle < 0 || handle >= MDFS_MAX_MOUNTS) return;
    if (!g_mdfs[handle].in_use) return;
    
    /* Flush any cached data for this mount */
    mdfs_block_cache_flush(handle);
    mdfs_inode_cache_flush(handle);
    
    /* Mark slot as available */
    memset(&g_mdfs[handle], 0, sizeof(g_mdfs[handle]));
    g_mdfs[handle].in_use = 0;
}

void mdfs_cache_init(void) {
    if (g_mdfs_cache_initialized) return;
    
    mdfs_buffer_pool_init();
    mdfs_block_cache_init();
    mdfs_inode_cache_init();
    
    g_mdfs_cache_initialized = 1;
}

void mdfs_cache_flush_all(void) {
    mdfs_block_cache_flush(-1);
    mdfs_inode_cache_flush(-1);
}

int mdfs_mkfs(int vdrive_id, uint32_t start_lba, uint32_t sectors, const char *label) {
    (void)label;

    // v1: only supports 4KiB blocks on 512B sector disks.
    vdrive_t *d = vdrive_get((uint8_t)vdrive_id);
    if (!d || !d->present) return -1;
    if (d->sector_size != 512) return -2;
    if (sectors < (MDFS_BLOCK_SIZE/512u) * 32u) return -3; // minimum size

    uint64_t total_blocks = (uint64_t)sectors / (MDFS_BLOCK_SIZE/512u);

    // Layout (v2 scalable):
    // block0 reserved, block1 super, block2 backup,
    // then block bitmap (N blocks), inode bitmap (M blocks), inode table (K blocks), data after.
    uint64_t block_bitmap_start = 3;

    // Compute bitmap sizes.
    // One bitmap block tracks (4096*8)=32768 blocks/inodes.
    uint64_t bits_per_bitmap_block = (uint64_t)MDFS_BLOCK_SIZE * 8ULL;

    uint64_t block_bitmap_blocks = (total_blocks + bits_per_bitmap_block - 1) / bits_per_bitmap_block;
    if (block_bitmap_blocks < 1) block_bitmap_blocks = 1;

    uint64_t inode_bitmap_start = block_bitmap_start + block_bitmap_blocks;

    // Heuristic inode count: 1 inode per 16 blocks (tunable), minimum 128.
    uint64_t total_inodes = total_blocks / 16ULL;
    if (total_inodes < 128) total_inodes = 128;
    // inode 0 reserved
    if (total_inodes > bits_per_bitmap_block * 1024ULL) {
        // hard cap to avoid insane metadata on 2TB; still plenty of inodes
        total_inodes = bits_per_bitmap_block * 1024ULL;
    }

    uint64_t inode_bitmap_blocks = (total_inodes + bits_per_bitmap_block - 1) / bits_per_bitmap_block;
    if (inode_bitmap_blocks < 1) inode_bitmap_blocks = 1;

    uint64_t inode_table_start  = inode_bitmap_start + inode_bitmap_blocks;

    // inode table blocks required
    uint64_t inodes_per_block = (MDFS_BLOCK_SIZE / MDFS_INODE_SIZE);
    uint64_t inode_table_blocks = (total_inodes + inodes_per_block - 1) / inodes_per_block;
    if (inode_table_blocks < 8) inode_table_blocks = 8;

    uint64_t meta_end = inode_table_start + inode_table_blocks;
    if (meta_end >= total_blocks) return -4;

    mdfs_superblock_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = MDFS_MAGIC;
    sb.version = MDFS_VERSION;
    sb.block_size = MDFS_BLOCK_SIZE;
    sb.total_blocks = total_blocks;
    sb.free_blocks = total_blocks - meta_end;
    sb.total_inodes = total_inodes;
    sb.free_inodes = total_inodes - 1; // root inode allocated
    sb.block_bitmap_start = block_bitmap_start;
    sb.block_bitmap_blocks = block_bitmap_blocks;
    sb.inode_bitmap_start = inode_bitmap_start;
    sb.inode_bitmap_blocks = inode_bitmap_blocks;
    sb.inode_table_start = inode_table_start;
    sb.inode_table_blocks = inode_table_blocks;
    sb.root_inode = 1;

    // checksum (CRC32)
    sb.checksum = 0;
    sb.checksum = mdfs_crc32(&sb, sizeof(sb));

    uint8_t *blk = mdfs_buffer_acquire();
    if (!blk) return -5;

    // Zero and write superblock (block1)
    memset(blk, 0, MDFS_BLOCK_SIZE);
    memcpy(blk, &sb, sizeof(sb));
    if (vdrive_write((uint8_t)vdrive_id, (uint64_t)start_lba + (uint64_t)(1 * (MDFS_BLOCK_SIZE/512u)), (MDFS_BLOCK_SIZE/512u), blk) != VDRIVE_SUCCESS) {
        mdfs_buffer_release((uint8_t*)blk);
        return -6;
    }

    // Backup superblock (block2)
    if (vdrive_write((uint8_t)vdrive_id, (uint64_t)start_lba + (uint64_t)(2 * (MDFS_BLOCK_SIZE/512u)), (MDFS_BLOCK_SIZE/512u), blk) != VDRIVE_SUCCESS) {
        mdfs_buffer_release((uint8_t*)blk);
        return -7;
    }

    // Create root directory block (first free block after metadata)
    uint64_t root_dir_block = meta_end;
    if (root_dir_block >= total_blocks) { mdfs_buffer_release((uint8_t*)blk); return -4; }

    // Initialize bitmaps and inode table
    memset(blk, 0, MDFS_BLOCK_SIZE);

    // Initialize all bitmap blocks to zero
    memset(blk, 0, MDFS_BLOCK_SIZE);
    for (uint64_t i = 0; i < block_bitmap_blocks; i++) {
        uint64_t lba = (uint64_t)start_lba + ((block_bitmap_start + i) * (uint64_t)(MDFS_BLOCK_SIZE/512u));
        if (vdrive_write((uint8_t)vdrive_id, lba, (MDFS_BLOCK_SIZE/512u), blk) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)blk); return -8; }
    }
    for (uint64_t i = 0; i < inode_bitmap_blocks; i++) {
        uint64_t lba = (uint64_t)start_lba + ((inode_bitmap_start + i) * (uint64_t)(MDFS_BLOCK_SIZE/512u));
        if (vdrive_write((uint8_t)vdrive_id, lba, (MDFS_BLOCK_SIZE/512u), blk) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)blk); return -9; }
    }

    // Mark metadata blocks used in block bitmap
    uint8_t *bb = blk;
    for (uint64_t b = 0; b < meta_end; b++) {
        uint64_t bit = b;
        uint64_t blk_index = bit / bits_per_bitmap_block;
        uint64_t off_bit = bit % bits_per_bitmap_block;
        memset(blk, 0, MDFS_BLOCK_SIZE);
        uint64_t lba = (uint64_t)start_lba + ((block_bitmap_start + blk_index) * (uint64_t)(MDFS_BLOCK_SIZE/512u));
        if (vdrive_read((uint8_t)vdrive_id, lba, (MDFS_BLOCK_SIZE/512u), blk) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)blk); return -10; }
        bb = blk;
        bb[off_bit / 8] |= (uint8_t)(1u << (off_bit % 8));
        if (vdrive_write((uint8_t)vdrive_id, lba, (MDFS_BLOCK_SIZE/512u), blk) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)blk); return -11; }
    }

    // also mark root_dir_block used
    {
        uint64_t bit = root_dir_block;
        uint64_t blk_index = bit / bits_per_bitmap_block;
        uint64_t off_bit = bit % bits_per_bitmap_block;
        uint64_t lba = (uint64_t)start_lba + ((block_bitmap_start + blk_index) * (uint64_t)(MDFS_BLOCK_SIZE/512u));
        memset(blk, 0, MDFS_BLOCK_SIZE);
        if (vdrive_read((uint8_t)vdrive_id, lba, (MDFS_BLOCK_SIZE/512u), blk) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)blk); return -12; }
        bb = blk;
        bb[off_bit / 8] |= (uint8_t)(1u << (off_bit % 8));
        if (vdrive_write((uint8_t)vdrive_id, lba, (MDFS_BLOCK_SIZE/512u), blk) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)blk); return -13; }
    }

    // account for allocated root dir block
    if (sb.free_blocks > 0) sb.free_blocks -= 1;

    // inode bitmap: mark inode 1 used (inode0 reserved)
    {
        uint64_t bit = 1;
        uint64_t blk_index = bit / bits_per_bitmap_block;
        uint64_t off_bit = bit % bits_per_bitmap_block;
        uint64_t lba = (uint64_t)start_lba + ((inode_bitmap_start + blk_index) * (uint64_t)(MDFS_BLOCK_SIZE/512u));
        memset(blk, 0, MDFS_BLOCK_SIZE);
        if (vdrive_read((uint8_t)vdrive_id, lba, (MDFS_BLOCK_SIZE/512u), blk) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)blk); return -14; }
        blk[off_bit / 8] |= (uint8_t)(1u << (off_bit % 8));
        if (vdrive_write((uint8_t)vdrive_id, lba, (MDFS_BLOCK_SIZE/512u), blk) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)blk); return -15; }
    }

    // inode table blocks zero
    memset(blk, 0, MDFS_BLOCK_SIZE);
    for (uint64_t i = 0; i < inode_table_blocks; i++) {
        uint64_t lba = (uint64_t)start_lba + ((inode_table_start + i) * (uint64_t)(MDFS_BLOCK_SIZE/512u));
        if (vdrive_write((uint8_t)vdrive_id, lba, (MDFS_BLOCK_SIZE/512u), blk) != VDRIVE_SUCCESS) {
            mdfs_buffer_release((uint8_t*)blk);
            return -10;
        }
    }

    // Write root inode (#1) into inode table
    mdfs_inode_t root;
    memset(&root, 0, sizeof(root));
    root.mode = 0x4000; // directory
    root.link_count = 1;
    root.size_bytes = 0;
    root.direct[0] = root_dir_block;

    // Root inode lives at inode_table_start + offset
    uint64_t byte_off = (uint64_t)sb.root_inode * (uint64_t)MDFS_INODE_SIZE;
    uint64_t inode_blk = inode_table_start + (byte_off / MDFS_BLOCK_SIZE);
    uint64_t inode_off = byte_off % MDFS_BLOCK_SIZE;

    memset(blk, 0, MDFS_BLOCK_SIZE);
    uint64_t inode_lba = (uint64_t)start_lba + (inode_blk * (uint64_t)(MDFS_BLOCK_SIZE/512u));
    if (vdrive_read((uint8_t)vdrive_id, inode_lba, (MDFS_BLOCK_SIZE/512u), blk) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)blk); return -11; }
    memcpy(blk + inode_off, &root, sizeof(root));
    if (vdrive_write((uint8_t)vdrive_id, inode_lba, (MDFS_BLOCK_SIZE/512u), blk) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)blk); return -12; }

    // Write empty root directory block
    memset(blk, 0, MDFS_BLOCK_SIZE);
    uint64_t dir_lba = (uint64_t)start_lba + (root_dir_block * (uint64_t)(MDFS_BLOCK_SIZE/512u));
    if (vdrive_write((uint8_t)vdrive_id, dir_lba, (MDFS_BLOCK_SIZE/512u), blk) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)blk); return -13; }

    // --- mkfs smoke test content (v2): create lost+found (dir) and test.txt (file)
    {
        mdfs_fs_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.in_use = 1;
        tmp.vdrive_id = vdrive_id;
        tmp.start_lba = start_lba;
        tmp.sb = sb;

        // lost+found directory
        uint32_t lf_ino = 0;
        if (mdfs_alloc_inode(&tmp, &lf_ino) == 0) {
            uint64_t lf_block = 0;
            if (mdfs_alloc_block(&tmp, &lf_block) == 0) {
                // zero dir block
                uint8_t *z = mdfs_buffer_acquire();
                if (z) {
                    memset(z, 0, MDFS_BLOCK_SIZE);
                    (void)mdfs_write_block(vdrive_id, start_lba, lf_block, z);
                    mdfs_buffer_release((uint8_t*)z);
                }

                mdfs_inode_t lf;
                memset(&lf, 0, sizeof(lf));
                lf.mode = 0x4000;
                lf.link_count = 1;
                lf.direct[0] = lf_block;
                (void)mdfs_write_inode_raw(&tmp, lf_ino, &lf);
                (void)mdfs_v2_root_add_export(&tmp, "lost+found", lf_ino, 2);
            }
        }

        // test.txt file
        uint32_t tf_ino = 0;
        if (mdfs_alloc_inode(&tmp, &tf_ino) == 0) {
            mdfs_inode_t fi;
            memset(&fi, 0, sizeof(fi));
            fi.mode = 0x8000;
            fi.link_count = 1;
            const char *msg = "MDFS OK\n";
            size_t msg_len = strlen(msg);

            // allocate one data block
            uint64_t fb = 0;
            if (mdfs_alloc_block(&tmp, &fb) == 0) {
                uint8_t *z = mdfs_buffer_acquire();
                if (z) {
                    memset(z, 0, MDFS_BLOCK_SIZE);
                    memcpy(z, msg, msg_len);
                    (void)mdfs_write_block(vdrive_id, start_lba, fb, z);
                    mdfs_buffer_release((uint8_t*)z);
                }
                fi.direct[0] = fb;
                fi.size_bytes = msg_len;
                (void)mdfs_write_inode_raw(&tmp, tf_ino, &fi);
                (void)mdfs_v2_root_add_export(&tmp, "test.txt", tf_ino, 1);
            }
        }
    }

    // Rewrite superblock (with updated free_blocks + checksum)
    sb.checksum = 0;
    sb.checksum = mdfs_crc32(&sb, sizeof(sb));
    memset(blk, 0, MDFS_BLOCK_SIZE);
    memcpy(blk, &sb, sizeof(sb));
    
    com_printf(COM1_PORT, "[MDFS] mkfs: writing superblock to block 1 (LBA %u)\n", 
               (unsigned)(start_lba + (1 * (MDFS_BLOCK_SIZE/512u))));
    
    if (mdfs_write_block(vdrive_id, start_lba, 1, blk) != VDRIVE_SUCCESS) { 
        mdfs_buffer_release(blk);
        return -14;
    }
    if (mdfs_write_block(vdrive_id, start_lba, 2, blk) != VDRIVE_SUCCESS) { 
        mdfs_buffer_release(blk);
        return -15;
    }

    mdfs_buffer_release(blk);
    
    /* Invalidate all vdrive cache entries to ensure fresh reads after mkfs */
    vdrive_cache_invalidate_all((uint8_t)vdrive_id);
    
    /* Flush device write cache to persist the superblock */
    vdrive_flush((uint8_t)vdrive_id);
    
    com_write_string(COM1_PORT, "[MDFS] mkfs: successfully created filesystem\n");
    return 0;
}
