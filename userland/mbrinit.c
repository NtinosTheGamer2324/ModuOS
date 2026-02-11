// mbrinit.c - initialize a minimal MBR on a vDrive
#include "libc.h"
#include "string.h"

static void usage(void) {
    printf("Usage: mbrinit <vdrive> [start_lba] [sectors|0] [type_hex] [--boot] [--force]\n");
    printf("  Writes a minimal MBR (LBA0) with a single primary partition in slot #1.\n");
    printf("  start_lba: default 2048\n");
    printf("  sectors:   0 means auto (disk_size - start_lba)\n");
    printf("  type_hex:  e.g. 83 (Linux/ext2), 0B/0C (FAT32) (default 83)\n");
    printf("Options:\n");
    printf("  --boot   Mark partition bootable\n");
    printf("  --force  Overwrite existing valid MBR signature\n");
    printf("Example:\n");
    printf("  mbrinit 1             (p1 from LBA2048 to end, type 0x83)\n");
    printf("  mbrinit 1 2048 0 0C   (FAT32 LBA type)\n");
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

static int parse_hex_u8(const char *s, uint8_t *out) {
    if (!s || !*s || !out) return -1;
    uint32_t v = 0;
    for (const char *p = s; *p; p++) {
        char c = *p;
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = 10u + (uint32_t)(c - 'a');
        else if (c >= 'A' && c <= 'F') d = 10u + (uint32_t)(c - 'A');
        else return -2;
        v = (v << 4) | d;
        if (v > 0xFFu) return -3;
    }
    *out = (uint8_t)v;
    return 0;
}

int md_main(long argc, char **argv) {
    if (argc < 2) { usage(); return 1; }

    uint32_t vdrive_u = 0;
    if (parse_u32(argv[1], &vdrive_u) != 0) {
        printf("mbrinit: invalid vdrive '%s'\n", argv[1]);
        return 1;
    }

    uint32_t start_lba = 2048u;
    uint32_t sectors = 0;
    uint8_t type = 0x83;
    uint8_t bootable = 0;
    uint16_t flags = 0;

    // positional: [start_lba] [sectors] [type_hex]
    int pos = 0;
    for (long i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--boot") == 0) {
            bootable = 1;
        } else if (strcmp(argv[i], "--force") == 0) {
            flags |= 0x1;
        } else {
            pos++;
            if (pos == 1) {
                if (parse_u32(argv[i], &start_lba) != 0) {
                    printf("mbrinit: invalid start_lba '%s'\n", argv[i]);
                    return 1;
                }
            } else if (pos == 2) {
                if (parse_u32(argv[i], &sectors) != 0) {
                    printf("mbrinit: invalid sectors '%s'\n", argv[i]);
                    return 1;
                }
            } else if (pos == 3) {
                if (parse_hex_u8(argv[i], &type) != 0) {
                    printf("mbrinit: invalid type_hex '%s'\n", argv[i]);
                    return 1;
                }
            } else {
                usage();
                return 1;
            }
        }
    }

    vfs_mbrinit_req_t req;
    memset(&req, 0, sizeof(req));
    req.vdrive_id = (int32_t)vdrive_u;
    req.start_lba = start_lba;
    req.sectors = sectors;
    req.type = type;
    req.bootable = bootable;
    req.flags = flags;

    int rc = vfs_mbrinit(&req);
    if (rc != 0) {
        if (rc == -11) {
            printf("mbrinit: disk already has a valid MBR (use --force to overwrite)\n");
        } else {
            printf("mbrinit: failed rc=%d\n", rc);
        }
        return 2;
    }

    printf("mbrinit: OK. You can now use: mkfs <fs> %u p1 ...\n", (unsigned)vdrive_u);
    return 0;
}
