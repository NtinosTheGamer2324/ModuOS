/*
 * md_vga_compatible_display_sqrm.c - MD VGA Compatible Display Driver (SQRM) for ModuOS
 *
 * This driver implements the SQRM GPU protocol for standard VGA hardware.
 * It provides a linear framebuffer interface for VGA text/graphics modes.
 *
 * Supports:
 *   - VGA text mode 80x25 (as a pixel framebuffer)
 *   - VGA graphics mode 320x200x8 (Mode 13h)
 *   - VESA VBE modes (if available via BIOS)
 *   - Direct framebuffer access
 *
 * Hardware Support:
 *   - Standard VGA (BIOS legacy)
 *   - VESA VBE 2.0+ compatible cards
 *   - PCI VGA devices (class 0x0300)
 */

#include "moduos/kernel/sqrm.h"

/*===========================================================================
 * VGA Hardware Constants
 *===========================================================================*/

// VGA I/O Ports
#define VGA_MISC_WRITE          0x3C2
#define VGA_MISC_READ           0x3CC
#define VGA_SEQ_INDEX           0x3C4
#define VGA_SEQ_DATA            0x3C5
#define VGA_GC_INDEX            0x3CE
#define VGA_GC_DATA             0x3CF
#define VGA_CRTC_INDEX          0x3D4
#define VGA_CRTC_DATA           0x3D5
#define VGA_ATTR_INDEX          0x3C0
#define VGA_ATTR_DATA_WRITE     0x3C0
#define VGA_ATTR_DATA_READ      0x3C1
#define VGA_DAC_WRITE_INDEX     0x3C8
#define VGA_DAC_DATA            0x3C9

// Standard VGA framebuffer addresses
#define VGA_TEXT_MEMORY         0xB8000
#define VGA_GRAPHICS_MEMORY     0xA0000

// Mode 13h dimensions
#define VGA_MODE13_WIDTH        320
#define VGA_MODE13_HEIGHT       200
#define VGA_MODE13_BPP          8

// Text mode dimensions (rendered as pixels)
#define VGA_TEXT_COLS           80
#define VGA_TEXT_ROWS           25
#define VGA_CHAR_WIDTH          8
#define VGA_CHAR_HEIGHT         16

/*===========================================================================
 * PCI VGA Discovery
 *===========================================================================*/

#define PCI_CONFIG_ADDRESS      0xCF8
#define PCI_CONFIG_DATA         0xCFC
#define PCI_CLASS_DISPLAY       0x03
#define PCI_SUBCLASS_VGA        0x00

typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint32_t bar0;
} pci_vga_device_t;

/*===========================================================================
 * Driver State
 *===========================================================================*/

static sqrm_module_desc_t sqrm_module_desc = {
    .abi_version = 1,
    .type = SQRM_TYPE_GPU,
    .name = "md_vga_compatible_display",
};

static const sqrm_kernel_api_t *g_api = NULL;
static sqrm_gpu_device_t g_dev;
static framebuffer_t g_fb;
static pci_vga_device_t g_vga_pci;

static int g_have_pci_vga = 0;

// Current mode tracking
static enum {
    VGA_MODE_TEXT,
    VGA_MODE_13H,
    VGA_MODE_VESA
} g_current_mode = VGA_MODE_13H;

/*===========================================================================
 * PCI Helper Functions
 * Note: I/O functions come from g_api (capability-gated)
 *===========================================================================*/

static uint32_t pci_config_read(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    if (!g_api || !g_api->outl || !g_api->inl) return 0xFFFFFFFF;
    
    uint32_t addr = 0x80000000 | ((uint32_t)bus << 16) | 
                    ((uint32_t)dev << 11) | ((uint32_t)func << 8) | 
                    (offset & 0xFC);
    g_api->outl(PCI_CONFIG_ADDRESS, addr);
    return g_api->inl(PCI_CONFIG_DATA);
}

static int pci_find_vga_device(pci_vga_device_t *out) {
    // Scan PCI bus for VGA devices
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t reg0 = pci_config_read(bus, dev, func, 0x00);
                uint16_t vendor = reg0 & 0xFFFF;
                
                if (vendor == 0xFFFF) continue; // No device
                
                uint32_t reg2 = pci_config_read(bus, dev, func, 0x08);
                uint8_t class_code = (reg2 >> 24) & 0xFF;
                uint8_t subclass = (reg2 >> 16) & 0xFF;
                
                if (class_code == PCI_CLASS_DISPLAY && subclass == PCI_SUBCLASS_VGA) {
                    out->vendor_id = vendor;
                    out->device_id = (reg0 >> 16) & 0xFFFF;
                    out->bus = bus;
                    out->device = dev;
                    out->function = func;
                    out->bar0 = pci_config_read(bus, dev, func, 0x10);
                    return 0; // Found!
                }
            }
        }
    }
    return -1;
}

/*===========================================================================
 * VGA Register Programming
 *===========================================================================*/

static void vga_set_mode_13h(void) {
    if (!g_api || !g_api->outb || !g_api->inb) return;
    
    // Set Mode 13h (320x200x8) via BIOS-style register programming
    
    // Misc register
    g_api->outb(VGA_MISC_WRITE, 0x63);
    
    // Sequencer registers
    const uint8_t seq_regs[] = {0x03, 0x01, 0x0F, 0x00, 0x0E};
    for (int i = 0; i < 5; i++) {
        g_api->outb(VGA_SEQ_INDEX, i);
        g_api->outb(VGA_SEQ_DATA, seq_regs[i]);
    }
    
    // CRTC registers (unlocked)
    g_api->outb(VGA_CRTC_INDEX, 0x03);
    g_api->outb(VGA_CRTC_DATA, g_api->inb(VGA_CRTC_DATA) | 0x80);
    g_api->outb(VGA_CRTC_INDEX, 0x11);
    g_api->outb(VGA_CRTC_DATA, g_api->inb(VGA_CRTC_DATA) & ~0x80);
    
    const uint8_t crtc_regs[] = {
        0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
        0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3, 0xFF
    };
    for (int i = 0; i < 25; i++) {
        g_api->outb(VGA_CRTC_INDEX, i);
        g_api->outb(VGA_CRTC_DATA, crtc_regs[i]);
    }
    
    // Graphics Controller
    const uint8_t gc_regs[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF};
    for (int i = 0; i < 9; i++) {
        g_api->outb(VGA_GC_INDEX, i);
        g_api->outb(VGA_GC_DATA, gc_regs[i]);
    }
    
    // Attribute Controller
    g_api->inb(0x3DA); // Reset flip-flop
    const uint8_t ac_regs[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
        0x41, 0x00, 0x0F, 0x00, 0x00
    };
    for (int i = 0; i < 21; i++) {
        g_api->outb(VGA_ATTR_INDEX, i);
        g_api->outb(VGA_ATTR_DATA_WRITE, ac_regs[i]);
    }
    g_api->outb(VGA_ATTR_INDEX, 0x20); // Enable video
}

static void vga_set_default_palette(void) {
    if (!g_api || !g_api->outb) return;

    // VGA DAC components are 6-bit (0..63).
    g_api->outb(VGA_DAC_WRITE_INDEX, 0);

    // Generate 6-6-6 color cube (216 colors) + grayscale.
    // Keep values clamped to 0..63 to avoid wraparound.
    for (int i = 0; i < 256; i++) {
        uint8_t r = 0, g = 0, b = 0;

        if (i < 216) {
            // Color cube
            r = (uint8_t)(((i / 36) % 6) * 12); // 0..60
            g = (uint8_t)(((i / 6) % 6) * 12);
            b = (uint8_t)((i % 6) * 12);
        } else {
            // Grayscale ramp: spread 40 entries across 0..63
            uint32_t idx = (uint32_t)(i - 216); // 0..39
            uint32_t v = (idx * 63u) / 39u;     // 0..63
            r = g = b = (uint8_t)v;
        }

        g_api->outb(VGA_DAC_DATA, r);
        g_api->outb(VGA_DAC_DATA, g);
        g_api->outb(VGA_DAC_DATA, b);
    }
}

/*===========================================================================
 * Framebuffer Operations
 *===========================================================================*/

static void vga_flush(const framebuffer_t *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    (void)fb; (void)x; (void)y; (void)w; (void)h;
    // VGA Mode 13h has direct scanout - no flush needed
}

static int vga_enumerate_modes(gfx_mode_t *out_modes, uint32_t max_modes) {
    if (!out_modes || max_modes == 0) return 0;
    
    int count = 0;
    
    // Mode 13h - 320x200x8
    if (count < max_modes) {
        out_modes[count].width = VGA_MODE13_WIDTH;
        out_modes[count].height = VGA_MODE13_HEIGHT;
        out_modes[count].bpp = VGA_MODE13_BPP;
        count++;
    }
    
    // Text mode rendered as graphics - 640x400x32
    if (count < max_modes) {
        out_modes[count].width = VGA_TEXT_COLS * VGA_CHAR_WIDTH;
        out_modes[count].height = VGA_TEXT_ROWS * VGA_CHAR_HEIGHT;
        out_modes[count].bpp = 32;
        count++;
    }
    
    return count;
}

static int vga_set_mode(uint32_t width, uint32_t height, uint32_t bpp) {
    if (!g_api) return -1;

    // Only support Mode 13h for now
    if (width == VGA_MODE13_WIDTH && height == VGA_MODE13_HEIGHT && bpp == VGA_MODE13_BPP) {
        vga_set_mode_13h();
        vga_set_default_palette();

        g_fb.phys_addr = (uint64_t)VGA_GRAPHICS_MEMORY;
        g_fb.size_bytes = (uint64_t)VGA_MODE13_WIDTH * (uint64_t)VGA_MODE13_HEIGHT;
        g_fb.width = VGA_MODE13_WIDTH;
        g_fb.height = VGA_MODE13_HEIGHT;
        g_fb.pitch = VGA_MODE13_WIDTH;
        g_fb.bpp = VGA_MODE13_BPP;
        g_fb.fmt = FB_FMT_UNKNOWN;

        // Ensure addr is still valid (it may be ioremapped).
        if (!g_fb.addr) {
            if (g_api->ioremap) {
                g_fb.addr = g_api->ioremap(g_fb.phys_addr, g_fb.size_bytes);
            }
            if (!g_fb.addr) g_fb.addr = (void*)(uintptr_t)VGA_GRAPHICS_MEMORY;
        }

        g_current_mode = VGA_MODE_13H;

        if (g_api->gfx_update_framebuffer) {
            g_api->gfx_update_framebuffer(&g_fb);
        }

        return 0;
    }

    return -1;
}

/*===========================================================================
 * Module Initialization
 *===========================================================================*/

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    if (!api || api->abi_version != 1) return -1;
    
    g_api = api;
    
    // Check if we have required I/O capabilities
    if (!api->outb || !api->inb || !api->outl || !api->inl) {
        if (api->com_write_string) {
            api->com_write_string(0x3F8, "[MD-VGA] Missing I/O port capabilities\n");
        }
        return -1;
    }
    
    if (api->com_write_string) {
        api->com_write_string(0x3F8, "[MD-VGA] MD VGA Compatible Display Driver initializing...\n");
    }
    
    // Try to discover the active display adapter via the kernel PCI API when available.
    // If not, fall back to raw config-space scanning.
    g_have_pci_vga = 0;
    if (api->pci_get_device_count && api->pci_get_device) {
        int n = api->pci_get_device_count();
        for (int i = 0; i < n; i++) {
            pci_device_t *d = api->pci_get_device(i);
            if (!d) continue;
            if (d->class_code == PCI_CLASS_DISPLAY && d->subclass == PCI_SUBCLASS_VGA) {
                g_vga_pci.vendor_id = d->vendor_id;
                g_vga_pci.device_id = d->device_id;
                g_vga_pci.bus = d->bus;
                g_vga_pci.device = d->device;
                g_vga_pci.function = d->function;
                g_vga_pci.bar0 = d->bar[0];
                g_have_pci_vga = 1;
                break;
            }
        }
    } else {
        // Raw scan: requires 0xCF8/0xCFC access.
        if (pci_find_vga_device(&g_vga_pci) == 0) {
            g_have_pci_vga = 1;
        }
    }

    if (g_have_pci_vga) {
        // If we detect a known accelerated/paravirtual adapter, defer to its driver.
        if (g_vga_pci.vendor_id == 0x15AD) {
            if (api->com_write_string) api->com_write_string(0x3F8, "[MD-VGA] Detected VMware SVGA; deferring to VMSVGA driver\n");
            return -1;
        }
        if (g_vga_pci.vendor_id == 0x1B36 && g_vga_pci.device_id == 0x0100) {
            if (api->com_write_string) api->com_write_string(0x3F8, "[MD-VGA] Detected QXL; deferring to QXL driver\n");
            return -1;
        }
        if (g_vga_pci.vendor_id == 0x80EE && g_vga_pci.device_id == 0xBEEF) {
            if (api->com_write_string) api->com_write_string(0x3F8, "[MD-VGA] Detected VirtualBox SVGA; deferring to VBox SVGA driver\n");
            return -1;
        }
        if (g_vga_pci.vendor_id == 0x1234 && g_vga_pci.device_id == 0x1111) {
            if (api->com_write_string) api->com_write_string(0x3F8, "[MD-VGA] Detected Bochs VBE; deferring to Bochs/VBE driver\n");
            return -1;
        }
        if (api->com_write_string) {
            api->com_write_string(0x3F8, "[MD-VGA] Found PCI VGA adapter; using VGA compatibility path\n");
        }
    } else {
        if (api->com_write_string) {
            api->com_write_string(0x3F8, "[MD-VGA] No PCI VGA detected; assuming legacy VGA\n");
        }
    }
    
    // Set Mode 13h as default
    vga_set_mode_13h();
    vga_set_default_palette();
    
    // Map VGA framebuffer.
    // If ioremap is available, map it; otherwise assume it's already accessible.
    g_fb.phys_addr = (uint64_t)VGA_GRAPHICS_MEMORY;
    g_fb.size_bytes = (uint64_t)VGA_MODE13_WIDTH * (uint64_t)VGA_MODE13_HEIGHT; // 64,000 bytes
    g_fb.width = VGA_MODE13_WIDTH;
    g_fb.height = VGA_MODE13_HEIGHT;
    g_fb.pitch = VGA_MODE13_WIDTH; // bytes per scanline for packed 8bpp
    g_fb.bpp = VGA_MODE13_BPP;
    g_fb.fmt = FB_FMT_UNKNOWN; // palette-indexed

    if (api->ioremap) {
        g_fb.addr = api->ioremap(g_fb.phys_addr, g_fb.size_bytes);
    }
    if (!g_fb.addr) {
        g_fb.addr = (void*)(uintptr_t)VGA_GRAPHICS_MEMORY;
    }

    // Palette-indexed modes do not have meaningful RGB masks; leave at 0.
    
    // Setup GPU device structure
    g_dev.fb = g_fb;
    g_dev.flush = NULL; // direct scanout (no explicit flush required)
    g_dev.enumerate_modes = vga_enumerate_modes;
    g_dev.set_mode = vga_set_mode;
    
    // Register with kernel
    if (api->gfx_register_framebuffer) {
        if (api->gfx_register_framebuffer(&g_dev) == 0) {
            if (api->com_write_string) {
                api->com_write_string(0x3F8, "[MD-VGA] Registered VGA Mode 13h framebuffer (320x200x8)\n");
            }
            return 0;
        } else {
            if (api->com_write_string) {
                api->com_write_string(0x3F8, "[MD-VGA] Failed to register framebuffer\n");
            }
            return -1;
        }
    }
    
    if (api->com_write_string) {
        api->com_write_string(0x3F8, "[MD-VGA] No gfx_register_framebuffer API available\n");
    }
    return -1;
}