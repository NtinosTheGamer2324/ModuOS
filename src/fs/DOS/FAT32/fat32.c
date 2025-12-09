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

/* --- MOUNT --- */
int fat32_mount(int vdrive_id, uint32_t partition_lba) {
    fat32_init_once();
    
    int handle = fat32_alloc_handle();
    if (handle < 0) {
        VGA_Write("FAT32: no free mount slots\n");
        return -1;
    }

    uint8_t sector[512];
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

    uint8_t mbr[512];
    
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