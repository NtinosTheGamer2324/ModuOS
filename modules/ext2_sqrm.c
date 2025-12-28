#include <stdint.h>
#include <stddef.h>

#include "moduos/kernel/sqrm.h"
#include "moduos/fs/fs.h"
#include "moduos/kernel/COM/com.h"

// SQRM modules are built -nostdlib; do not rely on kernel libc symbols unless they
// are explicitly passed via the API table. Provide minimal local helpers.
static void *m_memset(void *dest, int val, size_t len) {
    uint8_t *p = (uint8_t*)dest;
    for (size_t i = 0; i < len; i++) p[i] = (uint8_t)val;
    return dest;
}

static void *m_memcpy(void *dest, const void *src, size_t len) {
    uint8_t *d = (uint8_t*)dest;
    const uint8_t *s = (const uint8_t*)src;
    for (size_t i = 0; i < len; i++) d[i] = s[i];
    return dest;
}

static size_t m_strlen(const char *s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static int m_strcmp(const char *a, const char *b) {
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static char *m_strncpy(char *dst, const char *src, size_t n) {
    if (!dst || n == 0) return dst;
    size_t i = 0;
    if (src) {
        for (; i + 1 < n && src[i]; i++) dst[i] = src[i];
    }
    dst[i] = 0;
    return dst;
}

static char *m_strncat(char *dst, const char *src, size_t n) {
    if (!dst || n == 0) return dst;
    size_t dl = m_strlen(dst);
    if (dl >= n) return dst;
    size_t i = 0;
    if (src) {
        for (; dl + i + 1 < n && src[i]; i++) dst[dl + i] = src[i];
    }
    dst[dl + i] = 0;
    return dst;
}

// Declared here because ext2_probe() uses it for debug logging; definition is
// later in the file.
static void i32_to_dec(char *out, size_t out_sz, int32_t v);

static const sqrm_module_desc_t sqrm_module_desc = {
    .abi_version = 1,
    .type = SQRM_TYPE_FS,
    .name = "ext2",
};

static const sqrm_kernel_api_t *g_api;

#define EXT2_SUPERBLOCK_OFF 1024
#define EXT2_MAGIC 0xEF53

typedef struct __attribute__((packed)) {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    int32_t  s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    int16_t  s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    // ext2 rev1
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algorithm_usage_bitmap;
} ext2_superblock_t;

typedef struct __attribute__((packed)) {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} ext2_bgdt_t;

typedef struct __attribute__((packed)) {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} ext2_inode_t;

typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;
    char name[0];
} ext2_dirent_t;

typedef struct {
    blockdev_handle_t bdev;
    uint64_t part_lba; // partition base LBA
    ext2_superblock_t sb;
    uint32_t block_size;
    uint32_t groups;
    uint32_t bgdt_block;
    uint32_t inode_size;
} ext2_mount_ctx_t;

static int bdev_read_bytes(ext2_mount_ctx_t *m, uint64_t off, void *buf, size_t sz) {
    blockdev_handle_t bdev = m->bdev;
    blockdev_info_t bi;
    if (g_api->block_get_info(bdev, &bi) != 0) return -1;
    uint32_t ss = bi.sector_size;
    uint64_t first = (m->part_lba * (uint64_t)ss + off) / ss;
    uint64_t last = (m->part_lba * (uint64_t)ss + off + sz + ss - 1) / ss;
    uint32_t cnt = (uint32_t)(last - first);
    uint64_t tmp_sz = (uint64_t)cnt * ss;
    uint8_t *tmp = (uint8_t*)g_api->kmalloc((size_t)tmp_sz);
    if (!tmp) return -2;
    int r = g_api->block_read(bdev, first, cnt, tmp, (size_t)tmp_sz);
    if (r != 0) { g_api->kfree(tmp); return -3; }
    // offset within first sector
    uint64_t abs_off = m->part_lba * (uint64_t)ss + off;
    m_memcpy(buf, tmp + (abs_off - first * ss), sz);
    g_api->kfree(tmp);
    return 0;
}

static int ext2_read_super(ext2_mount_ctx_t *m, ext2_superblock_t *out) {
    return bdev_read_bytes(m, EXT2_SUPERBLOCK_OFF, out, sizeof(*out));
}

static void u16_to_hex4(char out[5], uint16_t v) {
    static const char h[] = "0123456789ABCDEF";
    out[0] = h[(v >> 12) & 0xF];
    out[1] = h[(v >> 8) & 0xF];
    out[2] = h[(v >> 4) & 0xF];
    out[3] = h[v & 0xF];
    out[4] = 0;
}

static int ext2_probe(int vdrive_id, uint32_t partition_lba) {
    if (!g_api || !g_api->block_get_handle_for_vdrive) return 0;

    blockdev_handle_t bdev = BLOCKDEV_INVALID_HANDLE;
    int hr = g_api->block_get_handle_for_vdrive(vdrive_id, &bdev);
    if (hr != 0) return 0;

    ext2_mount_ctx_t tmp;
    m_memset(&tmp, 0, sizeof(tmp));
    tmp.bdev = bdev;
    tmp.part_lba = partition_lba;

    ext2_superblock_t sb;
    int rr = ext2_read_super(&tmp, &sb);

#ifdef EXT2_DEBUG
    if (g_api->com_write_string) {
        g_api->com_write_string(COM1_PORT, "[ext2] probe vDrive=");
        char nb[16];
        i32_to_dec(nb, sizeof(nb), vdrive_id);
        g_api->com_write_string(COM1_PORT, nb);
        g_api->com_write_string(COM1_PORT, " lba=");
        i32_to_dec(nb, sizeof(nb), (int32_t)partition_lba);
        g_api->com_write_string(COM1_PORT, nb);
        g_api->com_write_string(COM1_PORT, " hr=");
        i32_to_dec(nb, sizeof(nb), hr);
        g_api->com_write_string(COM1_PORT, nb);
        g_api->com_write_string(COM1_PORT, " rr=");
        i32_to_dec(nb, sizeof(nb), rr);
        g_api->com_write_string(COM1_PORT, nb);
        g_api->com_write_string(COM1_PORT, " magic=0x");
        char hx[5];
        u16_to_hex4(hx, sb.s_magic);
        g_api->com_write_string(COM1_PORT, hx);
        g_api->com_write_string(COM1_PORT, "\n");
    }
#endif

    if (rr != 0) return 0;
    return (sb.s_magic == EXT2_MAGIC) ? 1 : 0;
}

static int ext2_read_bgdt(ext2_mount_ctx_t *m, uint32_t group, ext2_bgdt_t *out) {
    uint64_t off = (uint64_t)m->bgdt_block * m->block_size + (uint64_t)group * sizeof(ext2_bgdt_t);
    return bdev_read_bytes(m, off, out, sizeof(*out));
}

static int ext2_read_inode(ext2_mount_ctx_t *m, uint32_t ino, ext2_inode_t *out) {
    if (ino == 0) return -1;
    uint32_t idx = ino - 1;
    uint32_t group = idx / m->sb.s_inodes_per_group;
    uint32_t in_group = idx % m->sb.s_inodes_per_group;

    ext2_bgdt_t bg;
    if (ext2_read_bgdt(m, group, &bg) != 0) return -2;

    uint64_t off = (uint64_t)bg.bg_inode_table * m->block_size + (uint64_t)in_group * m->inode_size;
    m_memset(out, 0, sizeof(*out));
    // read only the common header portion
    size_t rd = sizeof(ext2_inode_t);
    if (m->inode_size < rd) rd = m->inode_size;
    return bdev_read_bytes(m, off, out, rd);
}

static int ext2_read_block(ext2_mount_ctx_t *m, uint32_t blk, void *buf) {
    uint64_t off = (uint64_t)blk * m->block_size;
    return bdev_read_bytes(m, off, buf, m->block_size);
}

static uint32_t ext2_get_block_ptr(ext2_mount_ctx_t *m, const ext2_inode_t *in, uint32_t lbn) {
    uint32_t ppb = m->block_size / 4;
    if (lbn < 12) return in->i_block[lbn];

    lbn -= 12;
    if (lbn < ppb) {
        uint32_t ind = in->i_block[12];
        if (!ind) return 0;
        uint32_t *tmp = (uint32_t*)g_api->kmalloc(m->block_size);
        if (!tmp) return 0;
        if (ext2_read_block(m, ind, tmp) != 0) { g_api->kfree(tmp); return 0; }
        uint32_t v = tmp[lbn];
        g_api->kfree(tmp);
        return v;
    }

    lbn -= ppb;
    // double indirect
    uint32_t dind = in->i_block[13];
    if (!dind) return 0;
    uint32_t *lvl1 = (uint32_t*)g_api->kmalloc(m->block_size);
    uint32_t *lvl2 = (uint32_t*)g_api->kmalloc(m->block_size);
    if (!lvl1 || !lvl2) { if(lvl1) g_api->kfree(lvl1); if(lvl2) g_api->kfree(lvl2); return 0; }
    if (ext2_read_block(m, dind, lvl1) != 0) { g_api->kfree(lvl1); g_api->kfree(lvl2); return 0; }

    uint32_t idx1 = lbn / ppb;
    uint32_t idx2 = lbn % ppb;
    if (idx1 >= ppb) { g_api->kfree(lvl1); g_api->kfree(lvl2); return 0; }
    uint32_t blk1 = lvl1[idx1];
    if (!blk1) { g_api->kfree(lvl1); g_api->kfree(lvl2); return 0; }
    if (ext2_read_block(m, blk1, lvl2) != 0) { g_api->kfree(lvl1); g_api->kfree(lvl2); return 0; }
    uint32_t v = lvl2[idx2];
    g_api->kfree(lvl1);
    g_api->kfree(lvl2);
    return v;
}

static int ext2_read_inode_data(ext2_mount_ctx_t *m, const ext2_inode_t *in, uint64_t off, void *buf, size_t sz) {
    uint8_t *out = (uint8_t*)buf;
    uint64_t file_size = in->i_size;
    if (off >= file_size) return 0;
    if (off + sz > file_size) sz = (size_t)(file_size - off);

    uint32_t bs = m->block_size;
    uint32_t start_lbn = (uint32_t)(off / bs);
    uint32_t end_lbn = (uint32_t)((off + sz + bs - 1) / bs);

    uint8_t *blkbuf = (uint8_t*)g_api->kmalloc(bs);
    if (!blkbuf) return -1;

    size_t outpos = 0;
    for (uint32_t lbn = start_lbn; lbn < end_lbn; lbn++) {
        uint32_t pblk = ext2_get_block_ptr(m, in, lbn);
        m_memset(blkbuf, 0, bs);
        if (pblk != 0) {
            (void)ext2_read_block(m, pblk, blkbuf);
        }

        uint64_t lbn_off = (uint64_t)lbn * bs;
        uint64_t copy_start = (off > lbn_off) ? (off - lbn_off) : 0;
        uint64_t copy_end = bs;
        if (lbn_off + copy_end > off + sz) copy_end = (off + sz) - lbn_off;
        if (copy_end > bs) copy_end = bs;

        size_t csz = (size_t)(copy_end - copy_start);
        m_memcpy(out + outpos, blkbuf + copy_start, csz);
        outpos += csz;
    }

    g_api->kfree(blkbuf);
    return (int)outpos;
}

static int ext2_readlink(ext2_mount_ctx_t *m, const ext2_inode_t *in, char *out, size_t out_sz) {
    if (out_sz == 0) return -1;
    size_t len = in->i_size;
    if (len >= out_sz) len = out_sz - 1;

    // Fast symlink: stored in i_block bytes if small
    if (in->i_size <= sizeof(in->i_block)) {
        m_memcpy(out, in->i_block, len);
        out[len] = 0;
        return 0;
    }

    int r = ext2_read_inode_data(m, in, 0, out, len);
    if (r < 0) return r;
    out[r] = 0;
    return 0;
}

static int ext2_lookup_in_dir(ext2_mount_ctx_t *m, const ext2_inode_t *dir, const char *name, uint32_t *out_ino) {
    uint32_t bs = m->block_size;
    uint8_t *blk = (uint8_t*)g_api->kmalloc(bs);
    if (!blk) return -1;

    uint32_t entries = (dir->i_size + bs - 1) / bs;
    for (uint32_t lbn = 0; lbn < entries; lbn++) {
        uint32_t pblk = ext2_get_block_ptr(m, dir, lbn);
        if (!pblk) continue;
        if (ext2_read_block(m, pblk, blk) != 0) continue;

        uint32_t off = 0;
        while (off + sizeof(ext2_dirent_t) <= bs) {
            ext2_dirent_t *de = (ext2_dirent_t*)(blk + off);
            if (de->rec_len == 0) break;
            if (de->inode != 0 && de->name_len > 0) {
                char nm[256];
                size_t nlen = de->name_len;
                if (nlen >= sizeof(nm)) nlen = sizeof(nm) - 1;
                m_memcpy(nm, de->name, nlen);
                nm[nlen] = 0;
                if (m_strcmp(nm, name) == 0) {
                    *out_ino = de->inode;
                    g_api->kfree(blk);
                    return 0;
                }
            }
            off += de->rec_len;
        }
    }

    g_api->kfree(blk);
    return -2;
}

static int ext2_resolve_path(ext2_mount_ctx_t *m, const char *path, uint32_t *out_ino, int hop) {
    if (!path || !out_ino) return -1;
    if (hop > 8) return -9;

    // Root
    uint32_t cur_ino = 2;
    ext2_inode_t cur;
    if (ext2_read_inode(m, cur_ino, &cur) != 0) return -2;

    // skip leading '/'
    while (*path == '/') path++;
    if (!*path) { *out_ino = cur_ino; return 0; }

    char seg[256];
    const char *p = path;
    while (*p) {
        size_t i = 0;
        while (p[i] && p[i] != '/') {
            if (i + 1 < sizeof(seg)) seg[i] = p[i];
            i++;
        }
        seg[(i < sizeof(seg)) ? i : (sizeof(seg) - 1)] = 0;

        while (p[i] == '/') i++;
        const char *next = p + i;

        uint32_t next_ino = 0;
        if (ext2_lookup_in_dir(m, &cur, seg, &next_ino) != 0) return -3;

        ext2_inode_t nin;
        if (ext2_read_inode(m, next_ino, &nin) != 0) return -4;

        // symlink?
        if ((nin.i_mode & 0xF000) == 0xA000) {
            char target[512];
            if (ext2_readlink(m, &nin, target, sizeof(target)) != 0) return -5;

            // build new path: target + '/' + remainder
            char newp[1024];
            newp[0] = 0;
            if (target[0] == '/') {
                m_strncpy(newp, target, sizeof(newp) - 1);
            } else {
                // relative to current directory
                m_strncpy(newp, "/", sizeof(newp) - 1);
                m_strncat(newp, target, sizeof(newp) - m_strlen(newp) - 1);
            }
            if (*next) {
                if (newp[m_strlen(newp) - 1] != '/') m_strncat(newp, "/", sizeof(newp) - m_strlen(newp) - 1);
                m_strncat(newp, next, sizeof(newp) - m_strlen(newp) - 1);
            }

            return ext2_resolve_path(m, newp, out_ino, hop + 1);
        }

        // advance
        cur_ino = next_ino;
        cur = nin;
        p = next;
        if (!*p) break;
    }

    *out_ino = cur_ino;
    return 0;
}

static int ext2_stat(fs_mount_t *mount, const char *path, fs_file_info_t *info) {
    ext2_mount_ctx_t *m = (ext2_mount_ctx_t*)mount->ext_ctx;
    uint32_t ino;
    if (ext2_resolve_path(m, path, &ino, 0) != 0) return -1;
    ext2_inode_t in;
    if (ext2_read_inode(m, ino, &in) != 0) return -2;

    m_memset(info, 0, sizeof(*info));
    info->size = in.i_size;
    info->is_directory = ((in.i_mode & 0xF000) == 0x4000);
    return 0;
}

static int ext2_file_exists(fs_mount_t *mount, const char *path) {
    fs_file_info_t i;
    if (ext2_stat(mount, path, &i) != 0) return 0;
    return i.is_directory ? 0 : 1;
}

static int ext2_dir_exists(fs_mount_t *mount, const char *path) {
    fs_file_info_t i;
    if (ext2_stat(mount, path, &i) != 0) return 0;
    return i.is_directory ? 1 : 0;
}

static int ext2_read_file(fs_mount_t *mount, const char *path, void *buffer, size_t buffer_size, size_t *bytes_read) {
    ext2_mount_ctx_t *m = (ext2_mount_ctx_t*)mount->ext_ctx;
    uint32_t ino;
    if (ext2_resolve_path(m, path, &ino, 0) != 0) return -1;
    ext2_inode_t in;
    if (ext2_read_inode(m, ino, &in) != 0) return -2;
    if (((in.i_mode & 0xF000) != 0x8000) && ((in.i_mode & 0xF000) != 0xA000)) return -3;

    int r = ext2_read_inode_data(m, &in, 0, buffer, buffer_size);
    if (r < 0) return r;
    if (bytes_read) *bytes_read = (size_t)r;
    return 0;
}

typedef struct {
    ext2_mount_ctx_t *m;
    ext2_inode_t dir_inode;
    uint32_t lbn;
    uint32_t off;
    uint8_t *blk;
} ext2_dir_iter_t;

static fs_dir_t* ext2_opendir(fs_mount_t *mount, const char *path) {
    ext2_mount_ctx_t *m = (ext2_mount_ctx_t*)mount->ext_ctx;
    uint32_t ino;
    if (ext2_resolve_path(m, path, &ino, 0) != 0) return NULL;
    ext2_inode_t in;
    if (ext2_read_inode(m, ino, &in) != 0) return NULL;
    if ((in.i_mode & 0xF000) != 0x4000) return NULL;

    fs_dir_t *d = (fs_dir_t*)g_api->kmalloc(sizeof(fs_dir_t));
    if (!d) return NULL;
    m_memset(d, 0, sizeof(*d));
    d->mount = mount;
    m_strncpy(d->path, path, sizeof(d->path) - 1);

    ext2_dir_iter_t *it = (ext2_dir_iter_t*)g_api->kmalloc(sizeof(ext2_dir_iter_t));
    if (!it) { g_api->kfree(d); return NULL; }
    m_memset(it, 0, sizeof(*it));
    it->m = m;
    it->dir_inode = in;
    it->lbn = 0;
    it->off = 0;
    it->blk = (uint8_t*)g_api->kmalloc(m->block_size);
    if (!it->blk) { g_api->kfree(it); g_api->kfree(d); return NULL; }

    d->fs_specific = it;
    return d;
}

static int ext2_readdir(fs_dir_t *dir, fs_dirent_t *entry) {
    ext2_dir_iter_t *it = (ext2_dir_iter_t*)dir->fs_specific;
    if (!it) return 0;

    uint32_t bs = it->m->block_size;

    while (1) {
        if (it->lbn * bs >= it->dir_inode.i_size) return 0;

        if (it->off == 0) {
            uint32_t pblk = ext2_get_block_ptr(it->m, &it->dir_inode, it->lbn);
            if (!pblk) return 0;
            if (ext2_read_block(it->m, pblk, it->blk) != 0) return 0;
        }

        if (it->off + sizeof(ext2_dirent_t) > bs) {
            it->lbn++;
            it->off = 0;
            continue;
        }

        ext2_dirent_t *de = (ext2_dirent_t*)(it->blk + it->off);
        if (de->rec_len == 0) {
            it->lbn++;
            it->off = 0;
            continue;
        }

        it->off += de->rec_len;

        if (de->inode == 0 || de->name_len == 0) continue;

        // Copy name
        size_t nlen = de->name_len;
        if (nlen >= sizeof(entry->name)) nlen = sizeof(entry->name) - 1;
        m_memcpy(entry->name, de->name, nlen);
        entry->name[nlen] = 0;

        // Determine if directory
        ext2_inode_t cin;
        if (ext2_read_inode(it->m, de->inode, &cin) != 0) continue;
        entry->is_directory = ((cin.i_mode & 0xF000) == 0x4000);
        entry->size = cin.i_size;
        return 1;
    }
}

static void ext2_closedir(fs_dir_t *dir) {
    if (!dir) return;
    ext2_dir_iter_t *it = (ext2_dir_iter_t*)dir->fs_specific;
    if (it) {
        if (it->blk) g_api->kfree(it->blk);
        g_api->kfree(it);
    }
    g_api->kfree(dir);
}

static int ext2_mount(int vdrive_id, uint32_t partition_lba, fs_mount_t *mount) {
    if (!g_api || !g_api->block_get_handle_for_vdrive) return -10;

    blockdev_handle_t bdev = BLOCKDEV_INVALID_HANDLE;
    if (g_api->block_get_handle_for_vdrive(vdrive_id, &bdev) != 0) return -11;

    ext2_mount_ctx_t *m = (ext2_mount_ctx_t*)g_api->kmalloc(sizeof(ext2_mount_ctx_t));
    if (!m) return -1;
    m_memset(m, 0, sizeof(*m));
    m->bdev = bdev;
    m->part_lba = partition_lba;

    if (ext2_read_super(m, &m->sb) != 0 || m->sb.s_magic != EXT2_MAGIC) {
        g_api->kfree(m);
        return -2;
    }

    m->block_size = 1024u << m->sb.s_log_block_size;
    m->inode_size = m->sb.s_inode_size ? m->sb.s_inode_size : 128;
    m->groups = (m->sb.s_blocks_count + m->sb.s_blocks_per_group - 1) / m->sb.s_blocks_per_group;

    // BGDT location
    if (m->block_size == 1024) m->bgdt_block = 2;
    else m->bgdt_block = 1;

    mount->ext_ctx = m;
    return 0;
}

static int ext2_list_directory(fs_mount_t *mount, const char *path) {
    // No VGA printing capability in SQRM v1; log to COM1.
    if (!g_api || !g_api->com_write_string) return -1;
    fs_dir_t *d = ext2_opendir(mount, path);
    if (!d) return -2;

    g_api->com_write_string(COM1_PORT, "[ext2] dir ");
    g_api->com_write_string(COM1_PORT, path ? path : "/");
    g_api->com_write_string(COM1_PORT, "\n");

    fs_dirent_t e;
    while (ext2_readdir(d, &e) > 0) {
        g_api->com_write_string(COM1_PORT, e.is_directory ? "  [D] " : "  [F] ");
        g_api->com_write_string(COM1_PORT, e.name);
        g_api->com_write_string(COM1_PORT, "\n");
    }

    ext2_closedir(d);
    return 0;
}

static void ext2_unmount(fs_mount_t *mount) {
    if (!mount || !g_api) return;
    ext2_mount_ctx_t *m = (ext2_mount_ctx_t*)mount->ext_ctx;
    if (m) g_api->kfree(m);
    mount->ext_ctx = NULL;
}

// --- mkfs (format) ---

static void set_bit(uint8_t *bmp, uint32_t bit) {
    bmp[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

static int ext2_mkfs(int vdrive_id, uint32_t partition_lba, uint32_t partition_sectors, const char *volume_label) {
    if (!g_api || !g_api->block_get_handle_for_vdrive || !g_api->block_write) return -1;

    if (partition_sectors < 128) return -2;

    blockdev_handle_t bdev = BLOCKDEV_INVALID_HANDLE;
    if (g_api->block_get_handle_for_vdrive(vdrive_id, &bdev) != 0) return -3;

    blockdev_info_t info;
    if (g_api->block_get_info && g_api->block_get_info(bdev, &info) == 0) {
        if (info.sector_size != 512) return -4;
    }

    // 4KiB blocks
    const uint32_t block_size = 4096;
    const uint32_t sectors_per_block = 8;
    uint32_t total_blocks = partition_sectors / sectors_per_block;
    if (total_blocks < 32) return -5;

    const uint32_t inodes_per_group = 256;
    const uint32_t inode_size = 128;
    const uint32_t inode_table_blocks = (inodes_per_group * inode_size + block_size - 1) / block_size;

    const uint32_t super_blockno = 0;
    const uint32_t bgdt_blockno = 1;
    const uint32_t block_bmp_blockno = 2;
    const uint32_t inode_bmp_blockno = 3;
    const uint32_t inode_table_blockno = 4;
    const uint32_t first_data_blockno = inode_table_blockno + inode_table_blocks;
    if (first_data_blockno + 2 >= total_blocks) return -6;

    const uint32_t root_dir_blockno = first_data_blockno;
    const uint32_t lostfound_blockno = first_data_blockno + 1;

    // Buffers
    uint8_t *blk = (uint8_t*)g_api->kmalloc(block_size);
    uint8_t *block_bmp = (uint8_t*)g_api->kmalloc(block_size);
    uint8_t *inode_bmp = (uint8_t*)g_api->kmalloc(block_size);
    if (!blk || !block_bmp || !inode_bmp) {
        if (blk) g_api->kfree(blk);
        if (block_bmp) g_api->kfree(block_bmp);
        if (inode_bmp) g_api->kfree(inode_bmp);
        return -7;
    }

    // Zero blocks up to lost+found
    m_memset(blk, 0, block_size);
    for (uint32_t b = 0; b < first_data_blockno + 2; b++) {
        uint64_t lba = (uint64_t)partition_lba + (uint64_t)b * sectors_per_block;
        if (g_api->block_write(bdev, lba, sectors_per_block, blk, block_size) != 0) {
            g_api->kfree(blk); g_api->kfree(block_bmp); g_api->kfree(inode_bmp);
            return -8;
        }
    }

    // Superblock
    ext2_superblock_t sb;
    m_memset(&sb, 0, sizeof(sb));
    sb.s_inodes_count = inodes_per_group;
    sb.s_blocks_count = total_blocks;
    sb.s_r_blocks_count = 0;
    sb.s_free_inodes_count = (inodes_per_group > 11) ? (inodes_per_group - 11) : 0;
    sb.s_first_data_block = 0;
    sb.s_log_block_size = 2;
    sb.s_log_frag_size = 2;
    sb.s_blocks_per_group = total_blocks;
    sb.s_frags_per_group = total_blocks;
    sb.s_inodes_per_group = inodes_per_group;
    sb.s_magic = EXT2_MAGIC;
    sb.s_state = 1;
    sb.s_errors = 1;
    sb.s_creator_os = 0;
    sb.s_rev_level = 1;
    sb.s_minor_rev_level = 0;
    sb.s_mnt_count = 0;
    sb.s_first_ino = 11;
    sb.s_inode_size = inode_size;
    sb.s_max_mnt_count = -1;
    sb.s_def_resuid = 0;
    sb.s_def_resgid = 0;

    // uuid
    for (int i = 0; i < 16; i++) sb.s_uuid[i] = (uint8_t)(0xA5u ^ (uint8_t)(i * 17u));
    sb.s_uuid[0] ^= (uint8_t)(vdrive_id & 0xFF);

    if (volume_label) {
        m_strncpy(sb.s_volume_name, volume_label, sizeof(sb.s_volume_name));
    }

    // Bitmaps
    m_memset(block_bmp, 0, block_size);
    m_memset(inode_bmp, 0, block_size);

    // Used blocks: 0..lostfound
    for (uint32_t b = 0; b <= lostfound_blockno; b++) {
        set_bit(block_bmp, b);
    }
    // Padding bits
    for (uint32_t bit = total_blocks; bit < block_size * 8u; bit++) set_bit(block_bmp, bit);

    // Used inodes: 1..11
    for (uint32_t ino = 1; ino <= 11; ino++) set_bit(inode_bmp, ino - 1);
    for (uint32_t bit = inodes_per_group; bit < block_size * 8u; bit++) set_bit(inode_bmp, bit);

    uint32_t used_in_group = lostfound_blockno + 1;
    uint32_t free_blocks = (total_blocks > used_in_group) ? (total_blocks - used_in_group) : 0;
    sb.s_free_blocks_count = free_blocks;

    // BGDT
    ext2_bgdt_t bg;
    m_memset(&bg, 0, sizeof(bg));
    bg.bg_block_bitmap = block_bmp_blockno;
    bg.bg_inode_bitmap = inode_bmp_blockno;
    bg.bg_inode_table = inode_table_blockno;
    bg.bg_free_blocks_count = (uint16_t)free_blocks;
    bg.bg_free_inodes_count = (uint16_t)sb.s_free_inodes_count;
    bg.bg_used_dirs_count = 2;

    // Write SB into block 0 at offset 1024
    m_memset(blk, 0, block_size);
    m_memcpy(blk + EXT2_SUPERBLOCK_OFF, &sb, sizeof(sb));
    if (g_api->block_write(bdev, (uint64_t)partition_lba + (uint64_t)super_blockno * sectors_per_block, sectors_per_block, blk, block_size) != 0) {
        g_api->kfree(blk); g_api->kfree(block_bmp); g_api->kfree(inode_bmp);
        return -9;
    }

    // BGDT
    m_memset(blk, 0, block_size);
    m_memcpy(blk, &bg, sizeof(bg));
    if (g_api->block_write(bdev, (uint64_t)partition_lba + (uint64_t)bgdt_blockno * sectors_per_block, sectors_per_block, blk, block_size) != 0) {
        g_api->kfree(blk); g_api->kfree(block_bmp); g_api->kfree(inode_bmp);
        return -10;
    }

    // block bitmap
    if (g_api->block_write(bdev, (uint64_t)partition_lba + (uint64_t)block_bmp_blockno * sectors_per_block, sectors_per_block, block_bmp, block_size) != 0) {
        g_api->kfree(blk); g_api->kfree(block_bmp); g_api->kfree(inode_bmp);
        return -11;
    }

    // inode bitmap
    if (g_api->block_write(bdev, (uint64_t)partition_lba + (uint64_t)inode_bmp_blockno * sectors_per_block, sectors_per_block, inode_bmp, block_size) != 0) {
        g_api->kfree(blk); g_api->kfree(block_bmp); g_api->kfree(inode_bmp);
        return -12;
    }

    // Root inode (#2) and lost+found inode (#11)
    ext2_inode_t root;
    m_memset(&root, 0, sizeof(root));
    root.i_mode = (uint16_t)(0x4000 | 0755);
    root.i_size = block_size;
    root.i_links_count = 3;
    root.i_blocks = sectors_per_block;
    root.i_block[0] = root_dir_blockno;

    ext2_inode_t lf;
    m_memset(&lf, 0, sizeof(lf));
    lf.i_mode = (uint16_t)(0x4000 | 0700);
    lf.i_size = block_size;
    lf.i_links_count = 2;
    lf.i_blocks = sectors_per_block;
    lf.i_block[0] = lostfound_blockno;

    m_memset(blk, 0, block_size);
    m_memcpy(blk + inode_size * 1, &root, sizeof(root));
    m_memcpy(blk + inode_size * 10, &lf, sizeof(lf));
    if (g_api->block_write(bdev, (uint64_t)partition_lba + (uint64_t)inode_table_blockno * sectors_per_block, sectors_per_block, blk, block_size) != 0) {
        g_api->kfree(blk); g_api->kfree(block_bmp); g_api->kfree(inode_bmp);
        return -13;
    }

    // Root directory block
    m_memset(blk, 0, block_size);
    ext2_dirent_t *de1 = (ext2_dirent_t*)blk;
    de1->inode = 2;
    de1->name_len = 1;
    de1->file_type = 2;
    de1->rec_len = 12;
    ((char*)blk)[8] = '.';

    ext2_dirent_t *de2 = (ext2_dirent_t*)(blk + de1->rec_len);
    de2->inode = 2;
    de2->name_len = 2;
    de2->file_type = 2;
    de2->rec_len = 12;
    ((char*)blk)[de1->rec_len + 8] = '.';
    ((char*)blk)[de1->rec_len + 9] = '.';

    ext2_dirent_t *de3 = (ext2_dirent_t*)(blk + de1->rec_len + de2->rec_len);
    de3->inode = 11;
    de3->name_len = 10;
    de3->file_type = 2;
    de3->rec_len = (uint16_t)(block_size - (de1->rec_len + de2->rec_len));
    m_memcpy((uint8_t*)de3 + 8, "lost+found", 10);

    if (g_api->block_write(bdev, (uint64_t)partition_lba + (uint64_t)root_dir_blockno * sectors_per_block, sectors_per_block, blk, block_size) != 0) {
        g_api->kfree(blk); g_api->kfree(block_bmp); g_api->kfree(inode_bmp);
        return -14;
    }

    // lost+found directory block
    m_memset(blk, 0, block_size);
    ext2_dirent_t *lf1 = (ext2_dirent_t*)blk;
    lf1->inode = 11;
    lf1->name_len = 1;
    lf1->file_type = 2;
    lf1->rec_len = 12;
    ((char*)blk)[8] = '.';

    ext2_dirent_t *lf2 = (ext2_dirent_t*)(blk + lf1->rec_len);
    lf2->inode = 2;
    lf2->name_len = 2;
    lf2->file_type = 2;
    lf2->rec_len = (uint16_t)(block_size - lf1->rec_len);
    ((char*)blk)[lf1->rec_len + 8] = '.';
    ((char*)blk)[lf1->rec_len + 9] = '.';

    if (g_api->block_write(bdev, (uint64_t)partition_lba + (uint64_t)lostfound_blockno * sectors_per_block, sectors_per_block, blk, block_size) != 0) {
        g_api->kfree(blk); g_api->kfree(block_bmp); g_api->kfree(inode_bmp);
        return -15;
    }

    g_api->kfree(blk);
    g_api->kfree(block_bmp);
    g_api->kfree(inode_bmp);
    return 0;
}

static const fs_ext_driver_ops_t g_ext2_ops = {
    .probe = ext2_probe,
    .mount = ext2_mount,
    .unmount = ext2_unmount,
    .mkfs = ext2_mkfs,
    .read_file = ext2_read_file,
    .stat = ext2_stat,
    .file_exists = ext2_file_exists,
    .directory_exists = ext2_dir_exists,
    .list_directory = ext2_list_directory,
    .opendir = ext2_opendir,
    .readdir = ext2_readdir,
    .closedir = ext2_closedir,
};

static void u32_to_dec(char *out, size_t out_sz, uint32_t v) {
    if (!out || out_sz == 0) return;
    char tmp[16];
    size_t n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v && n < sizeof(tmp));

    size_t pos = 0;
    while (n && pos + 1 < out_sz) {
        out[pos++] = tmp[--n];
    }
    out[pos] = 0;
}

static void i32_to_dec(char *out, size_t out_sz, int32_t v) {
    if (!out || out_sz == 0) return;
    if (v < 0) {
        if (out_sz < 2) { out[0] = 0; return; }
        out[0] = '-';
        u32_to_dec(out + 1, out_sz - 1, (uint32_t)(-v));
    } else {
        u32_to_dec(out, out_sz, (uint32_t)v);
    }
}

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    g_api = api;
    if (!api || api->abi_version != 1) return -1;
    if (!api->fs_register_driver) return -2;
    if (!api->block_get_handle_for_vdrive || !api->block_read || !api->block_get_info) return -3;

    int r = api->fs_register_driver("ext2", &g_ext2_ops);
    if (api->com_write_string) {
        api->com_write_string(COM1_PORT, "[SQRM-EXT2] registered ext2 driver rc=");
        char buf[16];
        i32_to_dec(buf, sizeof(buf), r);
        api->com_write_string(COM1_PORT, buf);
        api->com_write_string(COM1_PORT, "\n");
    }
    return r;
}
