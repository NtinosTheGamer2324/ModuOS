#pragma once
/*
 * sqrm_fs.h — SQRM external filesystem driver ABI (v1.1, read-write).
 *
 * Available to: FS modules only.
 * The kernel sets fs_register_driver to NULL for all other module types.
 *
 * An FS module must call api->fs_register_driver() from sqrm_module_init()
 * and provide a fully populated fs_ext_driver_ops_t.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Filesystem type tag                                                */
/* ------------------------------------------------------------------ */

typedef enum {
    FS_TYPE_UNKNOWN  = 0,
    FS_TYPE_FAT32    = 1,
    FS_TYPE_ISO9660  = 2,
    FS_TYPE_EXTERNAL = 3,
} fs_type_t;

/* ------------------------------------------------------------------ */
/*  File info (stat-like result)                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    char     name[260];
    uint32_t size;
    int      is_directory;
    uint32_t cluster;
} fs_file_info_t;

/* ------------------------------------------------------------------ */
/*  Mount object                                                       */
/* ------------------------------------------------------------------ */

struct fs_ext_driver_ops;   /* forward declaration */

typedef struct {
    fs_type_t type;
    int       handle;
    int       valid;

    const struct fs_ext_driver_ops *ext_ops;
    void  *ext_ctx;
    char   ext_name[16];
} fs_mount_t;

/* ------------------------------------------------------------------ */
/*  Directory iteration                                                */
/* ------------------------------------------------------------------ */

typedef struct fs_dir fs_dir_t;     /* opaque — defined by the FS driver */

typedef struct {
    char     name[260];
    uint32_t size;
    int      is_directory;
    uint32_t reserved;
} fs_dirent_t;

/* ------------------------------------------------------------------ */
/*  Driver operations table                                            */
/* ------------------------------------------------------------------ */

typedef struct fs_ext_driver_ops {
    /*
     * probe() — return non-zero if the driver recognises the filesystem
     * on the given partition.
     */
    int  (*probe)(int vdrive_id, uint32_t partition_lba);

    /* mount / unmount */
    int  (*mount)  (int vdrive_id, uint32_t partition_lba, fs_mount_t *mount);
    void (*unmount)(fs_mount_t *mount);

    /*
     * mkfs() — format the partition.
     * May be NULL if the driver is read-only.
     */
    int  (*mkfs)(int vdrive_id, uint32_t partition_lba,
                 uint32_t partition_sectors, const char *volume_label);

    /* File I/O */
    int  (*read_file) (fs_mount_t *mount, const char *path,
                       void *buffer, size_t buffer_size, size_t *bytes_read);
    int  (*write_file)(fs_mount_t *mount, const char *path,
                       const void *buffer, size_t size);

    /* Metadata */
    int  (*stat)            (fs_mount_t *mount, const char *path, fs_file_info_t *info);
    int  (*file_exists)     (fs_mount_t *mount, const char *path);
    int  (*directory_exists)(fs_mount_t *mount, const char *path);
    int  (*list_directory)  (fs_mount_t *mount, const char *path);

    /* Directory streaming */
    fs_dir_t *(*opendir) (fs_mount_t *mount, const char *path);
    int       (*readdir) (fs_dir_t *dir, fs_dirent_t *entry);
    void      (*closedir)(fs_dir_t *dir);
} fs_ext_driver_ops_t;

#ifdef __cplusplus
}
#endif