//fs.h - Kernel filesystem interface
#ifndef FS_H
#define FS_H

#include <stdint.h>
#include <stddef.h>

#define A 0
#define B 1
#define C 2
#define D 3
#define E 4
#define F 5
#define G 6
#define H 7
#define I 8
#define J 9
#define K 10
#define L 11
#define M 12
#define N 13
#define O 14
#define P 15
#define Q 16
#define R 17
#define S 18
#define T 19
#define U 20
#define V 21
#define W 22
#define X 23
#define Y 24
#define Z 25

/* Filesystem types */
typedef enum {
    FS_TYPE_UNKNOWN = 0,
    FS_TYPE_FAT32   = 1,
    FS_TYPE_ISO9660 = 2
} fs_type_t;

/* File information structure */
typedef struct {
    char name[260];           /* File/directory name */
    uint32_t size;            /* File size in bytes */
    int is_directory;         /* 1 if directory, 0 if file */
    uint32_t cluster;         /* Starting cluster (FAT32) or extent (ISO9660) */
} fs_file_info_t;

/* Mount handle - encapsulates filesystem-specific handle */
typedef struct {
    fs_type_t type;           /* Filesystem type */
    int handle;               /* Filesystem-specific handle */
    int valid;                /* Is this mount valid? */
} fs_mount_t;

/* --- KERNEL MOUNT TABLE MANAGEMENT --- */

/**
 * Initialize filesystem mount table
 * Called once during kernel init
 */
void fs_init(void);

/**
 * Mount a drive (auto-detect or specific type)
 * @param drive_index: Physical drive number (0-3)
 * @param partition_lba: Partition LBA offset (0 for whole disk)
 * @param type: FS_TYPE_UNKNOWN for auto-detect, or specific type
 * @return: Slot ID (0-25) on success, negative on error
 *          -2: Already mounted
 *          -3: Mount table full
 *          -4: Unknown filesystem type
 *          -5: Mount failed
 */
int fs_mount_drive(int drive_index, uint32_t partition_lba, fs_type_t type);

/**
 * Unmount filesystem by slot ID
 * @param slot: Slot ID (0-25, corresponds to A-Z)
 * @return: 0 on success, negative on error
 */
int fs_unmount_slot(int slot);

/**
 * Get mount handle by slot ID
 * @param slot: Slot ID (0-25)
 * @return: Pointer to mount structure, or NULL if invalid/unmounted
 */
fs_mount_t* fs_get_mount(int slot);

/**
 * Get mount metadata
 * @param slot: Slot ID
 * @param drive_index: Output - physical drive number (can be NULL)
 * @param partition_lba: Output - partition LBA (can be NULL)
 * @param type: Output - filesystem type (can be NULL)
 * @return: 0 on success, -1 if slot invalid/unmounted
 */
int fs_get_mount_info(int slot, int* drive_index, uint32_t* partition_lba, fs_type_t* type);

/**
 * List all active mounts (prints to VGA)
 */
void fs_list_mounts(void);

/**
 * Get total number of active mounts
 * @return: Number of mounted filesystems
 */
int fs_get_mount_count(void);

/* --- FILE OPERATIONS --- */

/**
 * Read entire file into buffer
 * @param mount: Mount handle (from fs_get_mount)
 * @param path: File path (e.g., "/dir/file.txt")
 * @param buffer: Output buffer
 * @param buffer_size: Size of output buffer
 * @param bytes_read: Optional - actual bytes read (can be NULL)
 * @return: 0 on success, negative on error
 */
int fs_read_file(fs_mount_t* mount, const char* path, void* buffer, 
                 size_t buffer_size, size_t* bytes_read);

/**
 * Get file information
 * @param mount: Mount handle
 * @param path: File path
 * @param info: Output file info structure
 * @return: 0 on success, negative on error
 */
int fs_stat(fs_mount_t* mount, const char* path, fs_file_info_t* info);

/**
 * Check if file exists
 * @param mount: Mount handle
 * @param path: File path
 * @return: 1 if exists, 0 if not
 */
int fs_file_exists(fs_mount_t* mount, const char* path);

/* --- DIRECTORY OPERATIONS --- */

/**
 * List directory contents
 * @param mount: Mount handle
 * @param path: Directory path (NULL or "/" for root)
 * @return: 0 on success, negative on error
 */
int fs_list_directory(fs_mount_t* mount, const char* path);

/**
 * Check if directory exists
 * @param mount: Mount handle
 * @param path: Directory path
 * @return: 1 if exists and is directory, 0 otherwise
 */
int fs_directory_exists(fs_mount_t* mount, const char* path);

/* --- UTILITY FUNCTIONS --- */

/**
 * Get filesystem type name
 * @param type: Filesystem type
 * @return: String name (e.g., "FAT32", "ISO9660")
 */
const char* fs_type_name(fs_type_t type);

#endif /* FS_H */