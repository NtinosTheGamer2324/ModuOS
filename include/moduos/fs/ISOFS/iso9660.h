#ifndef ISO9660_H
#define ISO9660_H

#include <stdint.h>
#include <stddef.h>

#define ISO9660_MAX_MOUNTS 8
#define ISO9660_MAX_FOLDER_ENTRIES 256

typedef struct {
    int active;
    int vdrive_id;      
    uint32_t partition_lba;
    uint32_t logical_block_size;
    uint32_t root_extent_lba;
    uint32_t root_size;
    int is_optical;
} iso9660_fs_t;

/* ISO9660 directory entry info (for fs_stat compatibility) */
typedef struct {
    char name[256];
    uint32_t extent_lba;
    uint32_t size;
    uint8_t flags;
} iso9660_dir_entry_t;

/* Directory entry for iso9660_read_folder */
typedef struct {
    char name[256];
    uint32_t size;
    uint32_t extent_lba;
    uint8_t is_directory;
} iso9660_folder_entry_t;

/* Mount returns a handle (index), or negative on error */
int iso9660_mount(int drive_index, uint32_t partition_lba);
int iso9660_mount_auto(int drive_index); /* Returns first successful mount handle */
void iso9660_unmount(int handle);
void iso9660_unmount_all(void);

/* All operations now require a handle */
int iso9660_list_root(int handle);
int iso9660_directory_exists(int handle, const char* path);
int iso9660_read_file_by_path(int handle, const char* path, void* out_buf, size_t buf_size, size_t* out_size);
int iso9660_list_directory(int handle, const char* path);
int iso9660_read_extent(int handle, uint32_t extent_lba, uint32_t size_bytes, void* buffer);

/* Find file and get its info (for fs_stat) */
int iso9660_find_file(int handle, const char* path, iso9660_dir_entry_t* out_entry);

/* Read folder entries - returns number of entries read (or negative on error) */
int iso9660_read_folder(int handle, const char* path, iso9660_folder_entry_t* entries, int max_entries);

/* Get filesystem info */
const iso9660_fs_t* iso9660_get_fs(int handle);
int iso9660_get_mount_count(void);

#endif /* ISO9660_H */