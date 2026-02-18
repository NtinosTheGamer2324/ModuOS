#include "moduos/fs/MDFS/mdfs.h"
#include "moduos/drivers/Drive/vDrive.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/fs/MDFS/mdfs_dir.h"
#include "moduos/fs/MDFS/mdfs_disk.h"

// Implementations live in mdfs.c (helpers are static there), but we expose a minimal
// API for the VFS. To keep this first version simple, we implement root-only
// operations directly by re-reading structures on demand.

#if 0
static int mdfs_read_sb(int vdrive_id, uint32_t start_lba, mdfs_superblock_t *out) {
    uint8_t *buf = (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
    if (!buf) return -1;
    memset(buf, 0, MDFS_BLOCK_SIZE);
    uint32_t lba = start_lba + (uint32_t)(1 * (MDFS_BLOCK_SIZE/512u));
    if (vdrive_read((uint8_t)vdrive_id, (uint64_t)lba, (MDFS_BLOCK_SIZE/512u), buf) != VDRIVE_SUCCESS) { kfree(buf); return -2; }
    memcpy(out, buf, sizeof(*out));
    kfree(buf);
    return 0;
}

#include "moduos/fs/MDFS/mdfs_disk.h"

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

    // read direct blocks only (v1)
    uint8_t *blk = (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
    if (!blk) return -5;

    size_t to_read = (size_t)ino.size_bytes;
    if (to_read > buffer_size) to_read = buffer_size;

    size_t done = 0;
    while (done < to_read) {
        uint64_t bi = done / MDFS_BLOCK_SIZE;
        uint64_t boff = done % MDFS_BLOCK_SIZE;
        if (bi >= MDFS_MAX_DIRECT) break;
        uint64_t bno = ino.direct[bi];
        if (!bno) break;
        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, bno, blk) != VDRIVE_SUCCESS) { kfree(blk); return -6; }
        size_t chunk = MDFS_BLOCK_SIZE - (size_t)boff;
        if (chunk > (to_read - done)) chunk = to_read - done;
        memcpy((uint8_t*)buffer + done, blk + boff, chunk);
        done += chunk;
    }

    kfree(blk);
    *bytes_read = done;
    return 0;
}

static int mdfs_alloc_inode_simple(const mdfs_fs_t *fs, uint32_t *out_ino) {
    // simple bitmap scan (1 block)
    uint8_t *bm = (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
    if (!bm) return -1;
    if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, fs->sb.inode_bitmap_start, bm) != VDRIVE_SUCCESS) { kfree(bm); return -2; }

    for (uint32_t i = 1; i < (uint32_t)fs->sb.total_inodes; i++) {
        uint32_t byte = i / 8;
        uint8_t bit = (uint8_t)(1u << (i % 8));
        if ((bm[byte] & bit) == 0) {
            bm[byte] |= bit;
            if (mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, fs->sb.inode_bitmap_start, bm) != VDRIVE_SUCCESS) { kfree(bm); return -3; }
            kfree(bm);
            *out_ino = i;
            return 0;
        }
    }

    kfree(bm);
    return -4;
}

static int mdfs_alloc_block_simple(const mdfs_fs_t *fs, uint64_t *out_block) {
    uint8_t *bm = (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
    if (!bm) return -1;
    if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, fs->sb.block_bitmap_start, bm) != VDRIVE_SUCCESS) { kfree(bm); return -2; }

    uint64_t start = fs->sb.inode_table_start + fs->sb.inode_table_blocks;
    for (uint64_t b = start; b < fs->sb.total_blocks; b++) {
        uint64_t byte = b / 8;
        uint8_t bit = (uint8_t)(1u << (b % 8));
        if ((bm[byte] & bit) == 0) {
            bm[byte] |= bit;
            if (mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, fs->sb.block_bitmap_start, bm) != VDRIVE_SUCCESS) { kfree(bm); return -3; }
            kfree(bm);
            *out_block = b;
            return 0;
        }
    }

    kfree(bm);
    return -4;
}

static int mdfs_free_inode_simple(const mdfs_fs_t *fs, uint32_t ino) {
    if (!fs || ino == 0) return -1;
    uint8_t *bm = (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
    if (!bm) return -2;
    if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, fs->sb.inode_bitmap_start, bm) != VDRIVE_SUCCESS) { kfree(bm); return -3; }
    uint32_t byte = ino / 8;
    uint8_t bit = (uint8_t)(1u << (ino % 8));
    bm[byte] &= (uint8_t)~bit;
    if (mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, fs->sb.inode_bitmap_start, bm) != VDRIVE_SUCCESS) { kfree(bm); return -4; }
    kfree(bm);
    return 0;
}

static int mdfs_free_block_simple(const mdfs_fs_t *fs, uint64_t bno) {
    if (!fs || bno == 0) return -1;
    uint8_t *bm = (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
    if (!bm) return -2;
    if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, fs->sb.block_bitmap_start, bm) != VDRIVE_SUCCESS) { kfree(bm); return -3; }
    uint64_t byte = bno / 8;
    uint8_t bit = (uint8_t)(1u << (bno % 8));
    bm[byte] &= (uint8_t)~bit;
    if (mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, fs->sb.block_bitmap_start, bm) != VDRIVE_SUCCESS) { kfree(bm); return -4; }
    kfree(bm);
    return 0;
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

    // Free data blocks (direct only)
    for (uint32_t i = 0; i < MDFS_MAX_DIRECT; i++) {
        if (fin.direct[i]) (void)mdfs_free_block_simple(fs, fin.direct[i]);
    }

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

    // Free data blocks (direct only)
    for (uint32_t i = 0; i < MDFS_MAX_DIRECT; i++) {
        if (din.direct[i]) (void)mdfs_free_block_simple(fs, din.direct[i]);
    }

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
    uint8_t *blk = (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
    if (!blk) return -9;
    memset(blk, 0, MDFS_BLOCK_SIZE);
    if (mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, bno, blk) != VDRIVE_SUCCESS) { kfree(blk); return -10; }
    kfree(blk);

    // Write inode
    mdfs_inode_t ino;
    memset(&ino, 0, sizeof(ino));
    ino.mode = 0x4000;
    ino.link_count = 1;
    ino.size_bytes = 0;
    ino.direct[0] = bno;

    if (mdfs_disk_write_inode(fs->vdrive_id, fs->start_lba, &fs->sb, ino_num, &ino) != 0) return -11;

    // Add entry to parent directory
    if (mdfs_v2_dir_add(fs, dir_ino, base, ino_num, 2) != 0) return -12;

    return 0;
}

int mdfs_write_file_by_path(int handle, const char *path, const void *buffer, size_t size) {
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

    // write file data (direct blocks only)
    mdfs_inode_t ino;
    if (mdfs_disk_read_inode(fs->vdrive_id, fs->start_lba, &fs->sb, ino_num, &ino) != 0) return -8;

    uint8_t *blk = (uint8_t*)kmalloc(MDFS_BLOCK_SIZE);
    if (!blk) return -9;

    size_t done = 0;
    while (done < size) {
        uint64_t bi = done / MDFS_BLOCK_SIZE;
        uint64_t boff = done % MDFS_BLOCK_SIZE;
        if (bi >= MDFS_MAX_DIRECT) { kfree(blk); return -10; }

        if (ino.direct[bi] == 0) {
            uint64_t nb = 0;
            if (mdfs_alloc_block_simple(fs, &nb) != 0) { kfree(blk); return -11; }
            ino.direct[bi] = nb;
            memset(blk, 0, MDFS_BLOCK_SIZE);
            if (mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, nb, blk) != VDRIVE_SUCCESS) { kfree(blk); return -12; }
        }

        uint64_t bno = ino.direct[bi];
        if (mdfs_disk_read_block(fs->vdrive_id, fs->start_lba, bno, blk) != VDRIVE_SUCCESS) { kfree(blk); return -13; }
        size_t chunk = MDFS_BLOCK_SIZE - (size_t)boff;
        if (chunk > (size - done)) chunk = size - done;
        memcpy(blk + boff, (const uint8_t*)buffer + done, chunk);
        if (mdfs_disk_write_block(fs->vdrive_id, fs->start_lba, bno, blk) != VDRIVE_SUCCESS) { kfree(blk); return -14; }
        done += chunk;
    }

    ino.size_bytes = size;
    if (mdfs_disk_write_inode(fs->vdrive_id, fs->start_lba, &fs->sb, ino_num, &ino) != 0) { kfree(blk); return -15; }

    kfree(blk);
    return 0;
}
