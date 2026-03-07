// SPDX-License-Identifier: GPL-2.0-only
//
// ModuOS Kernel (GPLv2)
// gfx_sqrm.c - kernel entry points for SQRM GPU modules
#include "moduos/kernel/sqrm.h"
#include "moduos/kernel/sqrm_internal.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/bootscreen.h"
#include "moduos/kernel/mdinit.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/gfx.h"
#include "moduos/kernel/memory/paging.h"

static sqrm_gpu_device_t g_active_gpu;
static int g_have_gpu = 0;
static char g_active_gpu_driver[64];

static void com_print_dec64(uint64_t v) {
    char tmp[32];
    int pos = 0;
    if (v == 0) { 
        com_write_string(COM1_PORT, "0"); 
        return; 
    }
    while (v > 0 && pos < 31) {
        tmp[pos++] = '0' + (v % 10);
        v /= 10;
    }
    for (int i = pos - 1; i >= 0; i--) {
        char c[2] = { tmp[i], 0 };
        com_write_string(COM1_PORT, c);
    }
}

static void com_print_hex64(uint64_t v) {
    const char hex[] = "0123456789abcdef";
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        buf[2 + i] = hex[(v >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = 0;
    com_write_string(COM1_PORT, buf);
}

int gfx_update_framebuffer(const framebuffer_t *new_fb) {
    if (!new_fb || !new_fb->addr || new_fb->width == 0 || new_fb->height == 0) return -1;

    VGA_SetFrameBuffer(new_fb);

    /* If we were already in graphics mode, we need to rebind the fb_console to the new geometry. */
    if (VGA_GetFrameBufferMode() == FB_MODE_GRAPHICS) {
        VGA_ReinitFrameBufferConsole();
    }

    return 0;
}

int gfx_request_set_mode(uint32_t width, uint32_t height, uint32_t bpp) {
    if (!g_have_gpu) return -1;
    if (!g_active_gpu.set_mode) return -2;
    return g_active_gpu.set_mode(width, height, bpp);
}

int gfx_enumerate_modes(gfx_mode_t *out_modes, uint32_t max_modes) {
    if (!out_modes || max_modes == 0) return -1;
    if (!g_have_gpu) return -2;
    if (!g_active_gpu.enumerate_modes) return -3;
    return g_active_gpu.enumerate_modes(out_modes, max_modes);
}

int gfx_update_framebuffer_from_sqrm(const framebuffer_t *fb) {
    if (!fb || !fb->addr || fb->width == 0 || fb->height == 0) return -1;
    if (!g_have_gpu) return -2;

    // Update cached copy and re-init graphics console to the new framebuffer.
    g_active_gpu.fb = *fb;
    return gfx_update_framebuffer(fb);
}

int gfx_register_framebuffer_from_sqrm(const sqrm_gpu_device_t *dev) {
    if (!dev) return -1;
    if (!dev->fb.addr || dev->fb.width == 0 || dev->fb.height == 0) return -2;

    // Debug: print framebuffer descriptor
    com_write_string(COM1_PORT, "[GFX] fb addr=");
    com_print_hex64((uint64_t)(uintptr_t)dev->fb.addr);
    com_write_string(COM1_PORT, " w=");
    com_print_dec64(dev->fb.width);
    com_write_string(COM1_PORT, " h=");
    com_print_dec64(dev->fb.height);
    com_write_string(COM1_PORT, " pitch=");
    com_print_dec64(dev->fb.pitch);
    com_write_string(COM1_PORT, " bpp=");
    com_print_dec64(dev->fb.bpp);
    com_write_string(COM1_PORT, "\n");

    // Switch kernel into graphics mode using the supplied framebuffer.
    // Some SQRM GPU modules only provide a kernel virtual address. VGA_SetFrameBuffer()
    // requires phys_addr and size_bytes; derive them if missing.
    framebuffer_t fb = dev->fb;
    if (fb.phys_addr == 0) {
        fb.phys_addr = paging_virt_to_phys((uint64_t)(uintptr_t)fb.addr);
    }
    if (fb.size_bytes == 0 && fb.pitch && fb.height) {
        uint64_t fb_size = (uint64_t)fb.pitch * (uint64_t)fb.height;
        fb.size_bytes = (fb_size + 4095ULL) & ~4095ULL;
    }

    /* Install flush hook BEFORE switching into graphics mode so the initial redraw flushes to the device. */
    VGA_SetFlushHook(dev->flush);
    VGA_SetFrameBuffer(&fb);

    // Keep a copy for optional future use.
    g_active_gpu = *dev;
    g_active_gpu.fb = fb; // store derived fields too
    g_have_gpu = 1;

    // Capture which SQRM module registered this GPU (for MD64API/neofetch).
    {
        const char *m = sqrm_get_current_module_name();
        if (!m) m = "";
        size_t i = 0;
        for (; i + 1 < sizeof(g_active_gpu_driver) && m[i]; i++) g_active_gpu_driver[i] = m[i];
        g_active_gpu_driver[i] = 0;
    }

    // Debug: confirm whether this GPU driver supports mode enumeration.
    com_write_string(COM1_PORT, "[GFX] enumerate_modes=");
    com_write_string(COM1_PORT, g_active_gpu.enumerate_modes ? "yes" : "no");
    com_write_string(COM1_PORT, "\n");

    // Ensure the current console is visible at least once after registration.
    VGA_ForceRedrawConsole();

    // Draw bootscreen after the console redraw so it remains visible (otherwise the redraw overwrites it).
    // This ensures we get a splash even when Multiboot did not provide a framebuffer.
    uint64_t mb2_ptr = mdinit_get_mb2_ptr();
    if (mb2_ptr) {
        (void)bootscreen_show((void*)(uintptr_t)mb2_ptr);
    }

    // Force a full-screen flush for paravirtual GPUs like QXL.
    VGA_FlushRect(0, 0, fb.width, fb.height);

    com_write_string(COM1_PORT, "[GFX] SQRM GPU registered framebuffer\n");
    return 0;
}

int gfx_have_sqrm_gpu(void) {
    return g_have_gpu;
}

const char *gfx_get_sqrm_gpu_driver_name(void) {
    if (!g_have_gpu) return "";
    return g_active_gpu_driver;
}

const sqrm_gpu_device_t *gfx_get_sqrm_gpu_device(void) {
    return g_have_gpu ? &g_active_gpu : NULL;
}

uint32_t gfx_get_gpu_caps(void) {
    return g_have_gpu ? g_active_gpu.caps : 0;
}

const char *gfx_get_active_driver_name(void) {
    return gfx_get_sqrm_gpu_driver_name();
}

const char *gfx_get_active_gpu_name(void) {
    return gfx_get_sqrm_gpu_driver_name();
}
