/*
 * grub-install.c — ModuOS GRUB Bootloader Installer
 *
 * Implements grub-mkimage internally:
 *   core.img = diskboot.img + kernel.img + modules + prefix block
 *
 * Usage:
 *   grub-install <device>
 *   grub-install <device> --cfg <path/to/grub.cfg>
 *   grub-install <device> --grub-dir <dir>   (default: /appdata/grub-install/)
 *   grub-install <device> --prefix <grub-prefix>  (default: auto-detect)
 *
 * Files expected in grub-dir:
 *   boot.img diskboot.img kernel.img
 *   biosdisk.mod fat.mod ext2.mod normal.mod part_msdos.mod
 */

#include "libc.h"
#include "string.h"
#include <stdint.h>

/* ── ANSI ──────────────────────────────────────────────────────────────── */
#define C_RESET  "\033[0m\b"
#define C_BOLD   "\033[1m\b"
#define C_RED    "\033[31m\b"
#define C_GREEN  "\033[32m\b"
#define C_YELLOW "\033[33m\b"
#define C_CYAN   "\033[36m\b"
#define C_WHITE  "\033[97m\b"
#define C_GRAY   "\033[90m\b"

/* ── Constants ─────────────────────────────────────────────────────────── */
#define DEFAULT_GRUB_DIR   "/appdata/grub-install/"
#define DEFAULT_CFG_DST    "/boot/grub/grub.cfg"
#define SECTOR_SIZE        512
#define MAX_CORE_SIZE      (512 * 1024)   /* 512 KiB absolute max core.img */
#define MAX_CORE_SECTORS   (MAX_CORE_SIZE / SECTOR_SIZE)

/*
 * GRUB i386-pc module block header (written just before module data).
 * This is the OBJ_ELF memdisk/module list format that kernel.img expects.
 *
 * Layout appended after kernel.img:
 *   [grub_module_info]
 *   for each module:
 *     [grub_module_header]  (type=OBJ_TYPE_ELF, size=sizeof(header)+data)
 *     <raw .mod data>
 *   [grub_module_header]    (type=OBJ_TYPE_END, size=sizeof(header))
 *
 * Then the prefix string is patched inside kernel.img at a known offset.
 */

#define GRUB_MODULE_MAGIC      0x676d696d   /* 'gmim' */
#define OBJ_TYPE_ELF           1
#define OBJ_TYPE_END           0

typedef struct __attribute__((packed)) {
    uint32_t magic;     /* GRUB_MODULE_MAGIC                    */
    uint32_t offset;    /* offset of first module from this hdr */
    uint32_t size;      /* total size of module area            */
    uint32_t version;   /* 1                                    */
} grub_module_info_t;

typedef struct __attribute__((packed)) {
    uint32_t type;      /* OBJ_TYPE_ELF or OBJ_TYPE_END         */
    uint32_t size;      /* sizeof(header) + data                */
} grub_module_header_t;

/* ── Helpers ───────────────────────────────────────────────────────────── */

static void die(const char *msg) {
    printf(C_RED "\nFatal: %s\n" C_RESET, msg);
    exit(1);
}

static void step(int n, const char *msg) {
    printf(C_CYAN "[%d] " C_RESET "%s\n", n, msg);
}

static void ok(const char *msg) {
    printf(C_GREEN "    OK  " C_RESET "%s\n", msg);
}

static void warn(const char *msg) {
    printf(C_YELLOW "    [!] " C_RESET "%s\n", msg);
}

static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void write_le64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8));
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint8_t vid_from_path(const char *path) {
    const char *p = path;
    while (*p) {
        if (strncmp(p, "vDrive", 6) == 0) {
            p += 6;
            uint8_t v = 0;
            while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
            return v;
        }
        p++;
    }
    return 0;
}

static uint8_t *read_file(const char *path, size_t *out_size) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return NULL;

    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0) { close(fd); return NULL; }
    lseek(fd, 0, SEEK_SET);

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { close(fd); return NULL; }

    size_t got = 0;
    while (got < (size_t)sz) {
        ssize_t n = read(fd, buf + got, (size_t)sz - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);

    if (got != (size_t)sz) { free(buf); return NULL; }
    *out_size = got;
    return buf;
}

static int copy_file(const char *src, const char *dst) {
    int in = open(src, O_RDONLY, 0);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); return -2; }
    uint8_t tmp[512];
    ssize_t n;
    int rc = 0;
    while ((n = read(in, tmp, sizeof(tmp))) > 0) {
        if (write(out, tmp, (size_t)n) != n) { rc = -3; break; }
    }
    close(in); close(out);
    return rc;
}

/*
 * Search for needle in haystack, return pointer to first occurrence or NULL.
 * Simple byte scan — no libc memmem available.
 */
static uint8_t *find_bytes(uint8_t *hay, size_t hay_len,
                           const uint8_t *needle, size_t needle_len) {
    if (needle_len == 0 || hay_len < needle_len) return NULL;
    for (size_t i = 0; i <= hay_len - needle_len; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0)
            return hay + i;
    }
    return NULL;
}

/*
 * Scan kernel.img for the prefix placeholder string and patch it.
 *
 * Real GRUB kernel.img contains a null-padded field that looks like:
 *   "/boot/grub" followed by many null bytes (128 bytes total field).
 * We find it by scanning for that pattern and overwrite with our prefix.
 */
static int patch_prefix(uint8_t *kernel, size_t kernel_size, const char *prefix) {
    /* The placeholder in a stock kernel.img is typically empty or contains
     * a build-time default. We scan for a run of at least 64 null bytes
     * preceded by a valid path character or the start of the binary,
     * which is where GRUB stores the prefix field.
     *
     * More reliably: GRUB embeds the prefix right after a known magic
     * sequence. We scan for the string "root" or look for the prefix
     * area via the known offset in i386-pc kernel.img (~0x14 into the
     * compressed payload, but varies by GRUB version).
     *
     * Safest approach: scan for existing prefix placeholder pattern.
     * Stock kernel.img built by grub-mkimage has the prefix baked in.
     * We look for the longest null run >= 64 bytes and write there,
     * OR we look for any existing path string starting with '/' or '('.
     */
    size_t prefix_len = strlen(prefix);
    if (prefix_len > 127) return -1;  /* field is 128 bytes */

    /* Strategy 1: find an existing "(hdX" or "/boot" string */
    const uint8_t *pat1 = (const uint8_t *)"(hd";
    const uint8_t *pat2 = (const uint8_t *)"/boot";

    uint8_t *found = find_bytes(kernel, kernel_size, pat1, 3);
    if (!found) found = find_bytes(kernel, kernel_size, pat2, 5);

    if (found) {
        /* Verify there's enough room (128 byte field) */
        if ((size_t)(found - kernel) + 128 <= kernel_size) {
            memset(found, 0, 128);
            memcpy(found, prefix, prefix_len);
            return 0;
        }
    }

    /* Strategy 2: find a run of 128 zero bytes (unset prefix field) */
    for (size_t i = 0; i + 128 <= kernel_size; i++) {
        int all_zero = 1;
        for (int j = 0; j < 128; j++) {
            if (kernel[i + j] != 0) { all_zero = 0; break; }
        }
        if (all_zero) {
            memcpy(kernel + i, prefix, prefix_len);
            return 0;
        }
    }

    return -1;  /* could not find prefix field */
}

/*
 * Auto-detect the GRUB prefix for a given vdrive.
 *
 * Scans the mount table to find which slot is on this vdrive,
 * then builds "(hdX,msdosY)/boot/grub" where Y is the partition
 * number derived from the partition LBA → MBR entry index.
 *
 * Returns 1 on success, 0 if nothing mounted on this drive.
 */
static int detect_prefix(uint8_t vid, char *out, size_t out_sz) {
    uint8_t mbr[512];
    if (vdrive_read_sector(vid, 0, mbr) < 0) return 0;

    fs_mount_info_t mounts[MAX_MOUNTS];
    int count = list_mounts(mounts, MAX_MOUNTS);
    if (count <= 0) return 0;

    for (int i = 0; i < count; i++) {
        if (mounts[i].vdrive_id != (int)vid) continue;

        uint32_t slot_lba = mounts[i].partition_lba;

        for (int p = 0; p < 4; p++) {
            uint8_t *ent = mbr + 0x1BE + p * 16;
            uint32_t part_lba = read_le32(ent + 8);
            uint32_t part_cnt = read_le32(ent + 12);
            if (part_cnt == 0) continue;
            if (part_lba == slot_lba) {
                snprintf(out, out_sz,
                         "(hd%u,msdos%d)/boot/grub", (unsigned)vid, p + 1);
                return 1;
            }
        }

        snprintf(out, out_sz, "(hd%u)/boot/grub", (unsigned)vid);
        return 1;
    }

    return 0;
}

/* ── Module names to load ──────────────────────────────────────────────── */

static const char *g_modules[] = {
    "biosdisk.mod",
    "part_msdos.mod",
    "fat.mod",
    "ext2.mod",
    "normal.mod",
    "all_video.mod",
    "gfxterm.mod",
    "multiboot2.mod",
    "png.mod",
    "echo.mod",
    "font.mod", // This particural one is a custom one, needs to support FNT. 
    "halt.mod",
    "ls.mod",
    "fs.lst",
    "moddep.lst",
    "search_fs_file.mod",
    "search.mod",
    "video_fb.mod",
    "video.mod",
    NULL
};

/* ── Entry point ───────────────────────────────────────────────────────── */

int md_main(long argc, char **argv) {
    printf(C_BOLD C_WHITE "\n  grub-install — ModuOS GRUB Installer\n" C_RESET);
    printf("------------------------------------------------------------------------");
    printf("\n\n");

    if (argc < 2) {
        printf("Usage: grub-install <device> [options]\n\n");
        printf("Options:\n");
        printf("  --cfg      <path>   grub.cfg to install (optional)\n");
        printf("  --grub-dir <path>   directory with GRUB files\n");
        printf("                      (default: /appdata/grub-install/)\n");
        printf("  --prefix   <str>    GRUB prefix string\n");
        printf("                      (default: auto-detect from mount table)\n");
        printf("                      e.g. \"(hd0,msdos1)/boot/grub\"\n\n");
        return 1;
    }

    const char *device    = argv[1];
    const char *cfg_src   = NULL;
    const char *grub_dir  = DEFAULT_GRUB_DIR;
    const char *prefix_arg = NULL;

    for (int i = 2; i < (int)argc; i++) {
        if (strcmp(argv[i], "--cfg") == 0 && i + 1 < (int)argc)
            cfg_src = argv[++i];
        else if (strcmp(argv[i], "--grub-dir") == 0 && i + 1 < (int)argc)
            grub_dir = argv[++i];
        else if (strcmp(argv[i], "--prefix") == 0 && i + 1 < (int)argc)
            prefix_arg = argv[++i];
        else
            printf(C_YELLOW "Warning: unknown arg '%s'\n" C_RESET, argv[i]);
    }

    uint8_t vid = vid_from_path(device);

    /* ── Determine prefix ────────────────────────────────────────────── */
    char prefix_buf[128];
    const char *prefix = prefix_arg;

    if (!prefix) {
        if (detect_prefix(vid, prefix_buf, sizeof(prefix_buf))) {
            prefix = prefix_buf;
        } else {
            /* Fallback — user can fix grub.cfg manually */
            snprintf(prefix_buf, sizeof(prefix_buf),
                     "(hd%u,msdos1)/boot/grub", (unsigned)vid);
            prefix = prefix_buf;
            warn("Could not auto-detect mounted partition — using fallback prefix.");
            warn("If GRUB drops to rescue shell, re-run with --prefix.");
        }
    }

    printf("Target:    " C_WHITE "%s" C_RESET " (vDrive %u)\n", device, (unsigned)vid);
    printf("GRUB dir:  " C_WHITE "%s" C_RESET "\n", grub_dir);
    printf("Prefix:    " C_WHITE "%s" C_RESET "\n", prefix);
    if (cfg_src)
        printf("grub.cfg:  " C_WHITE "%s" C_RESET "\n", cfg_src);
    printf("\n");

    /* ── Step 1: Load boot.img ───────────────────────────────────────── */
    step(1, "Loading boot.img...");
    char path[256];
    snprintf(path, sizeof(path), "%sboot.img", grub_dir);
    size_t boot_size = 0;
    uint8_t *boot_img = read_file(path, &boot_size);
    if (!boot_img || boot_size < 512) die("Cannot load boot.img");
    ok("boot.img loaded (512 bytes)");

    /* ── Step 2: Load diskboot.img ───────────────────────────────────── */
    step(2, "Loading diskboot.img...");
    snprintf(path, sizeof(path), "%sdiskboot.img", grub_dir);
    size_t diskboot_size = 0;
    uint8_t *diskboot_img = read_file(path, &diskboot_size);
    if (!diskboot_img || diskboot_size < 512) die("Cannot load diskboot.img");
    ok("diskboot.img loaded");

    /* ── Step 3: Load kernel.img ─────────────────────────────────────── */
    step(3, "Loading kernel.img...");
    snprintf(path, sizeof(path), "%skernel.img", grub_dir);
    size_t kernel_size = 0;
    uint8_t *kernel_img = read_file(path, &kernel_size);
    if (!kernel_img || kernel_size == 0) die("Cannot load kernel.img");

    char tmp[128];
    snprintf(tmp, sizeof(tmp), "kernel.img loaded (%u bytes)", (unsigned)kernel_size);
    ok(tmp);

    /* ── Step 4: Patch prefix into kernel.img ────────────────────────── */
    step(4, "Patching prefix into kernel.img...");
    if (patch_prefix(kernel_img, kernel_size, prefix) != 0) {
        warn("Could not find prefix field in kernel.img — GRUB may use built-in default.");
    } else {
        snprintf(tmp, sizeof(tmp), "Prefix set to: %s", prefix);
        ok(tmp);
    }

    /* ── Step 5: Load modules ────────────────────────────────────────── */
    step(5, "Loading modules...");

    /* Count and load all modules */
    #define MAX_MODS 8
    uint8_t *mod_data[MAX_MODS];
    size_t   mod_size[MAX_MODS];
    int      mod_count = 0;

    for (int m = 0; g_modules[m] && mod_count < MAX_MODS; m++) {
        snprintf(path, sizeof(path), "%s%s", grub_dir, g_modules[m]);
        size_t msz = 0;
        uint8_t *mdata = read_file(path, &msz);
        if (!mdata) {
            printf(C_YELLOW "    [!] Could not load %s — skipping\n" C_RESET,
                   g_modules[m]);
            continue;
        }
        mod_data[mod_count] = mdata;
        mod_size[mod_count] = msz;
        mod_count++;
        snprintf(tmp, sizeof(tmp), "Loaded %s (%u bytes)", g_modules[m], (unsigned)msz);
        ok(tmp);
    }

    /* ── Step 6: Build module block ──────────────────────────────────── */
    step(6, "Building module block...");

    /*
     * Module block layout:
     *   grub_module_info        (16 bytes)
     *   for each mod:
     *     grub_module_header    (8 bytes)
     *     <mod data, padded to 4-byte boundary>
     *   grub_module_header      type=END (8 bytes)
     */
    size_t mod_block_size = sizeof(grub_module_info_t);
    for (int m = 0; m < mod_count; m++) {
        mod_block_size += sizeof(grub_module_header_t);
        mod_block_size += (mod_size[m] + 3) & ~3u;  /* 4-byte aligned */
    }
    mod_block_size += sizeof(grub_module_header_t);  /* END marker */

    uint8_t *mod_block = (uint8_t *)malloc(mod_block_size);
    if (!mod_block) die("Out of memory building module block");
    memset(mod_block, 0, mod_block_size);

    grub_module_info_t *minfo = (grub_module_info_t *)mod_block;
    minfo->magic   = GRUB_MODULE_MAGIC;
    minfo->offset  = sizeof(grub_module_info_t);
    minfo->size    = (uint32_t)mod_block_size;
    minfo->version = 1;

    size_t pos = sizeof(grub_module_info_t);
    for (int m = 0; m < mod_count; m++) {
        size_t aligned = (mod_size[m] + 3) & ~3u;
        grub_module_header_t *hdr = (grub_module_header_t *)(mod_block + pos);
        hdr->type = OBJ_TYPE_ELF;
        hdr->size = (uint32_t)(sizeof(grub_module_header_t) + aligned);
        pos += sizeof(grub_module_header_t);
        memcpy(mod_block + pos, mod_data[m], mod_size[m]);
        pos += aligned;
        free(mod_data[m]);
    }

    /* END marker */
    grub_module_header_t *end_hdr = (grub_module_header_t *)(mod_block + pos);
    end_hdr->type = OBJ_TYPE_END;
    end_hdr->size = sizeof(grub_module_header_t);

    snprintf(tmp, sizeof(tmp), "Module block: %u bytes", (unsigned)mod_block_size);
    ok(tmp);

    /* ── Step 7: Assemble core.img in memory ─────────────────────────── */
    step(7, "Assembling core.img...");

    /*
     * core.img layout:
     *   [diskboot.img — 512 bytes, sector 0, loaded by BIOS]
     *   [kernel.img   — GRUB core]
     *   [module block — module list]
     *
     * The whole thing is padded to sector boundary.
     */
    size_t raw_core_size = 512 + kernel_size + mod_block_size;
    size_t core_sectors  = (raw_core_size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    size_t core_padded   = core_sectors * SECTOR_SIZE;

    if (core_padded > MAX_CORE_SIZE) {
        printf(C_RED "Error: core.img would be %u bytes (max %u).\n" C_RESET,
               (unsigned)core_padded, MAX_CORE_SIZE);
        die("core.img too large — remove some modules");
    }

    uint8_t *core_img = (uint8_t *)malloc(core_padded);
    if (!core_img) die("Out of memory for core.img");
    memset(core_img, 0, core_padded);

    /* diskboot.img goes first */
    memcpy(core_img, diskboot_img, 512);
    free(diskboot_img);

    /* kernel.img follows immediately */
    memcpy(core_img + 512, kernel_img, kernel_size);
    free(kernel_img);

    /* module block at the end */
    memcpy(core_img + 512 + kernel_size, mod_block, mod_block_size);
    free(mod_block);

    snprintf(tmp, sizeof(tmp),
             "core.img assembled: %u bytes (%u sectors)",
             (unsigned)core_padded, (unsigned)core_sectors);
    ok(tmp);

    /* ── Step 8: Patch diskboot.img fields ───────────────────────────── */
    step(8, "Patching diskboot.img header...");

    /*
     * diskboot.img offsets (i386-pc):
     *   0x08 - 0x0F : LE64 first sector of core.img (always 1)
     *   0x10 - 0x13 : LE32 number of sectors in core.img
     */
    const uint64_t CORE_LBA = 1;
    write_le64(core_img + 0x08, CORE_LBA);
    write_le32(core_img + 0x10, (uint32_t)core_sectors);

    ok("diskboot.img: core LBA=1, sector count patched");

    /* ── Step 9: Read existing MBR ───────────────────────────────────── */
    step(9, "Reading existing MBR...");
    uint8_t mbr[512];
    if (vdrive_read_sector(vid, 0, mbr) < 0) die("Cannot read MBR");
    ok("MBR read");

    /* Check first partition starts after core.img */
    uint32_t first_lba = read_le32(mbr + 0x1BE + 8);
    if (first_lba > 1 && first_lba < 1 + (uint32_t)core_sectors) {
        printf(C_RED
               "Error: First partition at LBA %u overlaps core.img (needs LBA >= %u).\n"
               "       Re-partition leaving at least %u sectors gap after MBR.\n"
               C_RESET,
               first_lba, 1 + (uint32_t)core_sectors,
               1 + (uint32_t)core_sectors);
        free(core_img); free(boot_img);
        return 1;
    }

    /* ── Step 10: Patch boot.img ─────────────────────────────────────── */
    step(10, "Patching boot.img...");

    /*
     * boot.img offsets:
     *   0x44 - 0x47 : LE32 LBA of core.img first sector
     *   0x5C        : boot drive (0x80 = first HDD)
     *   0x1B8-0x1BD : disk sig + reserved (preserve from existing MBR)
     *   0x1BE-0x1FD : partition table (preserve from existing MBR)
     *   0x1FE-0x1FF : 0x55 0xAA
     */
    write_le32(boot_img + 0x44, (uint32_t)CORE_LBA);
    boot_img[0x5C] = 0x80;
    memcpy(boot_img + 0x1B8, mbr + 0x1B8, 6);   /* disk sig */
    memcpy(boot_img + 0x1BE, mbr + 0x1BE, 64);  /* partition table */
    boot_img[0x1FE] = 0x55;
    boot_img[0x1FF] = 0xAA;

    ok("boot.img patched");

    /* ── Step 11: Write core.img → LBA 1..N ─────────────────────────── */
    step(11, "Writing core.img to disk (LBA 1 onwards)...");
    int r = vdrive_write(vid, CORE_LBA, (uint32_t)core_sectors, core_img);
    free(core_img);
    if (r < 0) {
        printf(C_RED "Failed to write core.img (err %d)\n" C_RESET, r);
        free(boot_img);
        return 1;
    }
    snprintf(tmp, sizeof(tmp), "core.img written to LBA 1-%u", (unsigned)core_sectors);
    ok(tmp);

    /* ── Step 12: Write boot.img → LBA 0 ────────────────────────────── */
    step(12, "Writing boot.img to MBR (LBA 0)...");
    r = vdrive_write_sector(vid, 0, boot_img);
    free(boot_img);
    if (r < 0) {
        printf(C_RED "Failed to write MBR (err %d)\n" C_RESET, r);
        return 1;
    }
    ok("MBR written");

    /* ── Step 13: Optional grub.cfg ──────────────────────────────────── */
    if (cfg_src) {
        step(14, "Installing grub.cfg...");
        mkdir("/boot");
        mkdir("/boot/grub");
        if (copy_file(cfg_src, DEFAULT_CFG_DST) < 0)
            warn("Could not copy grub.cfg — is a partition mounted?");
        else
            ok("grub.cfg installed to " DEFAULT_CFG_DST);
    }

    /* ── Done ────────────────────────────────────────────────────────── */
    printf("\n");
    printf("------------------------------------------------------------------------");
    printf("\n");
    printf(C_GREEN C_BOLD "  GRUB installed successfully!\n" C_RESET);
    printf(C_GRAY  "  Boot sequence: LBA 0 (boot.img) → LBA 1-%u (core.img) → grub.cfg\n"
                   "  Prefix: %s\n" C_RESET,
           (unsigned)core_sectors, prefix);
    printf(C_GRAY  "  Make sure this drive is first in your BIOS boot order.\n" C_RESET);
    printf("------------------------------------------------------------------------");
    printf("\n\n");

    return 0;
}