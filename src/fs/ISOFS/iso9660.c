#include "moduos/fs/ISOFS/iso9660.h"
#include "moduos/drivers/Drive/ATA/ata.h"
#include "moduos/drivers/Drive/ATA/atapi.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/memory/string.h"
#include <stdint.h>

#define ISO9660_PVD_SECTOR 16
#define ISO9660_ID "CD001"
#define ISO9660_ID_LEN 5
#define ISO9660_MAX_LB 4096
#define ISO9660_READ_BUFFER_LIMIT (64 * 1024)

/* Global array of mounted filesystems */
static iso9660_fs_t iso_mounts[ISO9660_MAX_MOUNTS];

/* Initialize on first use */
static void iso9660_init_once(void) {
    static int initialized = 0;
    if (!initialized) {
        memset(iso_mounts, 0, sizeof(iso_mounts));
        initialized = 1;
    }
}

/* Find free slot */
static int iso9660_alloc_handle(void) {
    for (int i = 0; i < ISO9660_MAX_MOUNTS; i++) {
        if (!iso_mounts[i].active) {
            return i;
        }
    }
    return -1;
}

/* Validate handle */
static int iso9660_valid_handle(int handle) {
    return (handle >= 0 && handle < ISO9660_MAX_MOUNTS && iso_mounts[handle].active);
}

/* Read blocks relative to partition for ATA, or absolute for ATAPI */
static int read_logical_blocks_rel(const iso9660_fs_t* fs, uint32_t block_rel, uint32_t block_count, void* buffer) {
    if (fs->is_atapi) {
        return atapi_read_blocks_pio(fs->drive_index, block_rel, block_count, buffer);
    } else {
        uint32_t sectors_per_block = fs->logical_block_size / 512;
        uint32_t start_sector = fs->partition_lba + block_rel * sectors_per_block;
        return ata_read_sectors(fs->drive_index, start_sector, buffer, sectors_per_block * block_count);
    }
}

/* --- PUBLIC API --- */

int iso9660_mount(int drive_index, uint32_t partition_lba) {
    iso9660_init_once();
    
    if (drive_index < 0 || drive_index > 3) return -1;

    int handle = iso9660_alloc_handle();
    if (handle < 0) {
        VGA_Write("ISO9660: no free mount slots\n");
        return -2;
    }

    iso9660_fs_t* fs = &iso_mounts[handle];
    memset(fs, 0, sizeof(iso9660_fs_t));
    
    fs->drive_index = drive_index;
    fs->partition_lba = partition_lba;

    /* Detect ATAPI device */
    const ata_drive_t* d = ata_get_drive(drive_index);
    fs->is_atapi = (d && d->exists && d->is_atapi);

    /* Read PVD sector (logical block 16) */
    uint8_t pvd_buf[2048];
    int r;
    if (fs->is_atapi) {
        r = atapi_read_blocks_pio(drive_index, ISO9660_PVD_SECTOR, 1, pvd_buf);
    } else {
        uint32_t pvd_sector = partition_lba + ISO9660_PVD_SECTOR * (2048 / 512);
        r = ata_read_sectors(drive_index, pvd_sector, pvd_buf, 2048 / 512);
    }
    if (r != 0) {
        VGA_Writef("ISO9660: failed to read PVD from drive %d\n", drive_index);
        return -3;
    }

    /* Validate PVD */
    if (pvd_buf[0] != 1 || memcmp(&pvd_buf[1], ISO9660_ID, ISO9660_ID_LEN) != 0 || pvd_buf[6] != 1) {
        VGA_Write("ISO9660: invalid Primary Volume Descriptor\n");
        return -4;
    }

    fs->logical_block_size = pvd_buf[128] | (pvd_buf[129] << 8);
    if (fs->logical_block_size == 0) fs->logical_block_size = 2048;
    if (fs->logical_block_size > ISO9660_MAX_LB) return -5;

    /* Root directory record at offset 156 */
    uint8_t* rd = &pvd_buf[156];
    uint8_t rd_len = rd[0];
    if (rd_len == 0) return -6;

    fs->root_extent_lba = rd[2] | (rd[3] << 8) | (rd[4] << 16) | (rd[5] << 24);
    fs->root_size = rd[10] | (rd[11] << 8) | (rd[12] << 16) | (rd[13] << 24);
    fs->active = 1;

    VGA_Writef("ISO9660: mounted handle=%d, drive=%d, lbs=%u, root_extent=%u, atapi=%d\n",
               handle, drive_index, fs->logical_block_size, fs->root_extent_lba, fs->is_atapi);
    return handle;
}

int iso9660_mount_auto(int drive_index) {
    iso9660_init_once();
    
    int start = (drive_index >= 0) ? drive_index : 0;
    int end = (drive_index >= 0) ? drive_index : 3;

    for (int d = start; d <= end; d++) {
        int handle = iso9660_mount(d, 0);
        if (handle >= 0) return handle;

        /* Try MBR partitions */
        uint8_t mbr[512];
        if (ata_read_sector_lba28(d, 0, mbr) != 0) continue;
        if (mbr[510] != 0x55 || mbr[511] != 0xAA) continue;

        for (int i = 0; i < 4; i++) {
            uint32_t off = 0x1BE + i * 16;
            uint32_t lba = mbr[off + 8] | (mbr[off + 9] << 8) | (mbr[off + 10] << 16) | (mbr[off + 11] << 24);
            if (lba == 0) continue;
            handle = iso9660_mount(d, lba);
            if (handle >= 0) return handle;
        }
    }
    VGA_Write("ISO9660: no valid PVD found\n");
    return -1;
}

void iso9660_unmount(int handle) {
    if (iso9660_valid_handle(handle)) {
        VGA_Writef("ISO9660: unmounting handle %d\n", handle);
        memset(&iso_mounts[handle], 0, sizeof(iso9660_fs_t));
    }
}

void iso9660_unmount_all(void) {
    for (int i = 0; i < ISO9660_MAX_MOUNTS; i++) {
        if (iso_mounts[i].active) {
            iso9660_unmount(i);
        }
    }
}

const iso9660_fs_t* iso9660_get_fs(int handle) {
    if (!iso9660_valid_handle(handle)) return NULL;
    return &iso_mounts[handle];
}

int iso9660_get_mount_count(void) {
    int count = 0;
    for (int i = 0; i < ISO9660_MAX_MOUNTS; i++) {
        if (iso_mounts[i].active) count++;
    }
    return count;
}

int iso9660_read_extent(int handle, uint32_t extent_lba, uint32_t size_bytes, void* buffer) {
    if (!iso9660_valid_handle(handle)) return -1;
    
    iso9660_fs_t* fs = &iso_mounts[handle];
    uint32_t lbs = fs->logical_block_size;
    uint32_t blocks = (size_bytes + lbs - 1) / lbs;
    
    // Temporary buffer for reading full blocks
    static uint8_t block_buf[2048];  // ISO9660 standard block size
    
    uint32_t remaining = size_bytes;
    uint8_t* dest = (uint8_t*)buffer;
    
    for (uint32_t b = 0; b < blocks; b++) {
        // Read block into temporary buffer
        int r = read_logical_blocks_rel(fs, extent_lba + b, 1, block_buf);
        if (r != 0) return -2;
        
        // Copy only the bytes we need from this block
        uint32_t to_copy = (remaining < lbs) ? remaining : lbs;
        memcpy(dest, block_buf, to_copy);
        
        dest += to_copy;
        remaining -= to_copy;
    }
    
    return 0;
}

/* Helper: Parse directory record at offset in buffer */
static int parse_dir_record(const uint8_t* buf, uint32_t offset, 
                            char* out_name, uint32_t* out_extent,
                            uint32_t* out_size, uint8_t* out_flags) {
    uint8_t len = buf[offset];
    if (len == 0) return 0;
    
    uint8_t name_len = buf[offset + 32];
    *out_flags = buf[offset + 25];
    *out_extent = buf[offset + 2] | (buf[offset + 3] << 8) | 
                  (buf[offset + 4] << 16) | (buf[offset + 5] << 24);
    *out_size = buf[offset + 10] | (buf[offset + 11] << 8) | 
                (buf[offset + 12] << 16) | (buf[offset + 13] << 24);
    
    /* Extract name */
    int nm = 0;
    for (int i = 0; i < name_len && i < 255; i++) {
        char c = buf[offset + 33 + i];
        if (c == ';') break; /* Version separator */
        out_name[nm++] = c;
    }
    out_name[nm] = '\0';
    
    return len;
}

/* Helper: Find entry in directory extent */
static int find_entry_in_extent(int handle, uint32_t extent_lba, uint32_t extent_size,
                                 const char* name, uint32_t* out_extent,
                                 uint32_t* out_size, uint8_t* out_flags) {
    if (!iso9660_valid_handle(handle)) return -1;
    
    iso9660_fs_t* fs = &iso_mounts[handle];
    uint32_t lbs = fs->logical_block_size;
    uint32_t blocks = (extent_size + lbs - 1) / lbs;
    
    if (blocks * lbs > ISO9660_READ_BUFFER_LIMIT) return -2;
    
    static uint8_t buf[ISO9660_READ_BUFFER_LIMIT];
    if (iso9660_read_extent(handle, extent_lba, blocks * lbs, buf) != 0) {
        return -3;
    }
    
    uint32_t offset = 0;
    uint32_t total = blocks * lbs;
    
    while (offset < total) {
        char entry_name[256];
        uint32_t entry_extent, entry_size;
        uint8_t entry_flags;
        
        int rec_len = parse_dir_record(buf, offset, entry_name, 
                                       &entry_extent, &entry_size, &entry_flags);
        if (rec_len == 0) {
            /* Skip to next block */
            offset = ((offset / lbs) + 1) * lbs;
            if (offset >= total) break;
            continue;
        }
        
        /* Skip . and .. entries */
        if (!(strlen(entry_name) == 1 && (entry_name[0] == 0 || entry_name[0] == 1))) {
            /* Case-insensitive compare */
            int match = 1;
            for (int i = 0; entry_name[i] && name[i]; i++) {
                char c1 = entry_name[i];
                char c2 = name[i];
                if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
                if (c2 >= 'a' && c2 <= 'z') c2 -= 32;
                if (c1 != c2) { match = 0; break; }
            }
            
            if (match && strlen(entry_name) == strlen(name)) {
                *out_extent = entry_extent;
                *out_size = entry_size;
                *out_flags = entry_flags;
                return 0; /* Found */
            }
        }
        
        offset += rec_len;
    }
    
    return -4; /* Not found */
}

/* Helper: List all entries in a directory extent */
static int list_extent_entries(int handle, uint32_t extent_lba, uint32_t extent_size) {
    if (!iso9660_valid_handle(handle)) return -1;
    
    iso9660_fs_t* fs = &iso_mounts[handle];
    uint32_t lbs = fs->logical_block_size;
    uint32_t blocks = (extent_size + lbs - 1) / lbs;
    
    if (blocks * lbs > ISO9660_READ_BUFFER_LIMIT) return -2;
    
    static uint8_t buf[ISO9660_READ_BUFFER_LIMIT];
    if (iso9660_read_extent(handle, extent_lba, blocks * lbs, buf) != 0) {
        return -3;
    }
    
    uint32_t offset = 0;
    uint32_t total = blocks * lbs;
    
    while (offset < total) {
        char name[256];
        uint32_t extent, size;
        uint8_t flags;
        
        int rec_len = parse_dir_record(buf, offset, name, &extent, &size, &flags);
        if (rec_len == 0) {
            /* Skip to next block */
            offset = ((offset / lbs) + 1) * lbs;
            if (offset >= total) break;
            continue;
        }
        
        /* Skip . and .. entries */
        if (!(strlen(name) == 1 && (name[0] == 0 || name[0] == 1))) {
            VGA_Writef("  %s %s size=%u extent=%u\n", 
                       name, 
                       (flags & 0x02) ? "<DIR>" : "", 
                       size, extent);
        }
        
        offset += rec_len;
    }
    
    return 0;
}

/* Find file and return its directory entry info */
int iso9660_find_file(int handle, const char* path, iso9660_dir_entry_t* out_entry) {
    if (!iso9660_valid_handle(handle)) return -1;
    if (path == NULL || path[0] == '\0' || out_entry == NULL) return -2;
    
    iso9660_fs_t* fs = &iso_mounts[handle];
    
    /* Start at root */
    uint32_t current_extent = fs->root_extent_lba;
    uint32_t current_size = fs->root_size;
    
    /* Parse and traverse path */
    char component[256];
    int comp_idx = 0;
    int path_idx = 0;
    
    /* Skip leading slash */
    if (path[0] == '/') path_idx++;
    
    /* Extract path components until we reach the file */
    while (path[path_idx] != '\0') {
        /* Extract path component */
        comp_idx = 0;
        while (path[path_idx] != '\0' && path[path_idx] != '/' && comp_idx < 255) {
            component[comp_idx++] = path[path_idx++];
        }
        component[comp_idx] = '\0';
        
        if (comp_idx == 0) {
            if (path[path_idx] == '/') path_idx++;
            continue;
        }
        
        /* Find this component in current directory */
        uint32_t next_extent, next_size;
        uint8_t flags;
        
        if (find_entry_in_extent(handle, current_extent, current_size,
                                 component, &next_extent, &next_size, &flags) != 0) {
            return -3; /* Not found */
        }
        
        /* Check if this is the last component */
        if (path[path_idx] == '\0') {
            /* Found it - fill out_entry */
            strncpy(out_entry->name, component, sizeof(out_entry->name) - 1);
            out_entry->name[sizeof(out_entry->name) - 1] = '\0';
            out_entry->extent_lba = next_extent;
            out_entry->size = next_size;
            out_entry->flags = flags;
            return 0; /* Success */
        }
        
        /* Not the last component - must be a directory */
        if (!(flags & 0x02)) {
            return -4; /* Not a directory */
        }
        
        /* Move to next directory */
        current_extent = next_extent;
        current_size = next_size;
        
        if (path[path_idx] == '/') path_idx++;
    }
    
    /* Should not reach here */
    return -5;
}

int iso9660_read_file_by_path(int handle, const char* path, void* out_buf, 
                               size_t buf_size, size_t* out_size) {
    if (!iso9660_valid_handle(handle)) {
        VGA_Write("ISO9660: invalid handle\n");
        return -1;
    }
    
    if (path == NULL || path[0] == '\0') {
        VGA_Write("ISO9660: invalid path\n");
        return -2;
    }
    
    if (out_buf == NULL || buf_size == 0) {
        VGA_Write("ISO9660: invalid buffer\n");
        return -3;
    }
    
    iso9660_fs_t* fs = &iso_mounts[handle];
    
    /* Start at root */
    uint32_t current_extent = fs->root_extent_lba;
    uint32_t current_size = fs->root_size;
    
    /* Parse and traverse path */
    char component[256];
    int comp_idx = 0;
    int path_idx = 0;
    
    /* Skip leading slash */
    if (path[0] == '/') path_idx++;
    
    /* Extract path components until we reach the file */
    while (path[path_idx] != '\0') {
        /* Extract path component */
        comp_idx = 0;
        while (path[path_idx] != '\0' && path[path_idx] != '/' && comp_idx < 255) {
            component[comp_idx++] = path[path_idx++];
        }
        component[comp_idx] = '\0';
        
        if (comp_idx == 0) {
            if (path[path_idx] == '/') path_idx++;
            continue;
        }
        
        /* Find this component in current directory */
        uint32_t next_extent, next_size;
        uint8_t flags;
        
        if (find_entry_in_extent(handle, current_extent, current_size,
                                 component, &next_extent, &next_size, &flags) != 0) {
            VGA_Writef("ISO9660: path component '%s' not found\n", component);
            return -4;
        }
        
        /* Check if this is the last component (the file) */
        if (path[path_idx] == '\0') {
            /* This should be a file, not a directory */
            if (flags & 0x02) {
                VGA_Writef("ISO9660: '%s' is a directory, not a file\n", component);
                return -5;
            }
            
            /* Found the file - read it */
            if (next_size > buf_size) {
                VGA_Writef("ISO9660: file size (%u) exceeds buffer size (%zu)\n", 
                          next_size, buf_size);
                return -6;
            }
            
            /* Read the file extent */
            if (iso9660_read_extent(handle, next_extent, next_size, out_buf) != 0) {
                VGA_Writef("ISO9660: failed to read file '%s'\n", path);
                return -7;
            }
            
            if (out_size != NULL) {
                *out_size = (size_t)next_size;
            }
            
            // VGA_Writef("ISO9660: successfully read file '%s' (%u bytes)\n", 
            //          path, next_size);
            return 0; /* Success */
        }
        
        /* Not the last component - must be a directory */
        if (!(flags & 0x02)) {
            VGA_Writef("ISO9660: '%s' is not a directory\n", component);
            return -8;
        }
        
        /* Move to next directory */
        current_extent = next_extent;
        current_size = next_size;
        
        if (path[path_idx] == '/') path_idx++;
    }
    
    /* Should not reach here */
    VGA_Write("ISO9660: empty filename\n");
    return -9;
}

/* Public API: List any directory by path */
int iso9660_list_directory(int handle, const char* path) {
    if (!iso9660_valid_handle(handle)) {
        VGA_Write("ISO9660: invalid handle\n");
        return -1;
    }
    
    iso9660_fs_t* fs = &iso_mounts[handle];
    
    /* Handle root or empty path */
    if (path == NULL || path[0] == '\0' || 
        (path[0] == '/' && path[1] == '\0')) {
        VGA_Writef("ISO9660 root directory (handle %d):\n", handle);
        return list_extent_entries(handle, fs->root_extent_lba, fs->root_size);
    }
    
    /* Parse path and traverse */
    uint32_t current_extent = fs->root_extent_lba;
    uint32_t current_size = fs->root_size;
    char component[256];
    int comp_idx = 0;
    int path_idx = 0;
    
    /* Skip leading slash */
    if (path[0] == '/') path_idx++;
    
    while (path[path_idx] != '\0') {
        /* Extract path component */
        comp_idx = 0;
        while (path[path_idx] != '\0' && path[path_idx] != '/' && comp_idx < 255) {
            component[comp_idx++] = path[path_idx++];
        }
        component[comp_idx] = '\0';
        
        if (comp_idx == 0) {
            if (path[path_idx] == '/') path_idx++;
            continue;
        }
        
        /* Find this component in current directory */
        uint32_t next_extent, next_size;
        uint8_t flags;
        
        if (find_entry_in_extent(handle, current_extent, current_size,
                                 component, &next_extent, &next_size, &flags) != 0) {
            VGA_Writef("ISO9660: path component '%s' not found\n", component);
            return -2;
        }
        
        /* Check if it's a directory */
        if (!(flags & 0x02)) {
            VGA_Writef("ISO9660: '%s' is not a directory\n", component);
            return -3;
        }
        
        /* Move to next directory */
        current_extent = next_extent;
        current_size = next_size;
        
        if (path[path_idx] == '/') path_idx++;
    }
    
    /* List the final directory */
    VGA_Writef("ISO9660 directory '%s' (handle %d):\n", path, handle);
    return list_extent_entries(handle, current_extent, current_size);
}

int iso9660_directory_exists(int handle, const char* path) {
    if (!iso9660_valid_handle(handle)) return 0;
    
    iso9660_fs_t* fs = &iso_mounts[handle];
    
    /* Handle root */
    if (path == NULL || path[0] == '\0' || 
        (path[0] == '/' && path[1] == '\0')) {
        return 1; /* Root always exists */
    }
    
    /* Parse path and traverse */
    uint32_t current_extent = fs->root_extent_lba;
    uint32_t current_size = fs->root_size;
    char component[256];
    int comp_idx = 0;
    int path_idx = 0;
    
    if (path[0] == '/') path_idx++;
    
    while (path[path_idx] != '\0') {
        comp_idx = 0;
        while (path[path_idx] != '\0' && path[path_idx] != '/' && comp_idx < 255) {
            component[comp_idx++] = path[path_idx++];
        }
        component[comp_idx] = '\0';
        
        if (comp_idx == 0) {
            if (path[path_idx] == '/') path_idx++;
            continue;
        }
        
        /* Find this component */
        uint32_t next_extent, next_size;
        uint8_t flags;
        
        if (find_entry_in_extent(handle, current_extent, current_size,
                                 component, &next_extent, &next_size, &flags) != 0) {
            return 0; /* Not found */
        }
        
        /* Check if it's a directory */
        if (!(flags & 0x02)) {
            return 0; /* Not a directory */
        }
        
        current_extent = next_extent;
        current_size = next_size;
        
        if (path[path_idx] == '/') path_idx++;
    }
    
    return 1; /* Path exists and is a directory */
}

int iso9660_list_root(int handle) {
    if (!iso9660_valid_handle(handle)) return -1;
    
    iso9660_fs_t* fs = &iso_mounts[handle];
    uint32_t lbs = fs->logical_block_size;
    uint32_t blocks = (fs->root_size + lbs - 1) / lbs;

    if (blocks * lbs > ISO9660_READ_BUFFER_LIMIT) return -2;
    static uint8_t buf[ISO9660_READ_BUFFER_LIMIT];
    if (iso9660_read_extent(handle, fs->root_extent_lba, blocks * lbs, buf) != 0) return -3;

    VGA_Writef("ISO9660 root directory (handle %d):\n", handle);
    uint32_t offset = 0;
    uint32_t total = blocks * lbs;
    while (offset < total) {
        uint8_t len = buf[offset];
        if (len == 0) {
            offset = ((offset / lbs) + 1) * lbs;
            continue;
        }

        uint8_t name_len = buf[offset + 32];
        uint8_t flags = buf[offset + 25];
        uint32_t extent = buf[offset + 2] | (buf[offset + 3] << 8) | (buf[offset + 4] << 16) | (buf[offset + 5] <<24);
        uint32_t data_len = buf[offset + 10] | (buf[offset + 11] << 8) | (buf[offset + 12] << 16) | (buf[offset + 13] << 24);
            char name[256];
            int nm = 0;
            for (int i = 0; i < name_len && i < (int)sizeof(name)-1; i++) {
                char c = buf[offset + 33 + i];
                if (c == ';') break;
                name[nm++] = c;
            }
            name[nm] = 0;
        
            if (!(name_len == 1 && (buf[offset + 33] == 0 || buf[offset + 33] == 1))) {
                VGA_Writef("  %s %s size=%u extent=%u\n", name, (flags & 0x02) ? "<DIR>" : "", data_len, extent);
            }
        
            offset += len;
        }

return 0;
}

