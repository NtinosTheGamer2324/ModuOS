//fs.c - Kernel-owned mount table with FORMAT support
#include "moduos/fs/fs.h"
#include "moduos/fs/DOS/FAT32/fat32.h"
#include "moduos/fs/ISOFS/iso9660.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/memory/string.h"

// External FS driver registry (third-party)
#define FS_EXT_MAX_DRIVERS 16

typedef struct {
    int in_use;
    char name[16];
    const fs_ext_driver_ops_t *ops;
} fs_ext_driver_entry_t;

static fs_ext_driver_entry_t g_ext_drivers[FS_EXT_MAX_DRIVERS];

int fs_register_driver(const char *name, const fs_ext_driver_ops_t *ops) {
    fs_init();
    if (!name || !name[0] || !ops || !ops->probe || !ops->mount) return -1;

    for (int i = 0; i < FS_EXT_MAX_DRIVERS; i++) {
        if (g_ext_drivers[i].in_use && strcmp(g_ext_drivers[i].name, name) == 0) return -2;
    }

    for (int i = 0; i < FS_EXT_MAX_DRIVERS; i++) {
        if (!g_ext_drivers[i].in_use) {
            g_ext_drivers[i].in_use = 1;
            strncpy(g_ext_drivers[i].name, name, sizeof(g_ext_drivers[i].name) - 1);
            g_ext_drivers[i].name[sizeof(g_ext_drivers[i].name) - 1] = 0;
            g_ext_drivers[i].ops = ops;
            com_write_string(COM1_PORT, "[FS] Registered external FS driver: ");
            com_write_string(COM1_PORT, g_ext_drivers[i].name);
            com_write_string(COM1_PORT, "\n");
            return 0;
        }
    }

    return -3;
}

int fs_ext_mkfs(const char *driver_name, int vdrive_id, uint32_t partition_lba, uint32_t partition_sectors, const char *volume_label) {
    fs_init();
    if (!driver_name || !driver_name[0]) return -1;

    for (int i = 0; i < FS_EXT_MAX_DRIVERS; i++) {
        if (!g_ext_drivers[i].in_use || !g_ext_drivers[i].ops) continue;
        if (strcmp(g_ext_drivers[i].name, driver_name) != 0) continue;
        if (!g_ext_drivers[i].ops->mkfs) return -2;
        return g_ext_drivers[i].ops->mkfs(vdrive_id, partition_lba, partition_sectors, volume_label);
    }

    return -3;
}

#include "moduos/drivers/Drive/vDrive.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/COM/com.h"

/* Kernel mount table - like Linux's mount namespace */
#define MAX_MOUNTS 26

/* MBR partition table parsing (for P1..P4 labels) */
#define MBR_PARTITION_TABLE_OFFSET 0x1BE
#define MBR_PARTITION_ENTRY_SIZE   16

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Return 0 if not found/unknown, else 1..4 for matching MBR partition entry. */
static int fs_mbr_partition_index_for_lba(int vdrive_id, uint32_t partition_lba) {
    if (partition_lba == 0) return 0;

    vdrive_t *d = vdrive_get((uint8_t)vdrive_id);
    if (!d || !d->present) return 0;

    /* MBR only makes sense on 512-byte sector disks. */
    if (d->sector_size != 512) return 0;

    static uint8_t mbr_page[4096] __attribute__((aligned(4096)));
    uint8_t *mbr = mbr_page;

    if (vdrive_read_sector((uint8_t)vdrive_id, 0, mbr) != VDRIVE_SUCCESS) return 0;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return 0;

    for (int i = 0; i < 4; i++) {
        const uint8_t *e = mbr + MBR_PARTITION_TABLE_OFFSET + i * MBR_PARTITION_ENTRY_SIZE;
        uint8_t type = e[4];
        uint32_t first_lba = read_le32(e + 8);
        if (type == 0x00) continue;
        if (first_lba == partition_lba) return i + 1;
    }
    return 0;
}

// Return number of valid MBR partitions (0..4) and fill arrays with first LBA/type.
static int fs_mbr_list_partitions(int vdrive_id, uint32_t out_first_lba[4], uint8_t out_type[4]) {
    if (out_first_lba) for (int i = 0; i < 4; i++) out_first_lba[i] = 0;
    if (out_type) for (int i = 0; i < 4; i++) out_type[i] = 0;

    vdrive_t *d = vdrive_get((uint8_t)vdrive_id);
    if (!d || !d->present) return 0;
    if (d->sector_size != 512) return 0;

    static uint8_t mbr_page[4096] __attribute__((aligned(4096)));
    uint8_t *mbr = mbr_page;

    if (vdrive_read_sector((uint8_t)vdrive_id, 0, mbr) != VDRIVE_SUCCESS) return 0;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return 0;

    int count = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = mbr + MBR_PARTITION_TABLE_OFFSET + i * MBR_PARTITION_ENTRY_SIZE;
        uint8_t type = e[4];
        uint32_t first_lba = read_le32(e + 8);
        if (type == 0x00 || first_lba == 0) continue;
        if (out_first_lba) out_first_lba[i] = first_lba;
        if (out_type) out_type[i] = type;
        count++;
    }
    return count;
}

int fs_mbr_get_partition(int vdrive_id, int part_no, uint32_t *out_start_lba, uint32_t *out_sectors, uint8_t *out_type) {
    fs_init();
    if (out_start_lba) *out_start_lba = 0;
    if (out_sectors) *out_sectors = 0;
    if (out_type) *out_type = 0;

    if (part_no < 1 || part_no > 4) return -1;

    vdrive_t *d = vdrive_get((uint8_t)vdrive_id);
    if (!d || !d->present) return -2;
    if (d->sector_size != 512) return -3;

    static uint8_t mbr_page[4096] __attribute__((aligned(4096)));
    uint8_t *mbr = mbr_page;

    if (vdrive_read_sector((uint8_t)vdrive_id, 0, mbr) != VDRIVE_SUCCESS) return -4;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return -5;

    int idx = part_no - 1;
    const uint8_t *e = mbr + MBR_PARTITION_TABLE_OFFSET + idx * MBR_PARTITION_ENTRY_SIZE;
    uint8_t type = e[4];
    uint32_t first_lba = read_le32(e + 8);
    uint32_t sectors = read_le32(e + 12);

    if (type == 0x00 || first_lba == 0 || sectors == 0) return -6;

    if (out_start_lba) *out_start_lba = first_lba;
    if (out_sectors) *out_sectors = sectors;
    if (out_type) *out_type = type;
    return 0;
}

static int fs_try_external_mount(int vdrive_id, uint32_t lba, fs_mount_t *mount, const char **out_name) {
    if (out_name) *out_name = NULL;
    if (!mount) return 0;

    for (int i = 0; i < FS_EXT_MAX_DRIVERS; i++) {
        if (!g_ext_drivers[i].in_use || !g_ext_drivers[i].ops) continue;

#ifdef FS_EXT_DEBUG
        // Debug: show probe address to verify SQRM relocations worked
        {
            uint64_t opsp = (uint64_t)g_ext_drivers[i].ops;
            uint64_t probep = (uint64_t)g_ext_drivers[i].ops->probe;
            com_printf(COM1_PORT,
                       "[FS] ext probe %s ops=%08x%08x probe=%08x%08x lba=%u\n",
                       g_ext_drivers[i].name,
                       (unsigned)(opsp >> 32), (unsigned)(opsp & 0xFFFFFFFFu),
                       (unsigned)(probep >> 32), (unsigned)(probep & 0xFFFFFFFFu),
                       (unsigned)lba);
        }
#endif

        int pr = g_ext_drivers[i].ops->probe(vdrive_id, lba);
        if (pr != 1) continue;

        // prepare external mount
        mount->type = FS_TYPE_EXTERNAL;
        mount->handle = -1;
        mount->valid = 1;
        mount->ext_ops = g_ext_drivers[i].ops;
        mount->ext_ctx = NULL;
        memset(mount->ext_name, 0, sizeof(mount->ext_name));
        strncpy(mount->ext_name, g_ext_drivers[i].name, sizeof(mount->ext_name) - 1);

        if (g_ext_drivers[i].ops->mount(vdrive_id, lba, mount) == 0) {
            if (out_name) *out_name = mount->ext_name;
            return 1;
        }

        // mount failed; clear and continue
        mount->valid = 0;
        mount->ext_ops = NULL;
        mount->ext_ctx = NULL;
        mount->ext_name[0] = 0;
    }

    return 0;
}

typedef struct {
    fs_mount_t mount;
    int vdrive_id;
    uint32_t partition_lba;
    int partition_index; /* 0=none/unknown, else 1..4 for MBR partitions */
    char mount_point[256];
    int in_use;
} mount_entry_t;

static mount_entry_t mount_table[MAX_MOUNTS];
static int mount_table_initialized = 0;

/* Initialize mount table */
void fs_init(void) {
    if (mount_table_initialized) return;
    
    for (int i = 0; i < MAX_MOUNTS; i++) {
        mount_table[i].in_use = 0;
        mount_table[i].mount.valid = 0;
        mount_table[i].mount.type = FS_TYPE_UNKNOWN;
        mount_table[i].mount.handle = -1;
        mount_table[i].vdrive_id = -1;
        mount_table[i].partition_lba = 0;
        mount_table[i].partition_index = 0;
        mount_table[i].mount_point[0] = '\0';
    }
    
    mount_table_initialized = 1;
    com_write_string(COM1_PORT, "[FS] Mount table initialized\n");
}

/* Find free slot */
static int find_free_slot(void) {
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mount_table[i].in_use) return i;
    }
    return -1;
}

/* Check if drive already mounted */
static int find_existing_mount(int vdrive_id, uint32_t partition_lba) {
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mount_table[i].in_use &&
            mount_table[i].vdrive_id == vdrive_id &&
            mount_table[i].partition_lba == partition_lba) {
            return i;
        }
    }
    return -1;
}

/* Format a partition with FAT32 */
int fs_format(int vdrive_id, uint32_t partition_lba, uint32_t partition_sectors,
              const char* volume_label, uint32_t sectors_per_cluster) {
    fs_init();
    
    /* Validate vDrive */
    if (!vdrive_is_ready(vdrive_id)) {
        com_write_string(COM1_PORT, "[FS] Format failed: vDrive not ready\n");
        VGA_Write("FS: vDrive not ready\n");
        return -1;
    }
    
    /* Check if already mounted - must unmount first */
    int existing = find_existing_mount(vdrive_id, partition_lba);
    if (existing >= 0) {
        com_write_string(COM1_PORT, "[FS] Format failed: Drive is mounted (unmount first)\n");
        VGA_Write("FS: Drive is mounted - unmount first\n");
        return -2;
    }
    
    /* Validate partition size */
    if (partition_sectors == 0) {
        com_write_string(COM1_PORT, "[FS] Format failed: Invalid partition size\n");
        VGA_Write("FS: Invalid partition size\n");
        return -3;
    }
    
    /* Format using FAT32 driver */
    com_write_string(COM1_PORT, "[FS] Formatting vDrive ");
    char vd_str[4];
    vd_str[0] = '0' + vdrive_id;
    vd_str[1] = '\0';
    com_write_string(COM1_PORT, vd_str);
    com_write_string(COM1_PORT, " at LBA ");
    char lba_str[16];
    itoa(partition_lba, lba_str, 10);
    com_write_string(COM1_PORT, lba_str);
    com_write_string(COM1_PORT, "...\n");
    
    VGA_Write("FS: Formatting ");
    VGA_Write(volume_label ? volume_label : "NO NAME");
    VGA_Write("...\n");
    
    int result = fat32_format(vdrive_id, partition_lba, partition_sectors,
                             volume_label, sectors_per_cluster);
    
    if (result != 0) {
        com_write_string(COM1_PORT, "[FS] Format failed with code ");
        char code_str[8];
        itoa(result, code_str, 10);
        com_write_string(COM1_PORT, code_str);
        com_write_string(COM1_PORT, "\n");
        VGA_Write("FS: Format FAILED\n");
        return -4;
    }
    
    com_write_string(COM1_PORT, "[FS] Format successful!\n");
    VGA_Write("FS: Format complete!\n");
    
    return 0;
}

/* Mount filesystem - USES vDrive! */
int fs_mount_drive(int vdrive_id, uint32_t partition_lba, fs_type_t type) {
    fs_init();
    
    /* Validate vDrive */
    if (!vdrive_is_ready(vdrive_id)) {
        com_write_string(COM1_PORT, "[FS] vDrive not ready\n");
        return -6;
    }
    
    /* Check if already mounted */
    int existing = find_existing_mount(vdrive_id, partition_lba);
    if (existing >= 0) {
        com_write_string(COM1_PORT, "[FS] Drive already mounted\n");
        return -2;
    }
    
    /* Find free slot */
    int slot = find_free_slot();
    if (slot < 0) {
        com_write_string(COM1_PORT, "[FS] Mount table full\n");
        return -3;
    }
    
    /* Mount the filesystem */
    fs_mount_t mount = {0};
    int handle = -1;

    /* Keep underlying error codes for diagnostics */
    int fat_rc = -9999;
    int iso_rc = -9999;

    /* If partition_lba is 0, auto-detect. Otherwise, use specified LBA */
    if (partition_lba == 0 && type == FS_TYPE_UNKNOWN) {
        /* Auto-detect filesystem AND partition (built-ins first) */
        fat_rc = fat32_mount_auto(vdrive_id);
        handle = fat_rc;
        if (handle >= 0) {
            mount.type = FS_TYPE_FAT32;
            mount.handle = handle;
            mount.valid = 1;
            /* Get actual LBA from mounted filesystem */
            const fat32_fs_t *fs = fat32_get_fs(handle);
            if (fs) partition_lba = fs->partition_lba;
        } else {
            iso_rc = iso9660_mount_auto(vdrive_id);
            handle = iso_rc;
            if (handle >= 0) {
                mount.type = FS_TYPE_ISO9660;
                mount.handle = handle;
                mount.valid = 1;
                /* Get actual LBA from mounted filesystem */
                const iso9660_fs_t *fs = iso9660_get_fs(handle);
                if (fs) partition_lba = fs->partition_lba;
            } else {
                // Try external filesystem drivers (after built-ins)
                com_write_string(COM1_PORT, "[FS] Trying external FS drivers...\n");

                // First try whole-disk (superfloppy) match at LBA 0.
                const char *ext_name = NULL;
                if (fs_try_external_mount(vdrive_id, 0, &mount, &ext_name)) {
                    com_write_string(COM1_PORT, "[FS] External FS matched: ");
                    com_write_string(COM1_PORT, ext_name);
                    com_write_string(COM1_PORT, " (LBA 0)\n");
                } else {
                    // Then try MBR partitions.
                    uint32_t first_lba[4];
                    uint8_t ptype[4];
                    (void)ptype;
                    int pc = fs_mbr_list_partitions(vdrive_id, first_lba, ptype);
                    if (pc > 0) {
                        com_write_string(COM1_PORT, "[FS] Probing external drivers on MBR partitions...\n");
                        for (int pi = 0; pi < 4; pi++) {
                            if (first_lba[pi] == 0) continue;
                            if (fs_try_external_mount(vdrive_id, first_lba[pi], &mount, &ext_name)) {
                                com_write_string(COM1_PORT, "[FS] External FS matched: ");
                                com_write_string(COM1_PORT, ext_name);
                                com_write_string(COM1_PORT, " (LBA ");
                                char lba_str[16];
                                itoa(first_lba[pi], lba_str, 10);
                                com_write_string(COM1_PORT, lba_str);
                                com_write_string(COM1_PORT, ")\n");
                                partition_lba = first_lba[pi];
                                break;
                            }
                        }
                    }
                }
            }
        }
    } else if (type == FS_TYPE_UNKNOWN) {
        /* User specified LBA, but unknown filesystem type - try built-ins then external */
        fat_rc = fat32_mount(vdrive_id, partition_lba);
        handle = fat_rc;
        if (handle >= 0) {
            mount.type = FS_TYPE_FAT32;
            mount.handle = handle;
            mount.valid = 1;
        } else {
            iso_rc = iso9660_mount(vdrive_id, partition_lba);
            handle = iso_rc;
            if (handle >= 0) {
                mount.type = FS_TYPE_ISO9660;
                mount.handle = handle;
                mount.valid = 1;
            } else {
                // external
                com_write_string(COM1_PORT, "[FS] Trying external FS drivers...\n");
                const char *ext_name = NULL;
                if (fs_try_external_mount(vdrive_id, partition_lba, &mount, &ext_name)) {
                    com_write_string(COM1_PORT, "[FS] External FS matched: ");
                    com_write_string(COM1_PORT, ext_name);
                    com_write_string(COM1_PORT, "\n");
                }
            }
        }
    } else {
        /* User specified both LBA and filesystem type */
        switch (type) {
            case FS_TYPE_FAT32:
                fat_rc = fat32_mount(vdrive_id, partition_lba);
                handle = fat_rc;
                if (handle >= 0) {
                    mount.type = FS_TYPE_FAT32;
                    mount.handle = handle;
                    mount.valid = 1;
                }
                break;

            case FS_TYPE_ISO9660:
                iso_rc = iso9660_mount(vdrive_id, partition_lba);
                handle = iso_rc;
                if (handle >= 0) {
                    mount.type = FS_TYPE_ISO9660;
                    mount.handle = handle;
                    mount.valid = 1;
                }
                break;

            default:
                return -4;
        }
    }

    if (!mount.valid) {
        com_printf(COM1_PORT,
                   "[FS] fs_mount_drive failed: vDrive=%d lba=%u type=%d fat_rc=%d iso_rc=%d\n",
                   vdrive_id, (unsigned)partition_lba, (int)type, fat_rc, iso_rc);
        return -5;
    }
    
    /* Store in mount table */
    mount_table[slot].mount = mount;
    mount_table[slot].vdrive_id = vdrive_id;
    mount_table[slot].partition_lba = partition_lba;
    mount_table[slot].partition_index = fs_mbr_partition_index_for_lba(vdrive_id, partition_lba);
    mount_table[slot].in_use = 1;
    
    com_write_string(COM1_PORT, "[FS] Mounted ");
    if (mount.type == FS_TYPE_EXTERNAL && mount.ext_name[0]) {
        com_write_string(COM1_PORT, mount.ext_name);
    } else {
        com_write_string(COM1_PORT, fs_type_name(mount.type));
    }
    com_write_string(COM1_PORT, " ");
    {
        char label[32];
        if (fs_get_mount_label(slot, label, sizeof(label)) == 0) {
            com_write_string(COM1_PORT, "(");
            com_write_string(COM1_PORT, label);
            com_write_string(COM1_PORT, ") ");
        }
    }
    com_write_string(COM1_PORT, "at LBA ");
    {
        char lba_str[16];
        itoa(partition_lba, lba_str, 10);
        com_write_string(COM1_PORT, lba_str);
    }
    com_write_string(COM1_PORT, " in slot ");
    {
        char slot_str[16];
        itoa(slot, slot_str, 10);
        com_write_string(COM1_PORT, slot_str);
    }
    com_write_string(COM1_PORT, "\n");
    
    return slot;
}

/* Unmount by slot ID */
int fs_unmount_slot(int slot) {
    fs_init();
    
    if (slot < 0 || slot >= MAX_MOUNTS || !mount_table[slot].in_use) {
        return -1;
    }
    
    fs_mount_t* mount = &mount_table[slot].mount;
    
    switch (mount->type) {
        case FS_TYPE_FAT32:
            fat32_unmount(mount->handle);
            break;
        case FS_TYPE_ISO9660:
            iso9660_unmount(mount->handle);
            break;
        case FS_TYPE_EXTERNAL:
            if (mount->ext_ops && mount->ext_ops->unmount) {
                mount->ext_ops->unmount(mount);
            }
            break;
        default:
            break;
    }
    
    mount_table[slot].in_use = 0;
    mount_table[slot].mount.valid = 0;
    
    com_write_string(COM1_PORT, "[FS] Unmounted slot ");
    char slot_str[4];
    slot_str[0] = '0' + slot;
    slot_str[1] = '\0';
    com_write_string(COM1_PORT, slot_str);
    com_write_string(COM1_PORT, "\n");
    
    return 0;
}

/* Get mount by slot ID */
fs_mount_t* fs_get_mount(int slot) {
    fs_init();
    
    if (slot < 0 || slot >= MAX_MOUNTS || !mount_table[slot].in_use) {
        return NULL;
    }
    
    return &mount_table[slot].mount;
}

/* Get mount info by slot ID */
int fs_get_mount_info(int slot, int* vdrive_id, uint32_t* partition_lba, fs_type_t* type) {
    fs_init();

    if (slot < 0 || slot >= MAX_MOUNTS || !mount_table[slot].in_use) {
        return -1;
    }

    if (vdrive_id) *vdrive_id = mount_table[slot].vdrive_id;
    if (partition_lba) *partition_lba = mount_table[slot].partition_lba;
    if (type) *type = mount_table[slot].mount.type;

    return 0;
}

int fs_get_mount_partition_index(int slot) {
    fs_init();
    if (slot < 0 || slot >= MAX_MOUNTS || !mount_table[slot].in_use) return 0;
    return mount_table[slot].partition_index;
}

int fs_get_mount_label(int slot, char *out, size_t out_size) {
    fs_init();
    if (!out || out_size == 0) return -1;
    out[0] = 0;
    if (slot < 0 || slot >= MAX_MOUNTS || !mount_table[slot].in_use) return -1;

    int vdid = mount_table[slot].vdrive_id;
    int pidx = mount_table[slot].partition_index;

    strcpy(out, "vDrive");
    char nbuf[16];
    itoa(vdid, nbuf, 10);
    strncat(out, nbuf, out_size - strlen(out) - 1);

    if (pidx > 0) {
        strncat(out, "-P", out_size - strlen(out) - 1);
        char pbuf[8];
        itoa(pidx, pbuf, 10);
        strncat(out, pbuf, out_size - strlen(out) - 1);
    }

    return 0;
}

/* List all mounts */
void fs_list_mounts(void) {
    fs_init();

    VGA_Write("Slot  Label         Type      vDrive  LBA\n");
    VGA_Write("----  ------------  --------  ------  ----------\n");

    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mount_table[i].in_use) {
            char num[16];

            /* Slot */
            itoa(i, num, 10);
            VGA_Write(num);
            for (int p = strlen(num); p < 4; p++) VGA_Write(" ");

            /* Label */
            char label[32];
            if (fs_get_mount_label(i, label, sizeof(label)) != 0) {
                strcpy(label, "<unknown>");
            }
            VGA_Write(label);
            for (int p = strlen(label); p < 14; p++) VGA_Write(" ");

            /* Type */
            const char* fs_name = fs_type_name(mount_table[i].mount.type);
            VGA_Write(fs_name);
            for (int p = strlen(fs_name); p < 10; p++) VGA_Write(" ");

            /* vDrive */
            itoa(mount_table[i].vdrive_id, num, 10);
            VGA_Write(num);
            for (int p = strlen(num); p < 7; p++) VGA_Write(" ");

            /* LBA */
            itoa(mount_table[i].partition_lba, num, 10);
            VGA_Write(num);
            VGA_Write("\n");
        }
    }
}

/* Get total mount count */
int fs_get_mount_count(void) {
    fs_init();
    
    int count = 0;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mount_table[i].in_use) count++;
    }
    return count;
}

/* --- FILE OPERATIONS --- */

int fs_read_file(fs_mount_t* mount, const char* path, void* buffer, 
                 size_t buffer_size, size_t* bytes_read) {
    if (!mount || !mount->valid || !path || !buffer) {
        return -1;
    }
    
    int result = -1;
    size_t size = 0;
    
    if (mount->type == FS_TYPE_EXTERNAL && mount->ext_ops && mount->ext_ops->read_file) {
        result = mount->ext_ops->read_file(mount, path, buffer, buffer_size, &size);
    } else {
        switch (mount->type) {
            case FS_TYPE_FAT32:
                result = fat32_read_file_by_path(mount->handle, path, buffer, 
                                                buffer_size, &size);
                break;

            case FS_TYPE_ISO9660:
                result = iso9660_read_file_by_path(mount->handle, path, buffer, buffer_size, &size);
                break;

            default:
                return -3;
        }
    }
    
    if (bytes_read) *bytes_read = size;
    return result;
}

int fs_write_file(fs_mount_t* mount, const char* path, const void* buffer, size_t size) {
    if (!mount || !mount->valid || !path || (!buffer && size != 0)) {
        return -1;
    }

    switch (mount->type) {
        case FS_TYPE_FAT32:
            return fat32_write_file_by_path(mount->handle, path, buffer, size);
        case FS_TYPE_ISO9660:
            return -2; // read-only
        default:
            return -3;
    }
}

int fs_stat(fs_mount_t* mount, const char* path, fs_file_info_t* info) {
    if (!mount || !mount->valid || !path || !info) {
        return -1;
    }
    
    memset(info, 0, sizeof(fs_file_info_t));
    
    if (mount->type == FS_TYPE_EXTERNAL && mount->ext_ops && mount->ext_ops->stat) {
        return mount->ext_ops->stat(mount, path, info);
    }

    switch (mount->type) {
        case FS_TYPE_FAT32: {
            struct __attribute__((packed)) {
                char name[11];
                uint8_t attr;
                uint8_t nt_reserved;
                uint8_t create_time_tenth;
                uint16_t create_time;
                uint16_t create_date;
                uint16_t last_access_date;
                uint16_t first_cluster_high;
                uint16_t write_time;
                uint16_t write_date;
                uint16_t first_cluster_low;
                uint32_t filesize;
            } entry;
            
            extern int fat32_find_file(int handle, const char* path, void* out_entry);
            
            int result = fat32_find_file(mount->handle, path, &entry);
            if (result != 0) return -2;
            
            int idx = 0;
            for (int j = 0; j < 8 && entry.name[j] != ' '; j++) {
                info->name[idx++] = entry.name[j];
            }
            int has_ext = 0;
            for (int j = 8; j < 11; j++) {
                if (entry.name[j] != ' ') { has_ext = 1; break; }
            }
            if (has_ext) {
                info->name[idx++] = '.';
                for (int j = 8; j < 11 && entry.name[j] != ' '; j++) {
                    info->name[idx++] = entry.name[j];
                }
            }
            info->name[idx] = '\0';
            
            info->size = entry.filesize;
            info->is_directory = (entry.attr & 0x10) ? 1 : 0;
            info->cluster = ((uint32_t)entry.first_cluster_high << 16) | 
                           (uint32_t)entry.first_cluster_low;
            return 0;
        }
            
        case FS_TYPE_ISO9660: {
            iso9660_dir_entry_t entry;
            
            int result = iso9660_find_file(mount->handle, path, &entry);
            if (result != 0) return -2;
            
            strncpy(info->name, entry.name, sizeof(info->name) - 1);
            info->name[sizeof(info->name) - 1] = '\0';
            
            info->size = entry.size;
            info->is_directory = (entry.flags & 0x02) ? 1 : 0;
            info->cluster = entry.extent_lba;
            return 0;
        }
            
        default:
            return -4;
    }
}

int fs_file_exists(fs_mount_t* mount, const char* path) {
    fs_file_info_t info;
    int result = fs_stat(mount, path, &info);
    return (result == 0 && !info.is_directory) ? 1 : 0;
}

/* --- DIRECTORY OPERATIONS --- */

int fs_list_directory(fs_mount_t* mount, const char* path) {
    if (!mount || !mount->valid) {
        VGA_Write("FS: Invalid mount\n");
        return -1;
    }

    // Unified listing implementation for all filesystem types.
    // Do not call filesystem-specific list_directory() printers (they emit different headers).
    // Instead, always go through fs_opendir/fs_readdir and print here.
    fs_dir_t *d = fs_opendir(mount, path ? path : "/");
    if (!d) return -2;

    fs_dirent_t e;
    while (1) {
        int rc = fs_readdir(d, &e);
        if (rc <= 0) break;
        VGA_Write(e.name);
        if (e.is_directory) VGA_Write("/");
        VGA_Write("  ");
    }
    VGA_Write("\n");

    fs_closedir(d);
    return 0;
}

int fs_directory_exists(fs_mount_t* mount, const char* path) {
    if (!mount || !mount->valid) return 0;
    
    if (mount->type == FS_TYPE_EXTERNAL && mount->ext_ops && mount->ext_ops->directory_exists) {
        return mount->ext_ops->directory_exists(mount, path);
    }

    switch (mount->type) {
        case FS_TYPE_FAT32:
            return fat32_directory_exists(mount->handle, path);

        case FS_TYPE_ISO9660:
            return iso9660_directory_exists(mount->handle, path);

        default:
            return 0;
    }
}

/* --- UTILITY FUNCTIONS --- */

const char* fs_type_name(fs_type_t type) {
    switch (type) {
        case FS_TYPE_FAT32:   return "FAT32";
        case FS_TYPE_ISO9660: return "ISO9660";
        case FS_TYPE_EXTERNAL:return "External";
        case FS_TYPE_UNKNOWN: return "Unknown";
        default:              return "Invalid";
    }
}

void fs_rescan_all(void) {
    fs_init();
    int count = vdrive_get_count();
    (void)count;

    com_write_string(COM1_PORT, "[FS] Rescanning drives for new filesystems...\n");
    for (int vdrive_id = 0; vdrive_id < VDRIVE_MAX_DRIVES; vdrive_id++) {
        if (!vdrive_is_ready(vdrive_id)) continue;

        // Try mount whole disk (auto-detect may choose a partition internally).
        int existing = find_existing_mount(vdrive_id, 0);
        if (existing >= 0) continue;

        int slot = fs_mount_drive(vdrive_id, 0, FS_TYPE_UNKNOWN);
        if (slot >= 0) {
            fs_mount_t *m = fs_get_mount(slot);
            if (m && m->valid && m->type == FS_TYPE_EXTERNAL) {
                com_write_string(COM1_PORT, "[FS] Rescan mounted external FS: ");
                com_write_string(COM1_PORT, m->ext_name);
                com_write_string(COM1_PORT, "\n");
            }
        }
    }
}



/* --- DIRECTORY ITERATION FUNCTIONS --- */



typedef struct {

    fat32_folder_entry_t* entries;

    int total_entries;

    int current_position;

} fat32_dir_data_t;



typedef struct {

    iso9660_folder_entry_t* entries;

    int total_entries;

    int current_position;

} iso9660_dir_data_t;



fs_dir_t* fs_opendir(fs_mount_t* mount, const char* path) {

    if (!mount || !mount->valid) return NULL;



    fs_dir_t* dir = (fs_dir_t*)kmalloc(sizeof(fs_dir_t));

    if (!dir) return NULL;



    memset(dir, 0, sizeof(fs_dir_t));

    dir->mount = mount;

    dir->position = 0;



    if (path) {

        strncpy(dir->path, path, sizeof(dir->path) - 1);

        dir->path[sizeof(dir->path) - 1] = '\0';

    } else {

        strcpy(dir->path, "/");

    }



    if (mount->type == FS_TYPE_EXTERNAL && mount->ext_ops && mount->ext_ops->opendir) {
       dir->ext_ops = mount->ext_ops;
       return mount->ext_ops->opendir(mount, path);
   }

   switch (mount->type) {

        case FS_TYPE_FAT32: {

            fat32_dir_data_t* data = (fat32_dir_data_t*)kmalloc(sizeof(fat32_dir_data_t));

            if (!data) { kfree(dir); return NULL; }



            data->entries = (fat32_folder_entry_t*)kmalloc(sizeof(fat32_folder_entry_t) * FAT32_MAX_FOLDER_ENTRIES);

            if (!data->entries) { kfree(data); kfree(dir); return NULL; }



            data->total_entries = fat32_read_folder(mount->handle, path, data->entries, FAT32_MAX_FOLDER_ENTRIES);

            if (data->total_entries < 0) { kfree(data->entries); kfree(data); kfree(dir); return NULL; }



            data->current_position = 0;

            dir->fs_specific = data;

            break;

        }



        case FS_TYPE_ISO9660: {

            iso9660_dir_data_t* data = (iso9660_dir_data_t*)kmalloc(sizeof(iso9660_dir_data_t));

            if (!data) { kfree(dir); return NULL; }



            data->entries = (iso9660_folder_entry_t*)kmalloc(sizeof(iso9660_folder_entry_t) * ISO9660_MAX_FOLDER_ENTRIES);

            if (!data->entries) { kfree(data); kfree(dir); return NULL; }



            data->total_entries = iso9660_read_folder(mount->handle, path, data->entries, ISO9660_MAX_FOLDER_ENTRIES);

            if (data->total_entries < 0) { kfree(data->entries); kfree(data); kfree(dir); return NULL; }



            data->current_position = 0;

            dir->fs_specific = data;

            break;

        }



        default:

            kfree(dir);

            return NULL;

    }



    return dir;

}



int fs_readdir(fs_dir_t* dir, fs_dirent_t* entry) {

    if (!dir || !entry || !dir->mount || !dir->fs_specific) return -1;



    if (dir->mount->type == FS_TYPE_EXTERNAL && dir->mount->ext_ops && dir->mount->ext_ops->readdir) {
       return dir->mount->ext_ops->readdir(dir, entry);
   }

   switch (dir->mount->type) {

        case FS_TYPE_FAT32: {

            fat32_dir_data_t* data = (fat32_dir_data_t*)dir->fs_specific;

            if (data->current_position >= data->total_entries) return 0;



            fat32_folder_entry_t* src = &data->entries[data->current_position];

            strncpy(entry->name, src->name, sizeof(entry->name) - 1);

            entry->name[sizeof(entry->name) - 1] = '\0';

            entry->size = src->size;

            entry->is_directory = src->is_directory;

            entry->reserved = 0;



            data->current_position++;

            dir->position++;

            return 1;

        }



        case FS_TYPE_ISO9660: {

            iso9660_dir_data_t* data = (iso9660_dir_data_t*)dir->fs_specific;

            if (data->current_position >= data->total_entries) return 0;



            iso9660_folder_entry_t* src = &data->entries[data->current_position];

            strncpy(entry->name, src->name, sizeof(entry->name) - 1);

            entry->name[sizeof(entry->name) - 1] = '\0';

            entry->size = src->size;

            entry->is_directory = src->is_directory;

            entry->reserved = 0;



            data->current_position++;

            dir->position++;

            return 1;

        }



        default:

            return -1;

    }

}



void fs_closedir(fs_dir_t* dir) {

    if (!dir) return;



    if (dir->mount && dir->fs_specific) {

        if (dir->mount->type == FS_TYPE_EXTERNAL && dir->mount->ext_ops && dir->mount->ext_ops->closedir) {
            dir->mount->ext_ops->closedir(dir);
            return;
        }

        switch (dir->mount->type) {

            case FS_TYPE_FAT32: {

                fat32_dir_data_t* data = (fat32_dir_data_t*)dir->fs_specific;

                if (data->entries) kfree(data->entries);

                kfree(data);

                break;

            }

            case FS_TYPE_ISO9660: {

                iso9660_dir_data_t* data = (iso9660_dir_data_t*)dir->fs_specific;

                if (data->entries) kfree(data->entries);

                kfree(data);

                break;

            }

            default:

                break;

        }

    }



    kfree(dir);

}