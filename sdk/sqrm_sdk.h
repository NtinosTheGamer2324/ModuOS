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
 *     - `sqrm_module_desc` (sqrm_module_desc_t or sqrm_module_desc_v2_t)
 *     - `sqrm_module_init(const sqrm_kernel_api_t *api)`
 * - Build as ELF64 ET_DYN with entrypoint `sqrm_module_init`.
 *
 * Capability-gated API availability by module type:
 *
 *   Field                     FS  DRIVE  USB  AUDIO  GPU  NET  HID  GENERIC
 *   -----------------------  --  -----  ---  -----  ---  ---  ---  -------
 *   kmalloc/kfree             Y    Y     Y     Y     Y    Y    Y     Y
 *   com_write_string          Y    Y     Y     Y     Y    Y    Y     Y
 *   sleep_ms / ticks          Y    Y     Y     Y     Y    Y    Y     Y
 *   sqrm_service_*            Y    Y     Y     Y     Y    Y    Y     Y
 *   devfs_register_path       Y    Y     Y     Y     Y    Y    Y     Y
 *   multiboot2_header         Y    Y     Y     Y     Y    Y    Y     Y
 *   dma_alloc/free            -    -     -     Y     -    -    -     -
 *   inb/inw/inl/out*          -    -     -     Y     -    -    -     -
 *   irq_install_handler       -    -     -     Y     -    -    -     -
 *   ioremap / _guarded        -    -     -     Y     Y    Y    Y     -
 *   virt_to_phys              -    -     Y     Y     Y    Y    Y     -
 *   pci_*                     -    -     Y     Y     Y    Y    Y     -
 *   pci_cfg_read32/write32    -    -     Y     Y     Y    Y    Y     -
 *   gfx_register_framebuffer  -    -     -     -     Y    -    -     -
 *   audio_register_pcm        -    -     -     Y     -    -    -     -
 *   fs_register_driver        Y    -     -     -     -    -    -     -
 *   block_get_info/read/write  Y   -     -     -     -    -    -     Y
 *   input_push_event          -    -     -     -     -    -    Y     -
 *   gfx_get_framebuffer       -    -     -     -     -    -    -     Y
 *   gfx_get_caps              -    -     -     -     -    -    -     Y
 *   gfx_fill_rect             -    -     -     -     -    -    -     Y
 *   gfx_blit_rect             -    -     -     -     -    -    -     Y
 *   gfx_blit_buffer           -    -     -     -     -    -    -     Y
 *   gfx_cursor_move           -    -     -     -     -    -    -     Y
 *   gfx_cursor_show           -    -     -     -     -    -    -     Y
 *   gfx_flush                 -    -     -     -     -    -    -     Y
 *   gfx_set_mode              -    -     -     -     -    -    -     Y
 *   devfs_mmap_region         -    -     -     -     -    -    -     Y
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- SQRM core ---- */

#define SQRM_ABI_V1 1u
#define SQRM_ABI_V2 2u

#ifndef SQRM_ABI_VERSION
// Default to v1 for maximum compatibility; modules can opt into v2.
#define SQRM_ABI_VERSION SQRM_ABI_V1
#endif

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
#if defined(__SIZE_TYPE__)
typedef __PTRDIFF_TYPE__ ssize_t;
#else
typedef long ssize_t;
#endif
#endif

#define SQRM_DESC_SYMBOL "sqrm_module_desc"

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

typedef struct {
    uint32_t abi_version;         /* must match SQRM_ABI_VERSION */
    sqrm_module_type_t type;
    const char *name;             /* short name (e.g., "ext2") */
} sqrm_module_desc_t;

// ABI v2 descriptor (backward-compatible extension)
// NOTE: This struct must start with the ABI v1 fields (abi_version/type/name).
typedef struct {
    // v1 prefix
    uint32_t abi_version; // must be SQRM_ABI_V2
    sqrm_module_type_t type;
    const char *name;

    // v2 additions
    uint16_t class_id;
    uint16_t subclass_id;
    uint16_t dep_count;
    uint16_t flags;
    const char * const *deps; // array of dependency names
} sqrm_module_desc_v2_t;

/* Helper macro to define the required descriptor symbol (ABI v1) */
#define SQRM_DEFINE_MODULE(_type, _name_literal) \
    const sqrm_module_desc_t sqrm_module_desc = { \
        .abi_version = SQRM_ABI_V1, \
        .type = (_type), \
        .name = (_name_literal), \
    }

/* Helper macro to define the required descriptor symbol (ABI v2) */
#define SQRM_DEFINE_MODULE_V2(_type, _name_literal, _class_id, _subclass_id, _dep_count, _deps_ptr) \
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

/* External FS driver ops (v1.1: read-write) */
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

/* ---- Graphics structures (GPU modules) ---- */

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
} gfx_mode_t;

typedef enum {
    FB_MODE_TEXT = 0,
    FB_MODE_GRAPHICS = 1,
} framebuffer_mode_t;

typedef enum {
    FB_FMT_UNKNOWN = 0,
    FB_FMT_RGB565 = 1,
    FB_FMT_XRGB8888 = 2,
} framebuffer_format_t;

typedef struct {
    void *addr;             // linear framebuffer virtual address (kernel)
    uint64_t phys_addr;     // physical base address of the framebuffer
    uint64_t size_bytes;    // mapped size in bytes

    uint32_t width;
    uint32_t height;
    uint32_t pitch;         // bytes per scanline
    uint8_t bpp;            // bits per pixel
    framebuffer_format_t fmt;

    // For Multiboot2 RGB framebuffers (fb_type=1): channel field positions.
    uint8_t red_pos, red_mask_size;
    uint8_t green_pos, green_mask_size;
    uint8_t blue_pos, blue_mask_size;
} framebuffer_t;

/* Source scatter-gather descriptor for blit_from_sg32 */
typedef struct {
    const void *addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
} gfx_src_sg_t;

/* GPU capability flags — reported in sqrm_gpu_device_t.caps.
 * SQRM_GPU_CAP_BLIT_BUF is MANDATORY: gfx_register_framebuffer will reject
 * any GPU device that does not provide a blit_buffer implementation. */
#define SQRM_GPU_CAP_2D_ACCEL      (1u << 0)  /* Has fill_rect32/blit_rect32      */
#define SQRM_GPU_CAP_3D_TRIANGLES  (1u << 1)  /* Has draw_triangle                */
#define SQRM_GPU_CAP_3D_TEXTURES   (1u << 2)  /* Has draw_textured_triangle       */
#define SQRM_GPU_CAP_HW_CURSOR     (1u << 3)  /* Has cursor hooks                 */
#define SQRM_GPU_CAP_VSYNC         (1u << 4)  /* Supports vsync                   */
#define SQRM_GPU_CAP_BLIT_BUF      (1u << 5)  /* Has blit_buffer (MANDATORY)      */

typedef struct sqrm_gpu_device {
    framebuffer_t fb;

    /* Optional: push framebuffer updates to hardware.
     * If NULL, fb.addr is assumed to be directly scanned out. */
    void (*flush)(const framebuffer_t *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

    /* Optional hardware cursor hooks (ARGB8888 pixels). */
    int (*cursor_set_argb32)(uint32_t w, uint32_t h, int32_t hot_x, int32_t hot_y, const uint32_t *pixels_argb);
    int (*cursor_move)(int32_t x, int32_t y);
    int (*cursor_show)(int visible);

    /* Optional 2D acceleration hooks (thread-context only, fb.bpp==32). */
    int (*fill_rect32_native)(const framebuffer_t *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t native_pixel);
    int (*blit_rect32)(const framebuffer_t *fb, uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t w, uint32_t h);
    int (*blit_from_sg32)(const framebuffer_t *fb, const gfx_src_sg_t *src,
                          uint32_t src_x, uint32_t src_y,
                          uint32_t dst_x, uint32_t dst_y,
                          uint32_t w, uint32_t h);

    /* Optional 3D acceleration hooks. */
    int (*draw_triangle)(const framebuffer_t *fb,
                         int32_t x0, int32_t y0, uint32_t color0,
                         int32_t x1, int32_t y1, uint32_t color1,
                         int32_t x2, int32_t y2, uint32_t color2);
    int (*draw_textured_triangle)(const framebuffer_t *fb,
                                  int32_t x0, int32_t y0, float u0, float v0,
                                  int32_t x1, int32_t y1, float u1, float v1,
                                  int32_t x2, int32_t y2, float u2, float v2,
                                  uint32_t texture_id);

    /* MANDATORY: copy an external pixel buffer into the framebuffer at
     * (dst_x, dst_y) for a region of (w x h) pixels, then push the dirty
     * rect to hardware (equivalent to a flush of that region).
     *
     * src_pixels points at the first pixel to copy — the caller (MVC3/
     * compositor) has already applied any src_x/src_y offset.
     * src_pitch is the byte stride of the source buffer.
     *
     * Must not be NULL — gfx_register_framebuffer rejects devices that
     * omit this.  A simple software row-copy fallback is acceptable. */
    int (*blit_buffer)(const framebuffer_t *fb,
                       uint32_t dst_x, uint32_t dst_y,
                       const void *src_pixels, uint32_t src_pitch,
                       uint32_t w, uint32_t h);

    /* Optional mode control. */
    int (*set_mode)(uint32_t width, uint32_t height, uint32_t bpp);
    int (*enumerate_modes)(gfx_mode_t *out_modes, uint32_t max_modes);

    /* Capability flags — see SQRM_GPU_CAP_* above.
     * SQRM_GPU_CAP_BLIT_BUF is set automatically by the kernel when
     * blit_buffer is non-NULL; GPU LKMs should set it explicitly too. */
    uint32_t caps;

    /* Optional: called on shutdown/unload. */
    void (*shutdown)(void);
} sqrm_gpu_device_t;

/* ---- Optional shared service ABIs (exported via sqrm_service_register/get) ---- */

// Network service API (L2 NIC API). Return negative errno on failure.
typedef struct {
    int (*get_link_up)(void);
    int (*get_mtu)(uint32_t *out);
    int (*get_mac)(uint8_t out_mac[6]);
    int (*tx_frame)(const void *frame, size_t len);
    int (*rx_poll)(void *out_frame, size_t out_cap, size_t *out_len);
    int (*rx_consume)(void);
} sqrm_net_api_v1_t;

// USB service API (minimal core).
typedef struct {
    int (*get_controller_count)(void);
    int (*get_device_count)(void);
    int (*enumerate)(void);
} sqrm_usb_api_v1_t;

// HID service API (minimal).
typedef struct {
    int (*get_keyboard_present)(void);
    int (*get_mouse_present)(void);
} sqrm_hid_api_v1_t;

typedef enum {
    SQRM_USB_SPEED_LOW  = 1,
    SQRM_USB_SPEED_FULL = 2,
    SQRM_USB_SPEED_HIGH = 3,
} sqrm_usb_speed_t;

typedef enum {
    SQRM_USB_XFER_CONTROL   = 1,
    SQRM_USB_XFER_BULK      = 2,
    SQRM_USB_XFER_INTERRUPT = 3,
} sqrm_usb_xfer_type_t;

typedef struct {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) sqrm_usb_setup_packet_t;

typedef struct {
    uint8_t dev_addr;
    uint8_t endpoint;
    uint8_t speed;
    uint8_t xfer_type;
    sqrm_usb_setup_packet_t setup;
    void   *data;
    uint32_t length;
    uint8_t direction_in;
    int32_t status;
    uint32_t actual_length;
} sqrm_usb_transfer_v1_t;

typedef uint32_t sqrm_usb_xfer_handle_t;
#define SQRM_USB_XFER_INVALID_HANDLE 0u

typedef struct {
    uint8_t bus, device, function;
    uint8_t irq_line;
    uint16_t io_base;
} sqrm_uhci_controller_info_v1_t;

typedef struct {
    int (*get_controller_count)(void);
    int (*get_controller_info)(int index, sqrm_uhci_controller_info_v1_t *out);
    sqrm_usb_xfer_handle_t (*submit)(int controller_index, sqrm_usb_transfer_v1_t *xfer);
    int (*wait)(sqrm_usb_xfer_handle_t handle, uint32_t timeout_ms);
    int (*cancel)(sqrm_usb_xfer_handle_t handle);
} sqrm_usbctl_uhci_api_v1_t;

/* ---- Kernel API table passed to modules ---- */

typedef struct {
    void    *virt;
    uint64_t phys;
    size_t   size;
} sqrm_dma_buffer_t;

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

    /* Logging — always available */
    int (*com_write_string)(uint16_t port, const char *s);

    /* Memory — always available */
    void *(*kmalloc)(size_t sz);
    void (*kfree)(void *p);

    /* DMA — AUDIO modules only; NULL for all other types. */
    int (*dma_alloc)(sqrm_dma_buffer_t *out, size_t size, size_t align);
    void (*dma_free)(sqrm_dma_buffer_t *buf);

    /* Port I/O — AUDIO modules only; NULL for all other types. */
    uint8_t  (*inb)(uint16_t port);
    uint16_t (*inw)(uint16_t port);
    uint32_t (*inl)(uint16_t port);
    void (*outb)(uint16_t port, uint8_t val);
    void (*outw)(uint16_t port, uint16_t val);
    void (*outl)(uint16_t port, uint32_t val);

    /* IRQ — AUDIO modules only; NULL for all other types. */
    void (*irq_install_handler)(int irq, void (*handler)(void));
    void (*irq_uninstall_handler)(int irq);
    void (*pic_send_eoi)(uint8_t irq);

    /* Timing — always available */
    uint64_t (*get_system_ticks)(void);
    uint64_t (*ticks_to_ms)(uint64_t ticks);
    uint64_t (*ms_to_ticks)(uint64_t ms);
    void (*sleep_ms)(uint64_t ms);

    /* VFS — FS modules only */
    int (*fs_register_driver)(const char *name, const fs_ext_driver_ops_t *ops);

    /* DEVFS — always available */
    int (*devfs_register_path)(const char *path, const void *ops, void *ctx);

    /* Multiboot2 header — raw pointer to the MB2 info struct from the bootloader. */
    const void *multiboot2_header;

    /* Input injection — HID modules only */
    void (*input_push_event)(const void *event);

    /* Graphics registration — GPU modules only.
     * Takes a fully populated sqrm_gpu_device_t; rejects devices with
     * NULL blit_buffer (returns non-zero on failure). */
    int (*gfx_register_framebuffer)(const sqrm_gpu_device_t *gpu_dev);
    int (*gfx_update_framebuffer)(const void *fb);

    /* PCI — AUDIO, GPU, NET, USB, HID modules; NULL for FS, DRIVE, GENERIC. */
    int (*pci_get_device_count)(void);
    void* (*pci_get_device)(int index);
    void* (*pci_find_device)(uint16_t vendor_id, uint16_t device_id);
    void (*pci_enable_memory_space)(void *dev);
    void (*pci_enable_io_space)(void *dev);
    void (*pci_enable_bus_mastering)(void *dev);

    /* PCI config space — AUDIO, NET, USB, HID only; NULL for GPU */
    uint32_t (*pci_cfg_read32)(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
    void (*pci_cfg_write32)(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value);

    /* MMIO mapping — AUDIO, GPU, NET, USB, HID modules */
    void* (*ioremap)(uint64_t phys_addr, uint64_t size);
    void* (*ioremap_guarded)(uint64_t phys_addr, uint64_t size);

    /* Address translation — AUDIO, NET, USB, HID modules. */
    uint64_t (*virt_to_phys)(uint64_t virt);

    /* Blockdev — FS and GENERIC modules only */
    int (*block_get_info)(blockdev_handle_t h, blockdev_info_t *out);
    int (*block_read)(blockdev_handle_t h, uint64_t lba, uint32_t count, void *buf, size_t buf_sz);
    int (*block_write)(blockdev_handle_t h, uint64_t lba, uint32_t count, const void *buf, size_t buf_sz);
    int (*block_get_handle_for_vdrive)(int vdrive_id, blockdev_handle_t *out_handle);
    int (*block_register)(const void *ops, void *ctx, blockdev_handle_t *out_handle);

    /* Audio — AUDIO modules only */
    int (*audio_register_pcm)(const char *dev_name, const audio_pcm_ops_t *ops, void *ctx);

    /* SQRM services — always available */
    int (*sqrm_service_register)(const char *service_name, const void *api_ptr, size_t api_size);
    const void* (*sqrm_service_get)(const char *service_name, size_t *out_size);

    /* System info — always available */
    const char *(*get_gpu_driver_name)(void);
    const char *(*get_smbios_field)(int field); /* 0=mfr 1=product 2=bios_vendor 3=bios_version */
    uint64_t (*phys_total_frames)(void);
    uint64_t (*phys_count_free_frames)(void);

    /* GPU access — GENERIC modules only; NULL if no GPU registered. */
    const framebuffer_t *(*gfx_get_framebuffer)(void);
    uint32_t (*gfx_get_caps)(void);
    int (*gfx_fill_rect)(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t pixel);
    int (*gfx_blit_rect)(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t w, uint32_t h);
    int (*gfx_cursor_move)(int32_t x, int32_t y);
    int (*gfx_cursor_show)(int visible);
    int (*gfx_flush)(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    int (*gfx_set_mode)(uint32_t width, uint32_t height, uint32_t bpp);

    /* Copy an external pixel buffer into the active framebuffer — GENERIC only.
     * Kernel wrapper for the active GPU's blit_buffer hook. */
    int (*gfx_blit_buffer)(uint32_t dx, uint32_t dy, uint32_t w, uint32_t h,
                           const void *src_buf, uint32_t src_stride);

    void *(*devfs_mmap_region)(uint64_t phys_or_virt, size_t size,
                               int prot, int is_phys);
} sqrm_kernel_api_t;

typedef int (*sqrm_module_init_fn)(const sqrm_kernel_api_t *api);

#ifdef __cplusplus
}
#endif