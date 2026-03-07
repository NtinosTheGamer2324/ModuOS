// SPDX-License-Identifier: GPL-2.0-only
//
// intel_i915_sqrm.c - Intel HD Graphics / UHD Graphics Driver (i915)
// SQRM GPU module for Intel integrated graphics
//
// Supports:
// - Intel HD Graphics 2000-6000 (Sandy Bridge - Skylake)
// - Intel UHD Graphics 600-770 (Kaby Lake - Arrow Lake)
// - Intel Iris Graphics (Gen 8-12)
// - Intel Iris Xe Graphics (Gen 12+)
// - Intel Core Ultra (Meteor Lake)
//
// Covers 2011-2024 hardware (Gen 6 - Gen 12.9)
//
// Based on Intel Graphics Programmer Reference Manual (PRM)

#include <stdint.h>
#include <stddef.h>

#include "moduos/kernel/sqrm.h"
#include "moduos/kernel/memory/string.h"

#define COM1_PORT 0x3F8

const sqrm_module_desc_t sqrm_module_desc = {
    .abi_version = 1,
    .type = SQRM_TYPE_GPU,
    .name = "intel_i915",
};

// Intel GPU PCI IDs (common integrated graphics)
static const uint16_t intel_gpu_devices[] = {
    // Sandy Bridge (Gen 6)
    0x0102, 0x0106, 0x010A, 0x0112, 0x0116, 0x0122, 0x0126,
    
    // Ivy Bridge (Gen 7)
    0x0152, 0x0156, 0x015A, 0x0162, 0x0166,
    
    // Haswell (Gen 7.5)
    0x0402, 0x0406, 0x040A, 0x0412, 0x0416, 0x041A, 0x041E,
    0x0A02, 0x0A06, 0x0A0A, 0x0A0E, 0x0A12, 0x0A16, 0x0A1A, 0x0A1E,
    0x0D02, 0x0D06, 0x0D0A, 0x0D0E, 0x0D12, 0x0D16, 0x0D1A, 0x0D1E,
    
    // Broadwell (Gen 8)
    0x1602, 0x1606, 0x160A, 0x160E, 0x1612, 0x1616, 0x161A, 0x161E,
    0x1622, 0x1626, 0x162A, 0x162E,
    
    // Skylake (Gen 9)
    0x1902, 0x1906, 0x190A, 0x190E, 0x1912, 0x1916, 0x191A, 0x191E,
    0x1923, 0x1926, 0x1927, 0x192A,
    
    // Kaby Lake (Gen 9.5)
    0x5902, 0x5906, 0x590A, 0x590E, 0x5912, 0x5916, 0x591A, 0x591E,
    0x5923, 0x5926, 0x5927,
    
    // Coffee Lake (Gen 9.5)
    0x3E90, 0x3E91, 0x3E92, 0x3E93, 0x3E94, 0x3E96, 0x3E98, 0x3E9B,
    
    // Comet Lake (Gen 9.5)
    0x9B21, 0x9BA0, 0x9BA2, 0x9BA4, 0x9BA5, 0x9BA8, 0x9BAA, 0x9BAB, 0x9BAC,
    
    // Ice Lake (Gen 11)
    0x8A50, 0x8A51, 0x8A52, 0x8A53, 0x8A56, 0x8A58, 0x8A5A, 0x8A5C,
    
    // Tiger Lake (Gen 12)
    0x9A40, 0x9A49, 0x9A59, 0x9A60, 0x9A68, 0x9A70, 0x9A78,
    
    // Alder Lake (Gen 12)
    0x4680, 0x4682, 0x4688, 0x468A, 0x468B, 0x4690, 0x4692, 0x4693,
    
    // Rocket Lake (Gen 12.5) - 2021
    0x4C80, 0x4C8A, 0x4C8B, 0x4C90, 0x4C9A,
    
    // Raptor Lake (Gen 12.7) - 2022-2023 (13th/14th gen Core)
    0xA720, 0xA721, 0xA780, 0xA781, 0xA782, 0xA783, 0xA788, 0xA789, 0xA78B,
    0xA78C, 0xA790, 0xA791, 0xA792, 0xA793, 0xA794, 0xA795, 0xA796, 0xA797,
    0xA798, 0xA799, 0xA79A, 0xA79B, 0xA79C, 0xA79D,
    
    // Meteor Lake (Gen 12.8) - 2023-2024 (Core Ultra)
    0x7D40, 0x7D41, 0x7D45, 0x7D51, 0x7D55, 0x7D60, 0x7D67, 0x7DD1, 0x7DD5,
    
    // Arrow Lake (Gen 12.9) - 2024 (15th gen Core)
    0xB640, 0xB641, 0xB650, 0xB651, 0xB660, 0xB661,
};

#define NUM_INTEL_GPU_IDS (sizeof(intel_gpu_devices) / sizeof(uint16_t))

// Global state
static const sqrm_kernel_api_t *g_api;
static pci_device_t *g_dev;
static void *g_gttmmadr;  // GTT (Graphics Translation Table) MMIO + Aperture
static void *g_gmadr;     // Graphics Memory Aperture (framebuffer)
static uint64_t g_gttmmadr_phys;
static uint64_t g_gttmmadr_size;
static uint64_t g_gmadr_phys;
static uint64_t g_gmadr_size;

// Display registers (MMIO offsets from GTTMMADR)
#define PIPE_A_CONF       0x70008
#define PIPE_B_CONF       0x71008
#define PIPE_A_STATUS     0x70024
#define DSPACNTR          0x70180  // Display A Control
#define DSPASURF          0x7019C  // Display A Surface Address
#define DSPASTRIDE        0x70188  // Display A Stride

// VGA control
#define VGA_CONTROL       0x041000

// Framebuffer info
static uint32_t g_fb_width = 1024;
static uint32_t g_fb_height = 768;
static uint32_t g_fb_pitch = 1024 * 4;
static uint32_t g_fb_bpp = 32;

// Helper: Write 32-bit MMIO register
static inline void mmio_write32(uint32_t offset, uint32_t value) {
    if (!g_gttmmadr) return;
    volatile uint32_t *reg = (volatile uint32_t *)((uint8_t *)g_gttmmadr + offset);
    *reg = value;
}

// Helper: Read 32-bit MMIO register
static inline uint32_t mmio_read32(uint32_t offset) {
    if (!g_gttmmadr) return 0;
    volatile uint32_t *reg = (volatile uint32_t *)((uint8_t *)g_gttmmadr + offset);
    return *reg;
}

// Detect and initialize Intel GPU
static int intel_i915_detect_and_init(void) {
    g_api->com_write_string(COM1_PORT, "[i915] Scanning for Intel GPU...\n");
    
    // Scan PCI for Intel GPU (Vendor 0x8086)
    for (int i = 0; i < 256; i++) {
        pci_device_t *dev = g_api->pci_get_device(i);
        if (!dev) continue;
        
        if (dev->vendor_id != 0x8086) continue;  // Intel vendor ID
        
        // Check if device ID matches known Intel GPU
        int found = 0;
        for (size_t j = 0; j < NUM_INTEL_GPU_IDS; j++) {
            if (dev->device_id == intel_gpu_devices[j]) {
                found = 1;
                break;
            }
        }
        
        if (!found) continue;
        
        g_api->com_write_string(COM1_PORT, "[i915] Found Intel GPU: ");
        // TODO: Print device ID
        g_api->com_write_string(COM1_PORT, "\n");
        
        g_dev = dev;
        
        // Map BAR0 (GTTMMADR - MMIO + GTT)
        if (dev->bar_type[0] != 0) {
            g_api->com_write_string(COM1_PORT, "[i915] ERROR: BAR0 is not MMIO\n");
            return -1;
        }
        
        g_gttmmadr_phys = (uint64_t)(dev->bar[0] & ~0xFULL);
        g_gttmmadr_size = dev->bar_size[0];
        
        g_api->com_write_string(COM1_PORT, "[i915] GTTMMADR at physical address: ");
        // TODO: Print address
        g_api->com_write_string(COM1_PORT, "\n");
        
        // Map GTTMMADR
        if (g_api->ioremap_guarded) {
            g_gttmmadr = g_api->ioremap_guarded(g_gttmmadr_phys, g_gttmmadr_size);
        } else {
            g_gttmmadr = g_api->ioremap(g_gttmmadr_phys, g_gttmmadr_size);
        }
        
        if (!g_gttmmadr) {
            g_api->com_write_string(COM1_PORT, "[i915] ERROR: Failed to map GTTMMADR\n");
            return -1;
        }
        
        // Map BAR2 (GMADR - Graphics Memory Aperture / Framebuffer)
        if (dev->bar_type[2] != 0) {
            g_api->com_write_string(COM1_PORT, "[i915] ERROR: BAR2 is not MMIO\n");
            return -1;
        }
        
        g_gmadr_phys = (uint64_t)(dev->bar[2] & ~0xFULL);
        g_gmadr_size = dev->bar_size[2];
        
        g_api->com_write_string(COM1_PORT, "[i915] GMADR at physical address: ");
        // TODO: Print address
        g_api->com_write_string(COM1_PORT, "\n");
        
        // Map GMADR (framebuffer aperture)
        if (g_api->ioremap_guarded) {
            g_gmadr = g_api->ioremap_guarded(g_gmadr_phys, g_gmadr_size);
        } else {
            g_gmadr = g_api->ioremap(g_gmadr_phys, g_gmadr_size);
        }
        
        if (!g_gmadr) {
            g_api->com_write_string(COM1_PORT, "[i915] ERROR: Failed to map GMADR\n");
            return -1;
        }
        
        g_api->com_write_string(COM1_PORT, "[i915] Intel GPU initialized successfully\n");
        return 0;
    }
    
    g_api->com_write_string(COM1_PORT, "[i915] No Intel GPU found\n");
    return -1;
}

// Initialize display (use legacy VGA framebuffer for now)
static int intel_i915_init_display(void) {
    g_api->com_write_string(COM1_PORT, "[i915] Initializing display...\n");
    
    // Disable VGA emulation
    mmio_write32(VGA_CONTROL, 0x80000000);
    
    // For now, use BIOS/UEFI-initialized framebuffer
    // TODO: Implement proper modesetting
    
    g_api->com_write_string(COM1_PORT, "[i915] Display initialized (using BIOS framebuffer)\n");
    return 0;
}

// Register framebuffer with kernel
static int intel_i915_register_fb(void) {
    if (!g_api->gfx_register_framebuffer) {
        g_api->com_write_string(COM1_PORT, "[i915] WARNING: gfx_register_framebuffer not available\n");
        return -1;
    }
    
    framebuffer_t fb;
    fb.addr = (uint32_t *)g_gmadr;
    fb.width = g_fb_width;
    fb.height = g_fb_height;
    fb.pitch = g_fb_pitch;
    fb.bpp = g_fb_bpp;
    
    int rc = g_api->gfx_register_framebuffer(&fb);
    if (rc != 0) {
        g_api->com_write_string(COM1_PORT, "[i915] ERROR: Failed to register framebuffer\n");
        return rc;
    }
    
    g_api->com_write_string(COM1_PORT, "[i915] Framebuffer registered with kernel\n");
    return 0;
}

// SQRM init function
int sqrm_init(const sqrm_kernel_api_t *api) {
    g_api = api;
    
    api->com_write_string(COM1_PORT, "[i915] Intel HD Graphics / UHD Graphics driver initializing\n");
    
    // Detect Intel GPU
    if (intel_i915_detect_and_init() != 0) {
        return -1;
    }
    
    // Initialize display
    if (intel_i915_init_display() != 0) {
        return -1;
    }
    
    // Register framebuffer
    if (intel_i915_register_fb() != 0) {
        return -1;
    }
    
    api->com_write_string(COM1_PORT, "[i915] Driver initialized successfully\n");
    return 0;
}

// SQRM cleanup function
void sqrm_cleanup(void) {
    g_api->com_write_string(COM1_PORT, "[i915] Driver cleanup\n");
    // TODO: Unmap BARs, restore VGA mode
}
