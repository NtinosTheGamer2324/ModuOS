/*
 * pakman.c  (ModuOS Package Manager)
 * Reads the MPK archive format (magic 0x214B504D "MPK!").
 *
 * MPK format (raw, no compression):
 *   Global header (11 bytes):
 *     [4]  magic       0x214B504D  ("MPK!")
 *     [1]  version     0x01
 *     [4]  entry_count uint32 LE
 *     [2]  reserved    0x0000
 *   Per entry:
 *     [1]  type        0x00 = file, 0x01 = directory
 *     [2]  path_len    uint16 LE, byte count, no NUL
 *     [N]  path        UTF-8, forward slashes, no leading slash, no NUL
 *     [4]  data_len    uint32 LE (0 for directories)
 *     [M]  data        file bytes (absent for directories)
 */

#include "libc.h"
#include "string.h"

/* =========================================================================
 * §0  FORWARD DECLARATIONS & SHARED MACROS
 * ========================================================================= */

#define MAX_PATH        512
#define COPY_BUF_SZ     4096
#define INI_LINE_MAX    256

/* string helpers */
static int    pm_streq(const char *a, const char *b);
static void   pm_strlcpy(char *dst, const char *src, size_t n);
static void   pm_strlcat(char *dst, const char *src, size_t n);
static char  *pm_strchr(const char *s, int c);
static void   pm_trim(char *s);

/* path helpers */
static void   path_join(char *out, size_t out_sz, const char *base, const char *rel);
static void   pakman_db_dir(char *out, size_t out_sz, const char *sysroot);
static void   pakman_tmp_dir(char *out, size_t out_sz, const char *sysroot);

/* file / directory helpers */
static char  *read_file_alloc(const char *path, size_t *out_len);
static int    write_file(const char *path, const char *data);
static int    ensure_dir(const char *path);
static int    mkdir_p(const char *path);
static int    remove_tree(const char *path);
static int    copy_file(const char *src, const char *dst);
static int    copy_tree(const char *src_dir, const char *dst_dir);

/* progress bar */
static void   progress_bar(const char *label, uint64_t done, uint64_t total);
static void   progress_bar_done(const char *label, uint64_t total);

/* hex printing (printf has no %%02X/%%08X support) */
static void   pm_puthex8(uint8_t v);
static void   pm_puthex32(uint32_t v);
static void   pm_putpad(const char *s, int width);

/* =========================================================================
 * §1  MPK ARCHIVE READER
 * ========================================================================= */

#define MPK_MAGIC        0x214B504Du   /* "MPK!" little-endian               */
#define MPK_VERSION      0x01
#define MPK_HDR_SIZE     11            /* 4 magic + 1 version + 4 count + 2 reserved */
#define MPK_TYPE_FILE    0x00
#define MPK_TYPE_DIR     0x01
#define MPK_MAX_PATH     4096
#define MPK_MAX_ENTRIES  65535

static uint16_t mpk_u16(const uint8_t *p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}
static uint32_t mpk_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Validate the MPK header.  Returns entry_count on success, -1 on error. */
static int mpk_check_header(const uint8_t *data, size_t size) {
    if (size < MPK_HDR_SIZE) {
        printf("pakman: file too small to be a .pak archive\n");
        return -1;
    }
    uint32_t magic = mpk_u32(data);
    if (magic != MPK_MAGIC) {
        printf("pakman: not an MPK archive (bad magic 0x"); pm_puthex32(magic); printf(")\n");
        return -1;
    }
    if (data[4] != MPK_VERSION) {
        printf("pakman: unsupported MPK version %d\n", (int)data[4]);
        return -1;
    }
    uint32_t count = mpk_u32(data + 5);
    if (count > MPK_MAX_ENTRIES) {
        printf("pakman: entry count %u exceeds limit\n", count);
        return -1;
    }
    /* bytes 9-10 are reserved, ignore them */
    return (int)count;
}

/* Count file entries (not dirs) by scanning the archive — used for the
 * progress bar before the real extraction pass. */
static size_t mpk_count_files(const uint8_t *data, size_t size,
                               uint32_t entry_count) {
    size_t pos   = MPK_HDR_SIZE;
    size_t files = 0;

    for (uint32_t i = 0; i < entry_count; i++) {
        if (pos + 7 > size) break;                   /* 1+2+4 minimum */
        uint8_t  type     = data[pos];
        uint16_t path_len = mpk_u16(data + pos + 1);
        uint32_t data_len = mpk_u32(data + pos + 3); /* data_len comes BEFORE path */
        pos += 7;                                     /* type+path_len+data_len */
        if (path_len == 0 || path_len > MPK_MAX_PATH) break;
        if (pos + path_len > size) break;
        pos += path_len;
        if (type == MPK_TYPE_FILE) files++;
        if (data_len > size - pos) break;
        pos += data_len;
    }
    return files;
}

/* Extract an MPK archive rooted at data/size into tmp_dir. */
static int mpk_extract(const uint8_t *data, size_t size,
                       const char *tmp_dir, uint32_t entry_count) {
    size_t total_files = mpk_count_files(data, size, entry_count);
    size_t done_files  = 0;

    printf("pakman: extracting %u file(s) from %u-byte archive\n",
           (uint32_t)total_files, (uint32_t)size);

    if (total_files > 0)
        progress_bar("Extracting", 0, total_files);

    size_t pos = MPK_HDR_SIZE;

    /* Dump first 32 bytes of archive for sanity check */
    printf("pakman: hdr bytes:");
    for (int _d = 0; _d < 32 && _d < (int)size; _d++) {
        printf(" "); pm_puthex8(data[_d]);
    }
    printf("\n");
    /* Dump first entry raw bytes */
    printf("pakman: entry0 raw:");
    for (int _d = MPK_HDR_SIZE; _d < MPK_HDR_SIZE + 16 && _d < (int)size; _d++) {
        printf(" "); pm_puthex8(data[_d]);
    }
    printf("\n");

    for (uint32_t i = 0; i < entry_count; i++) {
        /* --- read entry header --- */
        if (pos + 3 > size) {
            printf("\npakman: truncated archive at entry %u (pos=%u size=%u)\n",
                   i, (uint32_t)pos, (uint32_t)size);
            return -1;
        }
        uint8_t  type     = data[pos];
        uint16_t path_len = mpk_u16(data + pos + 1);
        uint32_t data_len = mpk_u32(data + pos + 3); /* data_len is BEFORE path in MPK format */

        /* Guard: need 7 bytes for the fixed header fields */
        if (pos + 7 > size) {
            printf("\npakman: truncated entry header at %u\n", i);
            return -1;
        }
        pos += 7; /* type(1) + path_len(2) + data_len(4) */

        /* Guard: path_len must fit in our stack buffer AND in the archive */
        if (path_len == 0 || path_len >= MAX_PATH || pos + path_len > size) {
            printf("\npakman: bad path length %u at entry %u\n", path_len, i);
            return -1;
        }

        /* copy path and NUL-terminate */
        char rel_path[MAX_PATH];
        memcpy(rel_path, data + pos, path_len);
        rel_path[path_len] = '\0';
        pos += path_len;

        if (i < 5) {
            printf("pakman: entry %u: type=0x", i); pm_puthex8(type);
            printf(" path_len=%u path='%s' data_len=%u\n", path_len, rel_path, data_len);
        }

        /* --- build full filesystem path (single MAX_PATH buffer) --- */
        char full_path[MAX_PATH];
        path_join(full_path, sizeof(full_path), tmp_dir, rel_path);

        if (type == MPK_TYPE_DIR) {
            printf("\npakman: [%u/%u] dir  %s\n", i+1, entry_count, rel_path);
            if (mkdir_p(full_path) < 0)
                printf("pakman:   warning: mkdir_p failed for %s\n", full_path);
            /* data_len must be 0 for dirs; skip if someone packed garbage */
            if (data_len > size - pos) {
                printf("pakman: dir entry has out-of-bounds data_len\n");
                return -1;
            }
            pos += data_len;
        } else if (type == MPK_TYPE_FILE) {
            /* ensure parent directory exists -- carve off basename in-place */
            size_t pl = strlen(full_path);
            while (pl > 0 && full_path[pl - 1] != '/') pl--;
            if (pl > 0) {
                char saved_ch = full_path[pl - 1];
                full_path[pl - 1] = '\0';
                mkdir_p(full_path);
                full_path[pl - 1] = saved_ch;   /* restore -- no extra buffer needed */
            }

            if (data_len > size - pos) {
                printf("\npakman: file data out of bounds for '%s'\n", rel_path);
                return -1;
            }

            int wfd = open(full_path, O_WRONLY | O_CREAT | O_TRUNC, 0);
            if (wfd < 0) {
                printf("\npakman: cannot create '%s'\n", full_path);
                return -1;
            }
            write(wfd, data + pos, (size_t)data_len);
            close(wfd);
            pos += data_len;

            done_files++;
            if (total_files > 0)
                progress_bar("Extracting", done_files, total_files);
        } else {
            printf("\npakman: unknown entry type 0x"); pm_puthex8(type); printf(" at entry %u\n", i);
            return -1;
        }
    }

    if (total_files > 0)
        progress_bar_done("Extracting", total_files);
    printf("pakman: extraction complete (%u files)\n", (uint32_t)done_files);
    return 0;
}

/* Read a .pak file into memory, validate its MPK header, and extract to tmp_dir. */
static int load_and_extract_pak(const char *pak_path, const char *sysroot) {
    char tmp_dir[MAX_PATH];
    pakman_tmp_dir(tmp_dir, sizeof(tmp_dir), sysroot);
    printf("pakman: clearing tmp dir: %s\n", tmp_dir);
    remove_tree(tmp_dir);
    printf("pakman: tmp cleared, creating...\n");
    if (mkdir_p(tmp_dir) < 0) {
        fs_file_info_t fi; memset(&fi, 0, sizeof(fi));
        if (stat(tmp_dir, &fi) < 0) {
            printf("pakman: cannot create temp dir\n");
            return -1;
        }
    }

    size_t    pak_size = 0;
    uint8_t  *pak_data = (uint8_t *)read_file_alloc(pak_path, &pak_size);
    if (!pak_data) {
        printf("pakman: cannot read: %s\n", pak_path);
        return -1;
    }

    printf("pakman: read %u bytes from %s\n", (uint32_t)pak_size, pak_path);

    int entry_count = mpk_check_header(pak_data, pak_size);
    if (entry_count < 0) {
        printf("pakman: header check failed, aborting\n");
        free(pak_data);
        return -1;
    }

    printf("pakman: %d entries, extracting to %s\n", entry_count, tmp_dir);
    int r = mpk_extract(pak_data, pak_size, tmp_dir, (uint32_t)entry_count);
    free(pak_data);
    return r;
}

/* =========================================================================
 * §2  PAKMAN CORE
 * ========================================================================= */

#define PAKMAN_DB_REL   "appdata/ntsoftware/pakman/db"
#define PAKMAN_TMP_REL  "appdata/ntsoftware/pakman/tmp"

/* Build sysroot-relative paths for db and tmp dirs.
 * sysroot="" or "/" means physical root -> /appdata/...
 * sysroot="$/mnt/foo"  ->  $/mnt/foo/appdata/... */

/* ---- string helpers ---- */

static int pm_streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    return strcmp(a, b) == 0;
}

static void pm_strlcpy(char *dst, const char *src, size_t n) {
    if (!dst || n == 0) return;
    size_t i = 0;
    while (i + 1 < n && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void pm_strlcat(char *dst, const char *src, size_t n) {
    if (!dst || !src || n == 0) return;
    size_t l = strlen(dst);
    if (l >= n - 1) return;
    pm_strlcpy(dst + l, src, n - l);
}

static void path_join(char *out, size_t out_sz, const char *base, const char *rel) {
    pm_strlcpy(out, base, out_sz);
    size_t bl = strlen(out);
    while (bl > 1 && out[bl - 1] == '/') { out[--bl] = 0; }
    pm_strlcat(out, "/", out_sz);
    while (*rel == '/') rel++;
    pm_strlcat(out, rel, out_sz);
}

static void pakman_db_dir(char *out, size_t out_sz, const char *sysroot) {
    if (!sysroot || !sysroot[0] || pm_streq(sysroot, "/"))
        pm_strlcpy(out, "/" PAKMAN_DB_REL, out_sz);
    else
        path_join(out, out_sz, sysroot, PAKMAN_DB_REL);
}
static void pakman_tmp_dir(char *out, size_t out_sz, const char *sysroot) {
    if (!sysroot || !sysroot[0] || pm_streq(sysroot, "/"))
        pm_strlcpy(out, "/" PAKMAN_TMP_REL, out_sz);
    else
        path_join(out, out_sz, sysroot, PAKMAN_TMP_REL);
}
/* COPY_BUF_SZ, MAX_PATH, INI_LINE_MAX defined in §0 */


static void strip_quotes(char *s) {
    if (!s) return;
    size_t l = strlen(s);
    if (l >= 2 && s[0] == '"' && s[l - 1] == '"') {
        memmove(s, s + 1, l - 2);
        s[l - 2] = 0;
    }
}

static void pm_trim(char *s) {
    if (!s) return;
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t l = strlen(s);
    while (l > 0 && (s[l-1]==' '||s[l-1]=='\t'||s[l-1]=='\r'||s[l-1]=='\n')) s[--l]=0;
}

static char *pm_strchr(const char *s, int c) {
    while (*s) { if ((unsigned char)*s == (unsigned char)c) return (char*)s; s++; }
    return NULL;
}

/* ---- kernel version ---- */

typedef struct { int major, minor, patch; } semver_t;

static semver_t parse_semver(const char *s) {
    semver_t v = {0,0,0};
    if (!s || !s[0]) return v;
    char tmp[64]; pm_strlcpy(tmp, s, sizeof(tmp));
    char *p = tmp;
    v.major = (int)strtol(p, &p, 10);
    if (*p == '.') { p++; v.minor = (int)strtol(p, &p, 10); }
    if (*p == '.') { p++; v.patch = (int)strtol(p, &p, 10); }
    return v;
}

static int semver_ge(semver_t running, semver_t required) {
    if (running.major != required.major) return running.major > required.major;
    if (running.minor != required.minor) return running.minor > required.minor;
    return running.patch >= required.patch;
}

static semver_t get_kernel_version(void) {
    semver_t v = {0,0,0};
    md64api_sysinfo_data_u *info =
        (md64api_sysinfo_data_u *)malloc(sizeof(md64api_sysinfo_data_u));
    if (!info) return v;
    memset(info, 0, sizeof(md64api_sysinfo_data_u));
    int fd = open("$/dev/md64api/sysinfo", O_RDONLY, 0);
    if (fd >= 0) {
        read(fd, info, sizeof(md64api_sysinfo_data_u));
        close(fd);
        if (info->KernelVersion[0])
            v = parse_semver(info->KernelVersion);
    }
    free(info);
    return v;
}


/* ---- hex / padding helpers (printf has no %02X/%08X/%-Ns) ---- */

static void pm_puthex8(uint8_t v) {
    const char *h = "0123456789ABCDEF";
    char buf[3];
    buf[0] = h[(v >> 4) & 0xF];
    buf[1] = h[v & 0xF];
    buf[2] = 0;
    printf("%s", buf);
}

static void pm_puthex32(uint32_t v) {
    const char *h = "0123456789ABCDEF";
    char buf[9];
    for (int i = 7; i >= 0; i--) { buf[i] = h[v & 0xF]; v >>= 4; }
    buf[8] = 0;
    printf("%s", buf);
}

static void pm_putpad(const char *s, int width) {
    int l = 0; const char *p = s; while (*p++) l++;
    printf("%s", s);
    while (l < width) { printf(" "); l++; }
}

/* ---- progress bar ---- */

#define PBAR_WIDTH 30

/* Draw (or redraw) a progress bar on the current line.
 * Uses \r to overwrite in place — call progress_bar_done() to finalize. */
static void progress_bar(const char *label, uint64_t done, uint64_t total) {
    int filled = (total > 0) ? (int)((done * PBAR_WIDTH) / total) : 0;
    if (filled > PBAR_WIDTH) filled = PBAR_WIDTH;

    /* Build the bar string manually (no snprintf width tricks needed) */
    char bar[PBAR_WIDTH + 3];
    bar[0] = '[';
    for (int i = 0; i < PBAR_WIDTH; i++)
        bar[i + 1] = (i < filled) ? '#' : '-';
    bar[PBAR_WIDTH + 1] = ']';
    bar[PBAR_WIDTH + 2] = 0;

    int pct = (total > 0) ? (int)((done * 100) / total) : 0;

    /* \r moves back to start of line so we redraw in place */
    printf("\r  %s %s %d%%", label, bar, pct);
}

static void progress_bar_done(const char *label, uint64_t total) {
    /* Print a completed bar and move to next line */
    char bar[PBAR_WIDTH + 3];
    bar[0] = '[';
    for (int i = 0; i < PBAR_WIDTH; i++) bar[i + 1] = '#';
    bar[PBAR_WIDTH + 1] = ']';
    bar[PBAR_WIDTH + 2] = 0;
    printf("\r  %s %s 100%%\n", label, bar);
    (void)total;
}

/* ---- file I/O helpers ---- */

static int copy_file(const char *src, const char *dst) {
    int rfd = open(src, O_RDONLY, 0);
    if (rfd < 0) { printf("pakman: cannot open: %s\n", src); return -1; }
    int wfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (wfd < 0) { printf("pakman: cannot create: %s\n", dst); close(rfd); return -1; }
    static char buf[COPY_BUF_SZ];
    ssize_t n; int ok = 0;
    while ((n = read(rfd, buf, sizeof(buf))) > 0) {
        if (write(wfd, buf, (size_t)n) != n) { ok = -1; break; }
    }
    close(rfd); close(wfd);
    return ok;
}

static char *read_file_alloc(const char *path, size_t *out_len) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return NULL;
    long sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0) { close(fd); return NULL; }
    lseek(fd, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { close(fd); return NULL; }
    /* read() may return less than sz in one call (e.g. capped per sector/cluster).
     * Loop until we have everything or hit an error. */
    size_t total = 0;
    while (total < (size_t)sz) {
        ssize_t got = read(fd, buf + total, (size_t)sz - total);
        if (got <= 0) break;   /* EOF or error */
        total += (size_t)got;
    }
    close(fd);
    if (total == 0) { free(buf); return NULL; }
    buf[total] = 0;
    if (out_len) *out_len = total;
    return buf;
}

static int write_file(const char *path, const char *data) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fd < 0) return -1;
    size_t l = strlen(data);
    ssize_t w = write(fd, data, l);
    close(fd);
    return (w == (ssize_t)l) ? 0 : -1;
}

static int ensure_dir(const char *path) {
    fs_file_info_t fi; memset(&fi, 0, sizeof(fi));
    if (stat(path, &fi) == 0) return 0;
    return mkdir(path);
}

static int mkdir_p(const char *path) {
    if (!path || !path[0]) return -1;
    char tmp[MAX_PATH]; pm_strlcpy(tmp, path, sizeof(tmp));
    size_t l = strlen(tmp);
    for (size_t i = 1; i < l; i++) {
        if (tmp[i] == '/') { tmp[i] = 0; ensure_dir(tmp); tmp[i] = '/'; }
    }
    return ensure_dir(tmp);
}


/* Count files under a directory tree — used to size the install progress bar. */
static size_t count_tree_files(const char *src_dir) {
    int dfd = opendir(src_dir);
    if (dfd < 0) return 0;
    char entry_name[256]; int is_dir = 0; uint32_t entry_size = 0;
    size_t count = 0;
    while (readdir(dfd, entry_name, sizeof(entry_name), &is_dir, &entry_size) == 0) {
        if (pm_streq(entry_name, ".") || pm_streq(entry_name, "..")) continue;
        if (is_dir) {
            char child[MAX_PATH]; path_join(child, sizeof(child), src_dir, entry_name);
            count += count_tree_files(child);
        } else {
            count++;
        }
    }
    closedir(dfd);
    return count;
}

/* Recursive copy with shared progress counters passed by pointer. */
static int copy_tree_r(const char *src_dir, const char *dst_dir,
                        size_t *done, size_t total) {
    int dfd = opendir(src_dir);
    if (dfd < 0) { printf("\npakman: cannot open dir: %s\n", src_dir); return -1; }
    mkdir_p(dst_dir);
    char entry_name[256]; int is_dir = 0; uint32_t entry_size = 0; int err = 0;
    while (readdir(dfd, entry_name, sizeof(entry_name), &is_dir, &entry_size) == 0) {
        if (pm_streq(entry_name, ".") || pm_streq(entry_name, "..")) continue;
        char sp[MAX_PATH], dp[MAX_PATH];
        path_join(sp, sizeof(sp), src_dir, entry_name);
        path_join(dp, sizeof(dp), dst_dir, entry_name);
        if (is_dir) {
            if (copy_tree_r(sp, dp, done, total) < 0) err = -1;
        } else {
            if (copy_file(sp, dp) < 0) err = -1;
            (*done)++;
            progress_bar("Installing ", *done, total);
        }
    }
    closedir(dfd);
    return err;
}

static int copy_tree(const char *src_dir, const char *dst_dir) {
    size_t total = count_tree_files(src_dir);
    size_t done  = 0;
    if (total > 0) progress_bar("Installing ", 0, total);
    int r = copy_tree_r(src_dir, dst_dir, &done, total);
    if (total > 0) progress_bar_done("Installing ", total);
    return r;
}

static int remove_tree(const char *path) {
    int dfd = opendir(path);
    if (dfd < 0) return -1;
    char entry_name[256]; int is_dir = 0; uint32_t entry_size = 0;
    while (readdir(dfd, entry_name, sizeof(entry_name), &is_dir, &entry_size) == 0) {
        if (pm_streq(entry_name, ".") || pm_streq(entry_name, "..")) continue;
        char child[MAX_PATH]; path_join(child, sizeof(child), path, entry_name);
        /* remove_tree() calls rmdir(path) at its own end -- do NOT call
         * rmdir(child) here too or the kernel gets a double-free on the
         * dir's internal header: [KHEAP] WARNING: kfree on unmapped header */
        if (is_dir) remove_tree(child);
        else unlink(child);
    }
    closedir(dfd);
    return rmdir(path);
}

/* ---- pakdata.ini parser ---- */

typedef struct {
    char name[128];
    char version[64];
    char description[256];
    char dependencies[256];
    char author[128];
    char license[64];
    char min_kernel_version[32];
} pakdata_t;

static int parse_pakdata(const char *ini_path, pakdata_t *out) {
    memset(out, 0, sizeof(*out));
    char *content = read_file_alloc(ini_path, NULL);
    if (!content) { printf("pakman: cannot read %s\n", ini_path); return -1; }

    int in_section = 0;
    char *line = content;
    while (*line) {
        char *end = line;
        while (*end && *end != '\n') end++;
        char saved = *end; *end = 0;
        pm_trim(line);
        if (line[0] == '[') {
            in_section = pm_streq(line, "[PAKDATA]");
        } else if (in_section && line[0] && line[0] != ';' && line[0] != '#') {
            char *eq = pm_strchr(line, '=');
            if (eq) {
                *eq = 0; char *key = line, *val = eq + 1;
                pm_trim(key); pm_trim(val); strip_quotes(val);
                if      (pm_streq(key, "name"))               pm_strlcpy(out->name,              val, sizeof(out->name));
                else if (pm_streq(key, "version"))            pm_strlcpy(out->version,            val, sizeof(out->version));
                else if (pm_streq(key, "description"))        pm_strlcpy(out->description,        val, sizeof(out->description));
                else if (pm_streq(key, "dependencies"))       pm_strlcpy(out->dependencies,       val, sizeof(out->dependencies));
                else if (pm_streq(key, "author"))             pm_strlcpy(out->author,             val, sizeof(out->author));
                else if (pm_streq(key, "License"))            pm_strlcpy(out->license,            val, sizeof(out->license));
                else if (pm_streq(key, "min_kernel_version")) pm_strlcpy(out->min_kernel_version, val, sizeof(out->min_kernel_version));
            }
        }
        *end = saved; line = (*end) ? end + 1 : end;
    }
    free(content);
    if (!out->name[0]) { printf("pakman: pakdata.ini missing 'name'\n"); return -1; }
    return 0;
}

/* ---- package database ---- */

static int db_record_install(const pakdata_t *pak, const char *sysroot) {
    char db_dir[MAX_PATH]; pakman_db_dir(db_dir, sizeof(db_dir), sysroot);
    mkdir_p(db_dir);
    char db_path[MAX_PATH];
    path_join(db_path, sizeof(db_path), db_dir, pak->name);
    pm_strlcat(db_path, ".ini", sizeof(db_path));
    char record[2048];
    snprintf(record, sizeof(record),
        "[PAKDATA]\nname=\"%s\"\nversion=\"%s\"\ndescription=\"%s\"\n"
        "dependencies=\"%s\"\nauthor=\"%s\"\nLicense=\"%s\"\nmin_kernel_version=\"%s\"\n",
        pak->name, pak->version, pak->description, pak->dependencies,
        pak->author, pak->license, pak->min_kernel_version);
    return write_file(db_path, record);
}

static int db_remove_record(const char *name, const char *sysroot) {
    char db_dir[MAX_PATH]; pakman_db_dir(db_dir, sizeof(db_dir), sysroot);
    char db_path[MAX_PATH];
    path_join(db_path, sizeof(db_path), db_dir, name);
    pm_strlcat(db_path, ".ini", sizeof(db_path));
    return unlink(db_path);
}

static int db_read_record(const char *name, pakdata_t *out, const char *sysroot) {
    char db_dir[MAX_PATH]; pakman_db_dir(db_dir, sizeof(db_dir), sysroot);
    char db_path[MAX_PATH];
    path_join(db_path, sizeof(db_path), db_dir, name);
    pm_strlcat(db_path, ".ini", sizeof(db_path));
    return parse_pakdata(db_path, out);
}

/* ---- commands ---- */

static int cmd_install(const char *pak_path, const char *sysroot) {
    if (!sysroot || !sysroot[0]) sysroot = "$";
    printf("pakman: installing %s -> sysroot=%s\n", pak_path, sysroot);

    char tmp_dir[MAX_PATH]; pakman_tmp_dir(tmp_dir, sizeof(tmp_dir), sysroot);
    if (load_and_extract_pak(pak_path, sysroot) < 0) return 1;

    char ini_path[MAX_PATH];
    path_join(ini_path, sizeof(ini_path), tmp_dir, "pakdata.ini");

    pakdata_t pak;
    if (parse_pakdata(ini_path, &pak) < 0) { remove_tree(tmp_dir); return 1; }

    printf("pakman: package  : %s %s\n", pak.name, pak.version);
    printf("pakman: author   : %s\n", pak.author);
    if (pak.description[0]) printf("pakman: desc     : %s\n", pak.description);

    if (pak.min_kernel_version[0]) {
        semver_t required = parse_semver(pak.min_kernel_version);
        semver_t running  = get_kernel_version();
        printf("pakman: requires kernel >= %s\n", pak.min_kernel_version);
        printf("pakman: running kernel %d.%d.%d\n", running.major, running.minor, running.patch);
        if (!semver_ge(running, required)) {
            printf("pakman: ERROR: kernel too old for this package.\n");
            remove_tree(tmp_dir); return 1;
        }
    }

    {
        pakdata_t existing;
        if (db_read_record(pak.name, &existing, sysroot) == 0) {
            printf("pakman: '%s' already installed (version %s).\n", pak.name, existing.version);
            printf("pakman: run: pakman uninstall %s\n", pak.name);
            remove_tree(tmp_dir); return 1;
        }
    }

    char src_sysroot[MAX_PATH];
    path_join(src_sysroot, sizeof(src_sysroot), tmp_dir, "sysroot");
    fs_file_info_t fi; memset(&fi, 0, sizeof(fi));
    if (stat(src_sysroot, &fi) < 0) {
        printf("pakman: archive has no sysroot/ directory\n");
        remove_tree(tmp_dir); return 1;
    }

    printf("pakman: copying files from %s -> %s\n", src_sysroot, sysroot);
    if (copy_tree(src_sysroot, sysroot) < 0) {
        printf("pakman: file installation failed\n");
        remove_tree(tmp_dir); return 1;
    }
    printf("pakman: files copied OK\n");

    printf("pakman: recording install to db...\n");
    if (db_record_install(&pak, sysroot) < 0) {
        printf("pakman: db_record_install failed\n");
        remove_tree(tmp_dir); return 1;
    }
    printf("pakman: cleaning up tmp dir...\n");
    remove_tree(tmp_dir);
    printf("pakman: '%s' installed successfully.\n", pak.name);
    return 0;
}

static int cmd_uninstall(const char *name, const char *sysroot) {
    pakdata_t pak;
    if (db_read_record(name, &pak, sysroot) < 0) {
        printf("pakman: package '%s' is not installed.\n", name); return 1;
    }
    printf("pakman: removing '%s' %s...\n", pak.name, pak.version);
    if (db_remove_record(name, sysroot) < 0) return 1;
    printf("pakman: '%s' removed.\n", name);
    printf("pakman: note: individual files are not yet auto-removed (no manifest yet).\n");
    return 0;
}

static int cmd_list(const char *sysroot) {
    char db_dir[MAX_PATH]; pakman_db_dir(db_dir, sizeof(db_dir), sysroot);
    int dfd = opendir(db_dir);
    if (dfd < 0) { printf("pakman: no packages installed.\n"); return 0; }
    pm_putpad("Name", 24); pm_putpad("Version", 12); printf("Description\n");
    printf("------------------------------------------------------------\n");
    char entry_name[256]; int is_dir = 0; uint32_t entry_size = 0; int count = 0;
    while (readdir(dfd, entry_name, sizeof(entry_name), &is_dir, &entry_size) == 0) {
        if (is_dir) continue;
        size_t nl = strlen(entry_name);
        if (nl < 5 || !pm_streq(entry_name + nl - 4, ".ini")) continue;
        entry_name[nl - 4] = 0;
        pakdata_t pak;
        if (db_read_record(entry_name, &pak, sysroot) == 0) {
            pm_putpad(pak.name, 24); pm_putpad(pak.version, 12); printf("%s\n", pak.description);
            count++;
        }
    }
    closedir(dfd);
    if (count == 0) printf("(none)\n");
    return 0;
}

static int cmd_info(const char *name, const char *sysroot) {
    pakdata_t pak;
    if (db_read_record(name, &pak, sysroot) < 0) {
        printf("pakman: package '%s' is not installed.\n", name); return 1;
    }
    printf("Name        : %s\n", pak.name);
    printf("Version     : %s\n", pak.version);
    printf("Description : %s\n", pak.description);
    printf("Author      : %s\n", pak.author);
    printf("License     : %s\n", pak.license);
    printf("Depends     : %s\n", pak.dependencies[0] ? pak.dependencies : "(none)");
    printf("Min Kernel  : %s\n", pak.min_kernel_version[0] ? pak.min_kernel_version : "(any)");
    return 0;
}

static void usage(const char *argv0) {
    const char *n = (argv0 && argv0[0]) ? argv0 : "pakman";
    printf("Usage:\n");
    printf("  %s install <file.pak> [--sysroot <path>]\n", n);
    printf("  %s uninstall <name>\n", n);
    printf("  %s list\n", n);
    printf("  %s info <name>\n", n);
    printf("  %s help\n", n);
}

int md_main(long argc, char **argv) {
    if (!argv || argc < 2) { usage(argv ? argv[0] : NULL); return 1; }
    const char *cmd = argv[1];

    if (pm_streq(cmd, "help") || pm_streq(cmd, "--help") || pm_streq(cmd, "-h")) {
        usage(argv[0]); return 0;
    }
    if (pm_streq(cmd, "install")) {
        if (argc < 3) { printf("pakman: install needs a .pak path\n"); usage(argv[0]); return 1; }
        const char *sysroot = NULL;
        for (long i = 3; i < argc; i++)
            if (pm_streq(argv[i], "--sysroot") && i + 1 < argc) sysroot = argv[++i];
        return cmd_install(argv[2], sysroot);
    }
    if (pm_streq(cmd, "uninstall")) {
        if (argc < 3) { printf("pakman: uninstall needs a name\n"); return 1; }
        return cmd_uninstall(argv[2], "/");
    }
    if (pm_streq(cmd, "list"))  return cmd_list("/");
    if (pm_streq(cmd, "info")) {
        if (argc < 3) { printf("pakman: info needs a name\n"); return 1; }
        return cmd_info(argv[2], "/");
    }
    printf("pakman: unknown command '%s'\n", cmd);
    usage(argv[0]); return 1;
}