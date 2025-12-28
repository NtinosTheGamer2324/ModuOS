// fat16_sqrm.c - FAT16 SQRM filesystem driver (probe/mount/dir/file + mkfs)
#include "moduos/kernel/sqrm.h"
#include "moduos/fs/fs.h"

// NOTE: SQRM modules are built -nostdlib; provide minimal local helpers.
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
static int m_memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *aa = (const uint8_t*)a;
    const uint8_t *bb = (const uint8_t*)b;
    for (size_t i = 0; i < n; i++) {
        if (aa[i] != bb[i]) return (int)aa[i] - (int)bb[i];
    }
    return 0;
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
static char up(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    return c;
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
static uint32_t div_ceil_u32(uint32_t a, uint32_t b) {
    return (a + b - 1) / b;
}

static const sqrm_kernel_api_t *g_api;

// FAT BPB (FAT16)
typedef struct __attribute__((packed)) {
    uint8_t jmp[3];
    char oem[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t media;
    uint16_t sectors_per_fat_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;

    // FAT16 extended
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_sig;
    uint32_t volume_id;
    char volume_label[11];
    char fs_type[8];
} fat16_bpb_t;

typedef struct __attribute__((packed)) {
    uint8_t name[11];
    uint8_t attr;
    uint8_t nt_reserved;
    uint8_t create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t last_access_date;
    uint16_t first_cluster_high; // unused in FAT16
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_low;
    uint32_t filesize;
} fat_dirent_t;

#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LONG_NAME 0x0F

#define FAT16_EOC_MIN 0xFFF8u

typedef struct {
    blockdev_handle_t bdev;
    uint64_t part_lba;
    blockdev_info_t info;

    fat16_bpb_t bpb;
    uint32_t total_sectors;

    uint32_t fat_start_lba;
    uint32_t root_start_lba;
    uint32_t root_sectors;
    uint32_t data_start_lba;

    uint32_t bytes_per_cluster;
    uint32_t cluster_count;
} fat16_mount_ctx_t;

static int read512(fat16_mount_ctx_t *m, uint32_t lba_rel, void *buf512) {
    return g_api->block_read(m->bdev, m->part_lba + lba_rel, 1, buf512, 512);
}
static int write512(fat16_mount_ctx_t *m, uint32_t lba_rel, const void *buf512) {
    return g_api->block_write(m->bdev, m->part_lba + lba_rel, 1, buf512, 512);
}

static uint32_t fat_total_sectors(const fat16_bpb_t *bpb) {
    return bpb->total_sectors_16 ? bpb->total_sectors_16 : bpb->total_sectors_32;
}

static void fat16_compute_layout(fat16_mount_ctx_t *m) {
    const fat16_bpb_t *b = &m->bpb;
    m->total_sectors = fat_total_sectors(b);
    m->fat_start_lba = b->reserved_sectors;
    m->root_sectors = div_ceil_u32((uint32_t)b->root_entry_count * 32u, 512u);
    m->root_start_lba = m->fat_start_lba + (uint32_t)b->num_fats * (uint32_t)b->sectors_per_fat_16;
    m->data_start_lba = m->root_start_lba + m->root_sectors;
    m->bytes_per_cluster = (uint32_t)b->sectors_per_cluster * 512u;

    uint32_t data_sectors = (m->total_sectors > m->data_start_lba) ? (m->total_sectors - m->data_start_lba) : 0;
    m->cluster_count = (b->sectors_per_cluster ? (data_sectors / b->sectors_per_cluster) : 0);
}

static int fat16_read_bpb(int vdrive_id, uint32_t partition_lba, fat16_mount_ctx_t *m) {
    blockdev_handle_t bdev = BLOCKDEV_INVALID_HANDLE;
    if (g_api->block_get_handle_for_vdrive(vdrive_id, &bdev) != 0) return -1;
    m->bdev = bdev;
    m->part_lba = partition_lba;

    if (g_api->block_get_info && g_api->block_get_info(bdev, &m->info) != 0) return -2;
    if (m->info.sector_size != 512) return -3;

    uint8_t sec[512];
    if (read512(m, 0, sec) != 0) return -4;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return -5;

    m_memcpy(&m->bpb, sec, sizeof(fat16_bpb_t));

    if (m->bpb.bytes_per_sector != 512) return -6;
    if (m->bpb.sectors_per_cluster == 0) return -7;
    if (m->bpb.reserved_sectors == 0) return -8;
    if (m->bpb.num_fats == 0) return -9;
    if (m->bpb.root_entry_count == 0) return -10;
    if (m->bpb.sectors_per_fat_16 == 0) return -11;

    fat16_compute_layout(m);

    // FAT16 identification: cluster_count in [4085, 65524]
    if (m->cluster_count < 4085 || m->cluster_count >= 65525) return -12;

    return 0;
}

static uint16_t fat16_get_fat_entry(fat16_mount_ctx_t *m, uint16_t cluster) {
    uint32_t off = (uint32_t)cluster * 2u;
    uint32_t sec_index = off / 512u;
    uint32_t sec_off = off % 512u;

    uint8_t sec[512];
    if (read512(m, m->fat_start_lba + sec_index, sec) != 0) return 0xFFFFu;
    return (uint16_t)(sec[sec_off] | ((uint16_t)sec[sec_off + 1] << 8));
}

static uint32_t fat16_cluster_to_lba(fat16_mount_ctx_t *m, uint16_t cluster) {
    return m->data_start_lba + (uint32_t)(cluster - 2u) * m->bpb.sectors_per_cluster;
}

static int fat16_make_83(const char *seg, uint8_t out11[11]) {
    for (int i = 0; i < 11; i++) out11[i] = ' ';
    if (!seg || !seg[0]) return -1;

    int name_i = 0;
    int ext_i = 0;
    int in_ext = 0;

    for (const char *p = seg; *p; p++) {
        char c = *p;
        if (c == '/') break;
        if (c == '.') { in_ext = 1; continue; }
        c = up(c);
        if (!in_ext) {
            if (name_i >= 8) return -2;
            out11[name_i++] = (uint8_t)c;
        } else {
            if (ext_i >= 3) return -3;
            out11[8 + ext_i++] = (uint8_t)c;
        }
    }

    return 0;
}

static void fat16_entry_to_name(const fat_dirent_t *e, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = 0;

    size_t pos = 0;
    for (int i = 0; i < 8 && pos + 1 < out_sz; i++) {
        if (e->name[i] == ' ') break;
        out[pos++] = (char)e->name[i];
    }

    int has_ext = 0;
    for (int i = 0; i < 3; i++) {
        if (e->name[8 + i] != ' ') { has_ext = 1; break; }
    }

    if (has_ext && pos + 1 < out_sz) out[pos++] = '.';

    for (int i = 0; i < 3 && pos + 1 < out_sz; i++) {
        if (e->name[8 + i] == ' ') break;
        out[pos++] = (char)e->name[8 + i];
    }

    out[pos] = 0;
}

static int fat16_read_dir_root(fat16_mount_ctx_t *m, uint32_t index, fat_dirent_t *out) {
    uint32_t byte_off = index * 32u;
    uint32_t sec = byte_off / 512u;
    uint32_t off = byte_off % 512u;
    if (sec >= m->root_sectors) return 0;

    uint8_t buf[512];
    if (read512(m, m->root_start_lba + sec, buf) != 0) return -1;
    m_memcpy(out, buf + off, sizeof(*out));
    return 1;
}

static int fat16_read_dir_cluster(fat16_mount_ctx_t *m, uint16_t start_cluster, uint32_t index, fat_dirent_t *out) {
    if (start_cluster < 2) return 0;

    uint32_t entries_per_cluster = m->bytes_per_cluster / 32u;
    uint32_t cluster_index = index / entries_per_cluster;
    uint32_t entry_in_cluster = index % entries_per_cluster;

    uint16_t cl = start_cluster;
    for (uint32_t i = 0; i < cluster_index; i++) {
        uint16_t nxt = fat16_get_fat_entry(m, cl);
        if (nxt >= FAT16_EOC_MIN || nxt < 2) return 0;
        cl = nxt;
    }

    uint32_t byte_off = entry_in_cluster * 32u;
    uint32_t sec_in_cluster = byte_off / 512u;
    uint32_t off = byte_off % 512u;

    uint32_t lba = fat16_cluster_to_lba(m, cl) + sec_in_cluster;
    uint8_t buf[512];
    if (read512(m, lba, buf) != 0) return -1;
    m_memcpy(out, buf + off, sizeof(*out));
    return 1;
}

static int fat16_find_in_dir(fat16_mount_ctx_t *m, uint16_t dir_cluster /*0=root*/, const char *name, fat_dirent_t *out) {
    uint8_t want[11];
    if (fat16_make_83(name, want) != 0) return 0;

    for (uint32_t idx = 0;; idx++) {
        fat_dirent_t e;
        int rr = (dir_cluster == 0) ? fat16_read_dir_root(m, idx, &e) : fat16_read_dir_cluster(m, dir_cluster, idx, &e);
        if (rr == 0) return 0;
        if (rr < 0) return 0;

        if (e.name[0] == 0x00) return 0;
        if (e.name[0] == 0xE5) continue;
        if (e.attr == ATTR_LONG_NAME) continue;
        if (e.attr & ATTR_VOLUME_ID) continue;

        if (m_memcmp(e.name, want, 11) == 0) {
            if (out) *out = e;
            return 1;
        }
    }
}

static int fat16_walk_path(fat16_mount_ctx_t *m, const char *path, fat_dirent_t *out, int *out_is_dir) {
    if (!path || !path[0] || (path[0] == '/' && path[1] == 0)) {
        if (out) m_memset(out, 0, sizeof(*out));
        if (out_is_dir) *out_is_dir = 1;
        return 1;
    }

    const char *p = path;
    while (*p == '/') p++;

    uint16_t dir_cluster = 0;

    char seg[64];
    while (*p) {
        size_t si = 0;
        while (*p && *p != '/') {
            if (si + 1 < sizeof(seg)) seg[si++] = *p;
            p++;
        }
        seg[si] = 0;
        while (*p == '/') p++;

        if (seg[0] == 0) continue;

        fat_dirent_t e;
        if (!fat16_find_in_dir(m, dir_cluster, seg, &e)) return 0;

        int is_dir = (e.attr & ATTR_DIRECTORY) ? 1 : 0;

        if (*p == 0) {
            if (out) *out = e;
            if (out_is_dir) *out_is_dir = is_dir;
            return 1;
        }

        if (!is_dir) return 0;
        dir_cluster = e.first_cluster_low;
        if (dir_cluster < 2) return 0;
    }

    return 0;
}

static int fat16_read_file(fs_mount_t *mount, const char *path, void *out, size_t out_sz, size_t *out_read) {
    if (out_read) *out_read = 0;
    if (!mount || !mount->ext_ctx || !path || !out) return -1;
    fat16_mount_ctx_t *m = (fat16_mount_ctx_t*)mount->ext_ctx;

    fat_dirent_t e;
    int is_dir = 0;
    if (!fat16_walk_path(m, path, &e, &is_dir) || is_dir) return -2;

    uint32_t to_read = e.filesize;
    if (to_read > out_sz) to_read = (uint32_t)out_sz;

    uint16_t cl = e.first_cluster_low;
    uint32_t pos = 0;

    uint8_t sec[512];
    while (cl >= 2 && cl < FAT16_EOC_MIN && pos < to_read) {
        uint32_t lba = fat16_cluster_to_lba(m, cl);
        for (uint32_t s = 0; s < m->bpb.sectors_per_cluster && pos < to_read; s++) {
            if (read512(m, lba + s, sec) != 0) return -3;
            uint32_t chunk = to_read - pos;
            if (chunk > 512) chunk = 512;
            m_memcpy((uint8_t*)out + pos, sec, chunk);
            pos += chunk;
        }
        uint16_t nxt = fat16_get_fat_entry(m, cl);
        if (nxt >= FAT16_EOC_MIN) break;
        cl = nxt;
    }

    if (out_read) *out_read = pos;
    return 0;
}

static int fat16_stat(fs_mount_t *mount, const char *path, fs_file_info_t *info) {
    if (!mount || !mount->ext_ctx || !path || !info) return -1;
    fat16_mount_ctx_t *m = (fat16_mount_ctx_t*)mount->ext_ctx;
    m_memset(info, 0, sizeof(*info));

    if (!path[0] || (path[0] == '/' && path[1] == 0)) {
        info->is_directory = 1;
        info->size = 0;
        return 0;
    }

    fat_dirent_t e;
    int is_dir = 0;
    if (!fat16_walk_path(m, path, &e, &is_dir)) return -2;

    info->is_directory = is_dir;
    info->size = e.filesize;
    return 0;
}

static int fat16_file_exists(fs_mount_t *mount, const char *path) {
    fs_file_info_t info;
    if (fat16_stat(mount, path, &info) != 0) return 0;
    return info.is_directory ? 0 : 1;
}

static int fat16_dir_exists(fs_mount_t *mount, const char *path) {
    fs_file_info_t info;
    if (fat16_stat(mount, path, &info) != 0) return 0;
    return info.is_directory ? 1 : 0;
}

typedef struct {
    fat16_mount_ctx_t *m;
    uint16_t dir_cluster; // 0=root
    uint32_t idx;
} fat16_dir_iter_t;

static fs_dir_t* fat16_opendir(fs_mount_t *mount, const char *path) {
    if (!mount || !mount->ext_ctx) return NULL;
    fat16_mount_ctx_t *m = (fat16_mount_ctx_t*)mount->ext_ctx;

    uint16_t dir_cluster = 0;
    if (path && path[0] && !(path[0] == '/' && path[1] == 0)) {
        fat_dirent_t e;
        int is_dir = 0;
        if (!fat16_walk_path(m, path, &e, &is_dir) || !is_dir) return NULL;
        dir_cluster = e.first_cluster_low;
        if (dir_cluster < 2) return NULL;
    }

    fat16_dir_iter_t *it = (fat16_dir_iter_t*)g_api->kmalloc(sizeof(*it));
    if (!it) return NULL;
    it->m = m;
    it->dir_cluster = dir_cluster;
    it->idx = 0;
    return (fs_dir_t*)it;
}

static int fat16_readdir(fs_dir_t *dir, fs_dirent_t *entry) {
    if (!dir || !entry) return -1;
    fat16_dir_iter_t *it = (fat16_dir_iter_t*)dir;

    while (1) {
        fat_dirent_t e;
        int rr = (it->dir_cluster == 0) ? fat16_read_dir_root(it->m, it->idx, &e)
                                        : fat16_read_dir_cluster(it->m, it->dir_cluster, it->idx, &e);
        it->idx++;

        if (rr == 0) return 0;
        if (rr < 0) return -2;

        if (e.name[0] == 0x00) return 0;
        if (e.name[0] == 0xE5) continue;
        if (e.attr == ATTR_LONG_NAME) continue;
        if (e.attr & ATTR_VOLUME_ID) continue;

        m_memset(entry, 0, sizeof(*entry));
        fat16_entry_to_name(&e, entry->name, sizeof(entry->name));
        entry->is_directory = (e.attr & ATTR_DIRECTORY) ? 1 : 0;
        entry->size = e.filesize;
        return 1;
    }
}

static void fat16_closedir(fs_dir_t *dir) {
    if (!dir || !g_api) return;
    g_api->kfree(dir);
}

static void fat16_unmount(fs_mount_t *mount) {
    if (!mount || !g_api) return;
    if (mount->ext_ctx) g_api->kfree(mount->ext_ctx);
    mount->ext_ctx = NULL;
}

static int fat16_mount(int vdrive_id, uint32_t partition_lba, fs_mount_t *mount) {
    if (!g_api || !mount) return -1;

    fat16_mount_ctx_t *m = (fat16_mount_ctx_t*)g_api->kmalloc(sizeof(*m));
    if (!m) return -2;
    m_memset(m, 0, sizeof(*m));

    int rc = fat16_read_bpb(vdrive_id, partition_lba, m);
    if (rc != 0) {
        g_api->kfree(m);
        return -3;
    }

    mount->ext_ctx = m;
    return 0;
}

static int fat16_probe(int vdrive_id, uint32_t partition_lba) {
    fat16_mount_ctx_t tmp;
    m_memset(&tmp, 0, sizeof(tmp));
    return (fat16_read_bpb(vdrive_id, partition_lba, &tmp) == 0) ? 1 : 0;
}

// --- mkfs (format) ---

static uint32_t pick_spc_fat16(uint32_t sectors) {
    // heuristic: try to keep cluster count <= 65524
    if (sectors <= 131072u) return 1; // <= 64MiB
    if (sectors <= 524288u) return 4; // <= 256MiB (2KiB clusters)
    return 8; // default 4KiB clusters
}

static int fat16_mkfs(int vdrive_id, uint32_t partition_lba, uint32_t partition_sectors, const char *label) {
    if (!g_api || !g_api->block_get_handle_for_vdrive || !g_api->block_write || !g_api->block_read) return -1;

    blockdev_handle_t bdev = BLOCKDEV_INVALID_HANDLE;
    if (g_api->block_get_handle_for_vdrive(vdrive_id, &bdev) != 0) return -2;

    blockdev_info_t info;
    if (g_api->block_get_info && g_api->block_get_info(bdev, &info) == 0) {
        if (info.sector_size != 512) return -3;
    }

    if (partition_sectors < 2048u) return -4;

    uint8_t spc = (uint8_t)pick_spc_fat16(partition_sectors);
    uint16_t reserved = 1;
    uint8_t fats = 2;
    uint16_t root_entries = 512;
    uint32_t root_sectors = div_ceil_u32((uint32_t)root_entries * 32u, 512u);

    // compute sectors_per_fat iteratively
    uint16_t spf = 1;
    for (int iter = 0; iter < 32; iter++) {
        uint32_t data_sectors = partition_sectors - (uint32_t)reserved - (uint32_t)fats * (uint32_t)spf - root_sectors;
        uint32_t clusters = data_sectors / spc;
        uint32_t fat_entries = clusters + 2;
        uint32_t fat_bytes = fat_entries * 2u;
        uint16_t new_spf = (uint16_t)div_ceil_u32(fat_bytes, 512u);
        if (new_spf == spf) break;
        spf = new_spf;
    }

    uint32_t data_sectors = partition_sectors - (uint32_t)reserved - (uint32_t)fats * (uint32_t)spf - root_sectors;
    uint32_t clusters = data_sectors / spc;
    if (clusters < 4085 || clusters >= 65525) return -5;

    // Write boot sector
    uint8_t sec[512];
    m_memset(sec, 0, sizeof(sec));

    fat16_bpb_t bpb;
    m_memset(&bpb, 0, sizeof(bpb));
    bpb.jmp[0] = 0xEB; bpb.jmp[1] = 0x3C; bpb.jmp[2] = 0x90;
    m_memcpy(bpb.oem, "MSDOS5.0", 8);
    bpb.bytes_per_sector = 512;
    bpb.sectors_per_cluster = spc;
    bpb.reserved_sectors = reserved;
    bpb.num_fats = fats;
    bpb.root_entry_count = root_entries;
    bpb.total_sectors_16 = (partition_sectors <= 0xFFFFu) ? (uint16_t)partition_sectors : 0;
    bpb.total_sectors_32 = (partition_sectors > 0xFFFFu) ? partition_sectors : 0;
    bpb.media = 0xF8;
    bpb.sectors_per_fat_16 = spf;
    bpb.sectors_per_track = 63;
    bpb.num_heads = 255;
    bpb.hidden_sectors = partition_lba;
    bpb.drive_number = 0x80;
    bpb.boot_sig = 0x29;
    bpb.volume_id = 0x12345678;
    m_memcpy(bpb.fs_type, "FAT16   ", 8);

    // label
    for (int i = 0; i < 11; i++) bpb.volume_label[i] = ' ';
    if (label && label[0]) {
        int li = 0;
        while (label[li] && li < 11) { bpb.volume_label[li] = (char)up(label[li]); li++; }
    } else {
        m_memcpy(bpb.volume_label, "NO NAME    ", 11);
    }

    m_memcpy(sec, &bpb, sizeof(bpb));
    sec[510] = 0x55;
    sec[511] = 0xAA;

    if (g_api->block_write(bdev, (uint64_t)partition_lba, 1, sec, 512) != 0) return -6;

    // Zero FATs + root dir
    uint8_t zero[512];
    m_memset(zero, 0, sizeof(zero));

    uint32_t fat0_lba = partition_lba + reserved;
    for (uint32_t fi = 0; fi < fats; fi++) {
        for (uint32_t s = 0; s < spf; s++) {
            if (g_api->block_write(bdev, (uint64_t)fat0_lba + (uint64_t)fi * spf + s, 1, zero, 512) != 0) return -7;
        }
    }

    uint32_t root_lba = fat0_lba + (uint32_t)fats * spf;
    for (uint32_t s = 0; s < root_sectors; s++) {
        if (g_api->block_write(bdev, (uint64_t)root_lba + s, 1, zero, 512) != 0) return -8;
    }

    // Initialize FAT[0] and FAT[1]
    // FAT[0] = media | 0xFF00, FAT[1] = 0xFFFF
    uint8_t fatsec[512];
    m_memset(fatsec, 0, sizeof(fatsec));
    fatsec[0] = 0xF8;
    fatsec[1] = 0xFF;
    fatsec[2] = 0xFF;
    fatsec[3] = 0xFF;

    for (uint32_t fi = 0; fi < fats; fi++) {
        if (g_api->block_write(bdev, (uint64_t)fat0_lba + (uint64_t)fi * spf, 1, fatsec, 512) != 0) return -9;
    }

    // Root dir volume label entry (optional)
    if (label && label[0]) {
        fat_dirent_t ve;
        m_memset(&ve, 0, sizeof(ve));
        m_memcpy(ve.name, bpb.volume_label, 11);
        ve.attr = ATTR_VOLUME_ID;
        uint8_t rsec[512];
        if (g_api->block_read(bdev, (uint64_t)root_lba, 1, rsec, 512) == 0) {
            m_memcpy(rsec, &ve, sizeof(ve));
            (void)g_api->block_write(bdev, (uint64_t)root_lba, 1, rsec, 512);
        }
    }

    return 0;
}

static const fs_ext_driver_ops_t g_fat16_ops = {
    .probe = fat16_probe,
    .mount = fat16_mount,
    .unmount = fat16_unmount,
    .mkfs = fat16_mkfs,
    .read_file = fat16_read_file,
    .stat = fat16_stat,
    .file_exists = fat16_file_exists,
    .directory_exists = fat16_dir_exists,
    .list_directory = NULL,
    .opendir = fat16_opendir,
    .readdir = fat16_readdir,
    .closedir = fat16_closedir,
};

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    g_api = api;
    if (!api || api->abi_version != 1) return -1;
    if (!api->fs_register_driver) return -2;
    if (!api->block_get_handle_for_vdrive || !api->block_read || !api->block_write) return -3;
    return api->fs_register_driver("fat16", &g_fat16_ops);
}
