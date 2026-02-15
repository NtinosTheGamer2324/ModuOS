#include "moduos/fs/MDFS/mdfs.h"
#include "moduos/drivers/Drive/vDrive.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/fs/MDFS/mdfs_dir.h"

#define MDFS_MAX_MOUNTS 16

static mdfs_fs_t g_mdfs[MDFS_MAX_MOUNTS];

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
    uint64_t byte_off = block_no * (uint64_t)MDFS_BLOCK_SIZE;
    uint64_t lba = start_lba + (uint32_t)(byte_off / 512u);
    uint32_t count = MDFS_BLOCK_SIZE / 512u;
    return vdrive_read((uint8_t)vdrive_id, (uint64_t)lba, count, out);
}

static int mdfs_write_block(int vdrive_id, uint32_t start_lba, uint64_t block_no, const void *in) {
    uint64_t byte_off = block_no * (uint64_t)MDFS_BLOCK_SIZE;
    uint64_t lba = start_lba + (uint32_t)(byte_off / 512u);
    uint32_t count = MDFS_BLOCK_SIZE / 512u;
    return vdrive_write((uint8_t)vdrive_id, (uint64_t)lba, count, in);
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

    uint8_t *blk = (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
    if (!blk) return -2;
    if (mdfs_read_block(fs->vdrive_id, fs->start_lba, block, blk) != VDRIVE_SUCCESS) { kfree(blk); return -3; }
    memcpy(out, blk + off, sizeof(mdfs_inode_t));
    kfree(blk);
    return 0;
}

static int mdfs_write_inode_raw(const mdfs_fs_t *fs, uint32_t ino, const mdfs_inode_t *in) {
    if (!fs || !in || ino == 0) return -1;
    uint64_t idx = (uint64_t)ino;
    uint64_t byte_off = idx * (uint64_t)MDFS_INODE_SIZE;
    uint64_t block = fs->sb.inode_table_start + (byte_off / MDFS_BLOCK_SIZE);
    uint64_t off = byte_off % MDFS_BLOCK_SIZE;

    uint8_t *blk = (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
    if (!blk) return -2;
    if (mdfs_read_block(fs->vdrive_id, fs->start_lba, block, blk) != VDRIVE_SUCCESS) { kfree(blk); return -3; }
    memcpy(blk + off, in, sizeof(mdfs_inode_t));
    if (mdfs_write_block(fs->vdrive_id, fs->start_lba, block, blk) != VDRIVE_SUCCESS) { kfree(blk); return -4; }
    kfree(blk);
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
    uint8_t *bm = (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
    if (!bm) return -2;
    if (mdfs_read_block(fs->vdrive_id, fs->start_lba, fs->sb.block_bitmap_start, bm) != VDRIVE_SUCCESS) { kfree(bm); return -3; }

    uint64_t total = fs->sb.total_blocks;
    uint64_t start = mdfs_meta_end(&fs->sb);
    if (start < 1) start = 1;

    for (uint64_t b = start; b < total; b++) {
        if (!mdfs_bitmap_test_set(bm, b)) {
            if (mdfs_write_block(fs->vdrive_id, fs->start_lba, fs->sb.block_bitmap_start, bm) != VDRIVE_SUCCESS) { kfree(bm); return -4; }
            kfree(bm);
            *out_block = b;
            return 0;
        }
    }

    kfree(bm);
    return -5;
}

static int mdfs_alloc_inode(const mdfs_fs_t *fs, uint32_t *out_ino) {
    if (!fs || !out_ino) return -1;
    uint8_t *bm = (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
    if (!bm) return -2;
    if (mdfs_read_block(fs->vdrive_id, fs->start_lba, fs->sb.inode_bitmap_start, bm) != VDRIVE_SUCCESS) { kfree(bm); return -3; }

    uint64_t total = fs->sb.total_inodes;
    for (uint64_t i = 1; i < total; i++) {
        if (!mdfs_bitmap_test_set(bm, i)) {
            if (mdfs_write_block(fs->vdrive_id, fs->start_lba, fs->sb.inode_bitmap_start, bm) != VDRIVE_SUCCESS) { kfree(bm); return -4; }
            kfree(bm);
            *out_ino = (uint32_t)i;
            return 0;
        }
    }

    kfree(bm);
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

#if 0
/* MDFS v1 fixed-dirent helpers removed in v2 (entry-set directory format). */
static int mdfs_find_in_root_removed(const mdfs_fs_t *fs, const char *name, uint32_t *out_ino, uint8_t *out_type) {
    if (!fs || !name || !name[0]) return -1;

    mdfs_inode_t root;
    if (mdfs_read_inode_raw(fs, (uint32_t)fs->sb.root_inode, &root) != 0) return -2;
    if ((root.mode & 0xF000) != 0x4000) return -3;
    uint64_t dir_block = root.direct[0];
    if (!dir_block) return -4;

    uint8_t *blk = (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
    if (!blk) return -5;
    if (mdfs_read_block(fs->vdrive_id, fs->start_lba, dir_block, blk) != VDRIVE_SUCCESS) { kfree(blk); return -6; }

    int max = (int)(MDFS_BLOCK_SIZE / MDFS_DIRENT_SIZE);
    for (int i = 0; i < max; i++) {
        mdfs_dirent_t *de = (mdfs_dirent_t*)(blk + i * MDFS_DIRENT_SIZE);
        if (de->inode == 0) continue;
        if (de->name_len == 0 || de->name_len > (MDFS_DIRENT_SIZE - 8)) continue;
        if (strncmp(de->name, name, de->name_len) == 0 && name[de->name_len] == 0) {
            if (out_ino) *out_ino = de->inode;
            if (out_type) *out_type = de->type;
            kfree(blk);
            return 0;
        }
    }

    kfree(blk);
    return -7;
}

/* MDFS v1 fixed-dirent helpers removed in v2 (entry-set directory format). */
static int mdfs_add_to_root_removed(const mdfs_fs_t *fs, const char *name, uint32_t ino, uint8_t type) {
    if (!fs || !name || !name[0]) return -1;
    size_t nl = strlen(name);
    if (nl > (MDFS_DIRENT_SIZE - 8)) return -2;

    mdfs_inode_t root;
    if (mdfs_read_inode_raw(fs, (uint32_t)fs->sb.root_inode, &root) != 0) return -3;
    uint64_t dir_block = root.direct[0];
    if (!dir_block) return -4;

    uint8_t *blk = (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
    if (!blk) return -5;
    if (mdfs_read_block(fs->vdrive_id, fs->start_lba, dir_block, blk) != VDRIVE_SUCCESS) { kfree(blk); return -6; }

    int max = (int)(MDFS_BLOCK_SIZE / MDFS_DIRENT_SIZE);
    for (int i = 0; i < max; i++) {
        mdfs_dirent_t *de = (mdfs_dirent_t*)(blk + i * MDFS_DIRENT_SIZE);
        if (de->inode != 0) continue;
        memset(de, 0, sizeof(*de));
        de->inode = ino;
        de->type = type;
        de->name_len = (uint8_t)nl;
        memcpy(de->name, name, nl);
        if (mdfs_write_block(fs->vdrive_id, fs->start_lba, dir_block, blk) != VDRIVE_SUCCESS) { kfree(blk); return -7; }
        kfree(blk);
        return 0;
    }

    kfree(blk);
    return -8;
}

static int mdfs_read_file_inode(const mdfs_fs_t *fs, const mdfs_inode_t *ino, void *buffer, size_t buffer_size, size_t *bytes_read) {
    if (!fs || !ino || !buffer || !bytes_read) return -1;
    size_t to_read = (size_t)ino->size_bytes;
    if (to_read > buffer_size) to_read = buffer_size;

    uint8_t *blk = (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
    if (!blk) return -2;

    size_t done = 0;
    while (done < to_read) {
        uint64_t bi = done / MDFS_BLOCK_SIZE;
        uint64_t boff = done % MDFS_BLOCK_SIZE;
        if (bi >= MDFS_MAX_DIRECT) break;
        uint64_t bno = ino->direct[bi];
        if (!bno) break;
        if (mdfs_read_block(fs->vdrive_id, fs->start_lba, bno, blk) != VDRIVE_SUCCESS) { kfree(blk); return -3; }
        size_t chunk = MDFS_BLOCK_SIZE - (size_t)boff;
        if (chunk > (to_read - done)) chunk = to_read - done;
        memcpy((uint8_t*)buffer + done, blk + boff, chunk);
        done += chunk;
    }

    kfree(blk);
    *bytes_read = done;
    return 0;
}

static int mdfs_write_file_inode(const mdfs_fs_t *fs, mdfs_inode_t *ino, const void *buffer, size_t size) {
    if (!fs || !ino || (!buffer && size != 0)) return -1;

    uint8_t *blk = (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
    if (!blk) return -2;

    size_t done = 0;
    while (done < size) {
        uint64_t bi = done / MDFS_BLOCK_SIZE;
        uint64_t boff = done % MDFS_BLOCK_SIZE;
        if (bi >= MDFS_MAX_DIRECT) { kfree(blk); return -3; }
        if (ino->direct[bi] == 0) {
            uint64_t nb = 0;
            if (mdfs_alloc_block(fs, &nb) != 0) { kfree(blk); return -4; }
            ino->direct[bi] = nb;
            // zero block
            memset(blk, 0, MDFS_BLOCK_SIZE);
            if (mdfs_write_block(fs->vdrive_id, fs->start_lba, nb, blk) != VDRIVE_SUCCESS) { kfree(blk); return -5; }
        }
        uint64_t bno = ino->direct[bi];
        if (mdfs_read_block(fs->vdrive_id, fs->start_lba, bno, blk) != VDRIVE_SUCCESS) { kfree(blk); return -6; }
        size_t chunk = MDFS_BLOCK_SIZE - (size_t)boff;
        if (chunk > (size - done)) chunk = size - done;
        memcpy(blk + boff, (const uint8_t*)buffer + done, chunk);
        if (mdfs_write_block(fs->vdrive_id, fs->start_lba, bno, blk) != VDRIVE_SUCCESS) { kfree(blk); return -7; }
        done += chunk;
    }

    ino->size_bytes = size;
    kfree(blk);
    return 0;
}
#endif

const mdfs_fs_t *mdfs_get_fs(int handle) {
    if (handle < 0 || handle >= MDFS_MAX_MOUNTS) return NULL;
    if (!g_mdfs[handle].in_use) return NULL;
    return &g_mdfs[handle];
}

int mdfs_mount(int vdrive_id, uint32_t start_lba) {
    uint8_t *buf = (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
    if (!buf) return -1;
    memset(buf, 0, MDFS_BLOCK_SIZE);

    if (mdfs_read_block(vdrive_id, start_lba, 1, buf) != VDRIVE_SUCCESS) {
        kfree(buf);
        return -2;
    }

    mdfs_superblock_t sb;
    memcpy(&sb, buf, sizeof(sb));
    kfree(buf);

    if (sb.magic != MDFS_MAGIC) return -3;
    if (sb.version != MDFS_VERSION) return -4; /* requires v2 */
    if (sb.block_size != MDFS_BLOCK_SIZE) return -5;

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

    return handle;
}

int mdfs_mkfs(int vdrive_id, uint32_t start_lba, uint32_t sectors, const char *label) {
    (void)label;

    // v1: only supports 4KiB blocks on 512B sector disks.
    vdrive_t *d = vdrive_get((uint8_t)vdrive_id);
    if (!d || !d->present) return -1;
    if (d->sector_size != 512) return -2;
    if (sectors < (MDFS_BLOCK_SIZE/512u) * 32u) return -3; // minimum size

    uint64_t total_blocks = (uint64_t)sectors / (MDFS_BLOCK_SIZE/512u);

    // Layout (very simple v1):
    // block0 reserved, block1 super, block2 backup, block3 block bitmap (1 block),
    // block4 inode bitmap (1 block), block5.. inode table (8 blocks), data after.
    uint64_t block_bitmap_start = 3;
    uint64_t block_bitmap_blocks = 1;
    uint64_t inode_bitmap_start = 4;
    uint64_t inode_bitmap_blocks = 1;
    uint64_t inode_table_start  = 5;
    uint64_t inode_table_blocks = 8;

    uint64_t meta_end = inode_table_start + inode_table_blocks;
    if (meta_end >= total_blocks) return -4;

    // Fixed inode count for v1: inode_table_blocks * (4096/256)
    uint64_t total_inodes = inode_table_blocks * (MDFS_BLOCK_SIZE / 256u);

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

    uint8_t *blk = (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
    if (!blk) return -5;

    // Zero and write superblock (block1)
    memset(blk, 0, MDFS_BLOCK_SIZE);
    memcpy(blk, &sb, sizeof(sb));
    if (vdrive_write((uint8_t)vdrive_id, (uint64_t)(start_lba + (uint32_t)(1 * (MDFS_BLOCK_SIZE/512u))), (MDFS_BLOCK_SIZE/512u), blk) != VDRIVE_SUCCESS) {
        kfree(blk);
        return -6;
    }

    // Backup superblock (block2)
    if (vdrive_write((uint8_t)vdrive_id, (uint64_t)(start_lba + (uint32_t)(2 * (MDFS_BLOCK_SIZE/512u))), (MDFS_BLOCK_SIZE/512u), blk) != VDRIVE_SUCCESS) {
        kfree(blk);
        return -7;
    }

    // Create root directory block (first free block after metadata)
    uint64_t root_dir_block = meta_end;
    if (root_dir_block >= total_blocks) { kfree(blk); return -4; }

    // Initialize bitmaps and inode table
    memset(blk, 0, MDFS_BLOCK_SIZE);

    // block bitmap: mark blocks [0..meta_end-1] used AND root_dir_block used
    uint8_t *bb = blk;
    for (uint64_t b = 0; b < meta_end; b++) {
        bb[b / 8] |= (uint8_t)(1u << (b % 8));
    }
    bb[root_dir_block / 8] |= (uint8_t)(1u << (root_dir_block % 8));

    // account for allocated root dir block
    if (sb.free_blocks > 0) sb.free_blocks -= 1;

    if (vdrive_write((uint8_t)vdrive_id, (uint64_t)(start_lba + (uint32_t)(block_bitmap_start * (MDFS_BLOCK_SIZE/512u))), (MDFS_BLOCK_SIZE/512u), blk) != VDRIVE_SUCCESS) {
        kfree(blk);
        return -8;
    }

    // inode bitmap: mark inode 1 used
    memset(blk, 0, MDFS_BLOCK_SIZE);
    blk[0] |= 0x02; // inode #1 => bit1 (assuming inode0 reserved)
    if (vdrive_write((uint8_t)vdrive_id, (uint64_t)(start_lba + (uint32_t)(inode_bitmap_start * (MDFS_BLOCK_SIZE/512u))), (MDFS_BLOCK_SIZE/512u), blk) != VDRIVE_SUCCESS) {
        kfree(blk);
        return -9;
    }

    // inode table blocks zero
    memset(blk, 0, MDFS_BLOCK_SIZE);
    for (uint64_t i = 0; i < inode_table_blocks; i++) {
        uint32_t lba = start_lba + (uint32_t)((inode_table_start + i) * (MDFS_BLOCK_SIZE/512u));
        if (vdrive_write((uint8_t)vdrive_id, (uint64_t)lba, (MDFS_BLOCK_SIZE/512u), blk) != VDRIVE_SUCCESS) {
            kfree(blk);
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
    uint32_t inode_lba = start_lba + (uint32_t)(inode_blk * (MDFS_BLOCK_SIZE/512u));
    if (vdrive_read((uint8_t)vdrive_id, (uint64_t)inode_lba, (MDFS_BLOCK_SIZE/512u), blk) != VDRIVE_SUCCESS) { kfree(blk); return -11; }
    memcpy(blk + inode_off, &root, sizeof(root));
    if (vdrive_write((uint8_t)vdrive_id, (uint64_t)inode_lba, (MDFS_BLOCK_SIZE/512u), blk) != VDRIVE_SUCCESS) { kfree(blk); return -12; }

    // Write empty root directory block
    memset(blk, 0, MDFS_BLOCK_SIZE);
    uint32_t dir_lba = start_lba + (uint32_t)(root_dir_block * (MDFS_BLOCK_SIZE/512u));
    if (vdrive_write((uint8_t)vdrive_id, (uint64_t)dir_lba, (MDFS_BLOCK_SIZE/512u), blk) != VDRIVE_SUCCESS) { kfree(blk); return -13; }

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
                uint8_t *z = (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
                if (z) {
                    memset(z, 0, MDFS_BLOCK_SIZE);
                    (void)mdfs_write_block(vdrive_id, start_lba, lf_block, z);
                    kfree(z);
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
                uint8_t *z = (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
                if (z) {
                    memset(z, 0, MDFS_BLOCK_SIZE);
                    memcpy(z, msg, msg_len);
                    (void)mdfs_write_block(vdrive_id, start_lba, fb, z);
                    kfree(z);
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
    if (vdrive_write((uint8_t)vdrive_id, (uint64_t)(start_lba + (uint32_t)(1 * (MDFS_BLOCK_SIZE/512u))), (MDFS_BLOCK_SIZE/512u), blk) != VDRIVE_SUCCESS) { kfree(blk); return -14; }
    if (vdrive_write((uint8_t)vdrive_id, (uint64_t)(start_lba + (uint32_t)(2 * (MDFS_BLOCK_SIZE/512u))), (MDFS_BLOCK_SIZE/512u), blk) != VDRIVE_SUCCESS) { kfree(blk); return -15; }

    kfree(blk);
    return 0;
}
