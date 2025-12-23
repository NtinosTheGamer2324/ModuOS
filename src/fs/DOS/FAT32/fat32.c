#include "moduos/fs/DOS/FAT32/fat32.h"
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
        VGA_Writef("FAT32: Using %u sectors per cluster\n", sectors_per_cluster);
    }
    
    /* Validate cluster size */
    if (sectors_per_cluster == 0 || sectors_per_cluster > 128 ||
        (sectors_per_cluster & (sectors_per_cluster - 1)) != 0) {
        VGA_Write("FAT32: Invalid cluster size (must be power of 2, max 128)\n");
        return -3;
    }
    
    /* Calculate FAT32 parameters */
    uint16_t bytes_per_sector = 512;
    uint16_t reserved_sectors = 32;  // FAT32 typically uses 32 reserved sectors
    uint8_t num_fats = 2;
    uint16_t root_entry_count = 0;   // FAT32 has no fixed root directory
    uint16_t media_type = 0xF8;      // Fixed disk
    
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
    
    VGA_Writef("FAT32: Reserved sectors: %u\n", reserved_sectors);
    VGA_Writef("FAT32: FAT size: %u sectors\n", fat_size_sectors);
    VGA_Writef("FAT32: First data sector: %u\n", first_data_sector);
    
    /* Allocate buffer for boot sector */
    uint8_t boot_sector[512];
    memset(boot_sector, 0, 512);
    
    /* Jump instruction */
    boot_sector[0] = 0xEB;  // JMP short
    boot_sector[1] = 0x58;
    boot_sector[2] = 0x90;  // NOP
    
    /* OEM Name */
    memcpy(&boot_sector[3], "MODUOS  ", 8);
    
    /* BPB (BIOS Parameter Block) */
    boot_sector[11] = bytes_per_sector & 0xFF;
    boot_sector[12] = (bytes_per_sector >> 8) & 0xFF;
    boot_sector[13] = sectors_per_cluster;
    boot_sector[14] = reserved_sectors & 0xFF;
    boot_sector[15] = (reserved_sectors >> 8) & 0xFF;
    boot_sector[16] = num_fats;
    boot_sector[17] = root_entry_count & 0xFF;
    boot_sector[18] = (root_entry_count >> 8) & 0xFF;
    boot_sector[19] = 0;  // Total sectors (16-bit, 0 for FAT32)
    boot_sector[20] = 0;
    boot_sector[21] = media_type;
    boot_sector[22] = 0;  // FAT size (16-bit, 0 for FAT32)
    boot_sector[23] = 0;
    boot_sector[24] = 63;  // Sectors per track
    boot_sector[25] = 0;
    boot_sector[26] = 255; // Number of heads
    boot_sector[27] = 0;
    
    /* Hidden sectors (LBA of partition) */
    boot_sector[28] = partition_lba & 0xFF;
    boot_sector[29] = (partition_lba >> 8) & 0xFF;
    boot_sector[30] = (partition_lba >> 16) & 0xFF;
    boot_sector[31] = (partition_lba >> 24) & 0xFF;
    
    /* Total sectors (32-bit) */
    boot_sector[32] = partition_sectors & 0xFF;
    boot_sector[33] = (partition_sectors >> 8) & 0xFF;
    boot_sector[34] = (partition_sectors >> 16) & 0xFF;
    boot_sector[35] = (partition_sectors >> 24) & 0xFF;
    
    /* FAT32 Extended BPB */
    boot_sector[36] = fat_size_sectors & 0xFF;
    boot_sector[37] = (fat_size_sectors >> 8) & 0xFF;
    boot_sector[38] = (fat_size_sectors >> 16) & 0xFF;
    boot_sector[39] = (fat_size_sectors >> 24) & 0xFF;
    
    boot_sector[40] = 0;  // Ext flags
    boot_sector[41] = 0;
    boot_sector[42] = 0;  // FS version
    boot_sector[43] = 0;
    
    /* Root cluster (typically 2) */
    boot_sector[44] = 2;
    boot_sector[45] = 0;
    boot_sector[46] = 0;
    boot_sector[47] = 0;
    
    boot_sector[48] = 1;  // FS info sector
    boot_sector[49] = 0;
    boot_sector[50] = 6;  // Backup boot sector
    boot_sector[51] = 0;
    
    /* Reserved (12 bytes) */
    for (int i = 52; i < 64; i++) {
        boot_sector[i] = 0;
    }
    
    boot_sector[64] = 0x80; // Drive number (0x80 = hard disk)
    boot_sector[65] = 0;    // Reserved
    boot_sector[66] = 0x29; // Extended boot signature
    
    /* Volume ID (serial number) - use simple timestamp-based value */
    boot_sector[67] = 0x12;
    boot_sector[68] = 0x34;
    boot_sector[69] = 0x56;
    boot_sector[70] = 0x78;
    
    /* Volume label */
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
    
    /* Filesystem type */
    memcpy(&boot_sector[82], "FAT32   ", 8);
    
    /* Boot code (simple message) */
    const char *msg = "This is not a bootable device. Please insert a bootable medium and press Ctrl+Alt+Del.";
    int msg_len = strlen(msg);
    for (int i = 0; i < msg_len && (90 + i) < 510; i++) {
        boot_sector[90 + i] = msg[i];
    }
    
    /* Boot signature */
    boot_sector[510] = 0x55;
    boot_sector[511] = 0xAA;
    
    /* Write boot sector */
    VGA_Write("FAT32: Writing boot sector...\n");
    if (vdrive_write_sector(vdrive_id, partition_lba, boot_sector) != 0) {
        VGA_Write("FAT32: Failed to write boot sector\n");
        return -5;
    }
    
    /* Write backup boot sector */
    if (vdrive_write_sector(vdrive_id, partition_lba + 6, boot_sector) != 0) {
        VGA_Write("FAT32: Failed to write backup boot sector\n");
        return -6;
    }
    
    /* Create FSInfo sector */
    uint8_t fsinfo[512];
    memset(fsinfo, 0, 512);
    
    fsinfo[0] = 0x52;  // Lead signature "RRaA"
    fsinfo[1] = 0x52;
    fsinfo[2] = 0x61;
    fsinfo[3] = 0x41;
    
    fsinfo[484] = 0x72;  // Struct signature "rrAa"
    fsinfo[485] = 0x72;
    fsinfo[486] = 0x41;
    fsinfo[487] = 0x61;
    
    /* Free cluster count (-1 = unknown) */
    fsinfo[488] = 0xFF;
    fsinfo[489] = 0xFF;
    fsinfo[490] = 0xFF;
    fsinfo[491] = 0xFF;
    
    /* Next free cluster (start at 3, since 2 is root) */
    fsinfo[492] = 3;
    fsinfo[493] = 0;
    fsinfo[494] = 0;
    fsinfo[495] = 0;
    
    fsinfo[510] = 0x55;  // Trail signature
    fsinfo[511] = 0xAA;
    
    /* Write FSInfo sector */
    VGA_Write("FAT32: Writing FSInfo sector...\n");
    if (vdrive_write_sector(vdrive_id, partition_lba + 1, fsinfo) != 0) {
        VGA_Write("FAT32: Failed to write FSInfo sector\n");
        return -7;
    }
    
    /* Initialize FAT tables */
    VGA_Write("FAT32: Initializing FAT tables...\n");
    
    uint8_t fat_sector[512];
    for (uint32_t fat_num = 0; fat_num < num_fats; fat_num++) {
        uint32_t fat_start = partition_lba + reserved_sectors + (fat_num * fat_size_sectors);
        
        /* First FAT sector has special entries */
        memset(fat_sector, 0, 512);
        
        /* Entry 0: Media type */
        fat_sector[0] = media_type;
        fat_sector[1] = 0xFF;
        fat_sector[2] = 0xFF;
        fat_sector[3] = 0x0F;
        
        /* Entry 1: EOC (End of Chain) */
        fat_sector[4] = 0xFF;
        fat_sector[5] = 0xFF;
        fat_sector[6] = 0xFF;
        fat_sector[7] = 0x0F;
        
        /* Entry 2: Root directory (EOC) */
        fat_sector[8] = 0xFF;
        fat_sector[9] = 0xFF;
        fat_sector[10] = 0xFF;
        fat_sector[11] = 0x0F;
        
        /* Write first FAT sector */
        if (vdrive_write_sector(vdrive_id, fat_start, fat_sector) != 0) {
            VGA_Writef("FAT32: Failed to write FAT %u first sector\n", fat_num);
            return -8;
        }
        
        /* Zero remaining FAT sectors */
        memset(fat_sector, 0, 512);
        for (uint32_t i = 1; i < fat_size_sectors; i++) {
            if (vdrive_write_sector(vdrive_id, fat_start + i, fat_sector) != 0) {
                VGA_Writef("FAT32: Failed to write FAT %u sector %u\n", fat_num, i);
                return -9;
            }
        }
    }
    
    /* Clear root directory */
    VGA_Write("FAT32: Clearing root directory...\n");
    uint32_t root_cluster_lba = partition_lba + first_data_sector;
    for (uint32_t i = 0; i < sectors_per_cluster; i++) {
        if (write_zero_sector(vdrive_id, root_cluster_lba + i) != 0) {
            VGA_Write("FAT32: Failed to clear root directory\n");
            return -10;
        }
    }
    
    VGA_Write("FAT32: Format complete!\n");
    VGA_Writef("FAT32: Volume label: %s\n", volume_label ? volume_label : "NO NAME");
    VGA_Writef("FAT32: Cluster size: %u KB\n", (sectors_per_cluster * 512) / 1024);
    VGA_Writef("FAT32: Total clusters: %u\n", total_clusters);
    
    return 0;
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
     * DMA safety: keep boot-sector reads within a single 4KiB page.
     * Using stack buffers here can cross a page boundary when code changes.
     */
    static uint8_t sector_page[4096] __attribute__((aligned(4096)));
    uint8_t *sector = sector_page;
    fat32_fs_t* fs = &fat32_mounts[handle];
    
    memset(fs, 0, sizeof(fat32_fs_t));
    fs->vdrive_id = vdrive_id;  
    fs->partition_lba = partition_lba;

    VGA_Writef("FAT32: attempting mount vdrive=%d, LBA=%u -> handle=%d\n", 
               vdrive_id, partition_lba, handle);  


    if (vdrive_read_sector(vdrive_id, partition_lba, sector) != VDRIVE_SUCCESS) {
        VGA_Write("FAT32: failed to read boot sector\n");
        return -2;
    }

    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        VGA_Writef("FAT32: invalid boot signature (got 0x%x 0x%x)\n", sector[510], sector[511]);
        return -3;
    }

    /* Parse BPB */
    fs->bytes_per_sector    = (uint16_t)sector[11] | ((uint16_t)sector[12] << 8);
    fs->sectors_per_cluster = sector[13];
    fs->reserved_sectors    = (uint16_t)sector[14] | ((uint16_t)sector[15] << 8);
    fs->num_fats            = sector[16];

    /* Validate before calculations */
    if (fs->bytes_per_sector == 0 || fs->sectors_per_cluster == 0 || fs->num_fats == 0) {
        VGA_Write("FAT32: invalid BPB values (zero)\n");
        return -4;
    }

    if (fs->bytes_per_sector != 512 && fs->bytes_per_sector != 1024 && 
        fs->bytes_per_sector != 2048 && fs->bytes_per_sector != 4096) {
        VGA_Writef("FAT32: unusual bytes_per_sector=%u\n", fs->bytes_per_sector);
        return -5;
    }

    if (fs->sectors_per_cluster > 128) {
        VGA_Writef("FAT32: suspiciously large sectors_per_cluster=%u\n", fs->sectors_per_cluster);
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
        return -7;
    }

    /* Get root cluster */
    fs->root_cluster = (uint32_t)sector[44] | ((uint32_t)sector[45] << 8) | 
                       ((uint32_t)sector[46] << 16) | ((uint32_t)sector[47] << 24);

    if (fs->root_cluster < 2) {
        VGA_Writef("FAT32: invalid root_cluster=%u (must be >= 2)\n", fs->root_cluster);
        return -8;
    }

    /* Calculate first data sector */
    fs->first_data_sector = fs->partition_lba + fs->reserved_sectors + 
                           ((uint32_t)fs->num_fats * fs->sectors_per_fat);

    /* Check cluster size */
    uint32_t clus_size = (uint32_t)fs->bytes_per_sector * (uint32_t)fs->sectors_per_cluster;
    if (clus_size > FAT32_MAX_CLUSTER_SIZE) {
        VGA_Writef("FAT32: cluster size %u > max %u\n", clus_size, FAT32_MAX_CLUSTER_SIZE);
        return -9;
    }

    /* Mark as active */
    fs->active = 1;
    
    VGA_Writef("FAT32: mount successful! handle=%d, root_cluster=%u\n", handle, fs->root_cluster);
    return handle;
}

int fat32_mount_auto(int vdrive_id) {
    fat32_init_once();
    
    int start = (vdrive_id >= 0) ? vdrive_id : 0;
    int end = (vdrive_id >= 0) ? vdrive_id : (vdrive_get_count() - 1);
    
    if (vdrive_id < 0) {
        VGA_Write("FAT32: scanning all vDrives...\n");
    }

    /* DMA safety: keep MBR reads within a single 4KiB page. */
    static uint8_t mbr_page[4096] __attribute__((aligned(4096)));
    uint8_t *mbr = mbr_page;

    for (int d = start; d <= end; d++) {
        if (!vdrive_is_ready(d)) {
            continue;
        }
        
        VGA_Writef("FAT32: checking vDrive %d\n", d);
        
        // CHANGED: Use vDrive
        if (vdrive_read_sector(d, 0, mbr) != VDRIVE_SUCCESS) {
            VGA_Writef("FAT32: cannot read vDrive %d\n", d);
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
                VGA_Writef("FAT32: found partition %d, type=0x%x, LBA=%u\n", i, type, lba);
                int handle = fat32_mount(d, lba);
                if (handle >= 0) {
                    return handle;
                }
            }
        }

        /* Try superfloppy */
        int handle = fat32_mount(d, 0);
        if (handle >= 0) {
            return handle;
        }
    }

    VGA_Write("FAT32: no filesystem found\n");
    return -1;
}

void fat32_unmount(int handle) {
    if (fat32_valid_handle(handle)) {
        VGA_Writef("FAT32: unmounting handle %d\n", handle);
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

    uint8_t sec[512];
    
    
    if (vdrive_read_sector(fs->vdrive_id, fat_sector, sec) != VDRIVE_SUCCESS) {
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
        uint8_t sec2[512];
        

        if (vdrive_read_sector(fs->vdrive_id, fat_sector + 1, sec2) != VDRIVE_SUCCESS) {
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
    }

    entry &= 0x0FFFFFFF;
    *out_next = entry;
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
    static uint8_t buf[FAT32_MAX_CLUSTER_SIZE];
    
    uint32_t cluster = dir_cluster;
    int iterations = 0;
    
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        if (++iterations > 1000) return -2;
        
        if (fat32_read_cluster(handle, cluster, buf) != 0) return -3;
        
        int result = find_entry_in_cluster(fs, buf, clus_size, name, out_entry);
        if (result == 1) return 0; /* Found */
        if (result < 0) return result;
        
        uint32_t next;
        if (fat32_next_cluster(handle, cluster, &next) != 0) return -4;
        if (next >= 0x0FFFFFF8 || next == cluster) break;
        cluster = next;
    }
    
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

    static uint8_t buf[FAT32_MAX_CLUSTER_SIZE];
    uint32_t cluster = dir_cluster;
    int iterations = 0;

    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        if (++iterations > 1000) {
            VGA_Write("FAT32: directory too deep\n");
            return -3;
        }

        if (fat32_read_cluster(handle, cluster, buf) != 0) {
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

            VGA_Writef("  %s %s size=%u\n",
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
        VGA_Writef("FAT32 root directory (handle %d):\n", handle);
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
        static uint8_t buf[FAT32_MAX_CLUSTER_SIZE];

        while (cluster >= 2 && cluster < 0x0FFFFFF8) {
            if (fat32_read_cluster(handle, cluster, buf) != 0) {
                VGA_Writef("FAT32: failed to read cluster %u\n", cluster);
                return -4;
            }

            if (find_entry_in_cluster(fs, buf, clus_size, component, &entry)) {
                found = 1;
                break;
            }

            uint32_t next;
            if (fat32_next_cluster(handle, cluster, &next) != 0)
                return -5;

            if (next >= 0x0FFFFFF8 || next == cluster) break;
            cluster = next;
        }

        if (!found) {
            VGA_Writef("FAT32: path component '%s' not found\n", component);
            return -2;
        }

        /* If it’s not a directory but more path remains → invalid */
        if (!(entry.attr & 0x10) && path[path_idx] != '\0') {
            VGA_Writef("FAT32: '%s' is not a directory\n", component);
            return -3;
        }

        /* Update current cluster to the directory’s cluster */
        current_cluster = ((uint32_t)entry.first_cluster_high << 16) |
                          (uint32_t)entry.first_cluster_low;

        /* Skip slash before next component */
        if (path[path_idx] == '/') path_idx++;
    }

    /* List the final directory */
    VGA_Writef("FAT32 directory '%s' (handle %d):\n", path, handle);
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

// Overwrite an existing file without reallocating clusters (fails if new data doesn't fit).
int fat32_write_file_by_path(int handle, const char* path, const void* data, size_t size) {
    if (!fat32_valid_handle(handle) || !path) return -1;
    if (!data && size != 0) return -2;

    fat32_fs_t* fs = &fat32_mounts[handle];
    uint32_t clus_size = (uint32_t)fs->bytes_per_sector * (uint32_t)fs->sectors_per_cluster;
    if (clus_size == 0 || clus_size > FAT32_MAX_CLUSTER_SIZE) return -3;

    // Resolve file entry (to get cluster chain)
    struct fat_dir_entry fe;
    if (fat32_find_file(handle, path, &fe) != 0) return -4;
    if (fe.attr & 0x10) return -5; // directory

    uint32_t first_cluster = ((uint32_t)fe.first_cluster_high << 16) | (uint32_t)fe.first_cluster_low;

    // Count clusters in chain to determine capacity
    uint32_t clusters = 0;
    uint32_t c = first_cluster;
    int guard = 0;
    while (c >= 2 && c < 0x0FFFFFF8) {
        if (++guard > 100000) return -6;
        clusters++;
        uint32_t next;
        if (fat32_next_cluster(handle, c, &next) != 0) break;
        if (next >= 0x0FFFFFF8 || next == c) break;
        c = next;
    }

    uint64_t capacity = (uint64_t)clusters * (uint64_t)clus_size;
    if ((uint64_t)size > capacity) {
        return -7; // no space without realloc
    }

    // Write data into existing cluster chain
    void *buf = kmalloc(clus_size);
    if (!buf) return -8;

    size_t written = 0;
    c = first_cluster;
    guard = 0;
    while (c >= 2 && c < 0x0FFFFFF8 && written < size) {
        if (++guard > 100000) { kfree(buf); return -9; }

        memset(buf, 0, clus_size);
        size_t chunk = clus_size;
        if (chunk > (size - written)) chunk = (size - written);
        if (chunk) memcpy(buf, (const uint8_t*)data + written, chunk);

        if (fat32_write_cluster(handle, c, buf) != 0) { kfree(buf); return -10; }
        written += chunk;

        uint32_t next;
        if (fat32_next_cluster(handle, c, &next) != 0) break;
        if (next >= 0x0FFFFFF8 || next == c) break;
        c = next;
    }

    kfree(buf);

    // Update directory entry filesize in its parent directory
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

    // Find directory cluster
    uint32_t dir_cluster = fs->root_cluster;
    if (!(parent[0] == '/' && parent[1] == 0)) {
        struct fat_dir_entry de;
        if (fat32_find_file(handle, parent, &de) != 0) return 0; // can't update size, but data written
        if (de.attr & 0x10) {
            dir_cluster = ((uint32_t)de.first_cluster_high << 16) | (uint32_t)de.first_cluster_low;
        }
    }

    void *dirbuf = fat32_alloc_cluster_buffer(fs);
    if (!dirbuf) return 0;

    uint32_t dcl = dir_cluster;
    guard = 0;
    while (dcl >= 2 && dcl < 0x0FFFFFF8) {
        if (++guard > 100000) break;
        if (fat32_read_cluster(handle, dcl, dirbuf) != 0) break;

        uint32_t idx;
        struct fat_dir_entry ent;
        if (find_entry_in_cluster_index(fs, (const uint8_t*)dirbuf, clus_size, leaf, &ent, &idx) == 1) {
            ent.filesize = (uint32_t)size;
            memcpy((uint8_t*)dirbuf + idx * 32, &ent, sizeof(ent));
            fat32_write_cluster(handle, dcl, dirbuf);
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
        VGA_Writef("FAT32: invalid cluster size %u\n", clus_size);
        return -2;
    }

    static uint8_t buf[FAT32_MAX_CLUSTER_SIZE];

    VGA_Writef("FAT32 root directory (handle %d):\n", handle);

    int iterations = 0;
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        if (++iterations > 100) {
            VGA_Write("FAT32: too many clusters\n");
            return -3;
        }

        if (fat32_read_cluster(handle, cluster, buf) != 0) {
            VGA_Writef("FAT32: failed to read cluster %u\n", cluster);
            return -4;
        }

        size_t entries = safe_divide(clus_size, 32);

        const uint8_t* lfn_stack[20];
        int lfn_count = 0;

        for (size_t i = 0; i < entries; i++) {
            struct fat_dir_entry* e = (struct fat_dir_entry*)(buf + i * 32);
            uint8_t first = (uint8_t)e->name[0];

            if (first == 0x00) return 0;
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

            VGA_Writef("  %s %s size=%u\n",
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
            VGA_Writef("FAT32: file not found: %s\n", component);
            return -2;
        }

        /* More path left? Must be directory */
        if (path[path_idx] == '/' && !(entry.attr & 0x10)) {
            VGA_Writef("FAT32: %s is not a directory\n", component);
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
            uint8_t cluster_buf[FAT32_MAX_CLUSTER_SIZE];
            uint8_t* dest = (uint8_t*)out_buf;
            size_t total_read = 0;
            uint32_t clus_size = (uint32_t)fs->bytes_per_sector * (uint32_t)fs->sectors_per_cluster;

            while (file_cluster >= 2 && file_cluster < 0x0FFFFFF8 && remaining > 0) {
                if (fat32_read_cluster(handle, file_cluster, cluster_buf) != 0) {
                    VGA_Writef("FAT32: failed to read cluster %u\n", file_cluster);
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
                    return -6;
                }

                if (next >= 0x0FFFFFF8 || next == file_cluster) break;
                file_cluster = next;
            }

            if (out_size) *out_size = total_read;
            // VGA_Writef("FAT32: read file '%s', %u bytes\n", component, (unsigned)total_read);
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
        static uint8_t buf[FAT32_MAX_CLUSTER_SIZE];

        while (cluster >= 2 && cluster < 0x0FFFFFF8) {
            if (fat32_read_cluster(handle, cluster, buf) != 0) return -2;

            if (find_entry_in_cluster(fs, buf, clus_size, component, &entry)) {
                found = 1;
                break;
            }

            uint32_t next;
            if (fat32_next_cluster(handle, cluster, &next) != 0) return -3;
            if (next >= 0x0FFFFFF8 || next == cluster) break;
            cluster = next;
        }

        if (!found) return -4; /* Component not found */

        /* If not the last component, must be directory */
        if (path[path_idx] != '\0' && !(entry.attr & 0x10)) return -5;

        current_cluster = ((uint32_t)entry.first_cluster_high << 16) |
                          (uint32_t)entry.first_cluster_low;

        if (path[path_idx] == '/') path_idx++;

        /* Last component reached */
        if (path[path_idx] == '\0') {
            memcpy(out_entry, &entry, sizeof(struct fat_dir_entry));
            return 0;
        }
    }

    return -6; /* Should not reach here */
}