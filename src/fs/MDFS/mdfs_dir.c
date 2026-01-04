#include "moduos/fs/MDFS/mdfs.h"
#include "moduos/fs/MDFS/mdfs_disk.h"
#include "moduos/drivers/Drive/vDrive.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"

// Directory entry-set helpers for MDFS v2.

static uint8_t mdfs_name_payload_per_record(void) {
    return 31;
}

static uint8_t mdfs_calc_record_count(uint16_t name_len) {
    uint32_t payload = mdfs_name_payload_per_record();
    uint32_t name_recs = (name_len + payload - 1) / payload;
    uint32_t total = 1 + name_recs;
    if (total > 255) total = 255;
    return (uint8_t)total;
}

static uint32_t mdfs_entry_crc32(const uint8_t *entry_set, uint32_t bytes) {
    // entry_set contains checksum field; caller must have zeroed it.
    return mdfs_crc32_buf(entry_set, bytes);
}

static int mdfs_v2_dir_read_inode(const mdfs_fs_t *fs, uint32_t dir_ino, mdfs_inode_t *out) {
    if (!fs || !out || dir_ino == 0) return -1;
    if (mdfs_disk_read_inode(fs->vdrive_id, fs->start_lba, &fs->sb, dir_ino, out) != 0) return -2;
    if ((out->mode & 0xF000) != 0x4000) return -3;
    return 0;
}

int mdfs_v2_dir_list(const mdfs_fs_t *fs, uint32_t dir_ino, mdfs_dirent_t *out, int max_entries) {
    if (!fs || !out || max_entries <= 0) return -1;

    mdfs_inode_t root;
    int irc = mdfs_v2_dir_read_inode(fs, dir_ino, &root);
    if (irc != 0) return -2;

    int outc = 0;
    uint8_t *blk = (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
    if (!blk) return -3;

    for (uint32_t di = 0; di < MDFS_MAX_DIRECT; di++) {
        uint64_t bno = root.direct[di];
        if (!bno) continue;
        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, bno, blk) != VDRIVE_SUCCESS) { kfree(blk); return -4; }

        for (uint32_t off = 0; off + MDFS_DIR_REC_SIZE <= MDFS_BLOCK_SIZE;) {
            mdfs_dir_primary_t *pr = (mdfs_dir_primary_t*)(blk + off);
            if (pr->rec_type == 0) break;
            if (pr->rec_type != MDFS_DIRREC_PRIMARY || (pr->flags & MDFS_DIRFLAG_VALID) == 0 || (pr->flags & MDFS_DIRFLAG_DELETED)) {
                // Skip unknown/deleted; advance by 1 record if malformed.
                uint32_t adv = (pr->rec_type == MDFS_DIRREC_PRIMARY && pr->record_count) ? pr->record_count : 1;
                off += adv * MDFS_DIR_REC_SIZE;
                continue;
            }

            uint32_t set_bytes = (uint32_t)pr->record_count * MDFS_DIR_REC_SIZE;
            if (off + set_bytes > MDFS_BLOCK_SIZE) break;

            // Verify checksum
            uint32_t saved = pr->checksum;
            pr->checksum = 0;
            uint32_t calc = mdfs_entry_crc32(blk + off, set_bytes);
            pr->checksum = saved;
            if (saved != 0 && saved != calc) {
                off += pr->record_count * MDFS_DIR_REC_SIZE;
                continue;
            }

            if (outc < max_entries) {
                mdfs_dirent_t *de = &out[outc++];
                memset(de, 0, sizeof(*de));
                de->inode = pr->inode;
                de->type = pr->entry_type;

                uint16_t nl = pr->name_len;
                if (nl > MDFS_MAX_NAME) nl = MDFS_MAX_NAME;
                uint16_t copied = 0;
                uint8_t payload = mdfs_name_payload_per_record();
                for (uint32_t ri = 1; ri < pr->record_count && copied < nl; ri++) {
                    mdfs_dir_name_t *nr = (mdfs_dir_name_t*)(blk + off + ri * MDFS_DIR_REC_SIZE);
                    if (nr->rec_type != MDFS_DIRREC_NAME) break;
                    uint16_t take = nl - copied;
                    if (take > payload) take = payload;
                    memcpy(de->name + copied, nr->name_bytes, take);
                    copied += take;
                }
                de->name[copied] = 0;
            }

            off += pr->record_count * MDFS_DIR_REC_SIZE;
        }
    }

    kfree(blk);
    return outc;
}

int mdfs_v2_dir_lookup(const mdfs_fs_t *fs, uint32_t dir_ino, const char *name, uint32_t *out_ino, uint8_t *out_type) {
    if (!fs || !name || !name[0]) return -1;
    size_t nl_req = strlen(name);
    if (nl_req > MDFS_MAX_NAME) return -2;

    mdfs_inode_t root;
    int irc = mdfs_v2_dir_read_inode(fs, dir_ino, &root);
    if (irc != 0) return -3;

    uint8_t *blk = (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
    if (!blk) return -4;

    for (uint32_t di = 0; di < MDFS_MAX_DIRECT; di++) {
        uint64_t bno = root.direct[di];
        if (!bno) continue;
        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, bno, blk) != VDRIVE_SUCCESS) { kfree(blk); return -5; }

        for (uint32_t off = 0; off + MDFS_DIR_REC_SIZE <= MDFS_BLOCK_SIZE;) {
            mdfs_dir_primary_t *pr = (mdfs_dir_primary_t*)(blk + off);
            if (pr->rec_type == 0) break;
            uint32_t adv = (pr->rec_type == MDFS_DIRREC_PRIMARY && pr->record_count) ? pr->record_count : 1;

            if (pr->rec_type == MDFS_DIRREC_PRIMARY && (pr->flags & MDFS_DIRFLAG_VALID) && !(pr->flags & MDFS_DIRFLAG_DELETED)) {
                uint16_t nl = pr->name_len;
                if (nl == nl_req && nl <= MDFS_MAX_NAME) {
                    uint32_t set_bytes = (uint32_t)pr->record_count * MDFS_DIR_REC_SIZE;
                    if (off + set_bytes <= MDFS_BLOCK_SIZE) {
                        uint32_t saved = pr->checksum;
                        pr->checksum = 0;
                        uint32_t calc = mdfs_entry_crc32(blk + off, set_bytes);
                        pr->checksum = saved;
                        if (saved == 0 || saved == calc) {
                            // compare name
                            char tmp[MDFS_MAX_NAME + 1];
                            memset(tmp, 0, sizeof(tmp));
                            uint16_t copied = 0;
                            uint8_t payload = mdfs_name_payload_per_record();
                            for (uint32_t ri = 1; ri < pr->record_count && copied < nl; ri++) {
                                mdfs_dir_name_t *nr = (mdfs_dir_name_t*)(blk + off + ri * MDFS_DIR_REC_SIZE);
                                if (nr->rec_type != MDFS_DIRREC_NAME) break;
                                uint16_t take = nl - copied;
                                if (take > payload) take = payload;
                                memcpy(tmp + copied, nr->name_bytes, take);
                                copied += take;
                            }
                            tmp[copied] = 0;
                            if (strcmp(tmp, name) == 0) {
                                if (out_ino) *out_ino = pr->inode;
                                if (out_type) *out_type = pr->entry_type;
                                kfree(blk);
                                return 0;
                            }
                        }
                    }
                }
            }

            off += adv * MDFS_DIR_REC_SIZE;
        }
    }

    kfree(blk);
    return -6;
}

int mdfs_v2_dir_remove(const mdfs_fs_t *fs, uint32_t dir_ino, const char *name) {
    if (!fs || !name || !name[0]) return -1;
    size_t nl_req = strlen(name);
    if (nl_req > MDFS_MAX_NAME) return -2;

    mdfs_inode_t root;
    int irc = mdfs_v2_dir_read_inode(fs, dir_ino, &root);
    if (irc != 0) return -3;

    uint8_t *blk = (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
    if (!blk) return -4;

    for (uint32_t di = 0; di < MDFS_MAX_DIRECT; di++) {
        uint64_t bno = root.direct[di];
        if (!bno) continue;
        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, bno, blk) != VDRIVE_SUCCESS) { kfree(blk); return -5; }

        for (uint32_t off = 0; off + MDFS_DIR_REC_SIZE <= MDFS_BLOCK_SIZE;) {
            mdfs_dir_primary_t *pr = (mdfs_dir_primary_t*)(blk + off);
            if (pr->rec_type == 0) break;
            uint32_t adv = (pr->rec_type == MDFS_DIRREC_PRIMARY && pr->record_count) ? pr->record_count : 1;

            if (pr->rec_type == MDFS_DIRREC_PRIMARY && (pr->flags & MDFS_DIRFLAG_VALID) && !(pr->flags & MDFS_DIRFLAG_DELETED)) {
                uint16_t nl = pr->name_len;
                if ((size_t)nl == nl_req && nl <= MDFS_MAX_NAME) {
                    uint32_t set_bytes = (uint32_t)pr->record_count * MDFS_DIR_REC_SIZE;
                    if (off + set_bytes <= MDFS_BLOCK_SIZE) {
                        // Verify checksum
                        uint32_t saved = pr->checksum;
                        pr->checksum = 0;
                        uint32_t calc = mdfs_entry_crc32(blk + off, set_bytes);
                        pr->checksum = saved;
                        if (saved == 0 || saved == calc) {
                            char tmp[MDFS_MAX_NAME + 1];
                            memset(tmp, 0, sizeof(tmp));
                            uint16_t copied = 0;
                            uint8_t payload = mdfs_name_payload_per_record();
                            for (uint32_t ri = 1; ri < pr->record_count && copied < nl; ri++) {
                                mdfs_dir_name_t *nr = (mdfs_dir_name_t*)(blk + off + ri * MDFS_DIR_REC_SIZE);
                                if (nr->rec_type != MDFS_DIRREC_NAME) break;
                                uint16_t take = nl - copied;
                                if (take > payload) take = payload;
                                memcpy(tmp + copied, nr->name_bytes, take);
                                copied += take;
                            }
                            tmp[copied] = 0;
                            if (strcmp(tmp, name) == 0) {
                                // Mark deleted and recompute checksum
                                pr->flags |= MDFS_DIRFLAG_DELETED;
                                pr->checksum = 0;
                                uint32_t crc = mdfs_entry_crc32(blk + off, set_bytes);
                                pr->checksum = crc;
                                if (mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, bno, blk) != VDRIVE_SUCCESS) { kfree(blk); return -6; }
                                kfree(blk);
                                return 0;
                            }
                        }
                    }
                }
            }

            off += adv * MDFS_DIR_REC_SIZE;
        }
    }

    kfree(blk);
    return -7;
}

int mdfs_v2_dir_add(const mdfs_fs_t *fs, uint32_t dir_ino, const char *name, uint32_t ino, uint8_t type) {
    if (!fs || !name || !name[0]) return -1;
    size_t nl_req = strlen(name);
    if (nl_req > MDFS_MAX_NAME) return -2;

    mdfs_inode_t root;
    int irc = mdfs_v2_dir_read_inode(fs, dir_ino, &root);
    if (irc != 0) return -3;

    uint8_t *blk = (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
    if (!blk) return -4;

    uint8_t rec_cnt = mdfs_calc_record_count((uint16_t)nl_req);
    uint32_t set_bytes = (uint32_t)rec_cnt * MDFS_DIR_REC_SIZE;

    // Find space in existing blocks, else allocate a new direct block.
    for (uint32_t di = 0; di < MDFS_MAX_DIRECT; di++) {
        uint64_t bno = root.direct[di];
        if (!bno) continue;
        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, bno, blk) != VDRIVE_SUCCESS) { kfree(blk); return -5; }

        for (uint32_t off = 0; off + set_bytes <= MDFS_BLOCK_SIZE; off += MDFS_DIR_REC_SIZE) {
            mdfs_dir_primary_t *pr = (mdfs_dir_primary_t*)(blk + off);
            if (pr->rec_type != 0) continue;

            // Build entry set in-place
            memset(blk + off, 0, set_bytes);
            mdfs_dir_primary_t *w = (mdfs_dir_primary_t*)(blk + off);
            w->rec_type = MDFS_DIRREC_PRIMARY;
            w->flags = MDFS_DIRFLAG_VALID;
            w->entry_type = type;
            w->record_count = rec_cnt;
            w->inode = ino;
            w->name_len = (uint16_t)nl_req;
            w->checksum = 0;

            // Fill name payload
            uint8_t payload = mdfs_name_payload_per_record();
            const uint8_t *nb = (const uint8_t*)name;
            uint32_t pos = 0;
            for (uint32_t ri = 1; ri < rec_cnt; ri++) {
                mdfs_dir_name_t *nr = (mdfs_dir_name_t*)(blk + off + ri * MDFS_DIR_REC_SIZE);
                nr->rec_type = MDFS_DIRREC_NAME;
                uint32_t take = (uint32_t)nl_req - pos;
                if (take > payload) take = payload;
                memcpy(nr->name_bytes, nb + pos, take);
                pos += take;
            }

            // CRC32 over entry set
            w->checksum = 0;
            uint32_t crc = mdfs_entry_crc32(blk + off, set_bytes);
            w->checksum = crc;

            if (mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, bno, blk) != VDRIVE_SUCCESS) { kfree(blk); return -6; }
            kfree(blk);
            return 0;
        }
    }

    kfree(blk);
    return -7;
}

// Exported wrappers used by mdfs_api.c can call these helpers by reusing the root-only bringup.
// (To keep symbols minimal, we expose only list/lookup/add via these names.)

