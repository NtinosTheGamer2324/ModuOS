#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SQRM_MODULE_DIR "/ModuOS/System64/md"

typedef enum {
    SQRM_TYPE_INVALID = 0,
    SQRM_TYPE_FS      = 1,
    SQRM_TYPE_DRIVE   = 2,
    SQRM_TYPE_USB     = 3,
    SQRM_TYPE_AUDIO   = 4,
} sqrm_module_type_t;

typedef struct {
    uint32_t abi_version;
    sqrm_module_type_t type;
    const char *name;
} sqrm_module_desc_t;

// Every module must export this symbol.
#define SQRM_DESC_SYMBOL "sqrm_module_desc"

#include "moduos/kernel/blockdev.h"
#include "moduos/fs/fs.h" // fs_ext_driver_ops_t
#include "moduos/kernel/audio.h"
#include "moduos/kernel/dma.h"
#include "moduos/kernel/io/io.h"
#include "moduos/drivers/PCI/pci.h"

typedef struct sqrm_kernel_api {
    uint32_t abi_version;
    sqrm_module_type_t module_type;
    const char *module_name;

    // logging
    int (*com_write_string)(uint16_t port, const char *s);

    // memory
    void *(*kmalloc)(size_t sz);
    void (*kfree)(void *p);

    // DMA (capability-gated; may be NULL)
    int (*dma_alloc)(dma_buffer_t *out, size_t size, size_t align);
    void (*dma_free)(dma_buffer_t *buf);

    // Low-level port I/O (capability-gated; may be NULL)
    uint8_t  (*inb)(uint16_t port);
    uint16_t (*inw)(uint16_t port);
    uint32_t (*inl)(uint16_t port);
    void (*outb)(uint16_t port, uint8_t val);
    void (*outw)(uint16_t port, uint16_t val);
    void (*outl)(uint16_t port, uint32_t val);

    // IRQ (capability-gated; may be NULL)
    void (*irq_install_handler)(int irq, void (*handler)(void));
    void (*irq_uninstall_handler)(int irq);
    void (*pic_send_eoi)(uint8_t irq);

    // VFS (capability-gated; may be NULL)
    int (*fs_register_driver)(const char *name, const fs_ext_driver_ops_t *ops);

    // DEVFS (capability-gated; may be NULL)
    int (*devfs_register_path)(const char *path, const void *ops, void *ctx);

    // Blockdev (capability-gated; may be NULL)
    int (*block_get_info)(blockdev_handle_t h, blockdev_info_t *out);
    int (*block_read)(blockdev_handle_t h, uint64_t lba, uint32_t count, void *buf, size_t buf_sz);
    int (*block_write)(blockdev_handle_t h, uint64_t lba, uint32_t count, const void *buf, size_t buf_sz);

    // Map a vDrive ID to its registered blockdev handle (if available).
    // Returns 0 on success.
    int (*block_get_handle_for_vdrive)(int vdrive_id, blockdev_handle_t *out_handle);

    // Drive modules will get a register function later (capability-gated)
    int (*block_register)(const void *ops, void *ctx, blockdev_handle_t *out_handle);

    // Audio (capability-gated; may be NULL)
    int (*audio_register_pcm)(const char *dev_name, const audio_pcm_ops_t *ops, void *ctx);
} sqrm_kernel_api_t;

typedef int (*sqrm_module_init_fn)(const sqrm_kernel_api_t *api);

// Load all *.sqrm modules from SQRM_MODULE_DIR on the boot filesystem.
// Safe to call multiple times; already-loaded modules will be skipped.
int sqrm_load_all(void);

#ifdef __cplusplus
}
#endif
