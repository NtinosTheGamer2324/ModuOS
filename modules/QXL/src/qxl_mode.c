// SPDX-License-Identifier: GPL-2.0-only

#include <stdint.h>
#include "moduos/kernel/sqrm.h"
#include "qxl_dev.h"

// Defined in qxl_gpu_sqrm.c
extern const sqrm_kernel_api_t *g_api;
extern volatile QXLRom *g_rom;
extern volatile QXLRam *g_ram_hdr;
extern uint16_t g_io_base;
extern uint64_t g_ram_phys;
extern void *g_ram_virt;
extern framebuffer_t g_fb;

static inline void qxl_io_write(uint32_t cmd, uint32_t val) {
    g_api->outl((uint16_t)(g_io_base + (uint16_t)(cmd * 4u)), val);
}

static int qxl_apply_mode(const QXLMode *m) {
    if (!m || m->bits != 32) return -1;

    uint32_t width = m->x_res;
    uint32_t height = m->y_res;
    uint32_t pitch = m->stride;

    uint64_t fb_bytes = (uint64_t)pitch * (uint64_t)height;
    if (g_rom->surface0_area_size && fb_bytes > (uint64_t)g_rom->surface0_area_size) return -2;

    uint64_t fb_off = (uint64_t)g_rom->draw_area_offset;
    void *fb_ptr = (void*)((uint8_t*)g_ram_virt + fb_off);
    uint64_t fb_phys = g_ram_phys + fb_off;

    g_ram_hdr->create_surface_id = 0;
    g_ram_hdr->create_surface.width = width;
    g_ram_hdr->create_surface.height = height;
    g_ram_hdr->create_surface.stride = (int32_t)pitch;
    g_ram_hdr->create_surface.format = QXL_SURF_FMT_32_xRGB;
    g_ram_hdr->create_surface.position = 0;
    g_ram_hdr->create_surface.flags = 0;
    g_ram_hdr->create_surface.type = 0;
    g_ram_hdr->create_surface.mem = fb_phys;

    // Recreate primary
    qxl_io_write(QXL_IO_DESTROY_PRIMARY_ASYNC, 0);
    qxl_io_write(QXL_IO_CREATE_PRIMARY_ASYNC, 0);
    qxl_io_write(QXL_IO_ATTACH_PRIMARY, 0);

    // Update exported framebuffer
    g_fb.addr = fb_ptr;
    g_fb.phys_addr = fb_phys;
    g_fb.size_bytes = fb_bytes;
    g_fb.width = width;
    g_fb.height = height;
    g_fb.pitch = pitch;
    g_fb.bpp = 32;
    g_fb.fmt = FB_FMT_XRGB8888;

    // Force an update so the new surface becomes visible.
    qxl_io_write(QXL_IO_UPDATE_AREA_ASYNC, 0);

    // Notify kernel of updated framebuffer geometry/address so it can rebind consoles/mappings.
    if (g_api && g_api->gfx_update_framebuffer) {
        int urc = g_api->gfx_update_framebuffer(&g_fb);
        if (urc != 0) return -4;
    }

    return 0;
}

int qxl_set_mode(uint32_t width, uint32_t height, uint32_t bpp) {
    if (!g_api || !g_rom || !g_ram_hdr) return -1;
    if (bpp != 32) return -2;

    const QXLMode *modes = (const QXLMode *)((const uint8_t*)g_rom + g_rom->modes_offset);
    for (uint32_t i = 0; i < 64; i++) {
        const QXLMode *m = &modes[i];
        if (m->x_res == 0 || m->y_res == 0 || m->bits == 0) break;
        if (m->bits != 32) continue;
        if (m->x_res == width && m->y_res == height) {
            return qxl_apply_mode(m);
        }
    }

    return -3;
}