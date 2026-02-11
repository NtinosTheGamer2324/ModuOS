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
    SQRM_TYPE_GPU     = 5,
    SQRM_TYPE_NET     = 6,
    SQRM_TYPE_HID     = 7,
    SQRM_TYPE_GENERIC = 8,
} sqrm_module_type_t;

// ABI v1 module descriptor (still supported)
typedef struct {
    uint32_t abi_version;
    sqrm_module_type_t type;
    const char *name;
} sqrm_module_desc_t;

// ABI v2 module descriptor (backward-compatible extension)
// NOTE: This struct must start with the ABI v1 fields (abi_version/type/name).
typedef struct {
    // v1 prefix
    uint32_t abi_version;       // must be 2 for this struct
    sqrm_module_type_t type;
    const char *name;

    // v2 additions
    uint16_t class_id;          // optional grouping (e.g. USB core vs controllers)
    uint16_t subclass_id;       // optional sub-class identifier
    uint16_t dep_count;         // number of required dependencies
    uint16_t flags;             // reserved for future use

    // Array of required dependency module names (each must match another module's desc.name)
    const char * const *deps;
} sqrm_module_desc_v2_t;

#define SQRM_ABI_V1 1u
#define SQRM_ABI_V2 2u

#ifndef SQRM_ABI_VERSION
// In-tree modules use the kernel header; default to ABI v1.
#define SQRM_ABI_VERSION SQRM_ABI_V1
#endif

// Every module must export this symbol.
#define SQRM_DESC_SYMBOL "sqrm_module_desc"

// Convenience macros for defining module descriptors.
// (Also available in the third-party SDK header.)
#define SQRM_DEFINE_MODULE(_type, _name_literal) \
    __attribute__((used)) \
    const sqrm_module_desc_t sqrm_module_desc = { \
        .abi_version = SQRM_ABI_V1, \
        .type = (_type), \
        .name = (_name_literal), \
    }

#define SQRM_DEFINE_MODULE_V2(_type, _name_literal, _class_id, _subclass_id, _dep_count, _deps_ptr) \
    __attribute__((used)) \
    const sqrm_module_desc_v2_t sqrm_module_desc = { \
        .abi_version = SQRM_ABI_V2, \
        .type = (_type), \
        .name = (_name_literal), \
        .class_id = (_class_id), \
        .subclass_id = (_subclass_id), \
        .dep_count = (_dep_count), \
        .flags = 0, \
        .deps = (_deps_ptr), \
    }
#include "moduos/kernel/blockdev.h"

// Forward declaration to avoid pulling in full events.h here.
typedef struct Event Event;
#include "moduos/fs/fs.h" // fs_ext_driver_ops_t
#include "moduos/kernel/audio.h"
#include "moduos/kernel/dma.h"
#include "moduos/kernel/io/io.h"
#include "moduos/drivers/PCI/pci.h"

#include "moduos/drivers/graphics/framebuffer.h"
#include "moduos/drivers/graphics/gfx_mode.h"

#include "moduos/kernel/gfx.h" /* gfx_src_sg_t */

typedef struct sqrm_gpu_device {
    framebuffer_t fb;
    // Optional: called after drawing into fb.addr to push updates to hardware.
    // If NULL, fb.addr is assumed to be directly scanned out.
    void (*flush)(const framebuffer_t *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

    /* ------------------------------------------------------------
     * Optional hardware cursor hooks.
     * If provided, the kernel can move/show the cursor without repainting the framebuffer.
     * Pixels are ARGB8888 (0xAARRGGBB).
     * Return 0 on success.
     * ------------------------------------------------------------ */
    int (*cursor_set_argb32)(uint32_t w, uint32_t h, int32_t hot_x, int32_t hot_y, const uint32_t *pixels_argb);
    int (*cursor_move)(int32_t x, int32_t y);
    int (*cursor_show)(int visible);

    /* ------------------------------------------------------------
     * Optional 2D acceleration hooks (thread-context only).
     * These are enabled by default when provided and fb.bpp==32.
     * All colors are native pixels for the current fb format (max speed).
     * Return 0 on success, negative on failure.
     * ------------------------------------------------------------ */
    int (*fill_rect32_native)(const framebuffer_t *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t native_pixel);
    int (*blit_rect32)(const framebuffer_t *fb, uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t w, uint32_t h);
    int (*blit_from_sg32)(const framebuffer_t *fb, const gfx_src_sg_t *src,
                          uint32_t src_x, uint32_t src_y,
                          uint32_t dst_x, uint32_t dst_y,
                          uint32_t w, uint32_t h);

    // Optional: request a mode change. Returns 0 on success.
    int (*set_mode)(uint32_t width, uint32_t height, uint32_t bpp);

    // Optional: enumerate supported modes.
    // Writes up to max_modes entries into out_modes and returns number of modes written.
    // Returns negative on error.
    int (*enumerate_modes)(gfx_mode_t *out_modes, uint32_t max_modes);

    // Optional: called on shutdown/unload (not implemented yet)
    void (*shutdown)(void);
} sqrm_gpu_device_t;

// Network service API (L2 NIC API)
// Exported via sqrm_service_register("net", &api, sizeof(api)).
typedef struct {
    int (*get_link_up)(void);
    int (*get_mtu)(uint32_t *out);
    int (*get_mac)(uint8_t out_mac[6]);
    int (*tx_frame)(const void *frame, size_t len);
    int (*rx_poll)(void *out_frame, size_t out_cap, size_t *out_len);
    int (*rx_consume)(void);
} sqrm_net_api_v1_t;

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

    // Timing (capability-gated; may be NULL)
    uint64_t (*get_system_ticks)(void);
    uint64_t (*ticks_to_ms)(uint64_t ticks);
    uint64_t (*ms_to_ticks)(uint64_t ms);
    void (*sleep_ms)(uint64_t ms);

    // VFS (capability-gated; may be NULL)
    int (*fs_register_driver)(const char *name, const fs_ext_driver_ops_t *ops);

    // DEVFS (capability-gated; may be NULL)
    int (*devfs_register_path)(const char *path, const void *ops, void *ctx);

    // Input injection (capability-gated; may be NULL)
    // Injects an input event into /dev/input/event0 and /dev/input/kbd0 (VT100 translation)
    // and also pushes it to the kernel event queue.
    void (*input_push_event)(const Event *e);

    // Graphics (GPU modules only): register/replace the active framebuffer.
    // Returns 0 on success.
    int (*gfx_register_framebuffer)(const sqrm_gpu_device_t *dev);
    // Update framebuffer descriptor after a mode change.
    int (*gfx_update_framebuffer)(const framebuffer_t *fb);

    // PCI (GPU/NET modules)
    int (*pci_get_device_count)(void);
    pci_device_t* (*pci_get_device)(int index);
    pci_device_t* (*pci_find_device)(uint16_t vendor_id, uint16_t device_id);
    void (*pci_enable_memory_space)(pci_device_t *dev);
    void (*pci_enable_io_space)(pci_device_t *dev);
    void (*pci_enable_bus_mastering)(pci_device_t *dev);

    // PCI config space access (restricted; may be NULL)
    uint32_t (*pci_cfg_read32)(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
    void (*pci_cfg_write32)(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value);

    // MMIO mapping (GPU/NET modules)
    void* (*ioremap)(uint64_t phys_addr, uint64_t size);
    void* (*ioremap_guarded)(uint64_t phys_addr, uint64_t size);

    // Address translation helpers (NET/USB modules that build DMA descriptor rings)
    // Returns physical address for a kernel virtual address, or 0 if unmapped.
    uint64_t (*virt_to_phys)(uint64_t virt);

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

    // SQRM services (module-to-kernel/module-to-module exports)
    // Register a named service API blob. Returns 0 on success.
    int (*sqrm_service_register)(const char *service_name, const void *api_ptr, size_t api_size);
    // Lookup a named service API blob. Returns pointer or NULL if not found.
    // If out_size is non-NULL, it will be filled with the api_size.
    const void* (*sqrm_service_get)(const char *service_name, size_t *out_size);
} sqrm_kernel_api_t;

typedef int (*sqrm_module_init_fn)(const sqrm_kernel_api_t *api);

// Load selected *.sqrm modules from SQRM_MODULE_DIR on the boot filesystem.
// Safe to call multiple times; already-loaded modules will be skipped.
int sqrm_load_early_drivers(void); // GPU, then FS
int sqrm_load_late_drivers(void);  // USB/NET/AUDIO/etc
int sqrm_load_all(void);           // backwards compatible: early + late

// Kernel-side service lookup for syscalls/subsystems.
// Returns NULL if not found.
const void* sqrm_service_get_kernel(const char *service_name, size_t *out_size);

#ifdef __cplusplus
}
#endif
