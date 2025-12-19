//fs.c - Kernel-owned mount table with FORMAT support
#include "moduos/fs/fs.h"
#include "moduos/fs/DOS/FAT32/fat32.h"
#include "moduos/fs/ISOFS/iso9660.h"
#include "moduos/drivers/Drive/vDrive.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/COM/com.h"

/* Kernel mount table - like Linux's mount namespace */
#define MAX_MOUNTS 26

typedef struct {
    fs_mount_t mount;
    int vdrive_id;
    uint32_t partition_lba;
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
    
    /* If partition_lba is 0, auto-detect. Otherwise, use specified LBA */
    if (partition_lba == 0 && type == FS_TYPE_UNKNOWN) {
        /* Auto-detect filesystem AND partition */
        handle = fat32_mount_auto(vdrive_id);
        if (handle >= 0) {
            mount.type = FS_TYPE_FAT32;
            mount.handle = handle;
            mount.valid = 1;
            /* Get actual LBA from mounted filesystem */
            const fat32_fs_t *fs = fat32_get_fs(handle);
            if (fs) partition_lba = fs->partition_lba;
        } else {
            handle = iso9660_mount_auto(vdrive_id);
            if (handle >= 0) {
                mount.type = FS_TYPE_ISO9660;
                mount.handle = handle;
                mount.valid = 1;
                /* Get actual LBA from mounted filesystem */
                const iso9660_fs_t *fs = iso9660_get_fs(handle);
                if (fs) partition_lba = fs->partition_lba;
            }
        }
    } else if (type == FS_TYPE_UNKNOWN) {
        /* User specified LBA, but unknown filesystem type - try both */
        handle = fat32_mount(vdrive_id, partition_lba);
        if (handle >= 0) {
            mount.type = FS_TYPE_FAT32;
            mount.handle = handle;
            mount.valid = 1;
        } else {
            handle = iso9660_mount(vdrive_id, partition_lba);
            if (handle >= 0) {
                mount.type = FS_TYPE_ISO9660;
                mount.handle = handle;
                mount.valid = 1;
            }
        }
    } else {
        /* User specified both LBA and filesystem type */
        switch (type) {
            case FS_TYPE_FAT32:
                handle = fat32_mount(vdrive_id, partition_lba);
                if (handle >= 0) {
                    mount.type = FS_TYPE_FAT32;
                    mount.handle = handle;
                    mount.valid = 1;
                }
                break;
                
            case FS_TYPE_ISO9660:
                handle = iso9660_mount(vdrive_id, partition_lba);
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
        return -5;
    }
    
    /* Store in mount table */
    mount_table[slot].mount = mount;
    mount_table[slot].vdrive_id = vdrive_id;
    mount_table[slot].partition_lba = partition_lba;
    mount_table[slot].in_use = 1;
    
    com_write_string(COM1_PORT, "[FS] Mounted ");
    com_write_string(COM1_PORT, fs_type_name(mount.type));
    com_write_string(COM1_PORT, " on vDrive ");
    char vd_str[4];
    vd_str[0] = '0' + vdrive_id;
    vd_str[1] = ' ';
    vd_str[2] = '\0';
    com_write_string(COM1_PORT, vd_str);
    com_write_string(COM1_PORT, "at LBA ");
    char lba_str[16];
    itoa(partition_lba, lba_str, 10);
    com_write_string(COM1_PORT, lba_str);
    com_write_string(COM1_PORT, " in slot ");
    char slot_str[4];
    slot_str[0] = '0' + slot;
    slot_str[1] = '\0';
    com_write_string(COM1_PORT, slot_str);
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

/* List all mounts */
void fs_list_mounts(void) {
    fs_init();
    
    VGA_Write("Slot  Type      vDrive  LBA\n");
    VGA_Write("----  --------  ------  ----------\n");
    
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mount_table[i].in_use) {
            char slot_ch = 'A' + i;
            VGA_WriteChar(slot_ch);
            VGA_Write("     ");
            
            const char* fs_name = fs_type_name(mount_table[i].mount.type);
            VGA_Write(fs_name);
            for (int j = strlen(fs_name); j < 10; j++) VGA_Write(" ");
            
            char num[16];
            itoa(mount_table[i].vdrive_id, num, 10);
            VGA_Write(num);
            VGA_Write("       ");
            
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
    
    if (bytes_read) *bytes_read = size;
    return result;
}

int fs_stat(fs_mount_t* mount, const char* path, fs_file_info_t* info) {
    if (!mount || !mount->valid || !path || !info) {
        return -1;
    }
    
    memset(info, 0, sizeof(fs_file_info_t));
    
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
    
    switch (mount->type) {
        case FS_TYPE_FAT32:
            return fat32_list_directory(mount->handle, path);
            
        case FS_TYPE_ISO9660:
            return iso9660_list_directory(mount->handle, path);
            
        default:
            return -2;
    }
}

int fs_directory_exists(fs_mount_t* mount, const char* path) {
    if (!mount || !mount->valid) return 0;
    
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
        case FS_TYPE_UNKNOWN: return "Unknown";
        default:              return "Invalid";
    }
}