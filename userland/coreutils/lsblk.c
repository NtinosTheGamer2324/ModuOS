#include "libc.h"
#include "string.h"
#include <stdint.h>

// ANSI colors
#define COL_RESET   "\033[0m\b"
#define COL_BOLD    "\033[1m\b"
#define COL_CYAN    "\033[36m\b"
#define COL_YELLOW  "\033[33m\b"
#define COL_GREEN   "\033[32m\b"
#define COL_GRAY    "\033[90m\b"
#define COL_WHITE   "\033[97m\b"

// ── String helpers ────────────────────────────────────────────────────────

static void print_padded(const char *s, int width) {
    int len = strlen(s);
    printf("%s", s);
    for (int i = len; i < width; i++) printf(" ");
}

static void fmt_size(int mb, char *out, int out_sz) {
    if (mb == 0) {
        strncpy(out, "<1M", out_sz);
    } else if (mb < 1024) {
        // itoa mb + "M"
        char tmp[16];
        itoa(mb, tmp, 10);
        strncpy(out, tmp, out_sz - 2);
        out[out_sz - 1] = '\0';
        int l = strlen(out);
        out[l]   = 'M';
        out[l+1] = '\0';
    } else {
        char tmp[16];
        itoa(mb / 1024, tmp, 10);
        strncpy(out, tmp, out_sz - 2);
        out[out_sz - 1] = '\0';
        int l = strlen(out);
        out[l]   = 'G';
        out[l+1] = '\0';
    }
}

// ── Key-value parser ──────────────────────────────────────────────────────

static char g_buf[2048];
static int  g_buf_len = 0;

static void kv_load(int fd) {
    g_buf_len = 0;
    char tmp[256];
    ssize_t n;
    while ((n = read(fd, tmp, sizeof(tmp))) > 0) {
        if (g_buf_len + (int)n < (int)sizeof(g_buf)) {
            memcpy(g_buf + g_buf_len, tmp, n);
            g_buf_len += n;
        }
    }
    g_buf[g_buf_len] = '\0';
}

static int kv_get(const char *key, char *out, int out_sz) {
    const char *p = g_buf;
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

static int kv_get_int(const char *key) {
    char tmp[32];
    kv_get(key, tmp, sizeof(tmp));
    int v = 0;
    for (int i = 0; tmp[i] >= '0' && tmp[i] <= '9'; i++)
        v = v * 10 + (tmp[i] - '0');
    return v;
}

// ── Main ──────────────────────────────────────────────────────────────────

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    printf("\n");

    // Header
    printf(COL_BOLD COL_WHITE);
    print_padded("NAME",   28);
    print_padded("SIZE",    7);
    print_padded("TYPE",   12);
    print_padded("FS",     12);
    print_padded("SERIAL", 14);
    printf("SLOT\n");
    printf(COL_RESET);

    // Separator
    printf(COL_GRAY);
    for (int i = 0; i < 76; i++) printf("-");
    printf("\n" COL_RESET);

    // Open $/dev/ and scan for vDrive* entries
    int dir = opendir("$/dev/");
    if (dir < 0) {
        printf(COL_YELLOW "lsblk: cannot open $/dev/\n" COL_RESET);
        return 1;
    }

    char entry[96];
    int  is_dir_flag;
    uint32_t entry_size;

    while (readdir(dir, entry, sizeof(entry), &is_dir_flag, &entry_size) > 0) {
        if (is_dir_flag) continue;
        if (strncmp(entry, "vDrive", 6) != 0) continue;

        // Open the node
        char full[128];
        snprintf(full, sizeof(full), "$/dev/%s", entry);
        int fd = open(full, O_RDONLY, 0);
        if (fd < 0) continue;
        kv_load(fd);
        close(fd);

        // Parse drive fields
        char model[48], type[24], serial[24], size_str[16];
        kv_get("model",  model,  sizeof(model));
        kv_get("type",   type,   sizeof(type));
        kv_get("serial", serial, sizeof(serial));
        int cap_mb  = kv_get_int("capacity_mb");
        int rd_only = kv_get_int("read_only");
        fmt_size(cap_mb, size_str, sizeof(size_str));

        // Drive row
        printf(COL_BOLD);
        printf(rd_only ? COL_CYAN : COL_GREEN);
        print_padded(entry,    28);
        printf(COL_RESET COL_BOLD);
        print_padded(size_str,  7);
        print_padded(type,     12);
        print_padded("-",      12);
        print_padded(serial,   14);
        printf("-\n" COL_RESET);

        // Partition rows
        for (int p = 1; p <= 4; p++) {
            char key[32];

            snprintf(key, sizeof(key), "part%d_present", p);
            if (!kv_get_int(key)) continue;

            char lba_str[16], fs[16], fstype[16], part_size[16], slot_str[8];
            int  part_mb, slot, mounted;

            snprintf(key, sizeof(key), "part%d_mb",      p); part_mb = kv_get_int(key);
            snprintf(key, sizeof(key), "part%d_lba",     p); kv_get(key, lba_str, sizeof(lba_str));
            snprintf(key, sizeof(key), "part%d_fs",      p); kv_get(key, fs,      sizeof(fs));
            snprintf(key, sizeof(key), "part%d_fstype",  p); kv_get(key, fstype,  sizeof(fstype));
            snprintf(key, sizeof(key), "part%d_mounted", p); mounted = kv_get_int(key);
            snprintf(key, sizeof(key), "part%d_slot",    p); slot    = kv_get_int(key);

            // Use mounted fstype if available, else fall back to mbr fs string
            const char *fs_show = (mounted && fstype[0]) ? fstype : fs;

            fmt_size(part_mb, part_size, sizeof(part_size));

            char slot_s[8];
            if (mounted) itoa(slot, slot_s, 10);
            else         strncpy(slot_s, "-", sizeof(slot_s));

            // Build lba label
            char lba_label[24];
            strncpy(lba_label, "LBA:", sizeof(lba_label));
            int ll = strlen(lba_label);
            strncpy(lba_label + ll, lba_str, sizeof(lba_label) - ll - 1);
            lba_label[sizeof(lba_label)-1] = '\0';

            // Build part name with tree drawing
            char part_name[16];
            strncpy(part_name, "part", sizeof(part_name));
            int pl = strlen(part_name);
            itoa(p, part_name + pl, 10);

            printf(COL_GRAY "  +--" COL_RESET);
            printf(COL_YELLOW);
            print_padded(part_name, 23);
            printf(COL_RESET);
            print_padded(part_size,  7);
            print_padded("partition", 12);
            print_padded(fs_show,    12);
            print_padded(lba_label,  14);
            printf("%s\n", slot_s);
        }
    }

    closedir(dir);

    // Footer
    printf(COL_GRAY);
    for (int i = 0; i < 76; i++) printf("-");
    printf("\n" COL_RESET);

    return 0;
}