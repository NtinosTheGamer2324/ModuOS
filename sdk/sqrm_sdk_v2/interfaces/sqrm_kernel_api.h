#pragma once
/*
 * sqrm_kernel_api.h — SQRM kernel API table.
 *
 * sqrm_kernel_api_t is passed to every module's sqrm_module_init() entry point.
 * Field order MUST match include/moduos/kernel/sqrm.h exactly.
 *
 * "Capability-gated" fields are set to NULL by the kernel when the loaded
 * module type does not have access to that subsystem.  ALWAYS NULL-check a
 * function pointer before calling it.  See the capability matrix at the top
 * of sqrm_sdk.h (or the individual interface headers) for the full table.
 *
 *   Field group                 Modules that receive a non-NULL pointer
 *   --------------------------  ----------------------------------------
 *   kmalloc / kfree             ALL
 *   com_write_string            ALL
 *   sleep_ms / ticks            ALL
 *   sqrm_service_*              ALL
 *   devfs_register_path         ALL
 *   multiboot2_header           ALL
 *   dma_alloc / dma_free        AUDIO
 *   inb … outl / irq_*         AUDIO
 *   ioremap / _guarded          AUDIO, GPU, NET, USB, HID
 *   virt_to_phys                AUDIO, NET, USB, HID
 *   pci_*                       AUDIO, GPU, NET, USB, HID
 *   pci_cfg_read32/write32      AUDIO, NET, USB, HID  (NOT GPU)
 *   gfx_register_framebuffer   GPU
 *   audio_register_pcm          AUDIO
 *   fs_register_driver          FS
 *   block_get_info/read/write   FS, GENERIC
 *   input_push_event            HID
 */

#include <stdint.h>
#include <stddef.h>

#include "sqrm_core.h"
#include "sqrm_blockdev.h"
#include "sqrm_fs.h"
#include "sqrm_audio.h"
#include "sqrm_gpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  DMA buffer (mirrors kernel dma_buffer_t exactly)                  */
/* ------------------------------------------------------------------ */

typedef struct {
    void    *virt;  /* kernel virtual address  */
    uint64_t phys;  /* physical address        */
    size_t   size;  /* allocation size (bytes) */
} sqrm_dma_buffer_t;

/* ------------------------------------------------------------------ */
/*  Kernel API table                                                   */
/* ------------------------------------------------------------------ */

typedef struct sqrm_kernel_api {
    uint32_t abi_version;
    sqrm_module_type_t module_type;
    const char *module_name;

    /* ---- Logging — always available -------------------------------- */
    int (*com_write_string)(uint16_t port, const char *s);

    /* ---- Memory — always available --------------------------------- */
    void *(*kmalloc)(size_t sz);
    void  (*kfree)  (void *p);

    /* ---- DMA — AUDIO only; NULL for all other types ----------------
     * HDA / AC97 use this for CORB/RIRB/BDL/PCM ring buffers.
     * Fall back to kmalloc + virt_to_phys when NULL.               */
    int  (*dma_alloc)(sqrm_dma_buffer_t *out, size_t size, size_t align);
    void (*dma_free) (sqrm_dma_buffer_t *buf);

    /* ---- Port I/O — AUDIO only; NULL for all other types -----------
     * AUDIO modules use raw CF8/CFC PCI scanning (same as AC97).   */
    uint8_t  (*inb)(uint16_t port);
    uint16_t (*inw)(uint16_t port);
    uint32_t (*inl)(uint16_t port);
    void (*outb)(uint16_t port, uint8_t  val);
    void (*outw)(uint16_t port, uint16_t val);
    void (*outl)(uint16_t port, uint32_t val);

    /* ---- IRQ — AUDIO only; NULL for all other types ----------------
     * Install a handler for the HDA/AC97 PCI IRQ line.             */
    void (*irq_install_handler)  (int irq, void (*handler)(void));
    void (*irq_uninstall_handler)(int irq);
    void (*pic_send_eoi)         (uint8_t irq);

    /* ---- Timing — always available --------------------------------- */
    uint64_t (*get_system_ticks)(void);
    uint64_t (*ticks_to_ms)    (uint64_t ticks);
    uint64_t (*ms_to_ticks)    (uint64_t ms);
    void     (*sleep_ms)       (uint64_t ms);

    /* ---- VFS — FS modules only ------------------------------------- */
    int (*fs_register_driver)(const char *name, const fs_ext_driver_ops_t *ops);

    /* ---- DevFS — always available ---------------------------------- */
    int (*devfs_register_path)(const char *path, const void *ops, void *ctx);

    /* ---- Multiboot2 — always available -----------------------------
     * Raw pointer to the MB2 info struct provided by the bootloader.
     * Parse MB2 tags directly from this pointer; always valid post-boot. */
    const void *multiboot2_header;

    /* ---- Input injection — HID modules only ------------------------ */
    void (*input_push_event)(const void *event);

    /* ---- Graphics — GPU modules only ------------------------------- */
    int (*gfx_register_framebuffer)(const void *gpu_dev);
    int (*gfx_update_framebuffer)  (const void *fb);

    /* ---- PCI — AUDIO, GPU, NET, USB, HID; NULL for FS/DRIVE/GENERIC */
    int   (*pci_get_device_count)(void);
    void *(*pci_get_device)      (int index);
    void *(*pci_find_device)     (uint16_t vendor_id, uint16_t device_id);
    void  (*pci_enable_memory_space) (void *dev);
    void  (*pci_enable_io_space)     (void *dev);
    void  (*pci_enable_bus_mastering)(void *dev);

    /* ---- PCI config space — AUDIO, NET, USB, HID; NULL for GPU ----- */
    uint32_t (*pci_cfg_read32) (uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
    void     (*pci_cfg_write32)(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value);

    /* ---- MMIO mapping — AUDIO, GPU, NET, USB, HID modules ---------- */
    void *(*ioremap)        (uint64_t phys_addr, uint64_t size);
    void *(*ioremap_guarded)(uint64_t phys_addr, uint64_t size);

    /* ---- Address translation — AUDIO, NET, USB, HID ----------------
     * AUDIO fallback: if dma_alloc is NULL use kmalloc + virt_to_phys.
     * Returns the physical address for a kernel virtual address, or 0. */
    uint64_t (*virt_to_phys)(uint64_t virt);

    /* ---- Block device — FS and GENERIC modules only ---------------- */
    int (*block_get_info)          (blockdev_handle_t h, blockdev_info_t *out);
    int (*block_read)              (blockdev_handle_t h, uint64_t lba, uint32_t count, void *buf,       size_t buf_sz);
    int (*block_write)             (blockdev_handle_t h, uint64_t lba, uint32_t count, const void *buf, size_t buf_sz);
    int (*block_get_handle_for_vdrive)(int vdrive_id, blockdev_handle_t *out_handle);
    int (*block_register)          (const void *ops, void *ctx, blockdev_handle_t *out_handle);

    /* ---- Audio — AUDIO modules only -------------------------------- */
    int (*audio_register_pcm)(const char *dev_name, const audio_pcm_ops_t *ops, void *ctx);

    /* ---- SQRM services — always available --------------------------
     * sqrm_service_register: publish a named API pointer.
     * sqrm_service_get:      look up a named API pointer.             */
    int          (*sqrm_service_register)(const char *service_name, const void *api_ptr, size_t api_size);
    const void * (*sqrm_service_get)     (const char *service_name, size_t *out_size);

    /* ---- System info — always available ---------------------------- */
    const char *(*get_gpu_driver_name)(void);
    const char *(*get_smbios_field)(int field);
    /*   field values: 0 = manufacturer  1 = product name
     *                 2 = BIOS vendor   3 = BIOS version           */
    uint64_t (*phys_total_frames)     (void);
    uint64_t (*phys_count_free_frames)(void);
} sqrm_kernel_api_t;

/* ------------------------------------------------------------------ */
/*  Module entry point type                                            */
/* ------------------------------------------------------------------ */

/*
 * Every module must export this function as its entry point.
 * The kernel calls it immediately after loading and resolving the module.
 * Return 0 on success, negative errno on failure (module will be unloaded).
 */
typedef int (*sqrm_module_init_fn)(const sqrm_kernel_api_t *api);

#ifdef __cplusplus
}
#endif