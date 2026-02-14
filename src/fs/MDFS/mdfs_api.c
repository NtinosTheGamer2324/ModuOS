#include "moduos/fs/MDFS/mdfs.h"
#include "moduos/fs/MDFS/mdfs_cache.h"
#include "moduos/drivers/Drive/vDrive.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/fs/MDFS/mdfs_dir.h"
#include "moduos/fs/MDFS/mdfs_disk.h"
#include "moduos/fs/MDFS/mdfs_cache.h"
#include "moduos/fs/MDFS/mdfs_acl_helpers.h"
#include "moduos/kernel/process/process.h"

/* Forward declarations (used by indirect helpers) */
static int mdfs_alloc_block_simple(const mdfs_fs_t *fs, uint64_t *out_block);
static int mdfs_free_block_simple(const mdfs_fs_t *fs, uint64_t bno);

/* --- Indirect block helpers (C, EXT2-style) --- */
#define MDFS_PTRS_PER_BLOCK ((uint32_t)(MDFS_BLOCK_SIZE / 8u))

static int mdfs_bitmap_test(uint8_t *bm, uint64_t idx) {
    uint64_t byte = idx / 8;
    uint8_t bit = (uint8_t)(1u << (idx % 8));
    return (bm[byte] & bit) != 0;
}

static void mdfs_bitmap_set(uint8_t *bm, uint64_t idx) {
    uint64_t byte = idx / 8;
    uint8_t bit = (uint8_t)(1u << (idx % 8));
    bm[byte] |= bit;
}

static void mdfs_bitmap_clear(uint8_t *bm, uint64_t idx) {
    uint64_t byte = idx / 8;
    uint8_t bit = (uint8_t)(1u << (idx % 8));
    bm[byte] &= (uint8_t)~bit;
}

/* Multi-block bitmap read/modify/write helper.
 * bit_base is the first bit index represented by bitmap block 0.
 */
static int mdfs_bitmap_find_and_set(const mdfs_fs_t *fs, uint64_t bitmap_start, uint64_t bitmap_blocks,
                                   uint64_t start_bit, uint64_t end_bit, uint64_t *out_bit) {
    if (!fs || bitmap_blocks == 0 || !out_bit) return -1;

    uint8_t *bm = mdfs_buffer_acquire();
    if (!bm) return -2;

    uint64_t bits_per_block = (uint64_t)MDFS_BLOCK_SIZE * 8ULL;

    for (uint64_t bit = start_bit; bit < end_bit; bit++) {
        uint64_t blk_index = bit / bits_per_block;
        uint64_t off_bit = bit % bits_per_block;
        if (blk_index >= bitmap_blocks) break;

        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, bitmap_start + blk_index, bm) != VDRIVE_SUCCESS) {
            mdfs_buffer_release((uint8_t*)bm);
            return -3;
        }

        if (!mdfs_bitmap_test(bm, off_bit)) {
            mdfs_bitmap_set(bm, off_bit);
            if (mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, bitmap_start + blk_index, bm) != VDRIVE_SUCCESS) {
                mdfs_buffer_release((uint8_t*)bm);
                return -4;
            }
            mdfs_buffer_release((uint8_t*)bm);
            *out_bit = bit;
            return 0;
        }
    }

    mdfs_buffer_release((uint8_t*)bm);
    return -5;
}

/* Allocate up to `want` bits from a multi-block bitmap, batching bitmap IO.
 * - Scans from start_bit to end_bit.
 * - Modifies each bitmap block in memory and writes it back at most once.
 */
static int mdfs_bitmap_alloc_many(const mdfs_fs_t *fs, uint64_t bitmap_start, uint64_t bitmap_blocks,
                                 uint64_t start_bit, uint64_t end_bit,
                                 uint32_t want, uint64_t *out_bits, uint32_t *out_got) {
    if (!fs || bitmap_blocks == 0 || want == 0 || !out_bits || !out_got) return -1;

    *out_got = 0;

    uint8_t *bm = mdfs_buffer_acquire();
    if (!bm) return -2;

    uint64_t bits_per_block = (uint64_t)MDFS_BLOCK_SIZE * 8ULL;

    uint64_t bit = start_bit;
    while (bit < end_bit && *out_got < want) {
        uint64_t blk_index = bit / bits_per_block;
        if (blk_index >= bitmap_blocks) break;

        uint64_t blk_bit_base = blk_index * bits_per_block;
        uint64_t blk_bit_end = blk_bit_base + bits_per_block;
        if (blk_bit_end > end_bit) blk_bit_end = end_bit;

        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, bitmap_start + blk_index, bm) != VDRIVE_SUCCESS) {
            mdfs_buffer_release((uint8_t*)bm);
            return -3;
        }

        int dirty = 0;

        /* Start within this bitmap block */
        if (bit < blk_bit_base) bit = blk_bit_base;

        for (; bit < blk_bit_end && *out_got < want; bit++) {
            uint64_t off_bit = bit - blk_bit_base;
            if (!mdfs_bitmap_test(bm, off_bit)) {
                mdfs_bitmap_set(bm, off_bit);
                out_bits[*out_got] = bit;
                (*out_got)++;
                dirty = 1;
            }
        }

        if (dirty) {
            if (mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, bitmap_start + blk_index, bm) != VDRIVE_SUCCESS) {
                mdfs_buffer_release((uint8_t*)bm);
                return -4;
            }
        }
    }

    mdfs_buffer_release((uint8_t*)bm);
    return (*out_got > 0) ? 0 : -5;
}

static int mdfs_bitmap_clear_bit(const mdfs_fs_t *fs, uint64_t bitmap_start, uint64_t bitmap_blocks, uint64_t bit) {
    if (!fs || bitmap_blocks == 0) return -1;

    uint64_t bits_per_block = (uint64_t)MDFS_BLOCK_SIZE * 8ULL;
    uint64_t blk_index = bit / bits_per_block;
    uint64_t off_bit = bit % bits_per_block;
    if (blk_index >= bitmap_blocks) return -2;

    uint8_t *bm = mdfs_buffer_acquire();
    if (!bm) return -3;

    if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, bitmap_start + blk_index, bm) != VDRIVE_SUCCESS) {
        mdfs_buffer_release((uint8_t*)bm);
        return -4;
    }

    mdfs_bitmap_clear(bm, off_bit);
    if (mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, bitmap_start + blk_index, bm) != VDRIVE_SUCCESS) {
        mdfs_buffer_release((uint8_t*)bm);
        return -5;
    }

    mdfs_buffer_release((uint8_t*)bm);
    return 0;
}

/* Clear up to `count` bits from a multi-block bitmap, batching bitmap IO.
 * This is critical for performance of unlink/rmdir on large files.
 */
static int mdfs_bitmap_clear_many(const mdfs_fs_t *fs, uint64_t bitmap_start, uint64_t bitmap_blocks,
                                 const uint64_t *bits, uint32_t count) {
    if (!fs || bitmap_blocks == 0 || !bits || count == 0) return -1;

    const uint64_t bits_per_block = (uint64_t)MDFS_BLOCK_SIZE * 8ULL;

    uint8_t *bm = mdfs_buffer_acquire();
    if (!bm) return -2;

    /* Simple O(n) grouping by bitmap block (count is typically small per call site).
     * If you later want faster, sort bits and do linear groups.
     */
    uint64_t used[256];
    uint32_t used_n = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint64_t bit = bits[i];
        uint64_t blk_index = bit / bits_per_block;
        if (blk_index >= bitmap_blocks) continue;

        /* Check if already processed this blk_index */
        int seen = 0;
        for (uint32_t j = 0; j < used_n; j++) {
            if (used[j] == blk_index) { seen = 1; break; }
        }
        if (seen) continue;

        /* Mark seen (note: this limits us to 256 bitmap blocks per clear_many call).
         * For large files we call this repeatedly with small batches.
         */
        if (used_n < (uint32_t)(sizeof(used)/sizeof(used[0]))) used[used_n++] = blk_index;

        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, bitmap_start + blk_index, bm) != VDRIVE_SUCCESS) {
            mdfs_buffer_release((uint8_t*)bm);
            return -3;
        }

        int dirty = 0;
        for (uint32_t k = 0; k < count; k++) {
            uint64_t b = bits[k];
            if ((b / bits_per_block) != blk_index) continue;
            uint64_t off_bit = b % bits_per_block;
            mdfs_bitmap_clear(bm, off_bit);
            dirty = 1;
        }

        if (dirty) {
            if (mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, bitmap_start + blk_index, bm) != VDRIVE_SUCCESS) {
                mdfs_buffer_release((uint8_t*)bm);
                return -4;
            }
        }
    }

    mdfs_buffer_release((uint8_t*)bm);
    return 0;
}

static int mdfs_set_block_ptr(const mdfs_fs_t *fs, mdfs_inode_t *in, uint64_t lbn, uint64_t pblk);
static int mdfs_set_block_ptr_range(const mdfs_fs_t *fs, mdfs_inode_t *in, uint64_t lbn_start, const uint64_t *pblks, uint32_t count);
typedef struct {
    uint64_t blocks[1024];
    uint32_t count;
} mdfs_free_accum_t;

static int mdfs_free_accum_flush(const mdfs_fs_t *fs, mdfs_free_accum_t *a) {
    if (!a || a->count == 0) return 0;
    (void)mdfs_bitmap_clear_many(fs, fs->sb.block_bitmap_start, fs->sb.block_bitmap_blocks, a->blocks, a->count);
    a->count = 0;
    return 0;
}

static inline void mdfs_free_accum_add(const mdfs_fs_t *fs, mdfs_free_accum_t *a, uint64_t bno) {
    if (!fs || !a || bno == 0) return;
    a->blocks[a->count++] = bno;
    if (a->count >= (uint32_t)(sizeof(a->blocks)/sizeof(a->blocks[0]))) {
        (void)mdfs_free_accum_flush(fs, a);
    }
}

static int mdfs_free_indirect_chain_accum(const mdfs_fs_t *fs, uint64_t ind_blk, int level, mdfs_free_accum_t *a);

static uint64_t mdfs_get_block_ptr(const mdfs_fs_t *fs, const mdfs_inode_t *in, uint64_t lbn) {
    if (!fs || !in) return 0;

    if (lbn < MDFS_MAX_DIRECT) return in->direct[lbn];
    lbn -= MDFS_MAX_DIRECT;

    uint32_t ppb = MDFS_PTRS_PER_BLOCK;

    /* single indirect */
    if (lbn < ppb) {
        if (!in->indirect1) return 0;
        uint64_t *tbl = (uint64_t*)mdfs_buffer_acquire();
        if (!tbl) return 0;
        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, in->indirect1, tbl) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)tbl); return 0; }
        uint64_t v = tbl[lbn];
        mdfs_buffer_release((uint8_t*)tbl);
        return v;
    }
    lbn -= ppb;

    /* double indirect */
    if (lbn < (uint64_t)ppb * (uint64_t)ppb) {
        if (!in->indirect2) return 0;
        uint64_t *lvl1 = (uint64_t*)mdfs_buffer_acquire();
        uint64_t *lvl2 = (uint64_t*)mdfs_buffer_acquire();
        if (!lvl1 || !lvl2) { if (lvl1) mdfs_buffer_release((uint8_t*)lvl1); if (lvl2) mdfs_buffer_release((uint8_t*)lvl2); return 0; }
        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, in->indirect2, lvl1) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2); return 0; }
        uint64_t idx1 = lbn / ppb;
        uint64_t idx2 = lbn % ppb;
        if (idx1 >= ppb) { mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2); return 0; }
        uint64_t blk1 = lvl1[idx1];
        if (!blk1) { mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2); return 0; }
        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, blk1, lvl2) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2); return 0; }
        uint64_t v = lvl2[idx2];
        mdfs_buffer_release((uint8_t*)lvl1);
        mdfs_buffer_release((uint8_t*)lvl2);
        return v;
    }
    lbn -= (uint64_t)ppb * (uint64_t)ppb;

    /* triple indirect */
    if (lbn < (uint64_t)ppb * (uint64_t)ppb * (uint64_t)ppb) {
        if (!in->indirect3) return 0;
        uint64_t *lvl1 = (uint64_t*)mdfs_buffer_acquire();
        uint64_t *lvl2 = (uint64_t*)mdfs_buffer_acquire();
        uint64_t *lvl3 = (uint64_t*)mdfs_buffer_acquire();
        if (!lvl1 || !lvl2 || !lvl3) { if (lvl1) mdfs_buffer_release((uint8_t*)lvl1); if (lvl2) mdfs_buffer_release((uint8_t*)lvl2); if (lvl3) mdfs_buffer_release((uint8_t*)lvl3); return 0; }
        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, in->indirect3, lvl1) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2); mdfs_buffer_release((uint8_t*)lvl3); return 0; }

        uint64_t idx1 = lbn / ((uint64_t)ppb * (uint64_t)ppb);
        uint64_t rem = lbn % ((uint64_t)ppb * (uint64_t)ppb);
        uint64_t idx2 = rem / ppb;
        uint64_t idx3 = rem % ppb;

        if (idx1 >= ppb) { mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2); mdfs_buffer_release((uint8_t*)lvl3); return 0; }
        uint64_t b1 = lvl1[idx1];
        if (!b1) { mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2); mdfs_buffer_release((uint8_t*)lvl3); return 0; }
        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, b1, lvl2) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2); mdfs_buffer_release((uint8_t*)lvl3); return 0; }
        if (idx2 >= ppb) { mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2); mdfs_buffer_release((uint8_t*)lvl3); return 0; }
        uint64_t b2 = lvl2[idx2];
        if (!b2) { mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2); mdfs_buffer_release((uint8_t*)lvl3); return 0; }
        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, b2, lvl3) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2); mdfs_buffer_release((uint8_t*)lvl3); return 0; }
        uint64_t v = lvl3[idx3];
        mdfs_buffer_release((uint8_t*)lvl1);
        mdfs_buffer_release((uint8_t*)lvl2);
        mdfs_buffer_release((uint8_t*)lvl3);
        return v;
    }
    lbn -= (uint64_t)ppb * (uint64_t)ppb * (uint64_t)ppb;

    /* quadruple indirect - supports up to 256 PB files */
    if (lbn < (uint64_t)ppb * (uint64_t)ppb * (uint64_t)ppb * (uint64_t)ppb) {
        if (!in->indirect4) return 0;
        uint64_t *lvl1 = (uint64_t*)mdfs_buffer_acquire();
        uint64_t *lvl2 = (uint64_t*)mdfs_buffer_acquire();
        uint64_t *lvl3 = (uint64_t*)mdfs_buffer_acquire();
        uint64_t *lvl4 = (uint64_t*)mdfs_buffer_acquire();
        if (!lvl1 || !lvl2 || !lvl3 || !lvl4) {
            if (lvl1) mdfs_buffer_release((uint8_t*)lvl1);
            if (lvl2) mdfs_buffer_release((uint8_t*)lvl2);
            if (lvl3) mdfs_buffer_release((uint8_t*)lvl3);
            if (lvl4) mdfs_buffer_release((uint8_t*)lvl4);
            return 0;
        }

        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, in->indirect4, lvl1) != VDRIVE_SUCCESS) {
            mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2);
            mdfs_buffer_release((uint8_t*)lvl3); mdfs_buffer_release((uint8_t*)lvl4);
            return 0;
        }

        uint64_t ppb2 = (uint64_t)ppb * (uint64_t)ppb;
        uint64_t ppb3 = ppb2 * (uint64_t)ppb;
        uint64_t idx1 = lbn / ppb3;
        uint64_t rem1 = lbn % ppb3;
        uint64_t idx2 = rem1 / ppb2;
        uint64_t rem2 = rem1 % ppb2;
        uint64_t idx3 = rem2 / ppb;
        uint64_t idx4 = rem2 % ppb;

        if (idx1 >= ppb) {
            mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2);
            mdfs_buffer_release((uint8_t*)lvl3); mdfs_buffer_release((uint8_t*)lvl4);
            return 0;
        }

        uint64_t b1 = lvl1[idx1];
        if (!b1) {
            mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2);
            mdfs_buffer_release((uint8_t*)lvl3); mdfs_buffer_release((uint8_t*)lvl4);
            return 0;
        }

        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, b1, lvl2) != VDRIVE_SUCCESS) {
            mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2);
            mdfs_buffer_release((uint8_t*)lvl3); mdfs_buffer_release((uint8_t*)lvl4);
            return 0;
        }

        uint64_t b2 = lvl2[idx2];
        if (!b2) {
            mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2);
            mdfs_buffer_release((uint8_t*)lvl3); mdfs_buffer_release((uint8_t*)lvl4);
            return 0;
        }

        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, b2, lvl3) != VDRIVE_SUCCESS) {
            mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2);
            mdfs_buffer_release((uint8_t*)lvl3); mdfs_buffer_release((uint8_t*)lvl4);
            return 0;
        }

        uint64_t b3 = lvl3[idx3];
        if (!b3) {
            mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2);
            mdfs_buffer_release((uint8_t*)lvl3); mdfs_buffer_release((uint8_t*)lvl4);
            return 0;
        }

        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, b3, lvl4) != VDRIVE_SUCCESS) {
            mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2);
            mdfs_buffer_release((uint8_t*)lvl3); mdfs_buffer_release((uint8_t*)lvl4);
            return 0;
        }

        uint64_t v = lvl4[idx4];
        mdfs_buffer_release((uint8_t*)lvl1);
        mdfs_buffer_release((uint8_t*)lvl2);
        mdfs_buffer_release((uint8_t*)lvl3);
        mdfs_buffer_release((uint8_t*)lvl4);
        return v;
    }

    return 0;
}

static int mdfs_set_block_ptr_range(const mdfs_fs_t *fs, mdfs_inode_t *in, uint64_t lbn_start, const uint64_t *pblks, uint32_t count) {
    if (!fs || !in || !pblks || count == 0) return -1;

    /* Batched pointer population for sequential allocation.
     * Supports direct + single + double + triple indirect.
     * Writes each indirect table block at most once per call.
     */

    uint32_t ppb = MDFS_PTRS_PER_BLOCK;

    /* Helpers to read/write pointer blocks */
    uint64_t *tbl1 = (uint64_t*)mdfs_buffer_acquire();
    uint64_t *tbl2 = (uint64_t*)mdfs_buffer_acquire();
    uint64_t *tbl3 = (uint64_t*)mdfs_buffer_acquire();
    if (!tbl1 || !tbl2 || !tbl3) { if (tbl1) mdfs_buffer_release((uint8_t*)tbl1); if (tbl2) mdfs_buffer_release((uint8_t*)tbl2); if (tbl3) mdfs_buffer_release((uint8_t*)tbl3); return -2; }

    /* 1) Direct */
    uint64_t lbn = lbn_start;
    uint32_t i = 0;
    while (i < count && lbn < MDFS_MAX_DIRECT) {
        in->direct[lbn] = pblks[i];
        lbn++;
        i++;
    }

    /* 2) Single indirect */
    if (i < count) {
        uint64_t l = lbn;
        if (l >= MDFS_MAX_DIRECT && l < (uint64_t)MDFS_MAX_DIRECT + (uint64_t)ppb) {
            if (in->indirect1 == 0) {
                uint64_t ind = 0;
                if (mdfs_alloc_block_simple(fs, &ind) != 0) { mdfs_buffer_release((uint8_t*)tbl1); mdfs_buffer_release((uint8_t*)tbl2); mdfs_buffer_release((uint8_t*)tbl3); return -3; }
                in->indirect1 = ind;
                memset(tbl1, 0, MDFS_BLOCK_SIZE);
            } else {
                if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, in->indirect1, tbl1) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)tbl1); mdfs_buffer_release((uint8_t*)tbl2); mdfs_buffer_release((uint8_t*)tbl3); return -4; }
            }

            uint64_t off = l - MDFS_MAX_DIRECT;
            uint32_t wrote = 0;
            while (i < count && off < ppb) {
                tbl1[off] = pblks[i];
                off++;
                i++;
                wrote++;
            }

            (void)mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, in->indirect1, tbl1);
            lbn = MDFS_MAX_DIRECT + off;
        }
    }

    /* 3) Double indirect */
    if (i < count) {
        uint64_t l = lbn;
        uint64_t dbl_base = (uint64_t)MDFS_MAX_DIRECT + (uint64_t)ppb;
        uint64_t dbl_span = (uint64_t)ppb * (uint64_t)ppb;
        if (l >= dbl_base && l < dbl_base + dbl_span) {
            if (in->indirect2 == 0) {
                uint64_t ind = 0;
                if (mdfs_alloc_block_simple(fs, &ind) != 0) { mdfs_buffer_release((uint8_t*)tbl1); mdfs_buffer_release((uint8_t*)tbl2); mdfs_buffer_release((uint8_t*)tbl3); return -5; }
                in->indirect2 = ind;
                memset(tbl1, 0, MDFS_BLOCK_SIZE);
            } else {
                if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, in->indirect2, tbl1) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)tbl1); mdfs_buffer_release((uint8_t*)tbl2); mdfs_buffer_release((uint8_t*)tbl3); return -6; }
            }

            uint64_t rel = l - dbl_base;
            uint64_t idx1 = rel / ppb;
            uint64_t idx2 = rel % ppb;

            int dirty_lvl1 = 0;

            while (i < count && idx1 < ppb) {
                uint64_t blk1 = tbl1[idx1];
                if (blk1 == 0) {
                    if (mdfs_alloc_block_simple(fs, &blk1) != 0) break;
                    tbl1[idx1] = blk1;
                    dirty_lvl1 = 1;
                    memset(tbl2, 0, MDFS_BLOCK_SIZE);
                } else {
                    if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, blk1, tbl2) != VDRIVE_SUCCESS) break;
                }

                int dirty_lvl2 = 0;
                while (i < count && idx2 < ppb) {
                    tbl2[idx2] = pblks[i];
                    dirty_lvl2 = 1;
                    idx2++;
                    i++;
                }

                if (dirty_lvl2) (void)mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, blk1, tbl2);

                if (idx2 >= ppb) { idx2 = 0; idx1++; }
            }

            if (dirty_lvl1) (void)mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, in->indirect2, tbl1);
            lbn = dbl_base + (idx1 * ppb) + idx2;
        }
    }

    /* 4) Triple indirect */
    if (i < count) {
        uint64_t l = lbn;
        uint64_t tri_base = (uint64_t)MDFS_MAX_DIRECT + (uint64_t)ppb + ((uint64_t)ppb * (uint64_t)ppb);
        uint64_t tri_span = (uint64_t)ppb * (uint64_t)ppb * (uint64_t)ppb;
        if (l >= tri_base && l < tri_base + tri_span) {
            if (in->indirect3 == 0) {
                uint64_t ind = 0;
                if (mdfs_alloc_block_simple(fs, &ind) != 0) { mdfs_buffer_release((uint8_t*)tbl1); mdfs_buffer_release((uint8_t*)tbl2); mdfs_buffer_release((uint8_t*)tbl3); return -7; }
                in->indirect3 = ind;
                memset(tbl1, 0, MDFS_BLOCK_SIZE);
            } else {
                if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, in->indirect3, tbl1) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)tbl1); mdfs_buffer_release((uint8_t*)tbl2); mdfs_buffer_release((uint8_t*)tbl3); return -8; }
            }

            uint64_t rel = l - tri_base;
            uint64_t idx1 = rel / ((uint64_t)ppb * (uint64_t)ppb);
            uint64_t rem = rel % ((uint64_t)ppb * (uint64_t)ppb);
            uint64_t idx2 = rem / ppb;
            uint64_t idx3 = rem % ppb;

            int dirty_lvl1 = 0;

            while (i < count && idx1 < ppb) {
                uint64_t b1 = tbl1[idx1];
                if (b1 == 0) {
                    if (mdfs_alloc_block_simple(fs, &b1) != 0) break;
                    tbl1[idx1] = b1;
                    dirty_lvl1 = 1;
                    memset(tbl2, 0, MDFS_BLOCK_SIZE);
                } else {
                    if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, b1, tbl2) != VDRIVE_SUCCESS) break;
                }

                int dirty_lvl2 = 0;

                while (i < count && idx2 < ppb) {
                    uint64_t b2 = tbl2[idx2];
                    if (b2 == 0) {
                        if (mdfs_alloc_block_simple(fs, &b2) != 0) break;
                        tbl2[idx2] = b2;
                        dirty_lvl2 = 1;
                        memset(tbl3, 0, MDFS_BLOCK_SIZE);
                    } else {
                        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, b2, tbl3) != VDRIVE_SUCCESS) break;
                    }

                    int dirty_lvl3 = 0;
                    while (i < count && idx3 < ppb) {
                        tbl3[idx3] = pblks[i];
                        dirty_lvl3 = 1;
                        idx3++;
                        i++;
                    }

                    if (dirty_lvl3) (void)mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, b2, tbl3);

                    if (idx3 >= ppb) { idx3 = 0; idx2++; }
                }

                if (dirty_lvl2) (void)mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, b1, tbl2);

                if (idx2 >= ppb) { idx2 = 0; idx1++; }
            }

            if (dirty_lvl1) (void)mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, in->indirect3, tbl1);
            lbn = tri_base + (idx1 * (uint64_t)ppb * (uint64_t)ppb) + (idx2 * (uint64_t)ppb) + idx3;
        }
    }

    mdfs_buffer_release((uint8_t*)tbl1);
    mdfs_buffer_release((uint8_t*)tbl2);
    mdfs_buffer_release((uint8_t*)tbl3);
    return 0;
}

static int mdfs_set_block_ptr(const mdfs_fs_t *fs, mdfs_inode_t *in, uint64_t lbn, uint64_t pblk) {
    if (!fs || !in) return -1;

    uint32_t ppb = MDFS_PTRS_PER_BLOCK;

    if (lbn < MDFS_MAX_DIRECT) {
        in->direct[lbn] = pblk;
        return 0;
    }
    lbn -= MDFS_MAX_DIRECT;

    /* single indirect */
    if (lbn < ppb) {
        if (in->indirect1 == 0) {
            uint64_t ind = 0;
            if (mdfs_alloc_block_simple(fs, &ind) != 0) return -2;
            in->indirect1 = ind;
            uint8_t *z = mdfs_buffer_acquire();
            if (!z) return -3;
            memset(z, 0, MDFS_BLOCK_SIZE);
            (void)mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, ind, z);
            mdfs_buffer_release((uint8_t*)z);
        }
        uint64_t *tbl = (uint64_t*)mdfs_buffer_acquire();
        if (!tbl) return -4;
        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, in->indirect1, tbl) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)tbl); return -5; }
        tbl[lbn] = pblk;
        int rc = mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, in->indirect1, tbl);
        mdfs_buffer_release((uint8_t*)tbl);
        return (rc == VDRIVE_SUCCESS) ? 0 : -6;
    }
    lbn -= ppb;

    /* double indirect */
    if (lbn < (uint64_t)ppb * (uint64_t)ppb) {
        if (in->indirect2 == 0) {
            uint64_t ind = 0;
            if (mdfs_alloc_block_simple(fs, &ind) != 0) return -7;
            in->indirect2 = ind;
            uint8_t *z = mdfs_buffer_acquire();
            if (!z) return -8;
            memset(z, 0, MDFS_BLOCK_SIZE);
            (void)mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, ind, z);
            mdfs_buffer_release((uint8_t*)z);
        }

        uint64_t idx1 = lbn / ppb;
        uint64_t idx2 = lbn % ppb;

        uint64_t *lvl1 = (uint64_t*)mdfs_buffer_acquire();
        uint64_t *lvl2 = (uint64_t*)mdfs_buffer_acquire();
        if (!lvl1 || !lvl2) { if (lvl1) mdfs_buffer_release((uint8_t*)lvl1); if (lvl2) mdfs_buffer_release((uint8_t*)lvl2); return -9; }

        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, in->indirect2, lvl1) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2); return -10; }
        if (lvl1[idx1] == 0) {
            uint64_t ind2 = 0;
            if (mdfs_alloc_block_simple(fs, &ind2) != 0) { mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2); return -11; }
            lvl1[idx1] = ind2;
            memset(lvl2, 0, MDFS_BLOCK_SIZE);
            (void)mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, ind2, lvl2);
            (void)mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, in->indirect2, lvl1);
        }
        uint64_t blk1 = lvl1[idx1];
        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, blk1, lvl2) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2); return -12; }
        lvl2[idx2] = pblk;
        int rc = mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, blk1, lvl2);
        mdfs_buffer_release((uint8_t*)lvl1);
        mdfs_buffer_release((uint8_t*)lvl2);
        return (rc == VDRIVE_SUCCESS) ? 0 : -13;
    }
    lbn -= (uint64_t)ppb * (uint64_t)ppb;

    /* triple indirect */
    if (lbn < (uint64_t)ppb * (uint64_t)ppb * (uint64_t)ppb) {
        if (in->indirect3 == 0) {
            uint64_t ind = 0;
            if (mdfs_alloc_block_simple(fs, &ind) != 0) return -14;
            in->indirect3 = ind;
            uint8_t *z = mdfs_buffer_acquire();
            if (!z) return -15;
            memset(z, 0, MDFS_BLOCK_SIZE);
            (void)mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, ind, z);
            mdfs_buffer_release((uint8_t*)z);
        }

        uint64_t idx1 = lbn / ((uint64_t)ppb * (uint64_t)ppb);
        uint64_t rem = lbn % ((uint64_t)ppb * (uint64_t)ppb);
        uint64_t idx2 = rem / ppb;
        uint64_t idx3 = rem % ppb;

        uint64_t *lvl1 = (uint64_t*)mdfs_buffer_acquire();
        uint64_t *lvl2 = (uint64_t*)mdfs_buffer_acquire();
        uint64_t *lvl3 = (uint64_t*)mdfs_buffer_acquire();
        if (!lvl1 || !lvl2 || !lvl3) { if (lvl1) mdfs_buffer_release((uint8_t*)lvl1); if (lvl2) mdfs_buffer_release((uint8_t*)lvl2); if (lvl3) mdfs_buffer_release((uint8_t*)lvl3); return -16; }

        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, in->indirect3, lvl1) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2); mdfs_buffer_release((uint8_t*)lvl3); return -17; }
        if (lvl1[idx1] == 0) {
            uint64_t b = 0;
            if (mdfs_alloc_block_simple(fs, &b) != 0) { mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2); mdfs_buffer_release((uint8_t*)lvl3); return -18; }
            lvl1[idx1] = b;
            memset(lvl2, 0, MDFS_BLOCK_SIZE);
            (void)mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, b, lvl2);
            (void)mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, in->indirect3, lvl1);
        }
        uint64_t b1 = lvl1[idx1];

        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, b1, lvl2) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2); mdfs_buffer_release((uint8_t*)lvl3); return -19; }
        if (lvl2[idx2] == 0) {
            uint64_t b = 0;
            if (mdfs_alloc_block_simple(fs, &b) != 0) { mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2); mdfs_buffer_release((uint8_t*)lvl3); return -20; }
            lvl2[idx2] = b;
            memset(lvl3, 0, MDFS_BLOCK_SIZE);
            (void)mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, b, lvl3);
            (void)mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, b1, lvl2);
        }
        uint64_t b2 = lvl2[idx2];

        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, b2, lvl3) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2); mdfs_buffer_release((uint8_t*)lvl3); return -21; }
        lvl3[idx3] = pblk;
        int rc = mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, b2, lvl3);
        mdfs_buffer_release((uint8_t*)lvl1);
        mdfs_buffer_release((uint8_t*)lvl2);
        mdfs_buffer_release((uint8_t*)lvl3);
        return (rc == VDRIVE_SUCCESS) ? 0 : -22;
    }
    lbn -= (uint64_t)ppb * (uint64_t)ppb * (uint64_t)ppb;

    /* quadruple indirect - supports up to 256 PB files */
    if (lbn < (uint64_t)ppb * (uint64_t)ppb * (uint64_t)ppb * (uint64_t)ppb) {
        if (in->indirect4 == 0) {
            uint64_t ind = 0;
            if (mdfs_alloc_block_simple(fs, &ind) != 0) return -24;
            in->indirect4 = ind;
            uint8_t *z = mdfs_buffer_acquire();
            if (!z) return -25;
            memset(z, 0, MDFS_BLOCK_SIZE);
            (void)mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, ind, z);
            mdfs_buffer_release((uint8_t*)z);
        }

        uint64_t ppb2 = (uint64_t)ppb * (uint64_t)ppb;
        uint64_t ppb3 = ppb2 * (uint64_t)ppb;
        uint64_t idx1 = lbn / ppb3;
        uint64_t rem1 = lbn % ppb3;
        uint64_t idx2 = rem1 / ppb2;
        uint64_t rem2 = rem1 % ppb2;
        uint64_t idx3 = rem2 / ppb;
        uint64_t idx4 = rem2 % ppb;

        uint64_t *lvl1 = (uint64_t*)mdfs_buffer_acquire();
        uint64_t *lvl2 = (uint64_t*)mdfs_buffer_acquire();
        uint64_t *lvl3 = (uint64_t*)mdfs_buffer_acquire();
        uint64_t *lvl4 = (uint64_t*)mdfs_buffer_acquire();
        if (!lvl1 || !lvl2 || !lvl3 || !lvl4) {
            if (lvl1) mdfs_buffer_release((uint8_t*)lvl1);
            if (lvl2) mdfs_buffer_release((uint8_t*)lvl2);
            if (lvl3) mdfs_buffer_release((uint8_t*)lvl3);
            if (lvl4) mdfs_buffer_release((uint8_t*)lvl4);
            return -26;
        }

        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, in->indirect4, lvl1) != VDRIVE_SUCCESS) {
            mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2);
            mdfs_buffer_release((uint8_t*)lvl3); mdfs_buffer_release((uint8_t*)lvl4);
            return -27;
        }

        if (lvl1[idx1] == 0) {
            uint64_t b = 0;
            if (mdfs_alloc_block_simple(fs, &b) != 0) {
                mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2);
                mdfs_buffer_release((uint8_t*)lvl3); mdfs_buffer_release((uint8_t*)lvl4);
                return -28;
            }
            lvl1[idx1] = b;
            memset(lvl2, 0, MDFS_BLOCK_SIZE);
            (void)mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, b, lvl2);
            (void)mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, in->indirect4, lvl1);
        }
        uint64_t b1 = lvl1[idx1];

        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, b1, lvl2) != VDRIVE_SUCCESS) {
            mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2);
            mdfs_buffer_release((uint8_t*)lvl3); mdfs_buffer_release((uint8_t*)lvl4);
            return -29;
        }

        if (lvl2[idx2] == 0) {
            uint64_t b = 0;
            if (mdfs_alloc_block_simple(fs, &b) != 0) {
                mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2);
                mdfs_buffer_release((uint8_t*)lvl3); mdfs_buffer_release((uint8_t*)lvl4);
                return -30;
            }
            lvl2[idx2] = b;
            memset(lvl3, 0, MDFS_BLOCK_SIZE);
            (void)mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, b, lvl3);
            (void)mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, b1, lvl2);
        }
        uint64_t b2 = lvl2[idx2];

        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, b2, lvl3) != VDRIVE_SUCCESS) {
            mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2);
            mdfs_buffer_release((uint8_t*)lvl3); mdfs_buffer_release((uint8_t*)lvl4);
            return -31;
        }

        if (lvl3[idx3] == 0) {
            uint64_t b = 0;
            if (mdfs_alloc_block_simple(fs, &b) != 0) {
                mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2);
                mdfs_buffer_release((uint8_t*)lvl3); mdfs_buffer_release((uint8_t*)lvl4);
                return -32;
            }
            lvl3[idx3] = b;
            memset(lvl4, 0, MDFS_BLOCK_SIZE);
            (void)mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, b, lvl4);
            (void)mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, b2, lvl3);
        }
        uint64_t b3 = lvl3[idx3];

        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, b3, lvl4) != VDRIVE_SUCCESS) {
            mdfs_buffer_release((uint8_t*)lvl1); mdfs_buffer_release((uint8_t*)lvl2);
            mdfs_buffer_release((uint8_t*)lvl3); mdfs_buffer_release((uint8_t*)lvl4);
            return -33;
        }

        lvl4[idx4] = pblk;
        int rc = mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, b3, lvl4);
        mdfs_buffer_release((uint8_t*)lvl1);
        mdfs_buffer_release((uint8_t*)lvl2);
        mdfs_buffer_release((uint8_t*)lvl3);
        mdfs_buffer_release((uint8_t*)lvl4);
        return (rc == VDRIVE_SUCCESS) ? 0 : -34;
    }

    return -35;
}

static int mdfs_free_indirect_chain_accum(const mdfs_fs_t *fs, uint64_t ind_blk, int level, mdfs_free_accum_t *a) {
    if (!fs) return -1;
    if (!ind_blk) return 0;
    if (level < 1 || level > 3) return -2;

    uint32_t ppb = MDFS_PTRS_PER_BLOCK;
    uint64_t *tbl = (uint64_t*)mdfs_buffer_acquire();
    if (!tbl) return -3;
    if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, ind_blk, tbl) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)tbl); return -4; }

    for (uint32_t i = 0; i < ppb; i++) {
        uint64_t v = tbl[i];
        if (!v) continue;
        if (level == 1) {
            mdfs_free_accum_add(fs, a, v);
        } else {
            (void)mdfs_free_indirect_chain_accum(fs, v, level - 1, a);
        }
    }

    mdfs_buffer_release((uint8_t*)tbl);
    mdfs_free_accum_add(fs, a, ind_blk);
    return 0;
}

// Implementations live in mdfs.c (helpers are static there), but we expose a minimal
// API for the VFS. To keep this first version simple, we implement root-only
// operations directly by re-reading structures on demand.

#if 0
static int mdfs_read_sb(int vdrive_id, uint32_t start_lba, mdfs_superblock_t *out) {
    uint8_t *buf = mdfs_buffer_acquire();
    if (!buf) return -1;
    memset(buf, 0, MDFS_BLOCK_SIZE);
    uint32_t lba = start_lba + (uint32_t)(1 * (MDFS_BLOCK_SIZE/512u));
    if (vdrive_read((uint8_t)vdrive_id, (uint64_t)lba, (MDFS_BLOCK_SIZE/512u), buf) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)buf); return -2; }
    memcpy(out, buf, sizeof(*out));
    mdfs_buffer_release((uint8_t*)buf);
    return 0;
}

#include "moduos/fs/MDFS/mdfs_disk.h"
#include "moduos/fs/MDFS/mdfs_cache.h"

static int mdfs_read_block_abs(int vdrive_id, uint32_t start_lba, uint64_t block_no, void *out) {
    uint32_t lba = start_lba + (uint32_t)(block_no * (MDFS_BLOCK_SIZE/512u));
    return mdfs_disk_read_block(vdrive_id, start_lba, block_no, out);
}

static int mdfs_write_block_abs(int vdrive_id, uint32_t start_lba, uint64_t block_no, const void *in) {
    uint32_t lba = start_lba + (uint32_t)(block_no * (MDFS_BLOCK_SIZE/512u));
    return mdfs_disk_write_block(vdrive_id, start_lba, block_no, in);
}

static int mdfs_read_inode_abs(int vdrive_id, uint32_t start_lba, const mdfs_superblock_t *sb, uint32_t ino, mdfs_inode_t *out) {
    return mdfs_disk_read_inode(vdrive_id, start_lba, sb, ino, out);
}

static int mdfs_write_inode_abs(int vdrive_id, uint32_t start_lba, const mdfs_superblock_t *sb, uint32_t ino, const mdfs_inode_t *in) {
    return mdfs_disk_write_inode(vdrive_id, start_lba, sb, ino, in);
}

#endif

static int mdfs_read_root_dir_block(int vdrive_id, uint32_t start_lba, const mdfs_superblock_t *sb, uint8_t *out_blk) {
    mdfs_inode_t root;
    if (mdfs_disk_read_inode(vdrive_id, start_lba, sb, (uint32_t)sb->root_inode, &root) != 0) return -1;
    uint64_t dir_block = root.direct[0];
    if (!dir_block) return -2;
    if (mdfs_disk_read_block(vdrive_id, start_lba, dir_block, out_blk) != VDRIVE_SUCCESS) return -3;
    return 0;
}

int mdfs_resolve_path(int handle, const char *path, uint32_t *out_ino, uint8_t *out_type) {
    const mdfs_fs_t *fs = mdfs_get_fs(handle);
    if (!fs || !path) return -1;

    // root
    if (path[0] == 0 || (path[0] == '/' && path[1] == 0)) {
        if (out_ino) *out_ino = (uint32_t)fs->sb.root_inode;
        if (out_type) *out_type = 2;
        return 0;
    }

    uint32_t cur = (uint32_t)fs->sb.root_inode;
    uint8_t cur_type = 2;

    // Walk components, case-sensitive.
    const char *p = path;
    if (*p == '/') p++;

    char name[MDFS_MAX_NAME + 1];

    while (*p) {
        // skip repeated '/'
        while (*p == '/') p++;
        if (!*p) break;

        size_t n = 0;
        while (p[n] && p[n] != '/') {
            if (n >= MDFS_MAX_NAME) return -2;
            name[n] = p[n];
            n++;
        }
        name[n] = 0;

        uint32_t nxt = 0;
        uint8_t nt = 0;
        int rc = mdfs_v2_dir_lookup(fs, cur, name, &nxt, &nt);
        if (rc != 0) return -3;

        cur = nxt;
        cur_type = nt;

        p += n;
        if (*p == '/') {
            // must be a directory if there are more components
            if (cur_type != 2) return -4;
        }
    }

    if (out_ino) *out_ino = cur;
    if (out_type) *out_type = cur_type;
    return 0;
}

static int mdfs_lookup_path(const mdfs_fs_t *fs, const char *path, uint32_t *out_ino, uint8_t *out_type) {
    if (!fs || !path) return -1;

    // root
    if (path[0] == 0 || (path[0] == '/' && path[1] == 0)) {
        if (out_ino) *out_ino = (uint32_t)fs->sb.root_inode;
        if (out_type) *out_type = 2;
        return 0;
    }

    uint32_t cur = (uint32_t)fs->sb.root_inode;
    uint8_t cur_type = 2;

    // Walk components, case-sensitive.
    const char *p = path;
    if (*p == '/') p++;

    char name[MDFS_MAX_NAME + 1];

    while (*p) {
        // skip repeated '/'
        while (*p == '/') p++;
        if (!*p) break;

        size_t n = 0;
        while (p[n] && p[n] != '/') {
            if (n >= MDFS_MAX_NAME) return -2;
            name[n] = p[n];
            n++;
        }
        name[n] = 0;

        uint32_t nxt = 0;
        uint8_t nt = 0;
        int rc = mdfs_v2_dir_lookup(fs, cur, name, &nxt, &nt);
        if (rc != 0) return -3;

        cur = nxt;
        cur_type = nt;

        p += n;
        if (*p == '/') {
            // must be a directory if there are more components
            if (cur_type != 2) return -4;
        }
    }

    if (out_ino) *out_ino = cur;
    if (out_type) *out_type = cur_type;
    return 0;
}

int mdfs_read_dir(int handle, const char *path, mdfs_dirent_t *out, int max_entries) {
    const mdfs_fs_t *fs = mdfs_get_fs(handle);
    if (!fs || !out || max_entries <= 0) return -1;
    if (!path) path = "/";

    uint32_t ino = 0;
    uint8_t type = 0;
    if (mdfs_lookup_path(fs, path, &ino, &type) != 0) return -2;
    if (type != 2) return -3;

    return mdfs_v2_dir_list(fs, ino, out, max_entries);
}

int mdfs_read_root_dir(int handle, mdfs_dirent_t *out, int max_entries) {
    return mdfs_read_dir(handle, "/", out, max_entries);
}

static int mdfs_find_root_entry(const mdfs_fs_t *fs, const char *name, mdfs_dirent_t *out_ent) {
    uint32_t ino = 0;
    uint8_t type = 0;
    int rc = mdfs_v2_root_lookup_export(fs, name, &ino, &type);
    if (rc != 0) return rc;
    if (out_ent) {
        memset(out_ent, 0, sizeof(*out_ent));
        out_ent->inode = ino;
        out_ent->type = type;
        strncpy(out_ent->name, name, sizeof(out_ent->name) - 1);
        out_ent->name[sizeof(out_ent->name) - 1] = 0;
    }
    return 0;
}

static int mdfs_add_root_entry(const mdfs_fs_t *fs, const char *name, uint32_t ino, uint8_t type) {
    return mdfs_v2_root_add_export(fs, name, ino, type);
}

static const char *mdfs_basename_only(const char *path) {
    if (!path) return "";
    const char *bn = path;
    for (const char *p = path; *p; p++) if (*p == '/' && p[1]) bn = p + 1;
    return bn;
}

int mdfs_stat_by_path(int handle, const char *path, uint32_t *out_size, int *out_is_dir) {
    const mdfs_fs_t *fs = mdfs_get_fs(handle);
    if (!fs || !path) return -1;
    if (path[0] == 0 || (path[0] == '/' && path[1] == 0)) {
        if (out_size) *out_size = 0;
        if (out_is_dir) *out_is_dir = 1;
        return 0;
    }

    uint32_t ino_n = 0;
    uint8_t typ = 0;
    if (mdfs_lookup_path(fs, path, &ino_n, &typ) != 0) return -2;

    if (out_is_dir) *out_is_dir = (typ == 2);
    if (out_size) {
        if (typ == 2) {
            *out_size = 0;
        } else {
            mdfs_inode_t ino;
            if (mdfs_disk_read_inode(fs->vdrive_id, fs->start_lba, &fs->sb, ino_n, &ino) != 0) return -3;
            *out_size = (uint32_t)ino.size_bytes;
        }
    }
    return 0;
}

int mdfs_read_file_by_path(int handle, const char *path, void *buffer, size_t buffer_size, size_t *bytes_read) {
    const mdfs_fs_t *fs = mdfs_get_fs(handle);
    if (!fs || !path || !buffer || !bytes_read) return -1;
    *bytes_read = 0;
    uint32_t ino_n = 0;
    uint8_t typ = 0;
    if (mdfs_lookup_path(fs, path, &ino_n, &typ) != 0) return -2;
    if (typ != 1) return -3;

    mdfs_inode_t ino;
    if (mdfs_disk_read_inode(fs->vdrive_id, fs->start_lba, &fs->sb, ino_n, &ino) != 0) return -4;

    /* read blocks */
    uint8_t *blk = mdfs_buffer_acquire();
    if (!blk) return -5;

    size_t to_read = (size_t)ino.size_bytes;
    if (to_read > buffer_size) to_read = buffer_size;

    size_t done = 0;
    while (done < to_read) {
        uint64_t lbn = done / MDFS_BLOCK_SIZE;
        uint64_t boff = done % MDFS_BLOCK_SIZE;
        uint64_t bno = mdfs_get_block_ptr(fs, &ino, lbn);
        if (!bno) break;
        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, bno, blk) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)blk); return -6; }
        size_t chunk = MDFS_BLOCK_SIZE - (size_t)boff;
        if (chunk > (to_read - done)) chunk = to_read - done;
        memcpy((uint8_t*)buffer + done, blk + boff, chunk);
        done += chunk;
    }

    mdfs_buffer_release((uint8_t*)blk);
    *bytes_read = done;
    return 0;
}

static int mdfs_alloc_inode_simple(const mdfs_fs_t *fs, uint32_t *out_ino) {
    if (!fs || !out_ino) return -1;
    uint64_t bit = 0;
    int rc = mdfs_bitmap_find_and_set(fs, fs->sb.inode_bitmap_start, fs->sb.inode_bitmap_blocks,
                                     1, fs->sb.total_inodes, &bit);
    if (rc != 0) return -2;
    *out_ino = (uint32_t)bit;
    return 0;
}

static int mdfs_alloc_block_simple(const mdfs_fs_t *fs, uint64_t *out_block) {
    if (!fs || !out_block) return -1;

    uint64_t start = fs->sb.inode_table_start + fs->sb.inode_table_blocks;
    uint64_t bit = 0;
    int rc = mdfs_bitmap_find_and_set(fs, fs->sb.block_bitmap_start, fs->sb.block_bitmap_blocks,
                                     start, fs->sb.total_blocks, &bit);
    if (rc != 0) return -2;
    *out_block = bit;
    return 0;
}

/* Allocate multiple data blocks in one go (for fast sequential file growth). */
static int mdfs_alloc_blocks_batch(const mdfs_fs_t *fs, uint32_t want, uint64_t *out_blocks, uint32_t *out_got) {
    if (!fs || want == 0 || !out_blocks || !out_got) return -1;

    uint64_t start = fs->sb.inode_table_start + fs->sb.inode_table_blocks;
    return mdfs_bitmap_alloc_many(fs, fs->sb.block_bitmap_start, fs->sb.block_bitmap_blocks,
                                 start, fs->sb.total_blocks, want, out_blocks, out_got);
}

static int mdfs_free_inode_simple(const mdfs_fs_t *fs, uint32_t ino) {
    if (!fs || ino == 0) return -1;
    return mdfs_bitmap_clear_bit(fs, fs->sb.inode_bitmap_start, fs->sb.inode_bitmap_blocks, (uint64_t)ino);
}

static int mdfs_free_block_simple(const mdfs_fs_t *fs, uint64_t bno) {
    if (!fs || bno == 0) return -1;
    return mdfs_bitmap_clear_bit(fs, fs->sb.block_bitmap_start, fs->sb.block_bitmap_blocks, bno);
}

int mdfs_unlink_by_path(int handle, const char *path) {
    const mdfs_fs_t *fs = mdfs_get_fs(handle);
    if (!fs || !path) return -1;

    if (path[0] == 0 || (path[0] == '/' && path[1] == 0)) return -2;

    // Split into parent + base
    const char *base = mdfs_basename_only(path);
    if (!base[0]) return -3;

    char parent[256];
    memset(parent, 0, sizeof(parent));
    const char *last = NULL;
    for (const char *p = path; *p; p++) if (*p == '/') last = p;
    if (!last || last == path) {
        strcpy(parent, "/");
    } else {
        size_t n = (size_t)(last - path);
        if (n >= sizeof(parent)) n = sizeof(parent) - 1;
        memcpy(parent, path, n);
        parent[n] = 0;
    }

    uint32_t pino = 0;
    uint8_t ptype = 0;
    if (mdfs_lookup_path(fs, parent, &pino, &ptype) != 0) return -4;
    if (ptype != 2) return -5;

    uint32_t ino = 0;
    uint8_t typ = 0;
    if (mdfs_v2_dir_lookup(fs, pino, base, &ino, &typ) != 0) return -6;
    if (typ != 1) return -7; // not a file

    // Read inode to discover allocated blocks
    mdfs_inode_t fin;
    if (mdfs_disk_read_inode(fs->vdrive_id, fs->start_lba, &fs->sb, ino, &fin) != 0) return -8;

    // Remove from parent
    if (mdfs_v2_dir_remove(fs, pino, base) != 0) return -9;

    // Free data blocks (direct + indirect) - batched bitmap updates
    mdfs_free_accum_t acc;
    memset(&acc, 0, sizeof(acc));

    for (uint32_t i = 0; i < MDFS_MAX_DIRECT; i++) {
        if (fin.direct[i]) mdfs_free_accum_add(fs, &acc, fin.direct[i]);
    }
    if (fin.indirect1) (void)mdfs_free_indirect_chain_accum(fs, fin.indirect1, 1, &acc);
    if (fin.indirect2) (void)mdfs_free_indirect_chain_accum(fs, fin.indirect2, 2, &acc);
    if (fin.indirect3) (void)mdfs_free_indirect_chain_accum(fs, fin.indirect3, 3, &acc);
    if (fin.indirect4) (void)mdfs_free_indirect_chain_accum(fs, fin.indirect4, 4, &acc);

    (void)mdfs_free_accum_flush(fs, &acc);

    // Clear inode then free
    mdfs_inode_t z;
    memset(&z, 0, sizeof(z));
    (void)mdfs_disk_write_inode(fs->vdrive_id, fs->start_lba, &fs->sb, ino, &z);
    (void)mdfs_free_inode_simple(fs, ino);

    return 0;
}

int mdfs_rmdir_by_path(int handle, const char *path) {
    const mdfs_fs_t *fs = mdfs_get_fs(handle);
    if (!fs || !path) return -1;

    // Cannot rmdir root.
    if (path[0] == 0 || (path[0] == '/' && path[1] == 0)) return -2;

    // Split into parent dir + basename
    const char *base = mdfs_basename_only(path);
    if (!base[0]) return -3;

    char parent[256];
    memset(parent, 0, sizeof(parent));
    const char *last = NULL;
    for (const char *p = path; *p; p++) if (*p == '/') last = p;
    if (!last || last == path) {
        strcpy(parent, "/");
    } else {
        size_t n = (size_t)(last - path);
        if (n >= sizeof(parent)) n = sizeof(parent) - 1;
        memcpy(parent, path, n);
        parent[n] = 0;
    }

    uint32_t pino = 0;
    uint8_t ptype = 0;
    if (mdfs_lookup_path(fs, parent, &pino, &ptype) != 0) return -4;
    if (ptype != 2) return -5;

    uint32_t ino = 0;
    uint8_t typ = 0;
    if (mdfs_v2_dir_lookup(fs, pino, base, &ino, &typ) != 0) return -6;
    if (typ != 2) return -7;

    // Must be empty
    mdfs_dirent_t tmp;
    int c = mdfs_v2_dir_list(fs, ino, &tmp, 1);
    if (c < 0) return -8;
    if (c > 0) return -9;

    // Read inode to discover allocated blocks
    mdfs_inode_t din;
    if (mdfs_disk_read_inode(fs->vdrive_id, fs->start_lba, &fs->sb, ino, &din) != 0) return -10;

    // Remove from parent first
    if (mdfs_v2_dir_remove(fs, pino, base) != 0) return -11;

    // Free data blocks (direct + indirect) - batched bitmap updates
    mdfs_free_accum_t acc;
    memset(&acc, 0, sizeof(acc));

    for (uint32_t i = 0; i < MDFS_MAX_DIRECT; i++) {
        if (din.direct[i]) mdfs_free_accum_add(fs, &acc, din.direct[i]);
    }
    if (din.indirect1) (void)mdfs_free_indirect_chain_accum(fs, din.indirect1, 1, &acc);
    if (din.indirect2) (void)mdfs_free_indirect_chain_accum(fs, din.indirect2, 2, &acc);
    if (din.indirect3) (void)mdfs_free_indirect_chain_accum(fs, din.indirect3, 3, &acc);
    if (din.indirect4) (void)mdfs_free_indirect_chain_accum(fs, din.indirect4, 4, &acc);

    (void)mdfs_free_accum_flush(fs, &acc);

    // Clear inode (best effort)
    mdfs_inode_t z;
    memset(&z, 0, sizeof(z));
    (void)mdfs_disk_write_inode(fs->vdrive_id, fs->start_lba, &fs->sb, ino, &z);

    // Free inode bitmap
    (void)mdfs_free_inode_simple(fs, ino);

    return 0;
}

int mdfs_mkdir_by_path(int handle, const char *path) {
    const mdfs_fs_t *fs = mdfs_get_fs(handle);
    if (!fs || !path) return -1;

    // Cannot mkdir root.
    if (path[0] == 0 || (path[0] == '/' && path[1] == 0)) return -2;

    // Split into parent dir + basename
    const char *base = mdfs_basename_only(path);
    if (!base[0]) return -3;

    char parent[256];
    memset(parent, 0, sizeof(parent));
    const char *last = NULL;
    for (const char *p = path; *p; p++) if (*p == '/') last = p;
    if (!last || last == path) {
        strcpy(parent, "/");
    } else {
        size_t n = (size_t)(last - path);
        if (n >= sizeof(parent)) n = sizeof(parent) - 1;
        memcpy(parent, path, n);
        parent[n] = 0;
    }

    uint32_t dir_ino = 0;
    uint8_t dir_type = 0;
    if (mdfs_lookup_path(fs, parent, &dir_ino, &dir_type) != 0) return -4;
    if (dir_type != 2) return -5;

    // Already exists?
    uint32_t existing_ino = 0;
    uint8_t existing_type = 0;
    if (mdfs_v2_dir_lookup(fs, dir_ino, base, &existing_ino, &existing_type) == 0) {
        // If it exists and is already a directory, treat as success.
        return (existing_type == 2) ? 0 : -6;
    }

    // Allocate inode for directory
    uint32_t ino_num = 0;
    if (mdfs_alloc_inode_simple(fs, &ino_num) != 0) return -7;

    // Allocate one directory data block (v2 dir list uses direct blocks)
    uint64_t bno = 0;
    if (mdfs_alloc_block_simple(fs, &bno) != 0) return -8;

    // Zero the new directory block
    uint8_t *blk = mdfs_buffer_acquire();
    if (!blk) return -9;
    memset(blk, 0, MDFS_BLOCK_SIZE);
    if (mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, bno, blk) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)blk); return -10; }
    mdfs_buffer_release((uint8_t*)blk);

    // Write inode with ACL
    mdfs_inode_t ino;
    memset(&ino, 0, sizeof(ino));
    ino.mode = 0x4000;
    ino.link_count = 1;
    ino.size_bytes = 0;
    ino.direct[0] = bno;
    
    // Get current process identity for ACL initialization
    process_t *proc = process_get_current();
    uint32_t owner_uid = proc ? proc->uid : 0;
    uint32_t owner_gid = proc ? proc->gid : 0;
    ino.uid = owner_uid;
    ino.gid = owner_gid;
    
    // Initialize default ACL
    mdfs_inode_init_acl(&ino, owner_uid, owner_gid);

    if (mdfs_disk_write_inode(fs->vdrive_id, fs->start_lba, &fs->sb, ino_num, &ino) != 0) return -11;

    // Add entry to parent directory
    if (mdfs_v2_dir_add(fs, dir_ino, base, ino_num, 2) != 0) return -12;

    return 0;
}

int mdfs_write_file_by_path(int handle, const char *path, const void *buffer, size_t size) {
    return mdfs_write_file_at_by_path(handle, path, buffer, size, 0);
}

typedef struct {
    int in_use;
    int handle;
    uint32_t ino_num;
    mdfs_inode_t ino;
    int dirty;
} mdfs_api_inode_entry_t;

#define MDFS_INODE_CACHE_MAX 64
static mdfs_api_inode_entry_t g_mdfs_inode_cache[MDFS_INODE_CACHE_MAX];

static mdfs_api_inode_entry_t* mdfs_inode_cache_get(const mdfs_fs_t *fs, int handle, uint32_t ino_num) {
    // find existing
    for (int i = 0; i < MDFS_INODE_CACHE_MAX; i++) {
        if (g_mdfs_inode_cache[i].in_use && g_mdfs_inode_cache[i].handle == handle && g_mdfs_inode_cache[i].ino_num == ino_num) {
            return &g_mdfs_inode_cache[i];
        }
    }
    // allocate new slot
    for (int i = 0; i < MDFS_INODE_CACHE_MAX; i++) {
        if (!g_mdfs_inode_cache[i].in_use) {
            g_mdfs_inode_cache[i].in_use = 1;
            g_mdfs_inode_cache[i].handle = handle;
            g_mdfs_inode_cache[i].ino_num = ino_num;
            g_mdfs_inode_cache[i].dirty = 0;
            if (mdfs_disk_read_inode(fs->vdrive_id, fs->start_lba, &fs->sb, ino_num, &g_mdfs_inode_cache[i].ino) != 0) {
                g_mdfs_inode_cache[i].in_use = 0;
                return NULL;
            }
            return &g_mdfs_inode_cache[i];
        }
    }
    return NULL;
}

int mdfs_flush_inode(int handle, uint32_t ino_num) {
    const mdfs_fs_t *fs = mdfs_get_fs(handle);
    if (!fs || ino_num == 0) return -1;

    for (int i = 0; i < MDFS_INODE_CACHE_MAX; i++) {
        if (g_mdfs_inode_cache[i].in_use && g_mdfs_inode_cache[i].handle == handle && g_mdfs_inode_cache[i].ino_num == ino_num) {
            if (g_mdfs_inode_cache[i].dirty) {
                if (mdfs_disk_write_inode(fs->vdrive_id, fs->start_lba, &fs->sb, ino_num, &g_mdfs_inode_cache[i].ino) != 0) {
                    return -2;
                }
                g_mdfs_inode_cache[i].dirty = 0;
            }
            return 0;
        }
    }
    return 0;
}

int mdfs_write_file_at_by_inode(int handle, uint32_t ino_num, const void *buffer, size_t size, size_t offset) {
    const mdfs_fs_t *fs = mdfs_get_fs(handle);
    if (!fs || ino_num == 0 || (!buffer && size != 0)) return -1;

    // use cached inode (write-behind)
    mdfs_api_inode_entry_t *ce = mdfs_inode_cache_get(fs, handle, ino_num);
    if (!ce) return -8;
    mdfs_inode_t *ino = &ce->ino;

    uint8_t *blk = mdfs_buffer_acquire();
    if (!blk) return -9;

    size_t done = 0;
    while (done < size) {
        uint64_t abs_off = (uint64_t)offset + (uint64_t)done;
        uint64_t lbn = abs_off / MDFS_BLOCK_SIZE;
        uint64_t boff = abs_off % MDFS_BLOCK_SIZE;

        uint64_t bno = mdfs_get_block_ptr(fs, ino, lbn);
        if (bno == 0) {
            /* Preallocate a run of blocks when we are doing aligned sequential writes.
             * This drastically reduces bitmap IO and speeds up mkbigfile-like workloads.
             */
            if (boff == 0) {
                /* Fixed preallocation run size (Linus-style: optimize the common case).
                 * 1024 blocks = 4 MiB per run.
                 */
                uint32_t want = 1024u;
                uint64_t blocks[1024];
                uint32_t got = 0;

                if (mdfs_alloc_blocks_batch(fs, want, blocks, &got) == 0 && got > 0) {
                    /* Batch-map a contiguous run starting at current lbn.
                     * We intentionally do NOT call mdfs_set_block_ptr() per block (too slow).
                     */
                    uint32_t used = 0;
                    while (used < got) {
                        uint64_t lbn2 = lbn + (uint64_t)used;
                        if (mdfs_get_block_ptr(fs, ino, lbn2) != 0) break;

                        /* Determine how many blocks we can map contiguously from here. */
                        uint32_t run = 1;
                        while (used + run < got) {
                            if (blocks[used + run] != blocks[used] + (uint64_t)run) break;
                            run++;
                        }

                        /* Map [lbn2, lbn2+run) in one batched operation (direct + 1/2/3 indirect). */
                        if (mdfs_set_block_ptr_range(fs, ino, lbn2, &blocks[used], run) != 0) {
                            /* On failure, free the unused ones (best-effort). */
                            for (uint32_t j = used; j < got; j++) (void)mdfs_free_block_simple(fs, blocks[j]);
                            break;
                        }

                        used += run;
                    }

                    /* Fast path: we just allocated + mapped a run for a sequential, aligned write.
                     * IMPORTANT: do NOT write beyond the caller-supplied buffer.
                     * The common mkbigfile workload writes in 4KiB chunks; preallocation may map
                     * up to several MiB of blocks, but we can only write what we actually have.
                     */
                    uint64_t max_blocks_by_input = (uint64_t)((size - done) / MDFS_BLOCK_SIZE);
                    uint32_t writable_blocks = (max_blocks_by_input < (uint64_t)used) ? (uint32_t)max_blocks_by_input : used;

                    const uint8_t *src2 = (const uint8_t*)buffer + done;
                    uint32_t off_blocks = 0;
                    while (off_blocks < writable_blocks) {
                        uint32_t run = 1;
                        while (off_blocks + run < writable_blocks) {
                            if (blocks[off_blocks + run] != blocks[off_blocks] + (uint64_t)run) break;
                            run++;
                        }
                        if (mdfs_disk_write_blocks(fs->vdrive_id, fs->start_lba, blocks[off_blocks], run,
                                                  src2 + ((size_t)off_blocks * MDFS_BLOCK_SIZE)) != VDRIVE_SUCCESS) {
                            mdfs_buffer_release((uint8_t*)blk);
                            return -14;
                        }
                        off_blocks += run;
                    }

                    done += (size_t)writable_blocks * MDFS_BLOCK_SIZE;

                    /* If we preallocated more blocks than we had data for, fall through to the
                     * normal path for the remaining bytes (if any) and keep the extra blocks as
                     * already-allocated file capacity.
                     */
                    if (done < size) {
                        continue;
                    }
                    break;
                } else {
                    uint64_t nb = 0;
                    if (mdfs_alloc_block_simple(fs, &nb) != 0) { mdfs_buffer_release((uint8_t*)blk); return -11; }
                    if (mdfs_set_block_ptr(fs, ino, lbn, nb) != 0) { (void)mdfs_free_block_simple(fs, nb); mdfs_buffer_release((uint8_t*)blk); return -12; }
                    bno = nb;
                    memset(blk, 0, MDFS_BLOCK_SIZE);
                    (void)mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, bno, blk);
                }
            } else {
                uint64_t nb = 0;
                if (mdfs_alloc_block_simple(fs, &nb) != 0) { mdfs_buffer_release((uint8_t*)blk); return -11; }
                if (mdfs_set_block_ptr(fs, ino, lbn, nb) != 0) { (void)mdfs_free_block_simple(fs, nb); mdfs_buffer_release((uint8_t*)blk); return -12; }
                bno = nb;
                memset(blk, 0, MDFS_BLOCK_SIZE);
                (void)mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, bno, blk);
            }
        }

        size_t chunk = MDFS_BLOCK_SIZE - (size_t)boff;
        if (chunk > (size - done)) chunk = size - done;

        const uint8_t *src = (const uint8_t*)buffer + done;

        /* Fast path: full-block aligned writes.
         * If subsequent logical blocks map to consecutive physical blocks, coalesce
         * into a single large disk write (huge speedup for mkbigfile).
         */
        if (boff == 0 && chunk == MDFS_BLOCK_SIZE) {
            uint32_t max_run = 1024; /* up to 4MiB per call */
            uint64_t blocks_avail = (uint64_t)(size - done) / MDFS_BLOCK_SIZE;
            uint32_t run = (blocks_avail > max_run) ? max_run : (uint32_t)blocks_avail;
            if (run < 1) run = 1;

            /* Shrink run until physical blocks are contiguous. */
            uint32_t r = 1;
            for (r = 1; r < run; r++) {
                uint64_t b2 = mdfs_get_block_ptr(fs, ino, lbn + (uint64_t)r);
                if (b2 == 0 || b2 != (bno + (uint64_t)r)) break;
            }
            run = r;
            if (run < 1) run = 1;

            if (run > 1) {
                if (mdfs_disk_write_blocks(fs->vdrive_id, fs->start_lba, bno, run, src) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)blk); return -14; }
                done += (size_t)run * (size_t)MDFS_BLOCK_SIZE;
                continue;
            }

            if (mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, bno, src) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)blk); return -14; }
        } else {
            if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, bno, blk) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)blk); return -13; }
            memcpy(blk + boff, src, chunk);
            if (mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, bno, blk) != VDRIVE_SUCCESS) { mdfs_buffer_release((uint8_t*)blk); return -14; }
        }

        done += chunk;
    }

    uint64_t end_pos = (uint64_t)offset + (uint64_t)size;
    if (end_pos > ino->size_bytes) ino->size_bytes = end_pos;
    ce->dirty = 1;

    mdfs_buffer_release((uint8_t*)blk);
    return 0;
}

int mdfs_create_file_trunc(int handle, const char *path, int truncate, uint32_t *out_ino) {
    const mdfs_fs_t *fs = mdfs_get_fs(handle);
    if (!fs || !path || !out_ino) return -1;

    if (path[0] == 0 || (path[0] == '/' && path[1] == 0)) return -2;

    const char *base = mdfs_basename_only(path);
    if (!base[0]) return -3;

    char parent[256];
    memset(parent, 0, sizeof(parent));
    const char *last = NULL;
    for (const char *p = path; *p; p++) if (*p == '/') last = p;
    if (!last || last == path) {
        strcpy(parent, "/");
    } else {
        size_t n = (size_t)(last - path);
        if (n >= sizeof(parent)) n = sizeof(parent) - 1;
        memcpy(parent, path, n);
        parent[n] = 0;
    }

    uint32_t dir_ino = 0;
    uint8_t dir_type = 0;
    if (mdfs_lookup_path(fs, parent, &dir_ino, &dir_type) != 0) return -4;
    if (dir_type != 2) return -5;

    uint32_t existing_ino = 0;
    uint8_t existing_type = 0;
    int exists = (mdfs_v2_dir_lookup(fs, dir_ino, base, &existing_ino, &existing_type) == 0);

    if (exists) {
        if (existing_type != 1) return -6;
        *out_ino = existing_ino;
        /* TODO: implement truncate (free blocks) */
        (void)truncate;
        return 0;
    }

    uint32_t ino_num = 0;
    if (mdfs_alloc_inode_simple(fs, &ino_num) != 0) return -7;
    mdfs_inode_t ino;
    memset(&ino, 0, sizeof(ino));
    ino.mode = 0x8000;
    ino.link_count = 1;
    
    // Get current process identity for ACL initialization
    process_t *proc = process_get_current();
    uint32_t owner_uid = proc ? proc->uid : 0;
    uint32_t owner_gid = proc ? proc->gid : 0;
    ino.uid = owner_uid;
    ino.gid = owner_gid;
    
    // Initialize default ACL
    mdfs_inode_init_acl(&ino, owner_uid, owner_gid);
    
    if (mdfs_disk_write_inode(fs->vdrive_id, fs->start_lba, &fs->sb, ino_num, &ino) != 0) return -8;
    if (mdfs_v2_dir_add(fs, dir_ino, base, ino_num, 1) != 0) return -9;

    *out_ino = ino_num;
    return 0;
}

int mdfs_write_file_at_by_path(int handle, const char *path, const void *buffer, size_t size, size_t offset) {
    const mdfs_fs_t *fs = mdfs_get_fs(handle);
    if (!fs || !path || (!buffer && size != 0)) return -1;

    if (path[0] == 0 || (path[0] == '/' && path[1] == 0)) return -2;

    // Split into parent dir + basename
    const char *base = mdfs_basename_only(path);
    if (!base[0]) return -3;

    char parent[256];
    memset(parent, 0, sizeof(parent));
    // parent path is everything before basename
    const char *last = NULL;
    for (const char *p = path; *p; p++) if (*p == '/') last = p;
    if (!last || last == path) {
        strcpy(parent, "/");
    } else {
        size_t n = (size_t)(last - path);
        if (n >= sizeof(parent)) n = sizeof(parent) - 1;
        memcpy(parent, path, n);
        parent[n] = 0;
    }

    uint32_t dir_ino = 0;
    uint8_t dir_type = 0;
    if (mdfs_lookup_path(fs, parent, &dir_ino, &dir_type) != 0) return -4;
    if (dir_type != 2) return -5;

    uint32_t existing_ino = 0;
    uint8_t existing_type = 0;
    int exists = (mdfs_v2_dir_lookup(fs, dir_ino, base, &existing_ino, &existing_type) == 0);

    uint32_t ino_num = 0;
    if (exists) {
        if (existing_type != 1) return -6;
        ino_num = existing_ino;
    } else {
        if (mdfs_alloc_inode_simple(fs, &ino_num) != 0) return -7;
        mdfs_inode_t ino;
        memset(&ino, 0, sizeof(ino));
        ino.mode = 0x8000;
        ino.link_count = 1;
        if (mdfs_disk_write_inode(fs->vdrive_id, fs->start_lba, &fs->sb, ino_num, &ino) != 0) return -8;
        if (mdfs_v2_dir_add(fs, dir_ino, base, ino_num, 1) != 0) return -9;
    }

    /* Delegate to fast inode-based writer */
    return mdfs_write_file_at_by_inode(handle, ino_num, buffer, size, offset);
}
