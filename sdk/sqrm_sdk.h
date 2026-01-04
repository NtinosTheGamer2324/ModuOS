#pragma once
/*
 * ModuOS SQRM Third-Party Module SDK (single-header)
 *
 * Goal: allow building .sqrm kernel modules outside the ModuOS source tree.
 * This header intentionally contains only the stable ABI surface used between
 * the kernel module loader and third-party modules.
 *
 * Notes:
 * - A module must export:
 *     - `sqrm_module_desc` (sqrm_module_desc_t)
 *     - `sqrm_module_init(const sqrm_kernel_api_t *api)`
 * - Build as ELF64 ET_DYN with entrypoint `sqrm_module_init`.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- SQRM core ---- */

#ifndef SQRM_ABI_VERSION
#define SQRM_ABI_VERSION 1u
#endif

#define SQRM_DESC_SYMBOL "sqrm_module_desc"

typedef enum {
    SQRM_TYPE_INVALID = 0,
    SQRM_TYPE_FS      = 1,
    SQRM_TYPE_DRIVE   = 2,
    SQRM_TYPE_USB     = 3,
    SQRM_TYPE_AUDIO   = 4,
} sqrm_module_type_t;

typedef struct {
    uint32_t abi_version;         /* must match SQRM_ABI_VERSION */
    sqrm_module_type_t type;
    const char *name;             /* short name (e.g., "ext2") */
} sqrm_module_desc_t;

/* Helper macro to define the required descriptor symbol */
#define SQRM_DEFINE_MODULE(_type, _name_literal) \
    const sqrm_module_desc_t sqrm_module_desc = { \
        .abi_version = SQRM_ABI_VERSION, \
        .type = (_type), \
        .name = (_name_literal), \
    }

/* ---- Minimal blockdev ABI (optional) ---- */

typedef uint32_t blockdev_handle_t;
#define BLOCKDEV_INVALID_HANDLE 0u

typedef enum {
    BLOCKDEV_F_READONLY  = 1u << 0,
    BLOCKDEV_F_REMOVABLE = 1u << 1,
} blockdev_flags_t;

typedef struct {
    uint32_t sector_size;
    uint64_t sector_count;
    uint32_t flags;
    char model[64];
} blockdev_info_t;

typedef struct {
    int (*get_info)(void *ctx, blockdev_info_t *out);
    int (*read)(void *ctx, uint64_t lba, uint32_t count, void *buf, size_t buf_sz);
    int (*write)(void *ctx, uint64_t lba, uint32_t count, const void *buf, size_t buf_sz);
} blockdev_ops_t;

/* ---- Minimal external FS ABI (optional) ---- */

typedef enum {
    FS_TYPE_UNKNOWN   = 0,
    FS_TYPE_FAT32     = 1,
    FS_TYPE_ISO9660   = 2,
    FS_TYPE_EXTERNAL  = 3
} fs_type_t;

typedef struct {
    char name[260];
    uint32_t size;
    int is_directory;
    uint32_t cluster;
} fs_file_info_t;

struct fs_ext_driver_ops;

typedef struct {
    fs_type_t type;
    int handle;
    int valid;

    const struct fs_ext_driver_ops *ext_ops;
    void *ext_ctx;
    char ext_name[16];
} fs_mount_t;

typedef struct fs_dir fs_dir_t;

typedef struct fs_dirent {
    char name[260];
    uint32_t size;
    int is_directory;
    uint32_t reserved;
} fs_dirent_t;

/* External FS driver ops (v1: read-only) */
typedef struct fs_ext_driver_ops {
    int (*probe)(int vdrive_id, uint32_t partition_lba);
    int (*mount)(int vdrive_id, uint32_t partition_lba, fs_mount_t *mount);
    void (*unmount)(fs_mount_t *mount);

    int (*mkfs)(int vdrive_id, uint32_t partition_lba, uint32_t partition_sectors, const char *volume_label);

    int (*read_file)(fs_mount_t *mount, const char *path, void *buffer, size_t buffer_size, size_t *bytes_read);
    int (*write_file)(fs_mount_t *mount, const char *path, const void *buffer, size_t size);
    int (*stat)(fs_mount_t *mount, const char *path, fs_file_info_t *info);
    int (*file_exists)(fs_mount_t *mount, const char *path);
    int (*directory_exists)(fs_mount_t *mount, const char *path);
    int (*list_directory)(fs_mount_t *mount, const char *path);

    fs_dir_t* (*opendir)(fs_mount_t *mount, const char *path);
    int (*readdir)(fs_dir_t *dir, fs_dirent_t *entry);
    void (*closedir)(fs_dir_t *dir);
} fs_ext_driver_ops_t;

/* ---- Kernel API table passed to modules ---- */

typedef enum {
    AUDIO_FMT_S16_LE = 1,
    AUDIO_FMT_S32_LE = 2,
    AUDIO_FMT_F32_LE = 3,
} audio_format_t;

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    audio_format_t format;
} audio_pcm_config_t;

typedef struct {
    char name[32];
    uint32_t flags;
    audio_pcm_config_t preferred;
} audio_device_info_t;

typedef struct {
    int (*open)(void *ctx);
    int (*set_config)(void *ctx, const audio_pcm_config_t *cfg);
    long (*write)(void *ctx, const void *buf, size_t bytes);
    int (*drain)(void *ctx);
    int (*close)(void *ctx);
    int (*get_info)(void *ctx, audio_device_info_t *out);
} audio_pcm_ops_t;

typedef struct sqrm_kernel_api {
    uint32_t abi_version;
    sqrm_module_type_t module_type;
    const char *module_name;

    /* logging */
    int (*com_write_string)(uint16_t port, const char *s);

    /* memory */
    void *(*kmalloc)(size_t sz);
    void (*kfree)(void *p);

    /* DMA (capability-gated; may be NULL) */
    int (*dma_alloc)(void *out_dma_buffer, size_t size, size_t align);
    void (*dma_free)(void *dma_buffer);

    /* Low-level port I/O (capability-gated; may be NULL) */
    uint8_t  (*inb)(uint16_t port);
    uint16_t (*inw)(uint16_t port);
    uint32_t (*inl)(uint16_t port);
    void (*outb)(uint16_t port, uint8_t val);
    void (*outw)(uint16_t port, uint16_t val);
    void (*outl)(uint16_t port, uint32_t val);

    /* IRQ (capability-gated; may be NULL) */
    void (*irq_install_handler)(int irq, void (*handler)(void));
    void (*irq_uninstall_handler)(int irq);
    void (*pic_send_eoi)(uint8_t irq);

    /* VFS (capability-gated; may be NULL) */
    int (*fs_register_driver)(const char *name, const fs_ext_driver_ops_t *ops);

    /* DEVFS (capability-gated; may be NULL) */
    int (*devfs_register_path)(const char *path, const void *ops, void *ctx);

    /* Blockdev (capability-gated; may be NULL) */
    int (*block_get_info)(blockdev_handle_t h, blockdev_info_t *out);
    int (*block_read)(blockdev_handle_t h, uint64_t lba, uint32_t count, void *buf, size_t buf_sz);
    int (*block_write)(blockdev_handle_t h, uint64_t lba, uint32_t count, const void *buf, size_t buf_sz);

    int (*block_get_handle_for_vdrive)(int vdrive_id, blockdev_handle_t *out_handle);
    int (*block_register)(const void *ops, void *ctx, blockdev_handle_t *out_handle);

    /* Audio (capability-gated; may be NULL) */
    int (*audio_register_pcm)(const char *dev_name, const audio_pcm_ops_t *ops, void *ctx);
} sqrm_kernel_api_t;

typedef int (*sqrm_module_init_fn)(const sqrm_kernel_api_t *api);

#ifdef __cplusplus
}
#endif
