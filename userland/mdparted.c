/*
 * mdparted.c — Interactive disk partitioning tool for ModuOS
 *
 * Usage:
 *   mdparted              → interactive mode (device picker)
 *   mdparted $/dev/vDrive0  → open that drive directly
 *
 * Supports:
 *   - List partitions (print)
 *   - Create partition  (mkpart)  — writes MBR entry directly via vdrive_read/write_sector
 *   - Delete partition  (rm)      — zeroes MBR entry directly
 *   - Mount / unmount   (mount / unmount)
 *   - MBR initialise    (mklabel) — zeroes entire MBR partition table
 *   - Quit              (quit / q)
 *
 * NOTE: formatting (mkfs) is intentionally NOT here — use a separate mkfs tool.
 */

#include "libc.h"
#include "string.h"
#include <stdint.h>

/* ── ANSI colours ──────────────────────────────────────────────────────── */
#define C_RESET   "\033[0m\b"
#define C_BOLD    "\033[1m\b"
#define C_RED     "\033[31m\b"
#define C_GREEN   "\033[32m\b"
#define C_YELLOW  "\033[33m\b"
#define C_BLUE    "\033[34m\b"
#define C_CYAN    "\033[36m\b"
#define C_WHITE   "\033[97m\b"
#define C_GRAY    "\033[90m\b"

/* ── Limits ────────────────────────────────────────────────────────────── */
#define MAX_PARTS     4
#define MAX_DRIVES    16
#define SECTOR_SIZE   512

/* ── Internal drive/partition model ───────────────────────────────────── */

typedef struct {
    int      present;
    int      index;          /* 1-based */
    uint64_t lba_start;
    uint64_t size_mb;
    char     fs[16];         /* MBR fs string  e.g. "FAT32" */
    char     fstype[16];     /* mounted fstype e.g. "FAT"   */
    int      mounted;
    int      slot;
} part_t;

typedef struct {
    char   dev_path[128];    /* $/dev/vDriveN        */
    char   model[48];
    char   type[24];
    char   serial[24];
    uint64_t capacity_mb;
    int    read_only;
    part_t parts[MAX_PARTS];
} drive_t;

/* ── KV parser (same approach as lsblk) ───────────────────────────────── */

static char g_kv[4096];
static int  g_kv_len;

static void kv_load(int fd) {
    g_kv_len = 0;
    char tmp[256];
    ssize_t n;
    while ((n = read(fd, tmp, sizeof(tmp))) > 0) {
        if (g_kv_len + (int)n < (int)sizeof(g_kv) - 1) {
            memcpy(g_kv + g_kv_len, tmp, n);
            g_kv_len += (int)n;
        }
    }
    g_kv[g_kv_len] = '\0';
}

static int kv_get(const char *key, char *out, int out_sz) {
    const char *p = g_kv;
    int klen = strlen(key);
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, key, klen) == 0) {
            const char *q = p + klen;
            while (*q == ' ' || *q == ':' || *q == '\t') q++;
            int i = 0;
            while (*q && *q != '\n' && i < out_sz - 1)
                out[i++] = *q++;
            out[i] = '\0';
            return 1;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    out[0] = '\0';
    return 0;
}

static uint64_t kv_get_u64(const char *key) {
    char tmp[32];
    kv_get(key, tmp, sizeof(tmp));
    uint64_t v = 0;
    for (int i = 0; tmp[i] >= '0' && tmp[i] <= '9'; i++)
        v = v * 10 + (tmp[i] - '0');
    return v;
}

static int kv_get_int(const char *key) {
    return (int)kv_get_u64(key);
}

/* ── Drive loader ──────────────────────────────────────────────────────── */

static void drive_load(drive_t *d) {
    int fd = open(d->dev_path, O_RDONLY, 0);
    if (fd < 0) return;
    kv_load(fd);
    close(fd);

    kv_get("model",  d->model,  sizeof(d->model));
    kv_get("type",   d->type,   sizeof(d->type));
    kv_get("serial", d->serial, sizeof(d->serial));
    d->capacity_mb = kv_get_u64("capacity_mb");
    d->read_only   = kv_get_int("read_only");

    for (int p = 1; p <= MAX_PARTS; p++) {
        char key[40];
        snprintf(key, sizeof(key), "part%d_present", p);
        if (!kv_get_int(key)) {
            d->parts[p-1].present = 0;
            continue;
        }
        part_t *pt = &d->parts[p-1];
        pt->present = 1;
        pt->index   = p;

        snprintf(key, sizeof(key), "part%d_lba",     p); pt->lba_start = kv_get_u64(key);
        snprintf(key, sizeof(key), "part%d_mb",      p); pt->size_mb   = kv_get_u64(key);
        snprintf(key, sizeof(key), "part%d_fs",      p); kv_get(key, pt->fs,     sizeof(pt->fs));
        snprintf(key, sizeof(key), "part%d_fstype",  p); kv_get(key, pt->fstype, sizeof(pt->fstype));
        snprintf(key, sizeof(key), "part%d_mounted", p); pt->mounted   = kv_get_int(key);
        snprintf(key, sizeof(key), "part%d_slot",    p); pt->slot      = kv_get_int(key);
    }
}

/* ── Size formatting ───────────────────────────────────────────────────── */

static void fmt_mb(uint64_t mb, char *out, int sz) {
    if (mb == 0) { strncpy(out, "<1M", sz); return; }
    if (mb < 1024) {
        char t[16]; itoa((int)mb, t, 10);
        snprintf(out, sz, "%sM", t);
    } else {
        char t[16]; itoa((int)(mb / 1024), t, 10);
        snprintf(out, sz, "%sG", t);
    }
}

/* ── Pretty printer ────────────────────────────────────────────────────── */

static void print_separator(void) {
    printf(C_GRAY);
    printf("------------------------------------------------------------------------");
    printf("\n" C_RESET);
}

static void print_padded(const char *s, int w) {
    int l = strlen(s);
    printf("%s", s);
    for (int i = l; i < w; i++) printf(" ");
}

static void cmd_print(drive_t *d) {
    printf("\n");
    printf(C_BOLD C_WHITE "Drive: " C_RESET C_CYAN "%s" C_RESET
           "  model=" C_WHITE "%s" C_RESET
           "  type=" C_WHITE "%s" C_RESET "\n",
           d->dev_path, d->model, d->type);

    char cap[16]; fmt_mb(d->capacity_mb, cap, sizeof(cap));
    printf("       serial=" C_WHITE "%s" C_RESET
           "  capacity=" C_WHITE "%s" C_RESET
           "  read_only=" C_WHITE "%s" C_RESET "\n\n",
           d->serial, cap, d->read_only ? "yes" : "no");

    print_separator();
    printf(C_BOLD C_WHITE);
    print_padded("#",       4);
    print_padded("START LBA", 14);
    print_padded("SIZE",    8);
    print_padded("FS",     10);
    print_padded("MOUNTED", 9);
    printf("SLOT\n");
    printf(C_RESET);
    print_separator();

    int any = 0;
    for (int i = 0; i < MAX_PARTS; i++) {
        part_t *pt = &d->parts[i];
        if (!pt->present) continue;
        any = 1;

        char sz[16]; fmt_mb(pt->size_mb, sz, sizeof(sz));
        const char *fs_show = (pt->mounted && pt->fstype[0]) ? pt->fstype : pt->fs;

        char slot_s[8];
        if (pt->mounted) itoa(pt->slot, slot_s, 10);
        else             strncpy(slot_s, "-", sizeof(slot_s));

        char lba_s[24]; itoa((int)pt->lba_start, lba_s, 10);
        char idx_s[4];  itoa(pt->index, idx_s, 10);

        printf(C_YELLOW); print_padded(idx_s,  4); printf(C_RESET);
        print_padded(lba_s, 14);
        print_padded(sz,     8);
        print_padded(fs_show, 10);
        printf(pt->mounted ? C_GREEN "yes" C_RESET : C_GRAY "no " C_RESET);
        printf("      %s\n", slot_s);
    }
    if (!any)
        printf(C_GRAY "  (no partitions)\n" C_RESET);

    print_separator();
    printf("\n");
}

/* ── atoi helper ───────────────────────────────────────────────────────── */

static int _atoi(const char *s) {
    int v = 0, neg = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

/* Strip leading/trailing whitespace in-place */
static void trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    int l = strlen(s);
    while (l > 0 && (s[l-1] == ' ' || s[l-1] == '\t' || s[l-1] == '\n' || s[l-1] == '\r'))
        s[--l] = '\0';
}

/* Split a string by spaces, returns token count (max n) */
static int split(char *s, char *toks[], int n) {
    int c = 0;
    while (*s && c < n) {
        while (*s == ' ') s++;
        if (!*s) break;
        toks[c++] = s;
        while (*s && *s != ' ') s++;
        if (*s) *s++ = '\0';
    }
    return c;
}

/* ── Commands ──────────────────────────────────────────────────────────── */

/* Extract numeric vdrive id from path like "$/dev/vDrive0" → 0 */
static uint8_t vid_from_path(const char *path) {
    /* Find "vDrive" in the path, then parse the number immediately after */
    const char *p = path;
    while (*p) {
        if (strncmp(p, "vDrive", 6) == 0) {
            p += 6;
            return (uint8_t)_atoi(p);
        }
        p++;
    }
    return 0;
}

/* mklabel — write a fresh MBR partition table via raw sector I/O.
 * Preserves bootstrap code (bytes 0-445), zeroes entries (446-509),
 * writes 0x55AA signature. No vfs_mbrinit needed.
 */
static void cmd_mklabel(drive_t *d) {
    if (d->read_only) { printf(C_RED "Error: drive is read-only.\n" C_RESET); return; }

    printf(C_YELLOW "WARNING: This will destroy all partition info on %s.\n" C_RESET, d->dev_path);
    printf("Type 'yes' to confirm: ");
    char ans[16];
    input_line_to_buffer(ans, sizeof(ans));
    trim(ans);
    if (strcmp(ans, "yes") != 0) { printf("Aborted.\n"); return; }

    uint8_t vid = vid_from_path(d->dev_path);

    uint8_t mbr[512];
    int r = vdrive_read_sector(vid, 0, mbr);
    if (r < 0) { printf(C_RED "Failed to read MBR (err %d).\n" C_RESET, r); return; }

    memset(mbr + 446, 0, 64);  /* zero all four partition entries */
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    r = vdrive_write_sector(vid, 0, mbr);
    if (r < 0) printf(C_RED "Failed to write MBR (err %d).\n" C_RESET, r);
    else { printf(C_GREEN "MBR partition table cleared.\n" C_RESET); drive_load(d); }
}

/* mkpart — create a new partition by writing an MBR entry directly.
 * Usage: mkpart <start_mb> <size_mb> [type_byte]
 *   type_byte: MBR partition type id in hex, e.g. 0x0B=FAT32, 0x83=Linux, 0x07=NTFS
 *   defaults to 0x0B (FAT32 LBA) if omitted.
 *
 * MBR partition entry layout (16 bytes at offset 446 + (n-1)*16):
 *   [0]    status       0x80=bootable, 0x00=not
 *   [1-3]  CHS first    (we set 0xFE 0xFF 0xFF for LBA-only)
 *   [4]    type         partition type byte
 *   [5-7]  CHS last     (we set 0xFE 0xFF 0xFF for LBA-only)
 *   [8-11] LBA start    little-endian uint32
 *   [12-15] LBA count   little-endian uint32
 */
static void cmd_mkpart(drive_t *d, int argc, char *argv[]) {
    if (d->read_only) { printf(C_RED "Error: drive is read-only.\n" C_RESET); return; }

    if (argc < 3) {
        printf("Usage: mkpart <start_mb> <size_mb> [type_byte]\n");
        printf("  type_byte (hex): 0x0B=FAT32  0x0E=FAT16  0x83=Linux  0x07=NTFS  0x00=empty\n");
        printf("  Default: 0x0B\n");
        return;
    }

    uint64_t start_mb = (uint64_t)_atoi(argv[1]);
    uint64_t size_mb  = (uint64_t)_atoi(argv[2]);
    uint8_t  type_byte = 0x0B; /* FAT32 LBA */

    if (argc >= 4) {
        /* Accept hex (0x..) or decimal */
        const char *ts = argv[3];
        if (ts[0] == '0' && (ts[1] == 'x' || ts[1] == 'X'))
            type_byte = (uint8_t)strtol(ts, NULL, 16);
        else
            type_byte = (uint8_t)_atoi(ts);
    }

    if (size_mb == 0) { printf(C_RED "Error: size must be > 0.\n" C_RESET); return; }
    if (start_mb + size_mb > d->capacity_mb) {
        printf(C_RED "Error: partition exceeds drive capacity (%luM).\n" C_RESET,
               (unsigned long)d->capacity_mb);
        return;
    }

    /* Find first free slot */
    int slot = -1;
    for (int i = 0; i < MAX_PARTS; i++) {
        if (!d->parts[i].present) { slot = i; break; }
    }
    if (slot < 0) {
        printf(C_RED "Error: MBR already has %d primary partitions.\n" C_RESET, MAX_PARTS);
        return;
    }

    uint32_t lba_start = (uint32_t)((start_mb * 1024ULL * 1024ULL) / SECTOR_SIZE);
    uint32_t lba_count = (uint32_t)((size_mb  * 1024ULL * 1024ULL) / SECTOR_SIZE);

    uint8_t vid = vid_from_path(d->dev_path);

    /* Read MBR */
    uint8_t mbr[512];
    int r = vdrive_read_sector(vid, 0, mbr);
    if (r < 0) { printf(C_RED "Failed to read MBR (err %d).\n" C_RESET, r); return; }

    /* Ensure signature */
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        printf(C_YELLOW "Warning: no MBR signature found — run 'mklabel' first or this drive may not boot.\n" C_RESET);
        mbr[510] = 0x55;
        mbr[511] = 0xAA;
    }

    /* Write partition entry */
    uint8_t *entry = mbr + 446 + slot * 16;
    entry[0]  = 0x00;            /* not bootable */
    entry[1]  = 0xFE;            /* CHS first (LBA-mode placeholder) */
    entry[2]  = 0xFF;
    entry[3]  = 0xFF;
    entry[4]  = type_byte;
    entry[5]  = 0xFE;            /* CHS last (LBA-mode placeholder) */
    entry[6]  = 0xFF;
    entry[7]  = 0xFF;
    /* LBA start — little-endian */
    entry[8]  = (uint8_t)(lba_start);
    entry[9]  = (uint8_t)(lba_start >> 8);
    entry[10] = (uint8_t)(lba_start >> 16);
    entry[11] = (uint8_t)(lba_start >> 24);
    /* LBA count — little-endian */
    entry[12] = (uint8_t)(lba_count);
    entry[13] = (uint8_t)(lba_count >> 8);
    entry[14] = (uint8_t)(lba_count >> 16);
    entry[15] = (uint8_t)(lba_count >> 24);

    r = vdrive_write_sector(vid, 0, mbr);
    if (r < 0) printf(C_RED "Failed to write MBR (err %d).\n" C_RESET, r);
    else {
        /* printf has no %02X width support — print high nibble then low */
        char type_str[5];
        type_str[0] = '0'; type_str[1] = 'x';
        type_str[2] = "0123456789ABCDEF"[(type_byte >> 4) & 0xF];
        type_str[3] = "0123456789ABCDEF"[type_byte & 0xF];
        type_str[4] = '\0';
        printf(C_GREEN "Partition %d created: start=%luM size=%luM type=%s\n" C_RESET,
               slot + 1, (unsigned long)start_mb, (unsigned long)size_mb, type_str);
        printf(C_GRAY "  (Use a separate mkfs tool to format it)\n" C_RESET);
        drive_load(d);
    }
}

/* rm — delete a partition by zeroing its MBR entry */
static void cmd_rm(drive_t *d, int argc, char *argv[]) {
    if (d->read_only) { printf(C_RED "Error: drive is read-only.\n" C_RESET); return; }
    if (argc < 2) { printf("Usage: rm <partition_number>\n"); return; }

    int pidx = _atoi(argv[1]);
    if (pidx < 1 || pidx > MAX_PARTS) {
        printf(C_RED "Error: partition number must be 1-%d.\n" C_RESET, MAX_PARTS);
        return;
    }

    part_t *pt = &d->parts[pidx - 1];
    if (!pt->present) {
        printf(C_RED "Error: partition %d does not exist.\n" C_RESET, pidx);
        return;
    }
    if (pt->mounted) {
        printf(C_RED "Error: partition %d is mounted (unmount first).\n" C_RESET, pidx);
        return;
    }

    printf(C_YELLOW "Delete partition %d on %s? Type 'yes' to confirm: " C_RESET, pidx, d->dev_path);
    char ans[16];
    input_line_to_buffer(ans, sizeof(ans));
    trim(ans);
    if (strcmp(ans, "yes") != 0) { printf("Aborted.\n"); return; }

    uint8_t vid = vid_from_path(d->dev_path);

    uint8_t mbr[512];
    int r = vdrive_read_sector(vid, 0, mbr);
    if (r < 0) { printf(C_RED "Failed to read MBR (err %d).\n" C_RESET, r); return; }

    /* Zero the 16-byte partition entry */
    memset(mbr + 446 + (pidx - 1) * 16, 0, 16);

    r = vdrive_write_sector(vid, 0, mbr);
    if (r < 0) printf(C_RED "Failed to write MBR (err %d).\n" C_RESET, r);
    else { printf(C_GREEN "Partition %d removed.\n" C_RESET, pidx); drive_load(d); }
}

/* mount — mount a partition */
static void cmd_mount(drive_t *d, int argc, char *argv[]) {
    if (argc < 2) { printf("Usage: mount <partition_number>\n"); return; }

    int pidx = _atoi(argv[1]);
    if (pidx < 1 || pidx > MAX_PARTS) {
        printf(C_RED "Error: invalid partition number.\n" C_RESET); return;
    }
    part_t *pt = &d->parts[pidx - 1];
    if (!pt->present) {
        printf(C_RED "Error: partition %d does not exist.\n" C_RESET, pidx); return;
    }
    if (pt->mounted) {
        printf(C_YELLOW "Partition %d already mounted at slot %d.\n" C_RESET, pidx, pt->slot);
        return;
    }

    uint8_t vid = vid_from_path(d->dev_path);

    /* fs_type: 0=auto — kernel can detect */
    int r = mount_drive((int)vid, (uint32_t)pt->lba_start, 0);
    if (r < 0) printf(C_RED "Mount failed (err %d)\n" C_RESET, r);
    else { printf(C_GREEN "Mounted at slot %d.\n" C_RESET, r); drive_load(d); }
}

/* unmount — unmount by slot */
static void cmd_unmount(drive_t *d, int argc, char *argv[]) {
    if (argc < 2) { printf("Usage: unmount <partition_number|slot>\n"); return; }

    int pidx = _atoi(argv[1]);
    if (pidx < 1 || pidx > MAX_PARTS) {
        printf(C_RED "Error: invalid partition number.\n" C_RESET); return;
    }
    part_t *pt = &d->parts[pidx - 1];
    if (!pt->present) {
        printf(C_RED "Error: partition %d does not exist.\n" C_RESET, pidx); return;
    }
    if (!pt->mounted) {
        printf(C_YELLOW "Partition %d is not mounted.\n" C_RESET, pidx); return;
    }

    int r = unmount_slot(pt->slot);
    if (r < 0) printf(C_RED "Unmount failed (err %d)\n" C_RESET, r);
    else { printf(C_GREEN "Unmounted (was slot %d).\n" C_RESET, pt->slot); drive_load(d); }
}

/* ── Drive picker ──────────────────────────────────────────────────────── */

static int pick_drive(drive_t *drives, int *count_out) {
    int count = 0;
    int dir = opendir("$/dev/");
    if (dir < 0) {
        printf(C_RED "Cannot open $/dev/\n" C_RESET);
        return -1;
    }

    char entry[96];
    int is_dir_flag;
    uint32_t entry_size;

    while (readdir(dir, entry, sizeof(entry), &is_dir_flag, &entry_size) > 0) {
        if (is_dir_flag) continue;
        if (strncmp(entry, "vDrive", 6) != 0) continue;
        if (count >= MAX_DRIVES) break;

        /* Kernel bug: $/dev/ may list each vDrive twice — deduplicate by name */
        char candidate[128];
        snprintf(candidate, sizeof(candidate), "$/dev/%s", entry);
        int already_seen = 0;
        for (int di = 0; di < count; di++) {
            if (strcmp(drives[di].dev_path, candidate) == 0) { already_seen = 1; break; }
        }
        if (already_seen) continue;

        strncpy(drives[count].dev_path, candidate, sizeof(drives[0].dev_path) - 1);
        drive_load(&drives[count]);
        count++;
    }
    closedir(dir);

    if (count == 0) {
        printf(C_YELLOW "No block devices found.\n" C_RESET);
        *count_out = 0;
        return -1;
    }

    printf(C_BOLD "\nAvailable drives:\n" C_RESET);
    print_separator();
    for (int i = 0; i < count; i++) {
        char sz[16];
        fmt_mb(drives[i].capacity_mb, sz, sizeof(sz));
        printf(C_YELLOW "  [%d] " C_RESET, i + 1);
        print_padded(drives[i].dev_path, 28);
        printf("  %s  (%s)\n", sz, drives[i].model);
    }
    print_separator();
    printf("Select drive [1-%d]: ", count);

    char line[8];
    input_line_to_buffer(line, sizeof(line));
    int sel = _atoi(line);
    if (sel < 1 || sel > count) {
        printf(C_RED "Invalid selection.\n" C_RESET);
        *count_out = count;
        return -1;
    }

    *count_out = count;
    return sel - 1;
}

/* ── Help ──────────────────────────────────────────────────────────────── */

static void print_help(void) {
    printf(C_BOLD "\nCommands:\n" C_RESET);
    printf("  " C_CYAN "print" C_RESET "                       Show partition table\n");
    printf("  " C_CYAN "mklabel" C_RESET "                     Initialise a new MBR\n");
    printf("  " C_CYAN "mkpart" C_RESET " <start_mb> <size_mb> [type]  Create partition\n");
    printf("  " C_GRAY "         type: 0x0B=FAT32  0x83=Linux  0x07=NTFS\n" C_RESET);
    printf("  " C_CYAN "rm" C_RESET " <num>                   Delete partition\n");
    printf("  " C_CYAN "mount" C_RESET " <num>                 Mount partition\n");
    printf("  " C_CYAN "unmount" C_RESET " <num>               Unmount partition\n");
    printf("  " C_CYAN "select" C_RESET "                      Switch to another drive\n");
    printf("  " C_CYAN "help" C_RESET "                        Show this help\n");
    printf("  " C_CYAN "quit" C_RESET " / " C_CYAN "q" C_RESET "                    Exit\n");

}

/* ── Entry point ───────────────────────────────────────────────────────── */

int md_main(long argc, char **argv) {

    printf(C_BOLD C_WHITE "\n  mdparted — ModuOS Partition Editor\n" C_RESET);
    print_separator();

    static drive_t drives[MAX_DRIVES];
    drive_t *cur = NULL;
    int ndrives = 0;

    /* If a path was given on the command line, open it directly */
    if (argc >= 2) {
        strncpy(drives[0].dev_path, argv[1], sizeof(drives[0].dev_path) - 1);
        drive_load(&drives[0]);
        cur = &drives[0];
        ndrives = 1;
    } else {
        int sel = pick_drive(drives, &ndrives);
        if (sel < 0) return 1;
        cur = &drives[sel];
    }

    cmd_print(cur);

    /* REPL */
    char line[256];
    for (;;) {
        /* Prompt shows drive name */
        const char *dn = cur->dev_path;
        /* Just show the trailing component */
        const char *slash = strrchr(dn, '/');
        printf(C_BOLD C_CYAN "(mdparted:%s)" C_RESET " ", slash ? slash + 1 : dn);

        ssize_t n = input_line_to_buffer(line, sizeof(line));
        if (n < 0) continue;
        trim(line);
        if (line[0] == '\0') continue;

        /* Tokenise */
        char copy[256];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[255] = '\0';

        char *toks[8];
        int tc = split(copy, toks, 8);
        if (tc == 0) continue;

        const char *cmd = toks[0];

        if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0) {
            printf(C_GRAY "Bye.\n" C_RESET);
            break;

        } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
            print_help();

        } else if (strcmp(cmd, "print") == 0 || strcmp(cmd, "p") == 0) {
            drive_load(cur);   /* refresh */
            cmd_print(cur);

        } else if (strcmp(cmd, "mklabel") == 0) {
            cmd_mklabel(cur);

        } else if (strcmp(cmd, "mkpart") == 0) {
            cmd_mkpart(cur, tc, toks);

        } else if (strcmp(cmd, "rm") == 0 || strcmp(cmd, "delete") == 0) {
            cmd_rm(cur, tc, toks);

        } else if (strcmp(cmd, "mount") == 0) {
            cmd_mount(cur, tc, toks);

        } else if (strcmp(cmd, "unmount") == 0 || strcmp(cmd, "umount") == 0) {
            cmd_unmount(cur, tc, toks);

        } else if (strcmp(cmd, "select") == 0) {
            int sel = pick_drive(drives, &ndrives);
            if (sel >= 0) { cur = &drives[sel]; cmd_print(cur); }

        } else {
            printf(C_RED "Unknown command: %s" C_RESET "  (type 'help' for list)\n", cmd);
        }
    }

    return 0;
}