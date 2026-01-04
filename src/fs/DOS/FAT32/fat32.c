#include "moduos/fs/DOS/FAT32/fat32.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/drivers/Drive/vDrive.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/memory/memory.h"
#include <stdint.h>
#include <stddef.h>

#define MBR_PARTITION_TABLE_OFFSET 0x1BE
#define PARTITION_ENTRY_SIZE 16
#define FAT32_TYPE_B 0x0B
#define FAT32_TYPE_C 0x0C
#define USE_DYNAMIC_BUFFERS 1

struct __attribute__((packed)) fat_dir_entry {
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
};

// Forward decl used by fat32_read_folder / fat32_write_file_by_path
int fat32_find_file(int handle, const char* path, struct fat_dir_entry* out_entry);

/* Global array of mounted filesystems */
static fat32_fs_t fat32_mounts[FAT32_MAX_MOUNTS];

static void* fat32_alloc_cluster_buffer(const fat32_fs_t* fs) {
    uint32_t clus_size = (uint32_t)fs->bytes_per_sector * (uint32_t)fs->sectors_per_cluster;
    if (clus_size == 0 || clus_size > FAT32_MAX_CLUSTER_SIZE) {
        return NULL;
    }
    return kmalloc(clus_size);
}

/* Initialize on first use */
static void fat32_init_once(void) {
    static int initialized = 0;
    if (!initialized) {
        memset(fat32_mounts, 0, sizeof(fat32_mounts));
        initialized = 1;
    }
}

/* Find free slot, returns index or -1 */
static int fat32_alloc_handle(void) {
    for (int i = 0; i < FAT32_MAX_MOUNTS; i++) {
        if (!fat32_mounts[i].active) {
            return i;
        }
    }
    return -1;
}

/* Validate handle */
static int fat32_valid_handle(int handle) {
    return (handle >= 0 && handle < FAT32_MAX_MOUNTS && fat32_mounts[handle].active);
}

/* Safe division helper */
static uint32_t safe_divide(uint32_t numerator, uint32_t denominator) {
    if (denominator == 0) return 0;
    return numerator / denominator;
}

static uint32_t calculate_cluster_size(uint64_t partition_sectors) {
    uint64_t size_mb = (partition_sectors * 512) / (1024 * 1024);
    
    /* Microsoft's recommended cluster sizes for FAT32:
       <= 260 MB: Not recommended for FAT32
       <= 8 GB: 4 KB (8 sectors)
       <= 16 GB: 8 KB (16 sectors)
       <= 32 GB: 16 KB (32 sectors)
       > 32 GB: 32 KB (64 sectors)
    */
    
    if (size_mb <= 260) {
        VGA_Write("Warning: Partition too small for FAT32 (< 260 MB)\n");
        return 1; // 512 bytes (minimum)
    } else if (size_mb <= 8192) {
        return 8;  // 4 KB
    } else if (size_mb <= 16384) {
        return 16; // 8 KB
    } else if (size_mb <= 32768) {
        return 32; // 16 KB
    } else {
        return 64; // 32 KB
    }
}

static int write_zero_sector(int vdrive_id, uint32_t lba) {
    /* DMA safety: keep sector buffer within a single 4KiB page. */
    static uint8_t sector_page[4096] __attribute__((aligned(4096)));
    uint8_t *sector = sector_page;
    memset(sector, 0, 512);
    return vdrive_write_sector(vdrive_id, lba, sector);
}

/**
 * Format a partition with FAT32
 * @param vdrive_id: Virtual drive ID
 * @param partition_lba: Starting LBA of partition
 * @param partition_sectors: Size of partition in sectors
 * @param volume_label: Volume label (11 chars max, or NULL for "NO NAME")
 * @param sectors_per_cluster: Sectors per cluster (0 for auto)
 * @return: 0 on success, negative on error
 */
int fat32_format(int vdrive_id, uint32_t partition_lba, uint32_t partition_sectors,
                 const char* volume_label, uint32_t sectors_per_cluster) {

    VGA_Write("FAT32: Formatting partition...\n");

    /* Validate parameters */
    if (!vdrive_is_ready(vdrive_id)) {
        VGA_Write("FAT32: vDrive not ready\n");
        return -1;
    }

    if (partition_sectors < 65536) {
        VGA_Write("FAT32: Partition too small (minimum 32 MB)\n");
        return -2;
    }

    /* Calculate cluster size if not specified */
    if (sectors_per_cluster == 0) {
        sectors_per_cluster = calculate_cluster_size(partition_sectors);
        com_printf(COM1_PORT, "FAT32: Using %u sectors per cluster\n", sectors_per_cluster);
    }

    /* Validate cluster size */
    if (sectors_per_cluster == 0 || sectors_per_cluster > 128 ||
        (sectors_per_cluster & (sectors_per_cluster - 1)) != 0) {
        VGA_Write("FAT32: Invalid cluster size (must be power of 2, max 128)\n");
        return -3;
    }

    /* Calculate FAT32 parameters */
    uint16_t bytes_per_sector = 512;
    uint16_t reserved_sectors = 32;
    uint8_t num_fats = 2;
    uint16_t root_entry_count = 0;
    uint16_t media_type = 0xF8;

    /* Calculate FAT size */
    uint32_t total_clusters = (partition_sectors - reserved_sectors) / sectors_per_cluster;
    uint32_t fat_size_sectors = ((total_clusters * 4) + (bytes_per_sector - 1)) / bytes_per_sector;

    /* Align FAT size to 4 KB boundary for better performance */
    fat_size_sectors = ((fat_size_sectors + 7) / 8) * 8;

    /* Calculate first data sector */
    uint32_t first_data_sector = reserved_sectors + (num_fats * fat_size_sectors);

    /* Verify we have enough space */
    if (first_data_sector >= partition_sectors) {
        VGA_Write("FAT32: Partition too small for calculated FAT size\n");
        return -4;
    }

    com_printf(COM1_PORT, "FAT32: Reserved sectors: %u\n", reserved_sectors);
    com_printf(COM1_PORT, "FAT32: FAT size: %u sectors\n", fat_size_sectors);
    com_printf(COM1_PORT, "FAT32: First data sector: %u\n", first_data_sector);

    // IMPORTANT: DMA safety. Do NOT use stack buffers for disk IO.
    uint8_t *boot_sector = (uint8_t*)kmalloc(512);
    uint8_t *fsinfo = (uint8_t*)kmalloc(512);
    uint8_t *fat_sector = (uint8_t*)kmalloc(512);
    if (!boot_sector || !fsinfo || !fat_sector) {
        if (boot_sector) kfree(boot_sector);
        if (fsinfo) kfree(fsinfo);
        if (fat_sector) kfree(fat_sector);
        return -11;
    }

    int rc = 0;

    /* Boot sector */
    memset(boot_sector, 0, 512);
    boot_sector[0] = 0xEB;
    boot_sector[1] = 0x58;
    boot_sector[2] = 0x90;
    memcpy(&boot_sector[3], "MODUOS  ", 8);

    boot_sector[11] = bytes_per_sector & 0xFF;
    boot_sector[12] = (bytes_per_sector >> 8) & 0xFF;
    boot_sector[13] = sectors_per_cluster;
    boot_sector[14] = reserved_sectors & 0xFF;
    boot_sector[15] = (reserved_sectors >> 8) & 0xFF;
    boot_sector[16] = num_fats;
    boot_sector[17] = root_entry_count & 0xFF;
    boot_sector[18] = (root_entry_count >> 8) & 0xFF;
    boot_sector[19] = 0;
    boot_sector[20] = 0;
    boot_sector[21] = media_type;
    boot_sector[22] = 0;
    boot_sector[23] = 0;
    boot_sector[24] = 63;
    boot_sector[25] = 0;
    boot_sector[26] = 255;
    boot_sector[27] = 0;

    boot_sector[28] = partition_lba & 0xFF;
    boot_sector[29] = (partition_lba >> 8) & 0xFF;
    boot_sector[30] = (partition_lba >> 16) & 0xFF;
    boot_sector[31] = (partition_lba >> 24) & 0xFF;

    boot_sector[32] = partition_sectors & 0xFF;
    boot_sector[33] = (partition_sectors >> 8) & 0xFF;
    boot_sector[34] = (partition_sectors >> 16) & 0xFF;
    boot_sector[35] = (partition_sectors >> 24) & 0xFF;

    boot_sector[36] = fat_size_sectors & 0xFF;
    boot_sector[37] = (fat_size_sectors >> 8) & 0xFF;
    boot_sector[38] = (fat_size_sectors >> 16) & 0xFF;
    boot_sector[39] = (fat_size_sectors >> 24) & 0xFF;

    boot_sector[40] = 0;
    boot_sector[41] = 0;
    boot_sector[42] = 0;
    boot_sector[43] = 0;

    boot_sector[44] = 2;
    boot_sector[45] = 0;
    boot_sector[46] = 0;
    boot_sector[47] = 0;

    boot_sector[48] = 1;
    boot_sector[49] = 0;
    boot_sector[50] = 6;
    boot_sector[51] = 0;

    for (int i = 52; i < 64; i++) boot_sector[i] = 0;

    boot_sector[64] = 0x80;
    boot_sector[65] = 0;
    boot_sector[66] = 0x29;

    boot_sector[67] = 0x12;
    boot_sector[68] = 0x34;
    boot_sector[69] = 0x56;
    boot_sector[70] = 0x78;

    if (volume_label && volume_label[0]) {
        int len = 0;
        while (volume_label[len] && len < 11) {
            boot_sector[71 + len] = volume_label[len];
            len++;
        }
        while (len < 11) {
            boot_sector[71 + len] = ' ';
            len++;
        }
    } else {
        memcpy(&boot_sector[71], "NO NAME    ", 11);
    }

    memcpy(&boot_sector[82], "FAT32   ", 8);

    const char *msg = "This is not a bootable device. Please insert a bootable medium and press Ctrl+Alt+Del.";
    int msg_len = strlen(msg);
    for (int i = 0; i < msg_len && (90 + i) < 510; i++) {
        boot_sector[90 + i] = msg[i];
    }

    boot_sector[510] = 0x55;
    boot_sector[511] = 0xAA;

    VGA_Write("FAT32: Writing boot sector...\n");
    if (vdrive_write_sector(vdrive_id, partition_lba, boot_sector) != 0) { rc = -5; goto cleanup; }
    if (vdrive_write_sector(vdrive_id, partition_lba + 6, boot_sector) != 0) { rc = -6; goto cleanup; }

    /* FSInfo */
    memset(fsinfo, 0, 512);
    fsinfo[0] = 0x52; fsinfo[1] = 0x52; fsinfo[2] = 0x61; fsinfo[3] = 0x41;
    fsinfo[484] = 0x72; fsinfo[485] = 0x72; fsinfo[486] = 0x41; fsinfo[487] = 0x61;
    fsinfo[488] = 0xFF; fsinfo[489] = 0xFF; fsinfo[490] = 0xFF; fsinfo[491] = 0xFF;
    fsinfo[492] = 3; fsinfo[493] = 0; fsinfo[494] = 0; fsinfo[495] = 0;
    fsinfo[510] = 0x55; fsinfo[511] = 0xAA;

    VGA_Write("FAT32: Writing FSInfo sector...\n");
    if (vdrive_write_sector(vdrive_id, partition_lba + 1, fsinfo) != 0) { rc = -7; goto cleanup; }

    /* FAT init */
    VGA_Write("FAT32: Initializing FAT tables...\n");
    for (uint32_t fat_num = 0; fat_num < num_fats; fat_num++) {
        uint32_t fat_start = partition_lba + reserved_sectors + (fat_num * fat_size_sectors);

        memset(fat_sector, 0, 512);
        fat_sector[0] = media_type;
        fat_sector[1] = 0xFF;
        fat_sector[2] = 0xFF;
        fat_sector[3] = 0x0F;
        fat_sector[4] = 0xFF;
        fat_sector[5] = 0xFF;
        fat_sector[6] = 0xFF;
        fat_sector[7] = 0x0F;
        fat_sector[8] = 0xFF;
        fat_sector[9] = 0xFF;
        fat_sector[10] = 0xFF;
        fat_sector[11] = 0x0F;

        if (vdrive_write_sector(vdrive_id, fat_start, fat_sector) != 0) { rc = -8; goto cleanup; }

        memset(fat_sector, 0, 512);
        for (uint32_t i = 1; i < fat_size_sectors; i++) {
            if (vdrive_write_sector(vdrive_id, fat_start + i, fat_sector) != 0) { rc = -9; goto cleanup; }
        }
    }

    /* Clear root directory */
    VGA_Write("FAT32: Clearing root directory...\n");
    uint32_t root_cluster_lba = partition_lba + first_data_sector;
    for (uint32_t i = 0; i < sectors_per_cluster; i++) {
        if (write_zero_sector(vdrive_id, root_cluster_lba + i) != 0) { rc = -10; goto cleanup; }
    }

    VGA_Write("FAT32: Format complete!\n");
    com_printf(COM1_PORT, "FAT32: Volume label: %s\n", volume_label ? volume_label : "NO NAME");
    com_printf(COM1_PORT, "FAT32: Cluster size: %u KB\n", (sectors_per_cluster * 512) / 1024);
    com_printf(COM1_PORT, "FAT32: Total clusters: %u\n", total_clusters);

cleanup:
    kfree(fat_sector);
    kfree(fsinfo);
    kfree(boot_sector);
    return rc;
}

/* --- MOUNT --- */
int fat32_mount(int vdrive_id, uint32_t partition_lba) {
    fat32_init_once();
    
    int handle = fat32_alloc_handle();
    if (handle < 0) {
        VGA_Write("FAT32: no free mount slots\n");
        return -1;
    }

    /*
     * NOTE (AHCI DMA): The SATA AHCI path uses paging_virt_to_phys() on the destination
     * buffer. Static .bss buffers are not always safely translatable in this kernel's
     * current paging model, which can result in "successful" reads that leave the
     * buffer unchanged (often all zeros).
     *
     * Use kmalloc() here to ensure a DMA-safe mapping.
     */
    uint8_t *sector = (uint8_t*)kmalloc(4096);
    if (!sector) return -1;
    memset(sector, 0, 4096);
    fat32_fs_t* fs = &fat32_mounts[handle];
    
    memset(fs, 0, sizeof(fat32_fs_t));
    fs->vdrive_id = vdrive_id;  
    fs->partition_lba = partition_lba;

    com_printf(COM1_PORT, "FAT32: attempting mount vdrive=%d, LBA=%u -> handle=%d\n", 
               vdrive_id, partition_lba, handle);  


    if (vdrive_read_sector(vdrive_id, partition_lba, sector) != VDRIVE_SUCCESS) {
        VGA_Write("FAT32: failed to read boot sector\n");
        kfree(sector);
        return -2;
    }

    /*
     * Parse BPB early to validate the boot signature at the correct location.
     * The 0x55AA signature is located at the end of the sector.
     * For classic 512-byte sectors this is offset 510/511, but on 2048/4096-byte
     * sector devices (e.g. ATAPI/SATAPI vDrives) it moves.
     */
    fs->bytes_per_sector    = (uint16_t)sector[11] | ((uint16_t)sector[12] << 8);

    uint16_t bps = fs->bytes_per_sector;
    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) {
        /* If BPB is garbage, fall back to the classic check for better diagnostics. */
        bps = 512;
    }
    if (bps > 4096) bps = 4096;

    uint16_t sig_off = (uint16_t)(bps - 2);
    if (sector[sig_off] != 0x55 || sector[sig_off + 1] != 0xAA) {
        com_printf(COM1_PORT,
                   "FAT32: invalid boot signature (got 0x%x 0x%x at off=%u, bps=%u)\n",
                   sector[sig_off], sector[sig_off + 1], sig_off, bps);
        kfree(sector);
        return -3;
    }

    fs->sectors_per_cluster = sector[13];
    fs->reserved_sectors    = (uint16_t)sector[14] | ((uint16_t)sector[15] << 8);
    fs->num_fats            = sector[16];

    /* Validate before calculations */
    if (fs->bytes_per_sector == 0 || fs->sectors_per_cluster == 0 || fs->num_fats == 0) {
        VGA_Write("FAT32: invalid BPB values (zero)\n");
        kfree(sector);
        return -4;
    }

    if (fs->bytes_per_sector != 512 && fs->bytes_per_sector != 1024 && 
        fs->bytes_per_sector != 2048 && fs->bytes_per_sector != 4096) {
        com_printf(COM1_PORT, "FAT32: unusual bytes_per_sector=%u\n", fs->bytes_per_sector);
        kfree(sector);
        return -5;
    }

    if (fs->sectors_per_cluster > 128) {
        com_printf(COM1_PORT, "FAT32: suspiciously large sectors_per_cluster=%u\n", fs->sectors_per_cluster);
        kfree(sector);
        return -6;
    }

    /* Get total sectors */
    uint16_t total16 = (uint16_t)sector[19] | ((uint16_t)sector[20] << 8);
    uint32_t total32 = (uint32_t)sector[32] | ((uint32_t)sector[33] << 8) | 
                       ((uint32_t)sector[34] << 16) | ((uint32_t)sector[35] << 24);
    fs->total_sectors = total16 ? total16 : total32;

    /* Get sectors per FAT */
    uint16_t spf16 = (uint16_t)sector[22] | ((uint16_t)sector[23] << 8);
    uint32_t spf32 = (uint32_t)sector[36] | ((uint32_t)sector[37] << 8) | 
                     ((uint32_t)sector[38] << 16) | ((uint32_t)sector[39] << 24);
    fs->sectors_per_fat = spf16 ? spf16 : spf32;

    if (fs->sectors_per_fat == 0) {
        VGA_Write("FAT32: sectors_per_fat is 0!\n");
        kfree(sector);
        return -7;
    }

    /* Get root cluster */
    fs->root_cluster = (uint32_t)sector[44] | ((uint32_t)sector[45] << 8) | 
                       ((uint32_t)sector[46] << 16) | ((uint32_t)sector[47] << 24);

    if (fs->root_cluster < 2) {
        com_printf(COM1_PORT, "FAT32: invalid root_cluster=%u (must be >= 2)\n", fs->root_cluster);
        kfree(sector);
        return -8;
    }

    /* Calculate first data sector */
    fs->first_data_sector = fs->partition_lba + fs->reserved_sectors + 
                           ((uint32_t)fs->num_fats * fs->sectors_per_fat);

    /* Check cluster size */
    uint32_t clus_size = (uint32_t)fs->bytes_per_sector * (uint32_t)fs->sectors_per_cluster;
    if (clus_size > FAT32_MAX_CLUSTER_SIZE) {
        com_printf(COM1_PORT, "FAT32: cluster size %u > max %u\n", clus_size, FAT32_MAX_CLUSTER_SIZE);
        kfree(sector);
        return -9;
    }

    /* Mark as active */
    fs->active = 1;
    
    com_printf(COM1_PORT, "FAT32: mount successful! handle=%d, root_cluster=%u\n", handle, fs->root_cluster);
    kfree(sector);
    return handle;
}

int fat32_mount_auto(int vdrive_id) {
    fat32_init_once();
    
    int start = (vdrive_id >= 0) ? vdrive_id : 0;
    int end = (vdrive_id >= 0) ? vdrive_id : (vdrive_get_count() - 1);
    
    if (vdrive_id < 0) {
        VGA_Write("FAT32: scanning all vDrives...\n");
    }

    uint8_t *mbr = (uint8_t*)kmalloc(512);
    if (!mbr) return -1;
    memset(mbr, 0, 512);

    for (int d = start; d <= end; d++) {
        if (!vdrive_is_ready(d)) {
            continue;
        }
        
        com_printf(COM1_PORT, "FAT32: checking vDrive %d\n", d);
        
        // CHANGED: Use vDrive
        if (vdrive_read_sector(d, 0, mbr) != VDRIVE_SUCCESS) {
            com_printf(COM1_PORT, "FAT32: cannot read vDrive %d\n", d);
            continue;
        }

        if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
            continue;
        }

        /* Try MBR partitions */
        for (int i = 0; i < 4; i++) {
            uint32_t off = MBR_PARTITION_TABLE_OFFSET + i * PARTITION_ENTRY_SIZE;
            uint8_t type = mbr[off + 4];
            uint32_t lba = (uint32_t)mbr[off + 8] | ((uint32_t)mbr[off + 9] << 8) |
                           ((uint32_t)mbr[off + 10] << 16) | ((uint32_t)mbr[off + 11] << 24);
            
            if ((type == FAT32_TYPE_B || type == FAT32_TYPE_C) && lba > 0) {
                com_printf(COM1_PORT, "FAT32: found partition %d, type=0x%x, LBA=%u\n", i, type, lba);
                int handle = fat32_mount(d, lba);
                if (handle >= 0) {
                    kfree(mbr);
                    return handle;
                }
            }
        }

        /* Try superfloppy */
        int handle = fat32_mount(d, 0);
        if (handle >= 0) {
            kfree(mbr);
            return handle;
        }
    }

    VGA_Write("FAT32: no filesystem found\n");
    kfree(mbr);
    return -1;
}

void fat32_unmount(int handle) {
    if (fat32_valid_handle(handle)) {
        com_printf(COM1_PORT, "FAT32: unmounting handle %d\n", handle);
        memset(&fat32_mounts[handle], 0, sizeof(fat32_fs_t));
    }
}

void fat32_unmount_all(void) {
    for (int i = 0; i < FAT32_MAX_MOUNTS; i++) {
        if (fat32_mounts[i].active) {
            fat32_unmount(i);
        }
    }
}

/* --- HELPERS --- */
static uint32_t cluster_to_lba(const fat32_fs_t* fs, uint32_t clus) {
    if (clus < 2) return 0;
    return fs->first_data_sector + (clus - 2) * (uint32_t)fs->sectors_per_cluster;
}

int fat32_read_cluster(int handle, uint32_t cluster, void* buffer) {
    if (!fat32_valid_handle(handle)) return -1;
    if (cluster < 2 || cluster >= 0x0FFFFFF8) return -2;
    if (buffer == NULL) return -3;

    fat32_fs_t* fs = &fat32_mounts[handle];
    uint32_t lba = cluster_to_lba(fs, cluster);

    int result = vdrive_read(fs->vdrive_id, lba, fs->sectors_per_cluster, buffer);
    return (result == VDRIVE_SUCCESS) ? 0 : -1;
}

static int fat32_write_cluster(int handle, uint32_t cluster, const void* buffer) {
    if (!fat32_valid_handle(handle)) return -1;
    if (cluster < 2 || cluster >= 0x0FFFFFF8) return -2;
    if (buffer == NULL) return -3;

    fat32_fs_t* fs = &fat32_mounts[handle];
    uint32_t lba = cluster_to_lba(fs, cluster);

    int result = vdrive_write(fs->vdrive_id, lba, fs->sectors_per_cluster, buffer);
    return (result == VDRIVE_SUCCESS) ? 0 : -1;
}

int fat32_next_cluster(int handle, uint32_t cluster, uint32_t* out_next) {
    if (!fat32_valid_handle(handle)) return -1;
    if (out_next == NULL) return -2;
    
    fat32_fs_t* fs = &fat32_mounts[handle];
    if (fs->bytes_per_sector == 0) return -3;
    
    uint32_t fat_offset = cluster * 4U;
    uint32_t fat_sector = fs->partition_lba + fs->reserved_sectors + 
                          safe_divide(fat_offset, fs->bytes_per_sector);
    uint32_t ent_offset = fat_offset % fs->bytes_per_sector;

    uint8_t *sec = (uint8_t*)kmalloc(512);
    if (!sec) return -4;

    if (vdrive_read_sector(fs->vdrive_id, fat_sector, sec) != VDRIVE_SUCCESS) {
        kfree(sec);
        return -4;
    }

    uint32_t entry;
    if ((ent_offset + 4) <= fs->bytes_per_sector) {
        entry = (uint32_t)sec[ent_offset] |
                ((uint32_t)sec[ent_offset + 1] << 8) |
                ((uint32_t)sec[ent_offset + 2] << 16) |
                ((uint32_t)sec[ent_offset + 3] << 24);
    } else {
        /* Entry crosses sector boundary */
        uint8_t *sec2 = (uint8_t*)kmalloc(512);
        if (!sec2) { kfree(sec); return -5; }

        if (vdrive_read_sector(fs->vdrive_id, fat_sector + 1, sec2) != VDRIVE_SUCCESS) {
            kfree(sec2);
            kfree(sec);
            return -5;
        }

        uint8_t tmp[4];
        uint32_t bytes_from_first = fs->bytes_per_sector - ent_offset;
        for (uint32_t i = 0; i < bytes_from_first; i++) {
            tmp[i] = sec[ent_offset + i];
        }
        for (uint32_t i = 0; i < (4 - bytes_from_first); i++) {
            tmp[bytes_from_first + i] = sec2[i];
        }
        entry = (uint32_t)tmp[0] | ((uint32_t)tmp[1] << 8) | 
                ((uint32_t)tmp[2] << 16) | ((uint32_t)tmp[3] << 24);
        kfree(sec2);
    }

    entry &= 0x0FFFFFFF;
    *out_next = entry;
    kfree(sec);
    return 0;
}

/* Helper: Find directory entry by name in a cluster chain */
static int find_entry_in_cluster(const fat32_fs_t* fs, const uint8_t* buf,
                                 uint32_t clus_size, const char* name,
                                 struct fat_dir_entry* out_entry) {
    size_t entries = clus_size / 32;

    /* Temporary storage for collecting LFN entries preceding a short entry */
    /* maximum LFN entries for a single name is 20 (255 chars / 13 chars per entry ceiling) */
    const uint8_t* lfn_stack[20];
    int lfn_count = 0;

    for (size_t i = 0; i < entries; i++) {
        const uint8_t* e = buf + i * 32;
        uint8_t first = e[0];

        if (first == 0x00) return 0; /* End of directory */
        if (first == 0xE5) { lfn_count = 0; continue; } /* Deleted - reset any collected LFN */
        if ((e[11] & 0x0F) == 0x0F) {
            /* LFN entry: push onto stack (we will read them in reverse when we hit the short entry) */
            if (lfn_count < (int)(sizeof(lfn_stack) / sizeof(lfn_stack[0]))) {
                lfn_stack[lfn_count++] = e;
            } else {
                /* too many LFN parts - ignore and reset */
                lfn_count = 0;
            }
            continue;
        }

        /* Short (8.3) entry */
        /* Build candidate name: if we have LFN parts, assemble them; otherwise build 8.3 name */
        char entry_name[260]; /* allow long names up to 255 */
        int idx = 0;

        if (lfn_count > 0) {
            /* Assemble LFN from stack in reverse order */
            for (int part = lfn_count - 1; part >= 0; part--) {
                const uint8_t* le = lfn_stack[part];

                /* name1: offsets 1..10 (5 UTF-16 chars) */
                for (int off = 1; off <= 10; off += 2) {
                    uint16_t wc = (uint16_t)le[off] | ((uint16_t)le[off + 1] << 8);
                    if (wc == 0x0000 || wc == 0xFFFF) break;
                    /* if ASCII, take low byte, else replace with '?' */
                    if ((wc & 0xFF00) == 0x0000) entry_name[idx++] = (char)(wc & 0xFF);
                    else entry_name[idx++] = '?';
                    if (idx >= (int)sizeof(entry_name)-1) break;
                }
                if (idx >= (int)sizeof(entry_name)-1) break;

                /* name2: offsets 14..25 (6 UTF-16 chars) */
                for (int off = 14; off <= 25; off += 2) {
                    uint16_t wc = (uint16_t)le[off] | ((uint16_t)le[off + 1] << 8);
                    if (wc == 0x0000 || wc == 0xFFFF) break;
                    if ((wc & 0xFF00) == 0x0000) entry_name[idx++] = (char)(wc & 0xFF);
                    else entry_name[idx++] = '?';
                    if (idx >= (int)sizeof(entry_name)-1) break;
                }
                if (idx >= (int)sizeof(entry_name)-1) break;

                /* name3: offsets 28..31 (2 UTF-16 chars) */
                for (int off = 28; off <= 31; off += 2) {
                    uint16_t wc = (uint16_t)le[off] | ((uint16_t)le[off + 1] << 8);
                    if (wc == 0x0000 || wc == 0xFFFF) break;
                    if ((wc & 0xFF00) == 0x0000) entry_name[idx++] = (char)(wc & 0xFF);
                    else entry_name[idx++] = '?';
                    if (idx >= (int)sizeof(entry_name)-1) break;
                }
                if (idx >= (int)sizeof(entry_name)-1) break;
            }
            entry_name[idx] = '\0';
        } else {
            /* No LFN - build 8.3 name as before */
            for (int j = 0; j < 8; j++) {
                if (e[j] != ' ') entry_name[idx++] = e[j];
            }

            int has_ext = 0;
            for (int j = 8; j < 11; j++) if (e[j] != ' ') { has_ext = 1; break; }
            if (has_ext) {
                entry_name[idx++] = '.';
                for (int j = 8; j < 11; j++) {
                    if (e[j] != ' ') entry_name[idx++] = e[j];
                }
            }
            entry_name[idx] = '\0';
        }

        /* Case-insensitive compare with requested name */
        int match = 1;
        for (int k = 0; ; k++) {
            char c1 = entry_name[k];
            char c2 = name[k];
            if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
            if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
            if (c1 == '\0' && c2 == '\0') break;
            if (c1 != c2) { match = 0; break; }
        }

        if (match) {
            /* Copy the short directory entry into out_entry (the caller expects a short entry) */
            memcpy(out_entry, e, sizeof(struct fat_dir_entry));
            return 1;
        }

        /* Reset LFN stack for next entry */
        lfn_count = 0;
    }

    return 0;
}


/* Helper: Find entry by name in entire directory */
static int find_dir_entry(int handle, uint32_t dir_cluster, const char* name,
                          struct fat_dir_entry* out_entry) {
    if (!fat32_valid_handle(handle)) return -1;
    
    fat32_fs_t* fs = &fat32_mounts[handle];
    uint32_t clus_size = (uint32_t)fs->bytes_per_sector * (uint32_t)fs->sectors_per_cluster;
    void *buf = fat32_alloc_cluster_buffer(fs);
    if (!buf) return -3;
    
    uint32_t cluster = dir_cluster;
    int iterations = 0;
    
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        if (++iterations > 1000) return -2;
        
        if (fat32_read_cluster(handle, cluster, buf) != 0) { kfree(buf); return -3; }
        
        int result = find_entry_in_cluster(fs, buf, clus_size, name, out_entry);
        if (result == 1) { kfree(buf); return 0; } /* Found */
        if (result < 0) { kfree(buf); return result; }
        
        uint32_t next;
        if (fat32_next_cluster(handle, cluster, &next) != 0) { kfree(buf); return -4; }
        if (next >= 0x0FFFFFF8 || next == cluster) break;
        cluster = next;
    }
    
    kfree(buf);
    return -5; /* Not found */
}

/* Helper: List entries in a directory cluster chain */
static int list_directory_cluster(int handle, uint32_t dir_cluster) {
    if (!fat32_valid_handle(handle)) return -1;

    fat32_fs_t* fs = &fat32_mounts[handle];
    uint32_t clus_size = (uint32_t)fs->bytes_per_sector * (uint32_t)fs->sectors_per_cluster;

    if (clus_size == 0 || clus_size > FAT32_MAX_CLUSTER_SIZE) {
        return -2;
    }

    void *buf = fat32_alloc_cluster_buffer(fs);
    if (!buf) return -3;
    uint32_t cluster = dir_cluster;
    int iterations = 0;

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        if (++iterations > 1000) {
            VGA_Write("FAT32: directory too deep\n");
            return -3;
        }

        if (fat32_read_cluster(handle, cluster, buf) != 0) {
            kfree(buf);
            return -4;
        }

        size_t entries = clus_size / 32;

        /* LFN stack similar to find_entry_in_cluster: collect LFN entries that precede a short entry */
        const uint8_t* lfn_stack[20];
        int lfn_count = 0;

        for (size_t i = 0; i < entries; i++) {
            struct fat_dir_entry* e = (struct fat_dir_entry*)(buf + i * 32);
            uint8_t first = (uint8_t)e->name[0];

            if (first == 0x00) return 0; /* End of directory */
            if (first == 0xE5) { lfn_count = 0; continue; } /* Deleted */
            if ((e->attr & 0x0F) == 0x0F) {
                /* LFN part - push */
                if (lfn_count < (int)(sizeof(lfn_stack) / sizeof(lfn_stack[0]))) {
                    lfn_stack[lfn_count++] = (const uint8_t*)e;
                } else {
                    lfn_count = 0;
                }
                continue;
            }

            /* Short entry - build display name using LFN if present */
            char name_buf[260];
            int idx = 0;

            if (lfn_count > 0) {
                for (int part = lfn_count - 1; part >= 0; part--) {
                    const uint8_t* le = lfn_stack[part];

                    for (int off = 1; off <= 10; off += 2) {
                        uint16_t wc = (uint16_t)le[off] | ((uint16_t)le[off + 1] << 8);
                        if (wc == 0x0000 || wc == 0xFFFF) break;
                        if ((wc & 0xFF00) == 0x0000) name_buf[idx++] = (char)(wc & 0xFF);
                        else name_buf[idx++] = '?';
                        if (idx >= (int)sizeof(name_buf)-1) break;
                    }
                    if (idx >= (int)sizeof(name_buf)-1) break;

                    for (int off = 14; off <= 25; off += 2) {
                        uint16_t wc = (uint16_t)le[off] | ((uint16_t)le[off + 1] << 8);
                        if (wc == 0x0000 || wc == 0xFFFF) break;
                        if ((wc & 0xFF00) == 0x0000) name_buf[idx++] = (char)(wc & 0xFF);
                        else name_buf[idx++] = '?';
                        if (idx >= (int)sizeof(name_buf)-1) break;
                    }
                    if (idx >= (int)sizeof(name_buf)-1) break;

                    for (int off = 28; off <= 31; off += 2) {
                        uint16_t wc = (uint16_t)le[off] | ((uint16_t)le[off + 1] << 8);
                        if (wc == 0x0000 || wc == 0xFFFF) break;
                        if ((wc & 0xFF00) == 0x0000) name_buf[idx++] = (char)(wc & 0xFF);
                        else name_buf[idx++] = '?';
                        if (idx >= (int)sizeof(name_buf)-1) break;
                    }
                    if (idx >= (int)sizeof(name_buf)-1) break;
                }
                name_buf[idx] = '\0';
            } else {
                /* Build 8.3 name */
                for (int j = 0; j < 8; j++) {
                    if (e->name[j] != ' ') name_buf[idx++] = e->name[j];
                }
                int has_ext = 0;
                for (int j = 8; j < 11; j++) if (e->name[j] != ' ') { has_ext = 1; break; }
                if (has_ext) {
                    name_buf[idx++] = '.';
                    for (int j = 8; j < 11; j++) {
                        if (e->name[j] != ' ') name_buf[idx++] = e->name[j];
                    }
                }
                name_buf[idx] = '\0';
            }

            com_printf(COM1_PORT, "  %s %s size=%u\n",
                       name_buf,
                       (e->attr & 0x10) ? "<DIR>" : "",
                       e->filesize);

            /* Reset LFN stack after consuming it */
            lfn_count = 0;
        }

        uint32_t next;
        if (fat32_next_cluster(handle, cluster, &next) != 0) {
            return -5;
        }

        if (next >= 0x0FFFFFF8 || next == cluster) break;
        cluster = next;
    }

    return 0;
}

/* Public API: List any directory by path */
int fat32_list_directory(int handle, const char* path) {
    if (!fat32_valid_handle(handle)) {
        VGA_Write("FAT32: invalid handle\n");
        return -1;
    }

    fat32_fs_t* fs = &fat32_mounts[handle];

    /* Handle root or empty path */
    if (path == NULL || path[0] == '\0' ||
        (path[0] == '/' && path[1] == '\0')) {
        com_printf(COM1_PORT, "FAT32 root directory (handle %d):\n", handle);
        return list_directory_cluster(handle, fs->root_cluster);
    }

    uint32_t current_cluster = fs->root_cluster;
    char component[260]; /* allow full LFN component */
    int comp_idx = 0;
    int path_idx = 0;

    /* Skip leading slash */
    if (path[0] == '/') path_idx++;

    while (path[path_idx] != '\0') {
        /* Extract path component (up to next '/') */
        comp_idx = 0;
        while (path[path_idx] != '\0' && path[path_idx] != '/' &&
               comp_idx < (int)sizeof(component) - 1) {
            component[comp_idx++] = path[path_idx++];
        }
        component[comp_idx] = '\0';

        if (comp_idx == 0) {
            if (path[path_idx] == '/') path_idx++;
            continue;
        }

        /* Find entry in the current directory (supports LFN now) */
        struct fat_dir_entry entry;
        int found = 0;

        /* Read each cluster in the directory and search */
        uint32_t cluster = current_cluster;
        uint32_t clus_size = (uint32_t)fs->bytes_per_sector * (uint32_t)fs->sectors_per_cluster;
        void *buf = fat32_alloc_cluster_buffer(fs);
        if (!buf) return -4;

        while (cluster >= 2 && cluster < 0x0FFFFFF8) {
            if (fat32_read_cluster(handle, cluster, buf) != 0) {
                com_printf(COM1_PORT, "FAT32: failed to read cluster %u\n", cluster);
                kfree(buf);
                return -4;
            }

            if (find_entry_in_cluster(fs, buf, clus_size, component, &entry)) {
                found = 1;
                break;
            }

            uint32_t next;
            if (fat32_next_cluster(handle, cluster, &next) != 0) {
                kfree(buf);
                return -5;
            }

            if (next >= 0x0FFFFFF8 || next == cluster) break;
            cluster = next;
        }

        if (!found) {
            com_printf(COM1_PORT, "FAT32: path component '%s' not found\n", component);
            kfree(buf);
            return -2;
        }

        /* If itâ€™s not a directory but more path remains â†’ invalid */
        if (!(entry.attr & 0x10) && path[path_idx] != '\0') {
            com_printf(COM1_PORT, "FAT32: '%s' is not a directory\n", component);
            kfree(buf);
            return -3;
        }

        /* Update current cluster to the directoryâ€™s cluster */
        current_cluster = ((uint32_t)entry.first_cluster_high << 16) |
                          (uint32_t)entry.first_cluster_low;

        /* Skip slash before next component */
        if (path[path_idx] == '/') path_idx++;

        kfree(buf);
    }

    /* List the final directory */
    com_printf(COM1_PORT, "FAT32 directory '%s' (handle %d):\n", path, handle);
    return list_directory_cluster(handle, current_cluster);
}

int fat32_directory_exists(int handle, const char* path) {
    if (!fat32_valid_handle(handle)) return 0;
    
    fat32_fs_t* fs = &fat32_mounts[handle];
    
    /* Handle root */
    if (path == NULL || path[0] == '\0' || 
        (path[0] == '/' && path[1] == '\0')) {
        return 1; /* Root always exists */
    }
    
    /* Parse path and traverse */
    uint32_t current_cluster = fs->root_cluster;
    char component[13];
    int comp_idx = 0;
    int path_idx = 0;
    
    if (path[0] == '/') path_idx++;
    
    while (path[path_idx] != '\0') {
        comp_idx = 0;
        while (path[path_idx] != '\0' && path[path_idx] != '/' && comp_idx < 12) {
            component[comp_idx++] = path[path_idx++];
        }
        component[comp_idx] = '\0';
        
        if (comp_idx == 0) {
            if (path[path_idx] == '/') path_idx++;
            continue;
        }
        
        /* Find this component */
        struct fat_dir_entry entry;
        if (find_dir_entry(handle, current_cluster, component, &entry) != 0) {
            return 0; /* Not found */
        }
        
        /* Check if it's a directory */
        if (!(entry.attr & 0x10)) {
            return 0; /* Not a directory */
        }
        
        current_cluster = ((uint32_t)entry.first_cluster_high << 16) | 
                          (uint32_t)entry.first_cluster_low;
        
        if (path[path_idx] == '/') path_idx++;
    }
    
    return 1; /* Path exists and is a directory */
}

/* Read folder entries for iteration (LFN-aware). */
static int fat32_write_cluster(int handle, uint32_t cluster, const void* buffer);

int fat32_read_folder(int handle, const char* path, fat32_folder_entry_t* entries, int max_entries) {
    if (!fat32_valid_handle(handle) || !entries || max_entries <= 0) {
        return -1;
    }

    fat32_fs_t* fs = &fat32_mounts[handle];

    uint32_t dir_cluster = fs->root_cluster;
    if (path && !(path[0] == 0 || (path[0] == '/' && path[1] == 0))) {
        struct fat_dir_entry dir_entry;
        if (fat32_find_file(handle, path, &dir_entry) != 0) {
            return -2;
        }
        if (!(dir_entry.attr & 0x10)) {
            return -3; /* Not a directory */
        }
        dir_cluster = ((uint32_t)dir_entry.first_cluster_high << 16) | (uint32_t)dir_entry.first_cluster_low;
        if (dir_cluster < 2) dir_cluster = fs->root_cluster;
    }

    uint32_t clus_size = (uint32_t)fs->bytes_per_sector * (uint32_t)fs->sectors_per_cluster;
    if (clus_size == 0 || clus_size > FAT32_MAX_CLUSTER_SIZE) {
        return -4;
    }

    void* cluster_buf = fat32_alloc_cluster_buffer(fs);
    if (!cluster_buf) {
        return -5;
    }

    int out_count = 0;
    uint32_t cluster = dir_cluster;
    int guard = 0;

    // LFN stack for assembling long names preceding a short entry
    const uint8_t* lfn_stack[20];
    int lfn_count = 0;

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        if (++guard > 1000) { kfree(cluster_buf); return -6; }

        if (fat32_read_cluster(handle, cluster, cluster_buf) != 0) {
            kfree(cluster_buf);
            return -7;
        }

        size_t n_entries = clus_size / 32;
        for (size_t i = 0; i < n_entries && out_count < max_entries; i++) {
            struct fat_dir_entry* e = (struct fat_dir_entry*)((uint8_t*)cluster_buf + i * 32);
            uint8_t first = (uint8_t)e->name[0];

            if (first == 0x00) { kfree(cluster_buf); return out_count; } // end
            if (first == 0xE5) { lfn_count = 0; continue; } // deleted

            if ((e->attr & 0x0F) == 0x0F) {
                // LFN entry
                if (lfn_count < (int)(sizeof(lfn_stack) / sizeof(lfn_stack[0]))) {
                    lfn_stack[lfn_count++] = (const uint8_t*)e;
                } else {
                    lfn_count = 0;
                }
                continue;
            }

            // Skip volume labels
            if (e->attr & 0x08) { lfn_count = 0; continue; }

            // Build display name (LFN if present, otherwise 8.3)
            char name_buf[260];
            int idx = 0;

            if (lfn_count > 0) {
                for (int part = lfn_count - 1; part >= 0; part--) {
                    const uint8_t* le = lfn_stack[part];

                    for (int off = 1; off <= 10; off += 2) {
                        uint16_t wc = (uint16_t)le[off] | ((uint16_t)le[off + 1] << 8);
                        if (wc == 0x0000 || wc == 0xFFFF) break;
                        name_buf[idx++] = ((wc & 0xFF00) == 0x0000) ? (char)(wc & 0xFF) : '?';
                        if (idx >= (int)sizeof(name_buf) - 1) break;
                    }
                    if (idx >= (int)sizeof(name_buf) - 1) break;

                    for (int off = 14; off <= 25; off += 2) {
                        uint16_t wc = (uint16_t)le[off] | ((uint16_t)le[off + 1] << 8);
                        if (wc == 0x0000 || wc == 0xFFFF) break;
                        name_buf[idx++] = ((wc & 0xFF00) == 0x0000) ? (char)(wc & 0xFF) : '?';
                        if (idx >= (int)sizeof(name_buf) - 1) break;
                    }
                    if (idx >= (int)sizeof(name_buf) - 1) break;

                    for (int off = 28; off <= 31; off += 2) {
                        uint16_t wc = (uint16_t)le[off] | ((uint16_t)le[off + 1] << 8);
                        if (wc == 0x0000 || wc == 0xFFFF) break;
                        name_buf[idx++] = ((wc & 0xFF00) == 0x0000) ? (char)(wc & 0xFF) : '?';
                        if (idx >= (int)sizeof(name_buf) - 1) break;
                    }
                    if (idx >= (int)sizeof(name_buf) - 1) break;
                }
                name_buf[idx] = 0;
            } else {
                for (int j = 0; j < 8; j++) {
                    if (e->name[j] != ' ') name_buf[idx++] = e->name[j];
                }
                int has_ext = 0;
                for (int j = 8; j < 11; j++) if (e->name[j] != ' ') { has_ext = 1; break; }
                if (has_ext) {
                    name_buf[idx++] = '.';
                    for (int j = 8; j < 11; j++) {
                        if (e->name[j] != ' ') name_buf[idx++] = e->name[j];
                    }
                }
                name_buf[idx] = 0;
            }

            // Skip . and ..
            if (name_buf[0] == '.' && (name_buf[1] == 0 || (name_buf[1] == '.' && name_buf[2] == 0))) {
                lfn_count = 0;
                continue;
            }

            fat32_folder_entry_t* out = &entries[out_count++];
            strncpy(out->name, name_buf, sizeof(out->name) - 1);
            out->name[sizeof(out->name) - 1] = 0;
            out->size = e->filesize;
            out->first_cluster = ((uint32_t)e->first_cluster_high << 16) | (uint32_t)e->first_cluster_low;
            out->is_directory = (e->attr & 0x10) ? 1 : 0;
            out->is_hidden = (e->attr & 0x02) ? 1 : 0;
            out->is_system = (e->attr & 0x04) ? 1 : 0;

            lfn_count = 0;
        }

        uint32_t next;
        if (fat32_next_cluster(handle, cluster, &next) != 0) break;
        if (next >= 0x0FFFFFF8 || next == cluster) break;
        cluster = next;
    }

    kfree(cluster_buf);
    return out_count;
}

// Find entry index in a directory cluster buffer by long/short name.
static int find_entry_in_cluster_index(const fat32_fs_t* fs, const uint8_t* buf,
                                      uint32_t clus_size, const char* name,
                                      struct fat_dir_entry* out_entry,
                                      uint32_t *out_index) {
    size_t entries = clus_size / 32;
    const uint8_t* lfn_stack[20];
    int lfn_count = 0;

    for (size_t i = 0; i < entries; i++) {
        const uint8_t* e = buf + i * 32;
        uint8_t first = e[0];

        if (first == 0x00) return 0;
        if (first == 0xE5) { lfn_count = 0; continue; }

        if ((e[11] & 0x0F) == 0x0F) {
            if (lfn_count < (int)(sizeof(lfn_stack) / sizeof(lfn_stack[0]))) lfn_stack[lfn_count++] = e;
            else lfn_count = 0;
            continue;
        }

        char entry_name[260];
        int idx = 0;
        if (lfn_count > 0) {
            for (int part = lfn_count - 1; part >= 0; part--) {
                const uint8_t* le = lfn_stack[part];
                for (int off = 1; off <= 10; off += 2) {
                    uint16_t wc = (uint16_t)le[off] | ((uint16_t)le[off + 1] << 8);
                    if (wc == 0x0000 || wc == 0xFFFF) break;
                    entry_name[idx++] = ((wc & 0xFF00) == 0) ? (char)(wc & 0xFF) : '?';
                    if (idx >= (int)sizeof(entry_name)-1) break;
                }
                if (idx >= (int)sizeof(entry_name)-1) break;
                for (int off = 14; off <= 25; off += 2) {
                    uint16_t wc = (uint16_t)le[off] | ((uint16_t)le[off + 1] << 8);
                    if (wc == 0x0000 || wc == 0xFFFF) break;
                    entry_name[idx++] = ((wc & 0xFF00) == 0) ? (char)(wc & 0xFF) : '?';
                    if (idx >= (int)sizeof(entry_name)-1) break;
                }
                if (idx >= (int)sizeof(entry_name)-1) break;
                for (int off = 28; off <= 31; off += 2) {
                    uint16_t wc = (uint16_t)le[off] | ((uint16_t)le[off + 1] << 8);
                    if (wc == 0x0000 || wc == 0xFFFF) break;
                    entry_name[idx++] = ((wc & 0xFF00) == 0) ? (char)(wc & 0xFF) : '?';
                    if (idx >= (int)sizeof(entry_name)-1) break;
                }
                if (idx >= (int)sizeof(entry_name)-1) break;
            }
            entry_name[idx] = 0;
        } else {
            for (int j = 0; j < 8; j++) if (e[j] != ' ') entry_name[idx++] = e[j];
            int has_ext = 0;
            for (int j = 8; j < 11; j++) if (e[j] != ' ') { has_ext = 1; break; }
            if (has_ext) {
                entry_name[idx++] = '.';
                for (int j = 8; j < 11; j++) if (e[j] != ' ') entry_name[idx++] = e[j];
            }
            entry_name[idx] = 0;
        }

        int match = 1;
        for (int k = 0;; k++) {
            char c1 = entry_name[k];
            char c2 = name[k];
            if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
            if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
            if (c1 == 0 && c2 == 0) break;
            if (c1 != c2) { match = 0; break; }
        }

        if (match) {
            if (out_entry) memcpy(out_entry, e, sizeof(struct fat_dir_entry));
            if (out_index) *out_index = (uint32_t)i;
            return 1;
        }

        lfn_count = 0;
    }

    return 0;
}

// --- FAT32 WRITE HELPERS ---

// On-disk LFN directory entry (FAT long filename)
struct __attribute__((packed)) fat_lfn_entry {
    uint8_t ord;
    uint16_t name1[5];
    uint8_t attr;        // 0x0F
    uint8_t type;        // 0
    uint8_t checksum;
    uint16_t name2[6];
    uint16_t first_cluster_low; // 0
    uint16_t name3[2];
};

static uint8_t fat32_lfn_checksum(const uint8_t short_name[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + short_name[i]);
    }
    return sum;
}

static int fat32_build_short_name_candidate(const char *long_name, int suffix, uint8_t out11[11]) {
    // Generates an uppercase 8.3 candidate. If suffix>0, uses ~N.
    if (!long_name || !long_name[0] || !out11) return -1;

    const char *dot = NULL;
    for (const char *p = long_name; *p; p++) if (*p == '.') dot = p;

    char base[9];
    char ext[4];
    memset(base, 0, sizeof(base));
    memset(ext, 0, sizeof(ext));

    int bi = 0;
    for (const char *p = long_name; *p && p != dot; p++) {
        char c = *p;
        if (c == ' ' || c == '\t') continue;
        if (c == '/' || c == '\\') return -1;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) c = '_';
        if (c >= 'a' && c <= 'z') c -= 32;
        if (bi < 8) base[bi++] = c;
    }
    if (bi == 0) return -1;

    if (dot && dot[1]) {
        int ei = 0;
        for (const char *p = dot + 1; *p && ei < 3; p++) {
            char c = *p;
            if (c == ' ' || c == '\t') continue;
            if (c == '/' || c == '\\') return -1;
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) c = '_';
            if (c >= 'a' && c <= 'z') c -= 32;
            ext[ei++] = c;
        }
    }

    char final_base[9];
    memset(final_base, ' ', 8);
    final_base[8] = 0;

    if (suffix <= 0) {
        for (int i = 0; i < 8; i++) final_base[i] = (base[i] ? base[i] : ' ');
    } else {
        char sufbuf[6];
        itoa(suffix, sufbuf, 10);
        int suflen = (int)strlen(sufbuf);
        if (suflen <= 0) suflen = 1;
        int keep = 8 - (1 + suflen);
        if (keep < 1) keep = 1;
        for (int i = 0; i < keep; i++) final_base[i] = (base[i] ? base[i] : ' ');
        final_base[keep] = '~';
        for (int i = 0; i < suflen && keep + 1 + i < 8; i++) final_base[keep + 1 + i] = sufbuf[i];
    }

    for (int i = 0; i < 8; i++) out11[i] = (uint8_t)final_base[i];
    for (int i = 0; i < 3; i++) out11[8 + i] = (uint8_t)(ext[i] ? ext[i] : ' ');
    return 0;
}

static int fat32_read_fat_entry(int handle, uint32_t cluster, uint32_t *out_val) {
    if (!out_val) return -1;
    *out_val = 0;
    if (!fat32_valid_handle(handle)) return -1;

    fat32_fs_t *fs = &fat32_mounts[handle];
    if (cluster < 2) return -2;

    uint32_t fat_offset = cluster * 4U;
    uint32_t fat_sector = fs->partition_lba + fs->reserved_sectors + safe_divide(fat_offset, fs->bytes_per_sector);
    uint32_t ent_offset = fat_offset % fs->bytes_per_sector;

    uint8_t *sec = (uint8_t*)kmalloc(512);
    if (!sec) return -3;

    if (vdrive_read_sector(fs->vdrive_id, fat_sector, sec) != VDRIVE_SUCCESS) { kfree(sec); return -4; }

    uint32_t entry;
    if ((ent_offset + 4) <= fs->bytes_per_sector) {
        entry = (uint32_t)sec[ent_offset] |
                ((uint32_t)sec[ent_offset + 1] << 8) |
                ((uint32_t)sec[ent_offset + 2] << 16) |
                ((uint32_t)sec[ent_offset + 3] << 24);
    } else {
        uint8_t *sec2 = (uint8_t*)kmalloc(512);
        if (!sec2) { kfree(sec); return -3; }
        if (vdrive_read_sector(fs->vdrive_id, fat_sector + 1, sec2) != VDRIVE_SUCCESS) { kfree(sec2); kfree(sec); return -4; }
        uint8_t tmp[4];
        uint32_t bytes_from_first = fs->bytes_per_sector - ent_offset;
        for (uint32_t i = 0; i < bytes_from_first; i++) tmp[i] = sec[ent_offset + i];
        for (uint32_t i = 0; i < (4 - bytes_from_first); i++) tmp[bytes_from_first + i] = sec2[i];
        entry = (uint32_t)tmp[0] | ((uint32_t)tmp[1] << 8) | ((uint32_t)tmp[2] << 16) | ((uint32_t)tmp[3] << 24);
        kfree(sec2);
    }

    kfree(sec);
    entry &= 0x0FFFFFFF;
    *out_val = entry;
    return 0;
}

static int fat32_write_fat_entry(int handle, uint32_t cluster, uint32_t value) {
    if (!fat32_valid_handle(handle)) return -1;

    fat32_fs_t *fs = &fat32_mounts[handle];
    if (cluster < 2) return -2;

    value &= 0x0FFFFFFF;

    uint32_t fat_offset = cluster * 4U;
    uint32_t fat_sector_rel = safe_divide(fat_offset, fs->bytes_per_sector);
    uint32_t ent_offset = fat_offset % fs->bytes_per_sector;

    for (uint32_t fat = 0; fat < fs->num_fats; fat++) {
        uint32_t fat_sector = fs->partition_lba + fs->reserved_sectors + fat_sector_rel + fat * fs->sectors_per_fat;

        uint8_t *sec = (uint8_t*)kmalloc(512);
        if (!sec) return -3;
        if (vdrive_read_sector(fs->vdrive_id, fat_sector, sec) != VDRIVE_SUCCESS) { kfree(sec); return -4; }

        if ((ent_offset + 4) <= fs->bytes_per_sector) {
            sec[ent_offset]     = (uint8_t)(value & 0xFF);
            sec[ent_offset + 1] = (uint8_t)((value >> 8) & 0xFF);
            sec[ent_offset + 2] = (uint8_t)((value >> 16) & 0xFF);
            sec[ent_offset + 3] = (uint8_t)((value >> 24) & 0xFF);
            if (vdrive_write_sector(fs->vdrive_id, fat_sector, sec) != VDRIVE_SUCCESS) { kfree(sec); return -5; }
        } else {
            uint8_t *sec2 = (uint8_t*)kmalloc(512);
            if (!sec2) { kfree(sec); return -3; }
            if (vdrive_read_sector(fs->vdrive_id, fat_sector + 1, sec2) != VDRIVE_SUCCESS) { kfree(sec2); kfree(sec); return -4; }

            uint8_t tmp[4];
            tmp[0] = (uint8_t)(value & 0xFF);
            tmp[1] = (uint8_t)((value >> 8) & 0xFF);
            tmp[2] = (uint8_t)((value >> 16) & 0xFF);
            tmp[3] = (uint8_t)((value >> 24) & 0xFF);

            uint32_t bytes_from_first = fs->bytes_per_sector - ent_offset;
            for (uint32_t i = 0; i < bytes_from_first; i++) sec[ent_offset + i] = tmp[i];
            for (uint32_t i = 0; i < (4 - bytes_from_first); i++) sec2[i] = tmp[bytes_from_first + i];

            if (vdrive_write_sector(fs->vdrive_id, fat_sector, sec) != VDRIVE_SUCCESS) { kfree(sec2); kfree(sec); return -5; }
            if (vdrive_write_sector(fs->vdrive_id, fat_sector + 1, sec2) != VDRIVE_SUCCESS) { kfree(sec2); kfree(sec); return -5; }

            kfree(sec2);
        }

        kfree(sec);
    }

    return 0;
}

static int fat32_zero_cluster(int handle, uint32_t cluster) {
    if (!fat32_valid_handle(handle)) return -1;
    fat32_fs_t *fs = &fat32_mounts[handle];
    uint32_t clus_size = (uint32_t)fs->bytes_per_sector * (uint32_t)fs->sectors_per_cluster;
    void *buf = kmalloc(clus_size);
    if (!buf) return -2;
    memset(buf, 0, clus_size);
    int rc = fat32_write_cluster(handle, cluster, buf);
    kfree(buf);
    return rc;
}

static int fat32_find_free_cluster(int handle, uint32_t *out_cluster) {
    if (!out_cluster) return -1;
    *out_cluster = 0;

    if (!fat32_valid_handle(handle)) return -1;
    fat32_fs_t *fs = &fat32_mounts[handle];

    // conservative maximum cluster number from volume size.
    uint32_t data_sectors = 0;
    if (fs->total_sectors > (fs->first_data_sector - fs->partition_lba)) {
        data_sectors = fs->total_sectors - (fs->first_data_sector - fs->partition_lba);
    }
    uint32_t data_clusters = safe_divide(data_sectors, fs->sectors_per_cluster);
    uint32_t maxc = data_clusters + 1;
    if (maxc < 2) return -2;

    for (uint32_t c = 2; c <= maxc; c++) {
        uint32_t v = 0;
        if (fat32_read_fat_entry(handle, c, &v) != 0) return -3;
        if (v == 0) { *out_cluster = c; return 0; }
    }

    return -4;
}

static int fat32_free_cluster_chain(int handle, uint32_t first_cluster) {
    if (!fat32_valid_handle(handle)) return -1;
    uint32_t c = first_cluster;
    int guard = 0;

    while (c >= 2 && c < 0x0FFFFFF8) {
        if (++guard > 200000) return -2;
        uint32_t next = 0;
        if (fat32_read_fat_entry(handle, c, &next) != 0) return -3;
        if (fat32_write_fat_entry(handle, c, 0) != 0) return -4;
        if (next >= 0x0FFFFFF8 || next == c) break;
        c = next;
    }

    return 0;
}

static int fat32_alloc_cluster_chain(int handle, uint32_t clusters, uint32_t *out_first) {
    if (!out_first) return -1;
    *out_first = 0;
    if (!fat32_valid_handle(handle)) return -1;

    if (clusters == 0) return 0;

    uint32_t first = 0;
    uint32_t prev = 0;

    for (uint32_t i = 0; i < clusters; i++) {
        uint32_t c = 0;
        if (fat32_find_free_cluster(handle, &c) != 0) {
            if (first) (void)fat32_free_cluster_chain(handle, first);
            return -2;
        }

        if (fat32_write_fat_entry(handle, c, 0x0FFFFFFF) != 0) {
            if (first) (void)fat32_free_cluster_chain(handle, first);
            return -3;
        }

        if (!first) first = c;
        if (prev) {
            if (fat32_write_fat_entry(handle, prev, c) != 0) {
                if (first) (void)fat32_free_cluster_chain(handle, first);
                return -3;
            }
        }

        prev = c;
        (void)fat32_zero_cluster(handle, c);
    }

    *out_first = first;
    return 0;
}

static int fat32_short_name_exists_in_dir(int handle, uint32_t dir_cluster, const uint8_t short11[11]) {
    if (!fat32_valid_handle(handle) || !short11) return 0;
    fat32_fs_t *fs = &fat32_mounts[handle];
    uint32_t clus_size = (uint32_t)fs->bytes_per_sector * (uint32_t)fs->sectors_per_cluster;
    void *buf = fat32_alloc_cluster_buffer(fs);
    if (!buf) return 0;

    uint32_t cluster = dir_cluster;
    int guard = 0;
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        if (++guard > 100000) break;
        if (fat32_read_cluster(handle, cluster, buf) != 0) break;
        size_t entries = clus_size / 32;
        for (size_t i = 0; i < entries; i++) {
            uint8_t *e = (uint8_t*)buf + i * 32;
            uint8_t first = e[0];
            if (first == 0x00) { kfree(buf); return 0; }
            if (first == 0xE5) continue;
            if ((e[11] & 0x0F) == 0x0F) continue;
            if (memcmp(e, short11, 11) == 0) { kfree(buf); return 1; }
        }

        uint32_t next;
        if (fat32_next_cluster(handle, cluster, &next) != 0) break;
        if (next >= 0x0FFFFFF8 || next == cluster) break;
        cluster = next;
    }

    kfree(buf);
    return 0;
}

static int fat32_find_free_dir_slots(int handle, uint32_t dir_cluster, uint32_t needed, uint32_t *out_cluster, uint32_t *out_index) {
    if (!out_cluster || !out_index) return -1;
    *out_cluster = 0;
    *out_index = 0;

    if (!fat32_valid_handle(handle)) return -1;
    fat32_fs_t *fs = &fat32_mounts[handle];
    uint32_t clus_size = (uint32_t)fs->bytes_per_sector * (uint32_t)fs->sectors_per_cluster;

    void *buf = fat32_alloc_cluster_buffer(fs);
    if (!buf) return -2;

    uint32_t cluster = dir_cluster;
    uint32_t prev = 0;
    int guard = 0;

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        if (++guard > 100000) { kfree(buf); return -3; }
        if (fat32_read_cluster(handle, cluster, buf) != 0) { kfree(buf); return -4; }

        size_t entries = clus_size / 32;
        uint32_t run = 0;
        uint32_t run_start = 0;

        for (uint32_t i = 0; i < (uint32_t)entries; i++) {
            uint8_t first = ((uint8_t*)buf)[i * 32];
            if (first == 0x00 || first == 0xE5) {
                if (run == 0) run_start = i;
                run++;
                if (run >= needed) {
                    *out_cluster = cluster;
                    *out_index = run_start;
                    kfree(buf);
                    return 0;
                }
            } else {
                run = 0;
            }
        }

        prev = cluster;
        uint32_t next;
        if (fat32_next_cluster(handle, cluster, &next) != 0) break;
        if (next >= 0x0FFFFFF8 || next == cluster) break;
        cluster = next;
    }

    uint32_t newc = 0;
    if (fat32_find_free_cluster(handle, &newc) != 0) { kfree(buf); return -5; }
    if (fat32_write_fat_entry(handle, newc, 0x0FFFFFFF) != 0) { kfree(buf); return -6; }
    if (prev) {
        if (fat32_write_fat_entry(handle, prev, newc) != 0) { kfree(buf); return -6; }
    }
    (void)fat32_zero_cluster(handle, newc);

    *out_cluster = newc;
    *out_index = 0;
    kfree(buf);
    return 0;
}

static int fat32_update_file_entry(int handle, const char *path, uint32_t new_first_cluster, uint32_t new_size) {
    if (!fat32_valid_handle(handle) || !path) return -1;

    fat32_fs_t *fs = &fat32_mounts[handle];
    uint32_t clus_size = (uint32_t)fs->bytes_per_sector * (uint32_t)fs->sectors_per_cluster;

    const char *leaf = path;
    const char *last_slash = NULL;
    for (const char *p = path; *p; p++) if (*p == '/') last_slash = p;

    char parent[256];
    if (last_slash) {
        size_t plen = (size_t)(last_slash - path);
        if (plen == 0) strcpy(parent, "/");
        else {
            if (plen >= sizeof(parent)) plen = sizeof(parent) - 1;
            memcpy(parent, path, plen);
            parent[plen] = 0;
        }
        leaf = last_slash + 1;
    } else {
        strcpy(parent, "/");
    }

    uint32_t dir_cluster = fs->root_cluster;
    if (!(parent[0] == '/' && parent[1] == 0)) {
        struct fat_dir_entry de;
        if (fat32_find_file(handle, parent, &de) != 0) return -2;
        if (de.attr & 0x10) {
            dir_cluster = ((uint32_t)de.first_cluster_high << 16) | (uint32_t)de.first_cluster_low;
            if (dir_cluster < 2) dir_cluster = fs->root_cluster;
        }
    }

    void *dirbuf = fat32_alloc_cluster_buffer(fs);
    if (!dirbuf) return -3;

    uint32_t dcl = dir_cluster;
    int guard = 0;
    while (dcl >= 2 && dcl < 0x0FFFFFF8) {
        if (++guard > 100000) break;
        if (fat32_read_cluster(handle, dcl, dirbuf) != 0) break;

        uint32_t idx;
        struct fat_dir_entry ent;
        if (find_entry_in_cluster_index(fs, (const uint8_t*)dirbuf, clus_size, leaf, &ent, &idx) == 1) {
            ent.filesize = new_size;
            ent.first_cluster_high = (uint16_t)((new_first_cluster >> 16) & 0xFFFF);
            ent.first_cluster_low = (uint16_t)(new_first_cluster & 0xFFFF);
            memcpy((uint8_t*)dirbuf + idx * 32, &ent, sizeof(ent));
            (void)fat32_write_cluster(handle, dcl, dirbuf);
            break;
        }

        uint32_t next;
        if (fat32_next_cluster(handle, dcl, &next) != 0) break;
        if (next >= 0x0FFFFFF8 || next == dcl) break;
        dcl = next;
    }

    kfree(dirbuf);
    return 0;
}

static int fat32_create_dir_entry_for_file(int handle, const char *path, uint32_t first_cluster, uint32_t size) {
    if (!fat32_valid_handle(handle) || !path) return -1;

    fat32_fs_t *fs = &fat32_mounts[handle];

    const char *leaf = path;
    const char *last_slash = NULL;
    for (const char *p = path; *p; p++) if (*p == '/') last_slash = p;

    char parent[256];
    if (last_slash) {
        size_t plen = (size_t)(last_slash - path);
        if (plen == 0) strcpy(parent, "/");
        else {
            if (plen >= sizeof(parent)) plen = sizeof(parent) - 1;
            memcpy(parent, path, plen);
            parent[plen] = 0;
        }
        leaf = last_slash + 1;
    } else {
        strcpy(parent, "/");
    }

    if (!leaf || !*leaf) return -2;

    uint32_t dir_cluster = fs->root_cluster;
    if (!(parent[0] == '/' && parent[1] == 0)) {
        struct fat_dir_entry de;
        if (fat32_find_file(handle, parent, &de) != 0) return -3;
        if (!(de.attr & 0x10)) return -4;
        dir_cluster = ((uint32_t)de.first_cluster_high << 16) | (uint32_t)de.first_cluster_low;
        if (dir_cluster < 2) dir_cluster = fs->root_cluster;
    }

    uint8_t short11[11];
    int ok = -1;
    for (int suffix = 0; suffix < 10000; suffix++) {
        if (fat32_build_short_name_candidate(leaf, suffix, short11) != 0) continue;
        if (!fat32_short_name_exists_in_dir(handle, dir_cluster, short11)) { ok = 0; break; }
    }
    if (ok != 0) return -5;

    uint8_t cksum = fat32_lfn_checksum(short11);

    size_t namelen = strlen(leaf);
    uint32_t lfn_entries = (uint32_t)((namelen + 12) / 13);
    uint32_t needed_slots = lfn_entries + 1;

    uint32_t target_cluster = 0, target_index = 0;
    if (fat32_find_free_dir_slots(handle, dir_cluster, needed_slots, &target_cluster, &target_index) != 0) return -6;

    uint32_t clus_size = (uint32_t)fs->bytes_per_sector * (uint32_t)fs->sectors_per_cluster;
    void *dirbuf = fat32_alloc_cluster_buffer(fs);
    if (!dirbuf) return -7;

    if (fat32_read_cluster(handle, target_cluster, dirbuf) != 0) { kfree(dirbuf); return -8; }

    for (uint32_t e = 0; e < lfn_entries; e++) {
        uint32_t part = lfn_entries - e;
        struct fat_lfn_entry l;
        memset(&l, 0, sizeof(l));
        l.ord = (uint8_t)part;
        if (part == lfn_entries) l.ord |= 0x40;
        l.attr = 0x0F;
        l.type = 0;
        l.checksum = cksum;
        l.first_cluster_low = 0;

        for (int i = 0; i < 5; i++) l.name1[i] = 0xFFFF;
        for (int i = 0; i < 6; i++) l.name2[i] = 0xFFFF;
        for (int i = 0; i < 2; i++) l.name3[i] = 0xFFFF;

        size_t start = (size_t)(part - 1) * 13;
        for (int i = 0; i < 13; i++) {
            size_t idx = start + (size_t)i;
            uint16_t wc;
            if (idx < namelen) wc = (uint16_t)(uint8_t)leaf[idx];
            else if (idx == namelen) wc = 0x0000;
            else wc = 0xFFFF;

            if (i < 5) l.name1[i] = wc;
            else if (i < 11) l.name2[i - 5] = wc;
            else l.name3[i - 11] = wc;
        }

        memcpy((uint8_t*)dirbuf + (target_index + e) * 32, &l, sizeof(l));
    }

    struct fat_dir_entry se;
    memset(&se, 0, sizeof(se));
    memcpy(se.name, short11, 11);
    se.attr = 0x20;
    se.first_cluster_high = (uint16_t)((first_cluster >> 16) & 0xFFFF);
    se.first_cluster_low = (uint16_t)(first_cluster & 0xFFFF);
    se.filesize = size;

    memcpy((uint8_t*)dirbuf + (target_index + lfn_entries) * 32, &se, sizeof(se));

    if (fat32_write_cluster(handle, target_cluster, dirbuf) != 0) { kfree(dirbuf); return -9; }

    kfree(dirbuf);
    return 0;
}

static int fat32_create_dir_entry_for_dir(int handle, const char *path, uint32_t first_cluster) {
    // For FAT, directories are just entries with attr=0x10 and a starting cluster.
    // Reuse the file-creation helper but with a directory attribute and size=0.
    // We implement it separately to ensure the short entry attr is correct.

    if (!fat32_valid_handle(handle) || !path) return -1;

    fat32_fs_t *fs = &fat32_mounts[handle];

    // Split into parent and leaf
    const char *leaf = path;
    const char *last_slash = NULL;
    for (const char *p = path; *p; p++) if (*p == '/') last_slash = p;

    char parent[256];
    if (last_slash) {
        size_t plen = (size_t)(last_slash - path);
        if (plen == 0) strcpy(parent, "/");
        else {
            if (plen >= sizeof(parent)) plen = sizeof(parent) - 1;
            memcpy(parent, path, plen);
            parent[plen] = 0;
        }
        leaf = last_slash + 1;
    } else {
        strcpy(parent, "/");
    }

    if (!leaf || !*leaf) return -2;

    uint32_t dir_cluster = fs->root_cluster;
    if (!(parent[0] == '/' && parent[1] == 0)) {
        struct fat_dir_entry de;
        if (fat32_find_file(handle, parent, &de) != 0) return -3;
        if (!(de.attr & 0x10)) return -4;
        dir_cluster = ((uint32_t)de.first_cluster_high << 16) | (uint32_t)de.first_cluster_low;
        if (dir_cluster < 2) dir_cluster = fs->root_cluster;
    }

    // Create unique 8.3 alias
    uint8_t short11[11];
    int ok = -1;
    for (int suffix = 0; suffix < 10000; suffix++) {
        if (fat32_build_short_name_candidate(leaf, suffix, short11) != 0) continue;
        if (!fat32_short_name_exists_in_dir(handle, dir_cluster, short11)) { ok = 0; break; }
    }
    if (ok != 0) return -5;

    uint8_t cksum = fat32_lfn_checksum(short11);

    size_t namelen = strlen(leaf);
    uint32_t lfn_entries = (uint32_t)((namelen + 12) / 13);
    uint32_t needed_slots = lfn_entries + 1;

    uint32_t target_cluster = 0, target_index = 0;
    if (fat32_find_free_dir_slots(handle, dir_cluster, needed_slots, &target_cluster, &target_index) != 0) return -6;

    uint32_t clus_size = (uint32_t)fs->bytes_per_sector * (uint32_t)fs->sectors_per_cluster;
    void *dirbuf = fat32_alloc_cluster_buffer(fs);
    if (!dirbuf) return -7;

    if (fat32_read_cluster(handle, target_cluster, dirbuf) != 0) { kfree(dirbuf); return -8; }

    // LFN entries
    for (uint32_t e = 0; e < lfn_entries; e++) {
        uint32_t part = lfn_entries - e;
        struct fat_lfn_entry l;
        memset(&l, 0, sizeof(l));
        l.ord = (uint8_t)part;
        if (part == lfn_entries) l.ord |= 0x40;
        l.attr = 0x0F;
        l.type = 0;
        l.checksum = cksum;
        l.first_cluster_low = 0;

        for (int i = 0; i < 5; i++) l.name1[i] = 0xFFFF;
        for (int i = 0; i < 6; i++) l.name2[i] = 0xFFFF;
        for (int i = 0; i < 2; i++) l.name3[i] = 0xFFFF;

        size_t start = (size_t)(part - 1) * 13;
        for (int i = 0; i < 13; i++) {
            size_t idx = start + (size_t)i;
            uint16_t wc;
            if (idx < namelen) wc = (uint16_t)(uint8_t)leaf[idx];
            else if (idx == namelen) wc = 0x0000;
            else wc = 0xFFFF;

            if (i < 5) l.name1[i] = wc;
            else if (i < 11) l.name2[i - 5] = wc;
            else l.name3[i - 11] = wc;
        }

        memcpy((uint8_t*)dirbuf + (target_index + e) * 32, &l, sizeof(l));
    }

    struct fat_dir_entry se;
    memset(&se, 0, sizeof(se));
    memcpy(se.name, short11, 11);
    se.attr = 0x10; // directory
    se.first_cluster_high = (uint16_t)((first_cluster >> 16) & 0xFFFF);
    se.first_cluster_low = (uint16_t)(first_cluster & 0xFFFF);
    se.filesize = 0;

    memcpy((uint8_t*)dirbuf + (target_index + lfn_entries) * 32, &se, sizeof(se));

    if (fat32_write_cluster(handle, target_cluster, dirbuf) != 0) { kfree(dirbuf); return -9; }

    kfree(dirbuf);
    return 0;
}

static int fat32_find_entry_location_in_dir(int handle, uint32_t dir_cluster, const char *leaf, uint32_t *out_cluster, uint32_t *out_index) {
    if (!fat32_valid_handle(handle) || !leaf || !out_cluster || !out_index) return -1;
    fat32_fs_t *fs = &fat32_mounts[handle];
    uint32_t clus_size = (uint32_t)fs->bytes_per_sector * (uint32_t)fs->sectors_per_cluster;

    void *buf = fat32_alloc_cluster_buffer(fs);
    if (!buf) return -2;

    uint32_t c = dir_cluster;
    int guard = 0;
    while (c >= 2 && c < 0x0FFFFFF8) {
        if (++guard > 200000) { kfree(buf); return -3; }
        if (fat32_read_cluster(handle, c, buf) != 0) { kfree(buf); return -4; }

        uint32_t idx;
        struct fat_dir_entry ent;
        if (find_entry_in_cluster_index(fs, (const uint8_t*)buf, clus_size, leaf, &ent, &idx) == 1) {
            *out_cluster = c;
            *out_index = idx;
            kfree(buf);
            return 0;
        }

        uint32_t next;
        if (fat32_next_cluster(handle, c, &next) != 0) break;
        if (next >= 0x0FFFFFF8 || next == c) break;
        c = next;
    }

    kfree(buf);
    return -5;
}

int fat32_unlink_by_path(int handle, const char* path) {
    if (!fat32_valid_handle(handle) || !path) return -1;

    fat32_fs_t* fs = &fat32_mounts[handle];
    uint32_t clus_size = (uint32_t)fs->bytes_per_sector * (uint32_t)fs->sectors_per_cluster;
    if (clus_size == 0 || clus_size > FAT32_MAX_CLUSTER_SIZE) return -2;

    // reject root
    if (path[0] == 0 || (path[0] == '/' && path[1] == 0)) return -3;

    // Find file entry
    struct fat_dir_entry de;
    if (fat32_find_file(handle, path, &de) != 0) return -4;
    if (de.attr & 0x10) return -5; // is a directory

    uint32_t first_cluster = ((uint32_t)de.first_cluster_high << 16) | (uint32_t)de.first_cluster_low;

    // Split path into parent + leaf
    const char *leaf = path;
    const char *last_slash = NULL;
    for (const char *p = path; *p; p++) if (*p == '/') last_slash = p;

    char parent[256];
    if (last_slash) {
        size_t plen = (size_t)(last_slash - path);
        if (plen == 0) strcpy(parent, "/");
        else {
            if (plen >= sizeof(parent)) plen = sizeof(parent) - 1;
            memcpy(parent, path, plen);
            parent[plen] = 0;
        }
        leaf = last_slash + 1;
    } else {
        strcpy(parent, "/");
    }

    uint32_t parent_cluster = fs->root_cluster;
    if (!(parent[0] == '/' && parent[1] == 0)) {
        struct fat_dir_entry pde;
        if (fat32_find_file(handle, parent, &pde) != 0) return -6;
        if (!(pde.attr & 0x10)) return -7;
        parent_cluster = ((uint32_t)pde.first_cluster_high << 16) | (uint32_t)pde.first_cluster_low;
        if (parent_cluster < 2) parent_cluster = fs->root_cluster;
    }

    uint32_t loc_cluster = 0, loc_index = 0;
    if (fat32_find_entry_location_in_dir(handle, parent_cluster, leaf, &loc_cluster, &loc_index) != 0) return -8;

    void *pbuf = fat32_alloc_cluster_buffer(fs);
    if (!pbuf) return -9;
    if (fat32_read_cluster(handle, loc_cluster, pbuf) != 0) { kfree(pbuf); return -10; }

    // Mark short entry deleted
    uint8_t *se = (uint8_t*)pbuf + loc_index * 32;
    se[0] = 0xE5;

    // Mark preceding LFN entries (in same cluster) deleted
    int32_t j = (int32_t)loc_index - 1;
    while (j >= 0) {
        uint8_t *le = (uint8_t*)pbuf + (uint32_t)j * 32;
        if ((le[11] & 0x0F) != 0x0F) break;
        le[0] = 0xE5;
        j--;
    }

    if (fat32_write_cluster(handle, loc_cluster, pbuf) != 0) { kfree(pbuf); return -11; }
    kfree(pbuf);

    if (first_cluster >= 2) {
        (void)fat32_free_cluster_chain(handle, first_cluster);
    }

    return 0;
}

int fat32_rmdir_by_path(int handle, const char* path) {
    if (!fat32_valid_handle(handle) || !path) return -1;

    fat32_fs_t* fs = &fat32_mounts[handle];
    uint32_t clus_size = (uint32_t)fs->bytes_per_sector * (uint32_t)fs->sectors_per_cluster;
    if (clus_size == 0 || clus_size > FAT32_MAX_CLUSTER_SIZE) return -2;

    // reject root
    if (path[0] == 0 || (path[0] == '/' && path[1] == 0)) return -3;

    // Find directory entry
    struct fat_dir_entry de;
    if (fat32_find_file(handle, path, &de) != 0) return -4;
    if (!(de.attr & 0x10)) return -5;

    uint32_t dir_cluster = ((uint32_t)de.first_cluster_high << 16) | (uint32_t)de.first_cluster_low;
    if (dir_cluster < 2) return -6;

    // Ensure directory is empty (ignore '.' and '..' and deleted/LFN entries)
    void *buf = fat32_alloc_cluster_buffer(fs);
    if (!buf) return -7;

    uint32_t c = dir_cluster;
    int guard = 0;
    int not_empty = 0;
    while (c >= 2 && c < 0x0FFFFFF8) {
        if (++guard > 200000) { kfree(buf); return -8; }
        if (fat32_read_cluster(handle, c, buf) != 0) { kfree(buf); return -9; }

        size_t entries = clus_size / 32;
        for (size_t i = 0; i < entries; i++) {
            uint8_t *e = (uint8_t*)buf + i * 32;
            uint8_t first = e[0];
            if (first == 0x00) { // end of dir
                goto empty_scan_done;
            }
            if (first == 0xE5) continue;
            if ((e[11] & 0x0F) == 0x0F) continue; // LFN

            // 8.3 name checks for '.' and '..'
            if (memcmp(e, ".          ", 11) == 0) continue;
            if (memcmp(e, "..         ", 11) == 0) continue;

            not_empty = 1;
            break;
        }
        if (not_empty) break;

        uint32_t next;
        if (fat32_next_cluster(handle, c, &next) != 0) break;
        if (next >= 0x0FFFFFF8 || next == c) break;
        c = next;
    }

empty_scan_done:
    kfree(buf);
    if (not_empty) return -10;

    // Split path into parent + leaf
    const char *leaf = path;
    const char *last_slash = NULL;
    for (const char *p = path; *p; p++) if (*p == '/') last_slash = p;

    char parent[256];
    if (last_slash) {
        size_t plen = (size_t)(last_slash - path);
        if (plen == 0) strcpy(parent, "/");
        else {
            if (plen >= sizeof(parent)) plen = sizeof(parent) - 1;
            memcpy(parent, path, plen);
            parent[plen] = 0;
        }
        leaf = last_slash + 1;
    } else {
        strcpy(parent, "/");
    }

    uint32_t parent_cluster = fs->root_cluster;
    if (!(parent[0] == '/' && parent[1] == 0)) {
        struct fat_dir_entry pde;
        if (fat32_find_file(handle, parent, &pde) != 0) return -11;
        if (!(pde.attr & 0x10)) return -12;
        parent_cluster = ((uint32_t)pde.first_cluster_high << 16) | (uint32_t)pde.first_cluster_low;
        if (parent_cluster < 2) parent_cluster = fs->root_cluster;
    }

    uint32_t loc_cluster = 0, loc_index = 0;
    if (fat32_find_entry_location_in_dir(handle, parent_cluster, leaf, &loc_cluster, &loc_index) != 0) return -13;

    void *pbuf = fat32_alloc_cluster_buffer(fs);
    if (!pbuf) return -14;
    if (fat32_read_cluster(handle, loc_cluster, pbuf) != 0) { kfree(pbuf); return -15; }

    // Mark short entry deleted
    uint8_t *se = (uint8_t*)pbuf + loc_index * 32;
    se[0] = 0xE5;

    // Mark preceding LFN entries (in same cluster) deleted
    int32_t j = (int32_t)loc_index - 1;
    while (j >= 0) {
        uint8_t *le = (uint8_t*)pbuf + (uint32_t)j * 32;
        if ((le[11] & 0x0F) != 0x0F) break;
        le[0] = 0xE5;
        j--;
    }

    if (fat32_write_cluster(handle, loc_cluster, pbuf) != 0) { kfree(pbuf); return -16; }
    kfree(pbuf);

    // Free the directory cluster chain
    (void)fat32_free_cluster_chain(handle, dir_cluster);

    return 0;
}

int fat32_mkdir_by_path(int handle, const char* path) {
    if (!fat32_valid_handle(handle) || !path) return -1;

    fat32_fs_t* fs = &fat32_mounts[handle];
    uint32_t clus_size = (uint32_t)fs->bytes_per_sector * (uint32_t)fs->sectors_per_cluster;
    if (clus_size == 0 || clus_size > FAT32_MAX_CLUSTER_SIZE) return -2;

    // reject root
    if (path[0] == 0 || (path[0] == '/' && path[1] == 0)) return -3;

    // already exists?
    struct fat_dir_entry existing;
    if (fat32_find_file(handle, path, &existing) == 0) {
        return (existing.attr & 0x10) ? 0 : -4;
    }

    // Determine parent cluster (must exist)
    const char *leaf = path;
    const char *last_slash = NULL;
    for (const char *p = path; *p; p++) if (*p == '/') last_slash = p;

    char parent[256];
    if (last_slash) {
        size_t plen = (size_t)(last_slash - path);
        if (plen == 0) strcpy(parent, "/");
        else {
            if (plen >= sizeof(parent)) plen = sizeof(parent) - 1;
            memcpy(parent, path, plen);
            parent[plen] = 0;
        }
        leaf = last_slash + 1;
    } else {
        strcpy(parent, "/");
    }

    if (!leaf || !*leaf) return -5;

    uint32_t parent_cluster = fs->root_cluster;
    if (!(parent[0] == '/' && parent[1] == 0)) {
        struct fat_dir_entry de;
        if (fat32_find_file(handle, parent, &de) != 0) return -6;
        if (!(de.attr & 0x10)) return -7;
        parent_cluster = ((uint32_t)de.first_cluster_high << 16) | (uint32_t)de.first_cluster_low;
        if (parent_cluster < 2) parent_cluster = fs->root_cluster;
    }

    // Allocate one cluster for the directory
    uint32_t dir_cluster = 0;
    if (fat32_alloc_cluster_chain(handle, 1, &dir_cluster) != 0) return -8;

    // Build '.' and '..' entries
    void *buf = kmalloc(clus_size);
    if (!buf) { (void)fat32_free_cluster_chain(handle, dir_cluster); return -9; }
    memset(buf, 0, clus_size);

    struct fat_dir_entry dot;
    memset(&dot, 0, sizeof(dot));
    memcpy(dot.name, ".          ", 11);
    dot.attr = 0x10;
    dot.first_cluster_high = (uint16_t)((dir_cluster >> 16) & 0xFFFF);
    dot.first_cluster_low = (uint16_t)(dir_cluster & 0xFFFF);
    dot.filesize = 0;

    struct fat_dir_entry dotdot;
    memset(&dotdot, 0, sizeof(dotdot));
    memcpy(dotdot.name, "..         ", 11);
    dotdot.attr = 0x10;
    uint32_t pc = parent_cluster;
    dotdot.first_cluster_high = (uint16_t)((pc >> 16) & 0xFFFF);
    dotdot.first_cluster_low = (uint16_t)(pc & 0xFFFF);
    dotdot.filesize = 0;

    memcpy((uint8_t*)buf + 0 * 32, &dot, sizeof(dot));
    memcpy((uint8_t*)buf + 1 * 32, &dotdot, sizeof(dotdot));

    if (fat32_write_cluster(handle, dir_cluster, buf) != 0) {
        kfree(buf);
        (void)fat32_free_cluster_chain(handle, dir_cluster);
        return -10;
    }
    kfree(buf);

    // Create directory entry in parent directory
    int rc = fat32_create_dir_entry_for_dir(handle, path, dir_cluster);
    if (rc != 0) {
        (void)fat32_free_cluster_chain(handle, dir_cluster);
        return -11;
    }

    return 0;
}

// Overwrite an existing file; if it doesn't exist, create it (LFN + 8.3 short alias).
int fat32_write_file_by_path(int handle, const char* path, const void* data, size_t size) {
    if (!fat32_valid_handle(handle) || !path) return -1;
    if (!data && size != 0) return -2;

    fat32_fs_t* fs = &fat32_mounts[handle];
    uint32_t clus_size = (uint32_t)fs->bytes_per_sector * (uint32_t)fs->sectors_per_cluster;
    if (clus_size == 0 || clus_size > FAT32_MAX_CLUSTER_SIZE) return -3;

    // Determine required clusters (at least 1 for non-empty files; allow 0 for empty).
    uint32_t need_clusters = 0;
    if (size > 0) {
        need_clusters = (uint32_t)((size + (size_t)clus_size - 1) / (size_t)clus_size);
        if (need_clusters == 0) need_clusters = 1;
    }

    // Try to find existing file.
    struct fat_dir_entry fe;
    int have = (fat32_find_file(handle, path, &fe) == 0);

    uint32_t first_cluster = 0;
    if (have) {
        if (fe.attr & 0x10) return -5; // directory
        first_cluster = ((uint32_t)fe.first_cluster_high << 16) | (uint32_t)fe.first_cluster_low;
    }

    // If file exists, free its current chain (we always do full overwrite semantics here).
    if (have && first_cluster >= 2) {
        (void)fat32_free_cluster_chain(handle, first_cluster);
        first_cluster = 0;
    }

    // Allocate new chain for the new size.
    if (need_clusters > 0) {
        if (fat32_alloc_cluster_chain(handle, need_clusters, &first_cluster) != 0) return -7;
    }

    // Create or update directory entry.
    if (!have) {
        int crc = fat32_create_dir_entry_for_file(handle, path, first_cluster, (uint32_t)size);
        if (crc != 0) {
            if (first_cluster >= 2) (void)fat32_free_cluster_chain(handle, first_cluster);
            return -8;
        }
    } else {
        (void)fat32_update_file_entry(handle, path, first_cluster, (uint32_t)size);
    }

    // Write file data.
    if (size == 0) {
        return 0;
    }

    void *buf = kmalloc(clus_size);
    if (!buf) return -9;

    size_t written = 0;
    uint32_t c = first_cluster;
    int guard = 0;
    while (c >= 2 && c < 0x0FFFFFF8 && written < size) {
        if (++guard > 200000) { kfree(buf); return -10; }

        memset(buf, 0, clus_size);
        size_t chunk = clus_size;
        if (chunk > (size - written)) chunk = (size - written);
        memcpy(buf, (const uint8_t*)data + written, chunk);

        if (fat32_write_cluster(handle, c, buf) != 0) { kfree(buf); return -11; }
        written += chunk;

        uint32_t next = 0;
        if (fat32_read_fat_entry(handle, c, &next) != 0) break;
        if (next >= 0x0FFFFFF8 || next == c) break;
        c = next;
    }

    kfree(buf);
    return 0;
}

/* --- ROOT LISTING --- */
int fat32_list_root(int handle) {
    if (!fat32_valid_handle(handle)) {
        VGA_Write("FAT32: invalid handle\n");
        return -1;
    }

    fat32_fs_t* fs = &fat32_mounts[handle];
    uint32_t cluster = fs->root_cluster;
    uint32_t clus_size = (uint32_t)fs->bytes_per_sector * (uint32_t)fs->sectors_per_cluster;

    if (clus_size == 0 || clus_size > FAT32_MAX_CLUSTER_SIZE) {
        com_printf(COM1_PORT, "FAT32: invalid cluster size %u\n", clus_size);
        return -2;
    }

    void *buf = fat32_alloc_cluster_buffer(fs);
    if (!buf) return -3;

    com_printf(COM1_PORT, "FAT32 root directory (handle %d):\n", handle);

    int iterations = 0;
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        if (++iterations > 100) {
            VGA_Write("FAT32: too many clusters\n");
            kfree(buf);
            return -3;
        }

        if (fat32_read_cluster(handle, cluster, buf) != 0) {
            com_printf(COM1_PORT, "FAT32: failed to read cluster %u\n", cluster);
            kfree(buf);
            return -4;
        }

        size_t entries = safe_divide(clus_size, 32);

        const uint8_t* lfn_stack[20];
        int lfn_count = 0;

        for (size_t i = 0; i < entries; i++) {
            struct fat_dir_entry* e = (struct fat_dir_entry*)(buf + i * 32);
            uint8_t first = (uint8_t)e->name[0];

            if (first == 0x00) { kfree(buf); return 0; }
            if (first == 0xE5) { lfn_count = 0; continue; }
            if ((e->attr & 0x0F) == 0x0F) {
                if (lfn_count < (int)(sizeof(lfn_stack) / sizeof(lfn_stack[0]))) {
                    lfn_stack[lfn_count++] = (const uint8_t*)e;
                } else {
                    lfn_count = 0;
                }
                continue;
            }

            char name[260];
            int idx = 0;
            if (lfn_count > 0) {
                for (int part = lfn_count - 1; part >= 0; part--) {
                    const uint8_t* le = lfn_stack[part];
                    for (int off = 1; off <= 10; off += 2) {
                        uint16_t wc = (uint16_t)le[off] | ((uint16_t)le[off + 1] << 8);
                        if (wc == 0x0000 || wc == 0xFFFF) break;
                        if ((wc & 0xFF00) == 0x0000) name[idx++] = (char)(wc & 0xFF);
                        else name[idx++] = '?';
                        if (idx >= (int)sizeof(name)-1) break;
                    }
                    if (idx >= (int)sizeof(name)-1) break;

                    for (int off = 14; off <= 25; off += 2) {
                        uint16_t wc = (uint16_t)le[off] | ((uint16_t)le[off + 1] << 8);
                        if (wc == 0x0000 || wc == 0xFFFF) break;
                        if ((wc & 0xFF00) == 0x0000) name[idx++] = (char)(wc & 0xFF);
                        else name[idx++] = '?';
                        if (idx >= (int)sizeof(name)-1) break;
                    }
                    if (idx >= (int)sizeof(name)-1) break;

                    for (int off = 28; off <= 31; off += 2) {
                        uint16_t wc = (uint16_t)le[off] | ((uint16_t)le[off + 1] << 8);
                        if (wc == 0x0000 || wc == 0xFFFF) break;
                        if ((wc & 0xFF00) == 0x0000) name[idx++] = (char)(wc & 0xFF);
                        else name[idx++] = '?';
                        if (idx >= (int)sizeof(name)-1) break;
                    }
                    if (idx >= (int)sizeof(name)-1) break;
                }
                name[idx] = '\0';
            } else {
                for (int j = 0; j < 8; j++) {
                    if (e->name[j] != ' ') name[idx++] = e->name[j];
                }

                int has_ext = 0;
                for (int j = 8; j < 11; j++) {
                    if (e->name[j] != ' ') { has_ext = 1; break; }
                }

                if (has_ext) {
                    name[idx++] = '.';
                    for (int j = 8; j < 11; j++) {
                        if (e->name[j] != ' ') name[idx++] = e->name[j];
                    }
                }
                name[idx] = '\0';
            }

            com_printf(COM1_PORT, "  %s %s size=%u\n",
                       name,
                       (e->attr & 0x10) ? "<DIR>" : "",
                       e->filesize);

            lfn_count = 0;
        }

        uint32_t next;
        if (fat32_next_cluster(handle, cluster, &next) != 0) {
            return -5;
        }

        if (next >= 0x0FFFFFF8 || next == cluster) break;
        cluster = next;
    }

    return 0;
}

/* --- INFO --- */
const fat32_fs_t* fat32_get_fs(int handle) {
    if (!fat32_valid_handle(handle)) return NULL;
    return &fat32_mounts[handle];
}

int fat32_get_mount_count(void) {
    int count = 0;
    for (int i = 0; i < FAT32_MAX_MOUNTS; i++) {
        if (fat32_mounts[i].active) count++;
    }
    return count;
}

int fat32_read_file_by_path(int handle, const char* path, void* out_buf, size_t buf_size, size_t* out_size) {
    if (!fat32_valid_handle(handle) || !path || !out_buf) {
        return -1;
    }

    fat32_fs_t* fs = &fat32_mounts[handle];
    uint32_t current_cluster = fs->root_cluster;
    char component[260];
    int comp_idx = 0;
    int path_idx = 0;

    if (path[0] == '/') path_idx++;

    /* Traverse path until final file component */
    while (path[path_idx] != '\0') {
        comp_idx = 0;
        while (path[path_idx] != '\0' && path[path_idx] != '/' &&
               comp_idx < (int)sizeof(component) - 1) {
            component[comp_idx++] = path[path_idx++];
        }
        component[comp_idx] = '\0';

        if (comp_idx == 0) {
            if (path[path_idx] == '/') path_idx++;
            continue;
        }

        /* Find entry in current directory */
        struct fat_dir_entry entry;
        if (find_dir_entry(handle, current_cluster, component, &entry) != 0) {
            com_printf(COM1_PORT, "FAT32: file not found: %s\n", component);
            return -2;
        }

        /* More path left? Must be directory */
        if (path[path_idx] == '/' && !(entry.attr & 0x10)) {
            com_printf(COM1_PORT, "FAT32: %s is not a directory\n", component);
            return -3;
        }

        /* Update cluster */
        current_cluster = ((uint32_t)entry.first_cluster_high << 16) |
                          (uint32_t)entry.first_cluster_low;

        if (path[path_idx] == '/') path_idx++;

        /* If this is the final path component and not a directory, it's the file */
        if (path[path_idx] == '\0' && !(entry.attr & 0x10)) {
            uint32_t file_cluster = current_cluster;
            uint32_t remaining = entry.filesize;
            void *cluster_buf = fat32_alloc_cluster_buffer(fs);
            if (!cluster_buf) return -4;
            uint8_t* dest = (uint8_t*)out_buf;
            size_t total_read = 0;
            uint32_t clus_size = (uint32_t)fs->bytes_per_sector * (uint32_t)fs->sectors_per_cluster;

            while (file_cluster >= 2 && file_cluster < 0x0FFFFFF8 && remaining > 0) {
                if (fat32_read_cluster(handle, file_cluster, cluster_buf) != 0) {
                    com_printf(COM1_PORT, "FAT32: failed to read cluster %u\n", file_cluster);
                    kfree(cluster_buf);
                    return -4;
                }

                size_t to_copy = clus_size;
                if (to_copy > remaining) to_copy = remaining;
                if (total_read + to_copy > buf_size) {
                    VGA_Write("FAT32: output buffer too small\n");
                    return -5;
                }

                memcpy(dest + total_read, cluster_buf, to_copy);
                total_read += to_copy;
                remaining -= to_copy;

                uint32_t next;
                if (fat32_next_cluster(handle, file_cluster, &next) != 0) {
                    kfree(cluster_buf);
                    return -6;
                }

                if (next >= 0x0FFFFFF8 || next == file_cluster) break;
                file_cluster = next;
            }

            if (out_size) *out_size = total_read;
            // com_printf(COM1_PORT, "FAT32: read file '%s', %u bytes\n", component, (unsigned)total_read);
            kfree(cluster_buf);
            return 0;
        }
    }

    return -7; /* Not a file or not found */
}

int fat32_find_file(int handle, const char* path, struct fat_dir_entry* out_entry) {
    if (!fat32_valid_handle(handle) || !path || !out_entry) {
        return -1; /* Invalid parameters */
    }

    fat32_fs_t* fs = &fat32_mounts[handle];
    uint32_t current_cluster = fs->root_cluster;
    char component[260];
    int comp_idx = 0;
    int path_idx = 0;

    if (path[0] == '/') path_idx++; /* Skip leading slash */

    while (path[path_idx] != '\0') {
        /* Extract path component */
        comp_idx = 0;
        while (path[path_idx] != '\0' && path[path_idx] != '/' &&
               comp_idx < (int)sizeof(component) - 1) {
            component[comp_idx++] = path[path_idx++];
        }
        component[comp_idx] = '\0';

        if (comp_idx == 0) {
            if (path[path_idx] == '/') path_idx++;
            continue;
        }

        struct fat_dir_entry entry;
        int found = 0;
        uint32_t cluster = current_cluster;
        uint32_t clus_size = (uint32_t)fs->bytes_per_sector * (uint32_t)fs->sectors_per_cluster;
        void *buf = fat32_alloc_cluster_buffer(fs);
        if (!buf) return -4;

        while (cluster >= 2 && cluster < 0x0FFFFFF8) {
            if (fat32_read_cluster(handle, cluster, buf) != 0) { kfree(buf); return -2; }

            if (find_entry_in_cluster(fs, buf, clus_size, component, &entry)) {
                found = 1;
                break;
            }

            uint32_t next;
            if (fat32_next_cluster(handle, cluster, &next) != 0) { kfree(buf); return -3; }
            if (next >= 0x0FFFFFF8 || next == cluster) break;
            cluster = next;
        }

        if (!found) { kfree(buf); return -4; } /* Component not found */

        kfree(buf);

        /* If not the last component, must be directory */
        if (path[path_idx] != '\0' && !(entry.attr & 0x10)) return -5;

        current_cluster = ((uint32_t)entry.first_cluster_high << 16) |
                          (uint32_t)entry.first_cluster_low;

        if (path[path_idx] == '/') path_idx++;

        /* Last component reached */
        if (path[path_idx] == '\0') {
            memcpy(out_entry, &entry, sizeof(struct fat_dir_entry));
            kfree(buf);
            return 0;
        }
    }

    return -6; /* Should not reach here */
}