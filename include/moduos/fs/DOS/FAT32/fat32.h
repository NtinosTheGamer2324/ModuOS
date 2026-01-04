#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include <stddef.h>

#define FAT32_MAX_MOUNTS 8
#define FAT32_MAX_CLUSTER_SIZE 65536
#define FAT32_MAX_FOLDER_ENTRIES 256

/* Directory entry for fat32_read_folder */
typedef struct {
    char name[260];           // Support long filenames
    uint32_t size;
    uint32_t first_cluster;
    uint8_t is_directory;
    uint8_t is_hidden;
    uint8_t is_system;
} fat32_folder_entry_t;

typedef struct {
    int active;
    int vdrive_id;          // CHANGED: was drive_index
    uint32_t partition_lba;
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t num_fats;
    uint32_t sectors_per_fat;
    uint32_t root_cluster;
    uint32_t total_sectors;
    uint32_t first_data_sector;
} fat32_fs_t;

/* Mount returns a handle (index), or negative on error */
int fat32_mount(int drive_index, uint32_t partition_lba);
int fat32_mount_auto(int drive_index); /* Returns first successful mount handle */
void fat32_unmount(int handle);
void fat32_unmount_all(void);

/* All operations now require a handle */
int fat32_list_root(int handle);
int fat32_directory_exists(int handle, const char* path);
int fat32_list_directory(int handle, const char* path);
int fat32_read_file_by_path(int handle, const char* path, void* out_buf, size_t buf_size, size_t* out_size);

/* Write/overwrite a file by path (existing file only). */
int fat32_write_file_by_path(int handle, const char* path, const void* data, size_t size);

// Create a directory (LFN supported)
int fat32_mkdir_by_path(int handle, const char* path);
int fat32_rmdir_by_path(int handle, const char* path);
int fat32_unlink_by_path(int handle, const char* path);
int fat32_read_cluster(int handle, uint32_t cluster, void* buffer);
int fat32_next_cluster(int handle, uint32_t cluster, uint32_t* out_next);

/* Read folder entries - returns number of entries read (or negative on error) */
int fat32_read_folder(int handle, const char* path, fat32_folder_entry_t* entries, int max_entries);

/* Get filesystem info */
const fat32_fs_t* fat32_get_fs(int handle);
int fat32_get_mount_count(void);
int fat32_format(int drive_index, uint32_t partition_lba, uint32_t partition_sectors,
                 const char* volume_label, uint32_t sectors_per_cluster);

#endif /* FAT32_H */