#include "moduos/fs/ISOFS/iso9660.h"
#include "moduos/drivers/Drive/vDrive.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/memory/memory.h"
#include <stdint.h>

#define ISO9660_PVD_SECTOR 16
#define ISO9660_ID "CD001"
#define ISO9660_ID_LEN 5
#define ISO9660_MAX_LB 4096
#define ISO9660_READ_BUFFER_LIMIT (64 * 1024)

/* Rock Ridge / SUSP signature identifiers */
#define RR_SIGNATURE_SP 0x5053  // "SP" - System Use Sharing Protocol
#define RR_SIGNATURE_NM 0x4D4E  // "NM" - Alternate Name (long filename)
#define RR_SIGNATURE_PX 0x5850  // "PX" - POSIX attributes
#define RR_SIGNATURE_CE 0x4543  // "CE" - Continuation Area

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

/* Read blocks relative to partition for ATA, or absolute for ATAPI/SATAPI */
static int read_logical_blocks_rel(const iso9660_fs_t* fs, uint32_t block_rel, uint32_t block_count, void* buffer) {
    if (fs->is_optical) {
        // For optical drives (both ATAPI and SATAPI), use vDrive which handles both
        uint32_t absolute_block = fs->partition_lba + block_rel;
        int result = vdrive_read(fs->vdrive_id, absolute_block, block_count, buffer);
        return (result == VDRIVE_SUCCESS) ? 0 : -1;
    } else {
        // For hard drives, convert logical blocks to 512-byte sectors
        uint32_t sectors_per_block = fs->logical_block_size / 512;
        uint32_t start_sector = fs->partition_lba + block_rel * sectors_per_block;
        uint32_t total_sectors = sectors_per_block * block_count;
        
        int result = vdrive_read(fs->vdrive_id, start_sector, total_sectors, buffer);
        return (result == VDRIVE_SUCCESS) ? 0 : -1;
    }
}

/* Parse Rock Ridge NM (Name) entry to extract long filename */
static int parse_rockridge_nm(const uint8_t* sue_data, uint8_t sue_len, char* name_buf, int* name_pos, int max_len) {
    if (sue_len < 5) return 0; // Too short
    
    uint8_t flags = sue_data[4];
    uint8_t name_len = sue_len - 5;
    
    // Copy name data
    for (uint8_t i = 0; i < name_len && *name_pos < max_len - 1; i++) {
        name_buf[(*name_pos)++] = sue_data[5 + i];
    }
    
    // Check if continuation flag is set (bit 0)
    return (flags & 0x01) ? 1 : 0;
}

/* Parse System Use Entry Area (SUSP) for Rock Ridge extensions */
static int parse_system_use_area(const uint8_t* sua, int sua_len, char* long_name, int max_name_len) {
    int offset = 0;
    int name_pos = 0;
    int found_name = 0;
    
    while (offset + 4 <= sua_len) {
        uint16_t signature = (uint16_t)sua[offset] | ((uint16_t)sua[offset + 1] << 8);
        uint8_t sue_len = sua[offset + 2];
        uint8_t sue_version = sua[offset + 3];
        
        if (sue_len < 4 || offset + sue_len > sua_len) {
            break; // Invalid or truncated entry
        }
        
        // Check for NM (alternate name) entry
        if (signature == RR_SIGNATURE_NM && sue_version == 1) {
            int continues = parse_rockridge_nm(sua + offset, sue_len, long_name, &name_pos, max_name_len);
            found_name = 1;
            
            if (!continues) {
                long_name[name_pos] = '\0';
                return 1; // Complete name found
            }
        }
        
        offset += sue_len;
    }
    
    if (found_name) {
        long_name[name_pos] = '\0';
        return 1;
    }
    
    return 0; // No Rock Ridge name found
}

/* Parse directory record and extract both ISO and Rock Ridge names */
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
    
    // First, extract standard ISO9660 name as fallback
    int nm = 0;
    for (int i = 0; i < name_len && i < 255; i++) {
        char c = buf[offset + 33 + i];
        if (c == ';') break; /* Version separator */
        out_name[nm++] = c;
    }
    out_name[nm] = '\0';
    
    // Check for System Use Area (Rock Ridge extensions)
    // System Use starts after: length(1) + ext_attr_len(1) + extent(8) + size(8) + date(7) + flags(1) + file_unit_size(1) + gap_size(1) + volume_seq(4) + name_len(1) + name + padding
    int sua_offset = 33 + name_len;
    if (name_len % 2 == 0) sua_offset++; // Padding byte if name_len is even
    
    int sua_len = len - sua_offset;
    if (sua_len > 0 && sua_offset < len) {
        char long_name[256];
        if (parse_system_use_area(buf + offset + sua_offset, sua_len, long_name, sizeof(long_name))) {
            // Rock Ridge name found - use it instead
            strncpy(out_name, long_name, 255);
            out_name[255] = '\0';
        }
    }
    
    return len;
}

/* --- PUBLIC API --- */

int iso9660_mount(int vdrive_id, uint32_t partition_lba) {
    iso9660_init_once();
    
    if (!vdrive_is_ready(vdrive_id)) {
        VGA_Writef("ISO9660: vDrive %d not ready\n", vdrive_id);
        return -1;
    }

    int handle = iso9660_alloc_handle();
    if (handle < 0) {
        VGA_Write("ISO9660: no free mount slots\n");
        return -2;
    }

    iso9660_fs_t* fs = &iso_mounts[handle];
    memset(fs, 0, sizeof(iso9660_fs_t));
    
    fs->vdrive_id = vdrive_id;
    fs->partition_lba = partition_lba;

    vdrive_t* vdrive = vdrive_get(vdrive_id);
    if (vdrive) {
        fs->is_optical = (vdrive->type == VDRIVE_TYPE_ATA_ATAPI || 
                         vdrive->type == VDRIVE_TYPE_SATA_OPTICAL);
    }

    /*
     * IMPORTANT (DMA safety): storage drivers often DMA into the provided buffer and assume
     * it is physically contiguous. Stack buffers can cross a 4KiB page boundary depending
     * on stack layout (which changes when you add/remove code, e.g. DEVFS graphics).
     *
     * Use a single-page, 4KiB-aligned scratch buffer so a 2048-byte read never crosses
     * a page boundary.
     */
    static uint8_t pvd_page[4096] __attribute__((aligned(4096)));
    uint8_t *pvd_buf = pvd_page;

    if (fs->is_optical) {
        // For optical drives (ATAPI or SATAPI), read 2048-byte blocks via vDrive
        int r = vdrive_read(vdrive_id, ISO9660_PVD_SECTOR, 1, pvd_buf);
        if (r != VDRIVE_SUCCESS) {
            VGA_Writef("ISO9660: optical drive read failed (error %d)\n", r);
            return -3;
        }
    } else {
        // For hard drives, convert 2048-byte blocks to 512-byte sectors
        uint32_t pvd_lba = partition_lba + (ISO9660_PVD_SECTOR * 4);
        int r = vdrive_read(vdrive_id, pvd_lba, 4, pvd_buf);
        if (r != VDRIVE_SUCCESS) {
            VGA_Writef("ISO9660: failed to read PVD from vDrive %d\n", vdrive_id);
            return -3;
        }
    }

    if (pvd_buf[0] != 1 || memcmp(&pvd_buf[1], ISO9660_ID, ISO9660_ID_LEN) != 0 || pvd_buf[6] != 1) {
        VGA_Write("ISO9660: invalid Primary Volume Descriptor\n");
        return -4;
    }

    fs->logical_block_size = pvd_buf[128] | (pvd_buf[129] << 8);
    if (fs->logical_block_size == 0) fs->logical_block_size = 2048;
    if (fs->logical_block_size > ISO9660_MAX_LB) return -5;

    uint8_t* rd = &pvd_buf[156];
    uint8_t rd_len = rd[0];
    if (rd_len == 0) return -6;

    fs->root_extent_lba = rd[2] | (rd[3] << 8) | (rd[4] << 16) | (rd[5] << 24);
    fs->root_size = rd[10] | (rd[11] << 8) | (rd[12] << 16) | (rd[13] << 24);
    fs->active = 1;

    VGA_Writef("ISO9660: mounted handle=%d, vdrive=%d, lbs=%u, root_extent=%u (with Rock Ridge LFN support)\n",
               handle, vdrive_id, fs->logical_block_size, fs->root_extent_lba);
    return handle;
}

int iso9660_mount_auto(int vdrive_id) {
    iso9660_init_once();
    
    int start = (vdrive_id >= 0) ? vdrive_id : 0;
    int end = (vdrive_id >= 0) ? vdrive_id : (vdrive_get_count() - 1);

    for (int d = start; d <= end; d++) {
        if (!vdrive_is_ready(d)) continue;
        
        int handle = iso9660_mount(d, 0);
        if (handle >= 0) return handle;

        vdrive_t* vdrive = vdrive_get(d);
        if (vdrive && !vdrive->read_only) {
            /* DMA safety: keep MBR reads within a single 4KiB page. */
            static uint8_t mbr_page[4096] __attribute__((aligned(4096)));
            uint8_t *mbr = mbr_page;
            if (vdrive_read_sector(d, 0, mbr) != VDRIVE_SUCCESS) continue;
            if (mbr[510] != 0x55 || mbr[511] != 0xAA) continue;

            for (int i = 0; i < 4; i++) {
                uint32_t off = 0x1BE + i * 16;
                uint32_t lba = mbr[off + 8] | (mbr[off + 9] << 8) | 
                              (mbr[off + 10] << 16) | (mbr[off + 11] << 24);
                if (lba == 0) continue;
                
                handle = iso9660_mount(d, lba);
                if (handle >= 0) return handle;
            }
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
    
    static uint8_t block_buf[2048];
    uint32_t remaining = size_bytes;
    uint8_t* dest = (uint8_t*)buffer;
    
    for (uint32_t b = 0; b < blocks; b++) {
        int r = read_logical_blocks_rel(fs, extent_lba + b, 1, block_buf);
        if (r != 0) return -2;
        
        uint32_t to_copy = (remaining < lbs) ? remaining : lbs;
        memcpy(dest, block_buf, to_copy);
        
        dest += to_copy;
        remaining -= to_copy;
    }
    
    return 0;
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
                return 0;
            }
        }
        
        offset += rec_len;
    }
    
    return -4;
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

int iso9660_find_file(int handle, const char* path, iso9660_dir_entry_t* out_entry) {
    if (!iso9660_valid_handle(handle)) return -1;
    if (path == NULL || path[0] == '\0' || out_entry == NULL) return -2;
    
    iso9660_fs_t* fs = &iso_mounts[handle];
    uint32_t current_extent = fs->root_extent_lba;
    uint32_t current_size = fs->root_size;
    
    /* Normalize path - remove trailing slashes */
    char normalized_path[256];
    strncpy(normalized_path, path, sizeof(normalized_path) - 1);
    normalized_path[sizeof(normalized_path) - 1] = '\0';
    int len = strlen(normalized_path);
    while (len > 1 && normalized_path[len - 1] == '/') {
        normalized_path[len - 1] = '\0';
        len--;
    }
    
    char component[256];
    int comp_idx = 0;
    int path_idx = 0;
    
    if (normalized_path[0] == '/') path_idx++;
    
    while (normalized_path[path_idx] != '\0') {
        comp_idx = 0;
        while (normalized_path[path_idx] != '\0' && normalized_path[path_idx] != '/' && comp_idx < 255) {
            component[comp_idx++] = normalized_path[path_idx++];
        }
        component[comp_idx] = '\0';
        
        if (comp_idx == 0) {
            if (normalized_path[path_idx] == '/') path_idx++;
            continue;
        }
        
        uint32_t next_extent, next_size;
        uint8_t flags;
        
        if (find_entry_in_extent(handle, current_extent, current_size,
                                 component, &next_extent, &next_size, &flags) != 0) {
            return -3;
        }
        
        if (normalized_path[path_idx] == '\0') {
            strncpy(out_entry->name, component, sizeof(out_entry->name) - 1);
            out_entry->name[sizeof(out_entry->name) - 1] = '\0';
            out_entry->extent_lba = next_extent;
            out_entry->size = next_size;
            out_entry->flags = flags;
            return 0;
        }
        
        if (!(flags & 0x02)) {
            return -4;
        }
        
        current_extent = next_extent;
        current_size = next_size;
        
        if (normalized_path[path_idx] == '/') path_idx++;
    }
    
    return -5;
}

int iso9660_read_file_by_path(int handle, const char* path, void* out_buf, 
                               size_t buf_size, size_t* out_size) {
    if (!iso9660_valid_handle(handle)) return -1;
    if (path == NULL || path[0] == '\0') return -2;
    if (out_buf == NULL || buf_size == 0) return -3;
    
    iso9660_fs_t* fs = &iso_mounts[handle];
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
        
        uint32_t next_extent, next_size;
        uint8_t flags;
        
        if (find_entry_in_extent(handle, current_extent, current_size,
                                 component, &next_extent, &next_size, &flags) != 0) {
            VGA_Writef("ISO9660: path component '%s' not found\n", component);
            return -4;
        }
        
        if (path[path_idx] == '\0') {
            if (flags & 0x02) {
                VGA_Writef("ISO9660: '%s' is a directory, not a file\n", component);
                return -5;
            }
            
            if (next_size > buf_size) {
                VGA_Writef("ISO9660: file size (%u) exceeds buffer size (%zu)\n", 
                          next_size, buf_size);
                return -6;
            }
            
            if (iso9660_read_extent(handle, next_extent, next_size, out_buf) != 0) {
                VGA_Writef("ISO9660: failed to read file '%s'\n", path);
                return -7;
            }
            
            if (out_size != NULL) {
                *out_size = (size_t)next_size;
            }
            
            return 0;
        }
        
        if (!(flags & 0x02)) {
            VGA_Writef("ISO9660: '%s' is not a directory\n", component);
            return -8;
        }
        
        current_extent = next_extent;
        current_size = next_size;
        
        if (path[path_idx] == '/') path_idx++;
    }
    
    VGA_Write("ISO9660: empty filename\n");
    return -9;
}

int iso9660_list_directory(int handle, const char* path) {
    if (!iso9660_valid_handle(handle)) return -1;
    
    iso9660_fs_t* fs = &iso_mounts[handle];
    
    if (path == NULL || path[0] == '\0' || 
        (path[0] == '/' && path[1] == '\0')) {
        VGA_Writef("ISO9660 root directory (handle %d):\n", handle);
        return list_extent_entries(handle, fs->root_extent_lba, fs->root_size);
    }
    
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
        
        uint32_t next_extent, next_size;
        uint8_t flags;
        
        if (find_entry_in_extent(handle, current_extent, current_size,
                                 component, &next_extent, &next_size, &flags) != 0) {
            VGA_Writef("ISO9660: path component '%s' not found\n", component);
            return -2;
        }
        
        if (!(flags & 0x02)) {
            VGA_Writef("ISO9660: '%s' is not a directory\n", component);
            return -3;
        }
        
        current_extent = next_extent;
        current_size = next_size;
        
        if (path[path_idx] == '/') path_idx++;
    }
    
    VGA_Writef("ISO9660 directory '%s' (handle %d):\n", path, handle);
    return list_extent_entries(handle, current_extent, current_size);
}

int iso9660_directory_exists(int handle, const char* path) {
    if (!iso9660_valid_handle(handle)) return 0;
    
    iso9660_fs_t* fs = &iso_mounts[handle];
    
    if (path == NULL || path[0] == '\0' || 
        (path[0] == '/' && path[1] == '\0')) {
        return 1;
    }
    
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
        
        uint32_t next_extent, next_size;
        uint8_t flags;
        
        if (find_entry_in_extent(handle, current_extent, current_size,
                                 component, &next_extent, &next_size, &flags) != 0) {
            return 0;
        }
        
        if (!(flags & 0x02)) {
            return 0;
        }
        
        current_extent = next_extent;
        current_size = next_size;
        
        if (path[path_idx] == '/') path_idx++;
    }
    
    return 1;
}

int iso9660_list_root(int handle) {
    if (!iso9660_valid_handle(handle)) return -1;
    
    iso9660_fs_t* fs = &iso_mounts[handle];
    return list_extent_entries(handle, fs->root_extent_lba, fs->root_size);
}

/* Read directory entries for iteration - NEW for directory reading support */
int iso9660_read_folder(int handle, const char* path, iso9660_folder_entry_t* entries, int max_entries) {
    if (!iso9660_valid_handle(handle) || !entries || max_entries <= 0) {
        return -1;
    }

    const iso9660_fs_t* fs = &iso_mounts[handle];
    uint32_t dir_extent_lba;
    uint32_t dir_size;

    /* Determine directory extent */
    if (!path || strcmp(path, "/") == 0) {
        dir_extent_lba = fs->root_extent_lba;
        dir_size = fs->root_size;
    } else {
        /* Find the directory */
        iso9660_dir_entry_t dir_entry;
        if (iso9660_find_file(handle, path, &dir_entry) != 0) {
            return -1;
        }

        if (!(dir_entry.flags & 0x02)) {
            return -1; /* Not a directory */
        }

        dir_extent_lba = dir_entry.extent_lba;
        dir_size = dir_entry.size;
    }
    
    /* Allocate buffer for directory data */
    if (dir_size == 0 || dir_size > ISO9660_READ_BUFFER_LIMIT) {
        return -1;
    }
    
    /* Calculate actual buffer size needed (must be block-aligned) */
    uint32_t blocks_to_read = (dir_size + fs->logical_block_size - 1) / fs->logical_block_size;
    uint32_t buffer_size = blocks_to_read * fs->logical_block_size;
    
    uint8_t* dir_buf = (uint8_t*)kmalloc(buffer_size);
    if (!dir_buf) {
        return -1;
    }
    
    /* Read directory extent */
    if (read_logical_blocks_rel(fs, dir_extent_lba, blocks_to_read, dir_buf) != 0) {
        kfree(dir_buf);
        return -1;
    }
    
    /* Parse directory entries */
    int entry_count = 0;
    uint32_t offset = 0;
    
    while (offset < dir_size && entry_count < max_entries) {
        uint8_t record_len = dir_buf[offset];
        
        /* End of entries */
        if (record_len == 0) {
            /* Skip to next block if not at block boundary */
            uint32_t next_block = ((offset / fs->logical_block_size) + 1) * fs->logical_block_size;
            if (next_block >= dir_size) break;
            offset = next_block;
            continue;
        }
        
        if (offset + record_len > dir_size) {
            break;
        }
        
        /* Parse directory record */
        uint8_t* rec = dir_buf + offset;
        uint8_t name_len = rec[32];
        char* name_ptr = (char*)(rec + 33);
        uint8_t flags = rec[25];
        
        /* Skip . and .. entries */
        if (name_len == 1 && (name_ptr[0] == 0x00 || name_ptr[0] == 0x01)) {
            offset += record_len;
            continue;
        }
        
        /* Get extent LBA and size */
        uint32_t extent_lba = *(uint32_t*)(rec + 2);  /* Little endian */
        uint32_t extent_size = *(uint32_t*)(rec + 10); /* Little endian */
        
        /* Copy entry data */
        iso9660_folder_entry_t* out = &entries[entry_count];
        
        /* Check for Rock Ridge alternate name */
        int has_rr_name = 0;
        /* System Use area starts after name + padding */
        int su_offset = 33 + name_len;
        if (name_len % 2 == 0) su_offset++; /* Skip padding byte */
        
        if (su_offset < record_len) {
            uint8_t* su_area = rec + su_offset;
            int su_remaining = record_len - su_offset;
            int su_pos = 0;
            
            while (su_pos + 4 <= su_remaining) {
                uint16_t sig = *(uint16_t*)(su_area + su_pos);
                uint8_t len = su_area[su_pos + 2];
                
                if (len < 4 || su_pos + len > su_remaining) break;
                
                if (sig == RR_SIGNATURE_NM) {
                    /* Found Rock Ridge alternate name */
                    uint8_t flags_nm = su_area[su_pos + 4];
                    int nm_len = len - 5;
                    if (nm_len > 0 && nm_len < 255) {
                        memcpy(out->name, su_area + su_pos + 5, nm_len);
                        out->name[nm_len] = '\0';
                        has_rr_name = 1;
                        break;
                    }
                }
                
                su_pos += len;
            }
        }
        
        /* Use standard ISO name if no Rock Ridge name */
        if (!has_rr_name) {
            int copy_len = name_len;
            if (copy_len > 255) copy_len = 255;
            
            memcpy(out->name, name_ptr, copy_len);
            out->name[copy_len] = '\0';
            
            /* Remove version suffix (;1) */
            char* semicolon = strchr(out->name, ';');
            if (semicolon) *semicolon = '\0';
            
            /* Remove trailing dot if it's a directory */
            if (flags & 0x02) {
                int len = strlen(out->name);
                if (len > 0 && out->name[len - 1] == '.') {
                    out->name[len - 1] = '\0';
                }
            }
        }
        
        out->size = extent_size;
        out->extent_lba = extent_lba;
        out->is_directory = (flags & 0x02) ? 1 : 0;
        
        entry_count++;
        offset += record_len;
    }
    
    kfree(dir_buf);
    return entry_count;
}