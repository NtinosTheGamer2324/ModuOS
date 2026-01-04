//fs.h - Kernel filesystem interface
#ifndef FS_H
#define FS_H

#include <stdint.h>
#include <stddef.h>

/* Filesystem types */
typedef enum {
    FS_TYPE_UNKNOWN   = 0,
    FS_TYPE_FAT32     = 1,
    FS_TYPE_ISO9660   = 2,
    FS_TYPE_EXTERNAL  = 3,
    FS_TYPE_MDFS      = 4
} fs_type_t;

/* File information structure */
typedef struct {
    char name[260];           /* File/directory name */
    uint32_t size;            /* File size in bytes */
    int is_directory;         /* 1 if directory, 0 if file */
    uint32_t cluster;         /* Starting cluster (FAT32) or extent (ISO9660) */
} fs_file_info_t;

/* Mount handle - encapsulates filesystem-specific handle */
struct fs_ext_driver_ops;

typedef struct {
    fs_type_t type;           /* Filesystem type */
    int handle;               /* Filesystem-specific handle */
    int valid;                /* Is this mount valid? */

    // External filesystem support (FS_TYPE_EXTERNAL)
    const struct fs_ext_driver_ops *ext_ops;
    void *ext_ctx;
    char ext_name[16];
} fs_mount_t;

/* --- KERNEL MOUNT TABLE MANAGEMENT --- */

/**
 * Initialize filesystem mount table
 * Called once during kernel init
 */
void fs_init(void);

/**
 * Format a partition with FAT32 filesystem
 * WARNING: This will erase all data on the specified partition!
 * 
 * The partition MUST be unmounted before formatting.
 * After formatting, you must mount it with fs_mount_drive() to use it.
 *
 * @param vdrive_id: Virtual drive ID (from vDrive subsystem)
 * @param partition_lba: Partition starting LBA offset
 * @param partition_sectors: Size of the partition in sectors (512 bytes each)
 * @param volume_label: Volume label string (max 11 chars, NULL for "NO NAME")
 * @param sectors_per_cluster: Cluster size in sectors (0 for auto, or 1/2/4/8/16/32/64/128)
 * @return: 0 on success, negative on error
 *          -1: vDrive not ready
 *          -2: Drive is mounted (unmount first)
 *          -3: Invalid partition size
 *          -4: Format failed
 * 
 * Example:
 *   // Format vDrive 0, starting at LBA 2048, size 1GB (2097152 sectors)
 *   fs_format(0, 2048, 2097152, "MYVOLUME", 0);
 *   // Then mount it:
 *   int slot = fs_mount_drive(0, 2048, FS_TYPE_FAT32);
 */
int fs_format(int vdrive_id, uint32_t partition_lba, uint32_t partition_sectors,
              const char* volume_label, uint32_t sectors_per_cluster);

/**
 * Mount a drive (auto-detect or specific type)
 * @param vdrive_id: Virtual drive ID (from vDrive subsystem)
 * @param partition_lba: Partition LBA offset (0 for whole disk/auto-detect)
 * @param type: FS_TYPE_UNKNOWN for auto-detect, or specific type
 * @return: Slot ID (0-25) on success, negative on error
 *          -2: Already mounted
 *          -3: Mount table full
 *          -4: Unknown filesystem type
 *          -5: Mount failed
 *          -6: vDrive not ready
 */
// Forward declarations for external driver ops
struct fs_dir;
struct fs_dirent;
typedef struct fs_dir fs_dir_t;
typedef struct fs_dirent fs_dirent_t;

// External FS driver ops (for third-party FS modules)
typedef struct fs_ext_driver_ops {
    // Return 1 if this FS recognizes the drive/partition, 0 otherwise.
    int (*probe)(int vdrive_id, uint32_t partition_lba);

    // Mount and populate mount->ext_ctx (and any other required fields).
    // mount->ext_ops and mount->ext_name are filled by core.
    int (*mount)(int vdrive_id, uint32_t partition_lba, fs_mount_t *mount);

    // Optional unmount hook. If NULL, core will just drop the mount.
    void (*unmount)(fs_mount_t *mount);

    // Optional format/mkfs hook.
    // vDrive-based; partition_lba is start of partition; partition_sectors is length.
    // Returns 0 on success.
    int (*mkfs)(int vdrive_id, uint32_t partition_lba, uint32_t partition_sectors, const char *volume_label);

    // Read support
    int (*read_file)(fs_mount_t *mount, const char *path, void *buffer, size_t buffer_size, size_t *bytes_read);

    // Optional write support (NULL => read-only)
    int (*write_file)(fs_mount_t *mount, const char *path, const void *buffer, size_t size);

    int (*stat)(fs_mount_t *mount, const char *path, fs_file_info_t *info);
    int (*file_exists)(fs_mount_t *mount, const char *path);
    int (*directory_exists)(fs_mount_t *mount, const char *path);
    int (*list_directory)(fs_mount_t *mount, const char *path);

    // Optional directory mutation
    int (*mkdir)(fs_mount_t *mount, const char *path);
    int (*rmdir)(fs_mount_t *mount, const char *path);

    // Optional file mutation
    int (*unlink)(fs_mount_t *mount, const char *path);

    // Directory iteration
    fs_dir_t* (*opendir)(fs_mount_t *mount, const char *path);
    int (*readdir)(fs_dir_t *dir, fs_dirent_t *entry);
    void (*closedir)(fs_dir_t *dir);
} fs_ext_driver_ops_t;

// Register external filesystem driver (string-based). Built-ins always win; external drivers are tried only after.
int fs_register_driver(const char *name, const fs_ext_driver_ops_t *ops);

// Invoke an external filesystem driver's mkfs callback (if provided).
int fs_ext_mkfs(const char *driver_name, int vdrive_id, uint32_t partition_lba, uint32_t partition_sectors, const char *volume_label);

// Internal helper: update MBR partition type for the partition starting at start_lba.
int fs_mbr_set_type_for_lba(int vdrive_id, uint32_t start_lba, uint8_t new_type);

int fs_mount_drive(int vdrive_id, uint32_t partition_lba, fs_type_t type);

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
 * @param vdrive_id: Output - virtual drive ID (can be NULL)
 * @param partition_lba: Output - partition LBA (can be NULL)
 * @param type: Output - filesystem type (can be NULL)
 * @return: 0 on success, -1 if slot invalid/unmounted
 */
int fs_get_mount_info(int slot, int* vdrive_id, uint32_t* partition_lba, fs_type_t* type);

/*
 * Get a stable human-readable mount label.
 * Examples:
 *  - "vDrive1" (whole-disk/superfloppy/ISO)
 *  - "vDrive1-P1" (partitioned disk, partition #1)
 */
int fs_get_mount_label(int slot, char *out, size_t out_size);

/* Return 0 if not a partitioned mount, otherwise 1..4 for MBR partitions. */
int fs_get_mount_partition_index(int slot);

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
 * Write entire file from buffer.
 * Currently supported for FAT32 only.
 *
 * @param mount: Mount handle (from fs_get_mount)
 * @param path: File path (e.g., "/dir/file.txt")
 * @param buffer: Input buffer
 * @param size: Number of bytes to write
 * @return: 0 on success, negative on error
 */
int fs_write_file(fs_mount_t* mount, const char* path, const void* buffer, size_t size);

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

/* Directory entry structure for iteration */
typedef struct fs_dirent {
    char name[260];           /* Entry name */
    uint32_t size;            /* File size in bytes */
    int is_directory;         /* 1 if directory, 0 if file */
    uint32_t reserved;        /* Reserved for future use */
} fs_dirent_t;

/* Directory handle for iteration */
typedef struct fs_dir {
    fs_mount_t* mount;        /* Mount point */
    char path[256];           /* Directory path */
    size_t position;          /* Current position in directory */
    void* fs_specific;        /* Filesystem-specific data */

    // External FS directory iteration
    const fs_ext_driver_ops_t *ext_ops;
} fs_dir_t;

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

// Create a directory at path (absolute within mount). Returns 0 on success.
int fs_mkdir(fs_mount_t* mount, const char* path);

// Remove an empty directory at path (absolute within mount). Returns 0 on success.
int fs_rmdir(fs_mount_t* mount, const char* path);

// Remove a file at path (absolute within mount). Returns 0 on success.
int fs_unlink(fs_mount_t* mount, const char* path);

/**
 * Open directory for iteration
 * @param mount: Mount handle
 * @param path: Directory path (NULL or "/" for root)
 * @return: Directory handle, or NULL on error
 */
fs_dir_t* fs_opendir(fs_mount_t* mount, const char* path);

/**
 * Read next directory entry
 * @param dir: Directory handle
 * @param entry: Output entry structure
 * @return: 1 on success, 0 at end of directory, -1 on error
 */
int fs_readdir(fs_dir_t* dir, fs_dirent_t* entry);

/**
 * Close directory handle
 * @param dir: Directory handle
 */
void fs_closedir(fs_dir_t* dir);

/* --- UTILITY FUNCTIONS --- */

/**
 * Get filesystem type name
 * @param type: Filesystem type
 * @return: String name (e.g., "FAT32", "ISO9660")
 */
const char* fs_type_name(fs_type_t type);

// Rescan all ready vDrives and mount any new filesystems using FS_TYPE_UNKNOWN.
// Intended to be called after SQRM modules register external filesystem drivers.
void fs_rescan_all(void);

// MBR partition lookup helper.
// part_no: 1..4 (primary partitions)
// Returns 0 on success.
int fs_mbr_get_partition(int vdrive_id, int part_no, uint32_t *out_start_lba, uint32_t *out_sectors, uint8_t *out_type);

#endif /* FS_H */