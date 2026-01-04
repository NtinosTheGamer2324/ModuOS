// mkfs.c - unified filesystem formatter (VFS mkfs syscall)
#include "libc.h"
#include "string.h"

static void usage(void) {
    printf("Usage: mkfs <fs> <vdrive> <pN|lba> [sizeMB|sectorsS] [label] [--spc N] [--force]\n");
    printf("  fs: fat32 | ext2 (or any registered external driver name)\n");
    printf("  size: default MB if plain number, or append 's' for sectors\n");
    printf("Examples:\n");
    printf("  mkfs ext2 2 p1 EXT2MDOS\n");
    printf("  mkfs ext2 2 p1 127 EXT2MDOS\n");
    printf("  mkfs fat32 1 p1 DATA         (auto-pick sectors/cluster)\n");
    printf("  mkfs fat32 1 p1 500 DATA --spc 8\n");
}

static int parse_u32(const char *s, uint32_t *out) {
    if (!s || !*s || !out) return -1;
    uint64_t v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return -2;
        v = v * 10 + (uint64_t)(*p - '0');
        if (v > 0xFFFFFFFFu) return -3;
    }
    *out = (uint32_t)v;
    return 0;
}

int md_main(long argc, char **argv) {
    if (argc < 4) {
        usage();
        return 1;
    }

    const char *fs = argv[1];
    const char *vd_s = argv[2];
    const char *part_s = argv[3];
    const char *size_s = (argc > 4) ? argv[4] : NULL;

    const char *label = NULL;
    uint32_t spc = 0;

    // Optional args: [label] [--spc N] [--force]
    uint32_t flags = 0;
    for (long i = 5; i < argc; i++) {
        if (strcmp(argv[i], "--spc") == 0 && i + 1 < argc) {
            uint32_t tmp;
            if (parse_u32(argv[i + 1], &tmp) == 0) spc = tmp;
            i++;
        } else if (strcmp(argv[i], "--force") == 0) {
            flags |= VFS_MKFS_FLAG_FORCE;
        } else if (!label) {
            label = argv[i];
        }
    }

    uint32_t vdrive_id_u;
    if (parse_u32(vd_s, &vdrive_id_u) != 0) {
        printf("mkfs: invalid vdrive '%s'\n", vd_s);
        return 1;
    }

    uint32_t start_lba = 0;
    uint32_t sectors = 0;

    int using_part_no = 0;
    int part_no = 0;

    // Resolve partition
    if ((part_s[0] == 'p' || part_s[0] == 'P') && part_s[1] >= '1' && part_s[1] <= '4' && part_s[2] == 0) {
        using_part_no = 1;
        part_no = (int)(part_s[1] - '0');
        vfs_part_req_t preq;
        vfs_part_info_t pinfo;
        memset(&preq, 0, sizeof(preq));
        memset(&pinfo, 0, sizeof(pinfo));
        preq.vdrive_id = (int32_t)vdrive_id_u;
        preq.part_no = (int32_t)part_no;

        int prc = vfs_getpart(&preq, &pinfo);
        if (prc != 0) {
            printf("mkfs: could not query %s on vDrive%u (rc=%d)\n", part_s, (unsigned)vdrive_id_u, prc);
            return 1;
        }
        start_lba = pinfo.start_lba;
        sectors = pinfo.sectors;

        // If a size was explicitly provided, override partition sectors.
        if (size_s) {
            // Parse size: default MB; 's' suffix means sectors
            size_t sl = strlen(size_s);
            int as_sectors = 0;
            char tmp[32];
            memset(tmp, 0, sizeof(tmp));
            strncpy(tmp, size_s, sizeof(tmp) - 1);
            if (sl >= 1 && (tmp[sl - 1] == 's' || tmp[sl - 1] == 'S')) { as_sectors = 1; tmp[sl - 1] = 0; }
            uint32_t n;
            if (parse_u32(tmp, &n) != 0 || n == 0) {
                printf("mkfs: invalid size '%s'\n", size_s);
                return 1;
            }
            sectors = as_sectors ? n : (n * 2048u);
        }
    } else {
        if (!size_s) {
            printf("mkfs: missing size (or use p1..p4 form)\n");
            return 1;
        }
        if (parse_u32(part_s, &start_lba) != 0) {
            printf("mkfs: invalid lba '%s'\n", part_s);
            return 1;
        }

        // Parse size: default MB; 's' suffix means sectors
        size_t sl = strlen(size_s);
        int as_sectors = 0;
        char tmp[32];
        memset(tmp, 0, sizeof(tmp));
        strncpy(tmp, size_s, sizeof(tmp) - 1);
        if (sl >= 1 && (tmp[sl - 1] == 's' || tmp[sl - 1] == 'S')) { as_sectors = 1; tmp[sl - 1] = 0; }
        uint32_t n;
        if (parse_u32(tmp, &n) != 0 || n == 0) {
            printf("mkfs: invalid size '%s'\n", size_s);
            return 1;
        }
        sectors = as_sectors ? n : (n * 2048u);
    }

    // Small-volume warning for FAT32 (non-fatal)
    if (strcmp(fs, "fat32") == 0) {
        // 64MiB in 512B sectors
        if (sectors < 131072u) {
            printf("mkfs: warning: FAT32 on very small volumes (<64MiB); FAT16 may be more appropriate\n");
        }
    }

    vfs_mkfs_req_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.fs_name, fs, sizeof(req.fs_name) - 1);
    if (label) strncpy(req.label, label, sizeof(req.label) - 1);
    req.vdrive_id = (int32_t)vdrive_id_u;
    req.start_lba = start_lba;
    req.sectors = sectors;
    req.flags = flags;
    req.fat32_sectors_per_cluster = spc;

    int rc = vfs_mkfs(&req);
    if (rc != 0) {
        if (rc == -10 && strcmp(fs, "fat32") == 0) {
            printf("mkfs: fat32 refused (>32GiB). Use --force\n");
        } else {
            printf("mkfs: failed rc=%d\n", rc);
        }
        return 1;
    }

    printf("mkfs: OK\n");
    return 0;
}
