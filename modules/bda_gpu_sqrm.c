/*
 * bda_gpu.c - Bochs Display Adapter (PCI BGA) SQRM GPU Driver
 *
 * Targets the modern bochs-display PCI device (1234:1111, class 0380).
 * Distinguishes it from the legacy VGA-compat variant (class 0300) by
 * probing BAR0 size via the standard PCI BAR-sizing write-back sequence.
 * No hardcoded BDFs or physical addresses.
 */

#include "sqrm_sdk.h"

static const sqrm_kernel_api_t *g_api = NULL;
static sqrm_gpu_device_t bda_gpu = {0};

#define BGA_INDEX      0x01CE
#define BGA_DATA       0x01CF

#define BGA_REG_XRES   0x01
#define BGA_REG_YRES   0x02
#define BGA_REG_BPP    0x03
#define BGA_REG_ENABLE 0x04

#define BGA_ENABLED    (1u << 0)
#define BGA_LFB        (1u << 1)

/* The modern bochs-display exports a 32 MB framebuffer BAR.
 * The legacy stdvga variant has 16 MB. We use this to select correctly. */
#define BDA_MODERN_BAR_SIZE 0x02000000ULL

static inline void bga_write(uint16_t reg, uint16_t val)
{
    g_api->outw(BGA_INDEX, reg);
    g_api->outw(BGA_DATA, val);
}

static void bda_set_mode_internal(uint32_t width, uint32_t height, uint32_t bpp)
{
    bga_write(BGA_REG_ENABLE, 0);
    bga_write(BGA_REG_XRES, (uint16_t)width);
    bga_write(BGA_REG_YRES, (uint16_t)height);
    bga_write(BGA_REG_BPP,  (uint16_t)bpp);
    bga_write(BGA_REG_ENABLE, BGA_ENABLED | BGA_LFB);
}

static int bda_set_mode(uint32_t width, uint32_t height, uint32_t bpp)
{
    if (width == 0 || height == 0 || (bpp != 32 && bpp != 24 && bpp != 16))
        return -1;

    bda_set_mode_internal(width, height, bpp);

    bda_gpu.fb.width      = width;
    bda_gpu.fb.height     = height;
    bda_gpu.fb.bpp        = (uint8_t)bpp;
    bda_gpu.fb.pitch      = width * ((bpp + 7) / 8);
    bda_gpu.fb.fmt        = (bpp == 32) ? FB_FMT_XRGB8888 : FB_FMT_RGB565;
    bda_gpu.fb.size_bytes = (uint64_t)width * height * ((bpp + 7) / 8);

    return 0;
}

static void bda_flush(const framebuffer_t *fb,
                      uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    (void)fb; (void)x; (void)y; (void)w; (void)h;
}

static void bda_draw_test_pattern(void)
{
    uint32_t *fb    = (uint32_t *)bda_gpu.fb.addr;
    uint32_t  pitch = bda_gpu.fb.pitch / 4;

    for (uint32_t y = 0; y < bda_gpu.fb.height; ++y) {
        for (uint32_t x = 0; x < bda_gpu.fb.width; ++x) {
            uint32_t color = 0xFF000000;

            if (y < bda_gpu.fb.height / 3)
                color |= 0x00FF0000;
            else if (y < bda_gpu.fb.height * 2 / 3)
                color |= 0x0000FF00;
            else
                color |= 0x000000FF;

            if ((x / 32 + y / 32) % 2)
                color |= 0x00808080;

            fb[y * pitch + x] = color;
        }
    }
}

/*
 * Scans bus 0 for 1234:1111 devices and returns the BAR0 physical base of
 * the one whose BAR size is >= BDA_MODERN_BAR_SIZE (32 MB).
 *
 * BAR sizing uses the standard PCI protocol: write all-ones, read back the
 * mask, restore the original value. The device with the larger aperture is
 * unambiguously the modern bochs-display (class 0380).
 */
static uint64_t bda_find_bar0(void)
{
    for (uint8_t slot = 0; slot < 32; ++slot) {
        uint32_t vid_did = g_api->pci_cfg_read32(0, slot, 0, 0x00);

        if ((vid_did & 0x0000FFFF) != 0x1234)
            continue;
        if ((vid_did >> 16) != 0x1111)
            continue;

        uint32_t bar0 = g_api->pci_cfg_read32(0, slot, 0, 0x10);

        g_api->pci_cfg_write32(0, slot, 0, 0x10, 0xFFFFFFFF);
        uint32_t mask = g_api->pci_cfg_read32(0, slot, 0, 0x10);
        g_api->pci_cfg_write32(0, slot, 0, 0x10, bar0);

        uint64_t size = (~(mask & ~0xFu) + 1u) & 0xFFFFFFFFu;

        if (size >= BDA_MODERN_BAR_SIZE)
            return bar0 & ~0xFULL;
    }

    return 0;
}

int sqrm_module_init(const sqrm_kernel_api_t *api)
{
    if (!api)
        return -1;

    g_api = api;
    g_api->com_write_string(0, "[BDA] Initializing PCI Bochs Display Adapter...\n");

    uint64_t fb_phys = bda_find_bar0();
    if (!fb_phys) {
        g_api->com_write_string(0, "[BDA] Modern bochs-display device not found\n");
        return -4;
    }

    /* pci_find_device is still needed to obtain a handle for
     * pci_enable_memory_space. Walk devices to match our BAR. */
    void *pci_dev = NULL;
    int count = g_api->pci_get_device_count();

    for (int i = 0; i < count; ++i) {
        void *dev = g_api->pci_get_device(i);
        if (!dev)
            continue;

        /* Identify by re-reading the slot's BAR0 — match against fb_phys. */
        int slot = i; /* pci_get_device index maps to slot in this SQRM impl */
        uint32_t bar0 = g_api->pci_cfg_read32(0, (uint8_t)slot, 0, 0x10);
        if ((bar0 & ~0xFu) == (uint32_t)fb_phys) {
            pci_dev = dev;
            break;
        }
    }

    if (!pci_dev) {
        /* Fallback: use whatever pci_find_device returns; memory space
         * enable may already be set by the firmware anyway. */
        pci_dev = g_api->pci_find_device(0x1234, 0x1111);
    }

    if (pci_dev)
        g_api->pci_enable_memory_space(pci_dev);

    bda_gpu.fb.phys_addr  = fb_phys;
    bda_gpu.fb.addr       = g_api->ioremap(fb_phys, BDA_MODERN_BAR_SIZE);
    bda_gpu.fb.size_bytes = BDA_MODERN_BAR_SIZE;

    if (!bda_gpu.fb.addr) {
        g_api->com_write_string(0, "[BDA] ioremap failed\n");
        return -5;
    }

    g_api->com_write_string(0, "[BDA] BAR0 mapped successfully\n");

    if (bda_set_mode(1024, 768, 32) != 0)
        return -2;

    bda_draw_test_pattern();

    bda_gpu.flush    = bda_flush;
    bda_gpu.set_mode = bda_set_mode;
    bda_gpu.caps     = SQRM_GPU_CAP_VSYNC;

    if (g_api->gfx_register_framebuffer(&bda_gpu) != 0)
        return -3;

    g_api->com_write_string(0, "[BDA] Initialized successfully\n");
    return 0;
}

SQRM_DEFINE_MODULE(SQRM_TYPE_GPU, "bda");