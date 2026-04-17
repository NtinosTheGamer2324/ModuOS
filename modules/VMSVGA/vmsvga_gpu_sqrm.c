#include "moduos/kernel/sqrm.h"
#include "moduos/kernel/memory/paging.h"
#include "include/vmsvga.h"

static const sqrm_kernel_api_t *g_api;
static pci_device_t *g_pci;

static uint16_t g_io_base = 0;
static volatile uint32_t *g_fb   = 0;
static volatile uint32_t *g_fifo = 0;
static uint32_t g_fifo_words = 0;

static sqrm_gpu_device_t g_dev;

static void com(const char *s) {
    if (g_api && g_api->com_write_string) g_api->com_write_string(0x3F8, s);
}

static void com_hex32(uint32_t v) {
    if (!g_api || !g_api->com_write_string) return;
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    const char *h = "0123456789ABCDEF";
    for (int i = 0; i < 8; i++)
        buf[2+i] = h[(v >> (28 - 4*i)) & 0xF];
    buf[10] = 0;
    g_api->com_write_string(0x3F8, buf);
}

static inline void svga_out(uint32_t index, uint32_t value) {
    g_api->outl((uint16_t)(g_io_base + SVGA_INDEX_PORT_OFF), index);
    g_api->outl((uint16_t)(g_io_base + SVGA_VALUE_PORT_OFF), value);
}

static inline uint32_t svga_in(uint32_t index) {
    g_api->outl((uint16_t)(g_io_base + SVGA_INDEX_PORT_OFF), index);
    return g_api->inl((uint16_t)(g_io_base + SVGA_VALUE_PORT_OFF));
}

static void svga_wait_for_fifo(void) {
    svga_out(SVGA_REG_SYNC, 1);
    while (svga_in(SVGA_REG_BUSY)) {}
}

/* -----------------------------------------------------------------------
 * FIFO write — dword-granular, no byte loops.
 *
 * The FIFO is a write-combining MMIO region. Writing it as 32-bit dwords
 * lets the CPU coalesce writes into cache-line-sized bursts. The old byte
 * loop defeated write combining entirely, serializing every byte through
 * the PCIe bus.
 *
 * We read NEXT_CMD/STOP once per call and only stall if truly full.
 * ----------------------------------------------------------------------- */
static void fifo_write(const void *src, uint32_t bytes) {
    if (!g_fifo || bytes == 0) return;

    /* Round up to dword boundary — SVGA FIFO is dword-granular. */
    uint32_t dwords = (bytes + 3u) >> 2;

    uint32_t min  = g_fifo[SVGA_FIFO_MIN];
    uint32_t max  = g_fifo[SVGA_FIFO_MAX];
    if (min == 0 || max <= min) return;

    uint32_t next = g_fifo[SVGA_FIFO_NEXT_CMD];

    const uint32_t *src32 = (const uint32_t *)src;

    for (uint32_t i = 0; i < dwords; i++) {
        /* Stall only when the single next slot is occupied. */
        uint32_t stop = g_fifo[SVGA_FIFO_STOP];
        uint32_t next_next = next + 4;
        if (next_next >= max) next_next = min;

        while (next_next == stop) {
            /* FIFO full — nudge hardware then re-check. */
            svga_wait_for_fifo();
            stop = g_fifo[SVGA_FIFO_STOP];
        }

        g_fifo[next >> 2] = src32[i];
        next += 4;
        if (next >= max) next = min;
    }

    /* Single write to commit the entire batch. */
    g_fifo[SVGA_FIFO_NEXT_CMD] = next;
}

/* -----------------------------------------------------------------------
 * Triangle rasterizer — emit the entire triangle as a single FIFO burst.
 *
 * Rather than flushing NEXT_CMD after every scanline, accumulate all
 * RECT_FILL dwords into a stack/heap buffer and submit once. This turns
 * N PCIe round-trips into one burst write.
 *
 * For triangles up to ~800px tall the buffer fits on the stack (worst
 * case: 800 * sizeof(svga_fifo_rect_fill_t) = 800*24 = 19200 bytes).
 * We cap at 768 scanlines (1024×768 screen height).
 * ----------------------------------------------------------------------- */
#define MAX_SCANLINES 768

static int draw_triangle_hw(const framebuffer_t *fb,
                            int32_t x0, int32_t y0, uint32_t color0,
                            int32_t x1, int32_t y1, uint32_t color1,
                            int32_t x2, int32_t y2, uint32_t color2) {
    if (!fb || !g_fifo) return -1;
    (void)color1; (void)color2;

    /* Sort vertices by Y. */
    if (y0 > y1) {
        int32_t tx = x0, ty = y0; uint32_t tc = color0;
        x0 = x1; y0 = y1; color0 = color1;
        x1 = tx; y1 = ty; color1 = tc;
    }
    if (y0 > y2) {
        int32_t tx = x0, ty = y0; uint32_t tc = color0;
        x0 = x2; y0 = y2; color0 = color2;
        x2 = tx; y2 = ty; color2 = tc;
    }
    if (y1 > y2) {
        int32_t tx = x1, ty = y1; uint32_t tc = color1;
        x1 = x2; y1 = y2; color1 = color2;
        x2 = tx; y2 = ty; color2 = tc;
    }

    if (y2 < 0 || y0 >= (int32_t)fb->height) return 0;

    int32_t y_start = y0 < 0 ? 0 : y0;
    int32_t y_end   = y2 >= (int32_t)fb->height ? (int32_t)fb->height - 1 : y2;
    int32_t n_lines = y_end - y_start + 1;
    if (n_lines <= 0) return 0;
    if (n_lines > MAX_SCANLINES) n_lines = MAX_SCANLINES;

    /* Stack-allocate the command batch.
     * sizeof(svga_fifo_rect_fill_t) == 24 bytes; 768 * 24 = 18432 bytes.
     * Kernel stacks are 16 KiB — use static storage to avoid overflow. */
    static svga_fifo_rect_fill_t batch[MAX_SCANLINES];
    int32_t count = 0;

    int32_t x_min_all = x0, x_max_all = x0;

    for (int32_t y = y_start; y <= y_end && count < MAX_SCANLINES; y++) {
        int32_t x_left, x_right;

        if (y < y1) {
            x_left  = (y1 != y0) ? (x0 + (x1 - x0) * (y - y0) / (y1 - y0)) : x0;
            x_right = (y2 != y0) ? (x0 + (x2 - x0) * (y - y0) / (y2 - y0)) : x0;
        } else {
            x_left  = (y2 != y1) ? (x1 + (x2 - x1) * (y - y1) / (y2 - y1)) : x1;
            x_right = (y2 != y0) ? (x0 + (x2 - x0) * (y - y0) / (y2 - y0)) : x0;
        }

        if (x_left > x_right) { int32_t t = x_left; x_left = x_right; x_right = t; }

        if (x_right < 0 || x_left >= (int32_t)fb->width) continue;
        if (x_left  < 0)                  x_left  = 0;
        if (x_right >= (int32_t)fb->width) x_right = (int32_t)fb->width - 1;

        uint32_t w = (uint32_t)(x_right - x_left + 1);
        if (w == 0) continue;

        if (x_left  < x_min_all) x_min_all = x_left;
        if (x_right > x_max_all) x_max_all = x_right;

        batch[count].cmd   = SVGA_CMD_RECT_FILL;
        batch[count].color = color0;
        batch[count].x     = (uint32_t)x_left;
        batch[count].y     = (uint32_t)y;
        batch[count].w     = w;
        batch[count].h     = 1;
        count++;
    }

    /* Submit entire batch in one call — single NEXT_CMD commit. */
    if (count > 0)
        fifo_write(batch, (uint32_t)count * sizeof(svga_fifo_rect_fill_t));

    /* One UPDATE for the bounding box. */
    if (x_min_all < 0) x_min_all = 0;
    if (x_max_all >= (int32_t)fb->width) x_max_all = (int32_t)fb->width - 1;

    svga_fifo_update_t upd;
    upd.cmd = SVGA_CMD_UPDATE;
    upd.x   = (uint32_t)x_min_all;
    upd.y   = (uint32_t)y_start;
    upd.w   = (uint32_t)(x_max_all - x_min_all + 1);
    upd.h   = (uint32_t)(y_end - y_start + 1);
    fifo_write(&upd, sizeof(upd));

    return 0;
}

static int fifo_init(void) {
    if (!g_fifo || g_fifo_words < 16) return -1;

    uint32_t min = 16 * 4;
    uint32_t max = g_fifo_words * 4;

    g_fifo[SVGA_FIFO_MIN]      = min;
    g_fifo[SVGA_FIFO_MAX]      = max;
    g_fifo[SVGA_FIFO_NEXT_CMD] = min;
    g_fifo[SVGA_FIFO_STOP]     = min;
    return 0;
}

static int vmsvga_fill_rect32_native(const framebuffer_t *fb,
                                     uint32_t x, uint32_t y,
                                     uint32_t w, uint32_t h,
                                     uint32_t native_pixel) {
    (void)fb;
    if (!g_fifo) return -1;

    svga_fifo_rect_fill_t c;
    c.cmd   = SVGA_CMD_RECT_FILL;
    c.color = native_pixel;
    c.x = x; c.y = y; c.w = w; c.h = h;
    fifo_write(&c, sizeof(c));
    return 0;
}

static int vmsvga_blit_rect32(const framebuffer_t *fb,
                              uint32_t src_x, uint32_t src_y,
                              uint32_t dst_x, uint32_t dst_y,
                              uint32_t w, uint32_t h) {
    (void)fb;
    if (!g_fifo) return -1;

    svga_fifo_rect_copy_t c;
    c.cmd   = SVGA_CMD_RECT_COPY;
    c.src_x = src_x; c.src_y = src_y;
    c.dst_x = dst_x; c.dst_y = dst_y;
    c.w = w; c.h = h;
    fifo_write(&c, sizeof(c));
    return 0;
}

static int vmsvga_blit_from_sg32(const framebuffer_t *fb, const gfx_src_sg_t *src,
                                 uint32_t src_x, uint32_t src_y,
                                 uint32_t dst_x, uint32_t dst_y,
                                 uint32_t w, uint32_t h) {
    if (!fb || !fb->addr || fb->bpp != 32) return -1;
    if (!src || !src->phys_pages || src->page_count == 0) return -1;
    if (w == 0 || h == 0) return 0;

    if (dst_x >= fb->width || dst_y >= fb->height) return 0;
    if (dst_x + w > fb->width)  w = fb->width  - dst_x;
    if (dst_y + h > fb->height) h = fb->height - dst_y;

    uint32_t src_bpp = (src->fmt == GFX_SRC_FMT_XRGB8888) ? 4u :
                       (src->fmt == GFX_SRC_FMT_RGB565)    ? 2u : 0u;
    if (src_bpp == 0 || src->pitch_bytes == 0) return -1;

    uint8_t *dst_base = (uint8_t *)fb->addr;

    for (uint32_t yy = 0; yy < h; yy++) {
        uint64_t src_off = src->base_offset
                         + (uint64_t)(src_y + yy) * src->pitch_bytes
                         + (uint64_t)src_x * src_bpp;
        uint32_t *dst_row = (uint32_t *)(dst_base + (uint64_t)(dst_y + yy) * fb->pitch) + dst_x;

        if (src_bpp == 4) {
            for (uint32_t xx = 0; xx < w; xx++) {
                uint64_t so       = src_off + (uint64_t)xx * 4u;
                uint32_t page     = (uint32_t)(so >> 12);
                uint32_t in_page  = (uint32_t)(so & 0xFFF);
                if (page >= src->page_count) break;
                uint8_t *pg = (uint8_t *)phys_to_virt_kernel(src->phys_pages[page]);
                if (!pg) break;
                dst_row[xx] = *(uint32_t *)(pg + in_page);
            }
        } else {
            for (uint32_t xx = 0; xx < w; xx++) {
                uint64_t so       = src_off + (uint64_t)xx * 2u;
                uint32_t page     = (uint32_t)(so >> 12);
                uint32_t in_page  = (uint32_t)(so & 0xFFF);
                if (page >= src->page_count) break;
                uint8_t *pg = (uint8_t *)phys_to_virt_kernel(src->phys_pages[page]);
                if (!pg) break;
                uint16_t px = *(uint16_t *)(pg + in_page);
                uint32_t r  = ((px >> 11) & 0x1F) * 255u / 31u;
                uint32_t g  = ((px >>  5) & 0x3F) * 255u / 63u;
                uint32_t b  = ( px        & 0x1F) * 255u / 31u;
                dst_row[xx] = (r << 16) | (g << 8) | b;
            }
        }
    }

    if (g_dev.flush) g_dev.flush(fb, dst_x, dst_y, w, h);
    return 0;
}

/* No per-flush FIFO sync — host drains asynchronously. */
static void vmsvga_flush(const framebuffer_t *fb,
                         uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    (void)fb;
    if (!g_fifo) return;

    svga_fifo_update_t u;
    u.cmd = SVGA_CMD_UPDATE;
    u.x = x; u.y = y; u.w = w; u.h = h;
    fifo_write(&u, sizeof(u));
}

static int svga_negotiate_id(void) {
    uint32_t ids[] = { SVGA_ID_2, SVGA_ID_1, SVGA_ID_0 };
    for (int i = 0; i < 3; i++) {
        svga_out(SVGA_REG_ID, ids[i]);
        uint32_t id = svga_in(SVGA_REG_ID);
        if (id == ids[i] || (id >= SVGA_ID_0 && id <= SVGA_ID_2)) return 0;
    }
    return -1;
}

static int set_mode_1024_768_32(void) {
    svga_out(SVGA_REG_ENABLE, 0);
    svga_out(SVGA_REG_WIDTH,  1024);
    svga_out(SVGA_REG_HEIGHT, 768);
    svga_out(SVGA_REG_BITS_PER_PIXEL, 32);
    svga_out(SVGA_REG_DEPTH,  32);
    svga_out(SVGA_REG_ENABLE, 1);
    svga_out(SVGA_REG_CONFIG_DONE, 1);

    uint32_t bpl = svga_in(SVGA_REG_BYTES_PER_LINE);
    if (!bpl) bpl = 1024 * 4;

    g_dev.fb.addr   = (void *)g_fb;
    g_dev.fb.width  = 1024;
    g_dev.fb.height = 768;
    g_dev.fb.pitch  = bpl;
    g_dev.fb.bpp    = 32;
    g_dev.fb.fmt    = FB_FMT_UNKNOWN;
    g_dev.fb.red_pos   = 16; g_dev.fb.red_mask_size   = 8;
    g_dev.fb.green_pos =  8; g_dev.fb.green_mask_size = 8;
    g_dev.fb.blue_pos  =  0; g_dev.fb.blue_mask_size  = 8;
    return 0;
}

static sqrm_module_desc_t sqrm_module_desc = {
    .abi_version = 1,
    .type = SQRM_TYPE_GPU,
    .name = "vmsvga",
};

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    g_api = api;
    if (!api || api->abi_version != 1) return -1;
    if (!api->pci_find_device || !api->outl || !api->inl) return -1;

    g_pci = api->pci_find_device(VMSVGA_VENDOR_VMWARE, VMSVGA_DEVICE_SVGA2);
    if (!g_pci) { com("[VMSVGA] 15ad:0405 not found\n"); return -1; }

    api->pci_enable_io_space(g_pci);
    api->pci_enable_memory_space(g_pci);
    api->pci_enable_bus_mastering(g_pci);

    if (g_pci->bar_type[0] != PCI_BAR_IO) {
        com("[VMSVGA] BAR0 not IO\n"); return -1;
    }
    g_io_base = (uint16_t)(g_pci->bar[0] & ~0x3);

    (void)svga_in(SVGA_REG_CAPABILITIES);

    if (svga_negotiate_id() != 0) {
        com("[VMSVGA] ID negotiation failed\n"); return -1;
    }

    if (g_pci->bar_type[1] == PCI_BAR_IO) {
        com("[VMSVGA] BAR1 not MEM\n"); return -1;
    }

    uint64_t bar1_phys = (uint64_t)(g_pci->bar[1] & ~0xFULL);
    uint64_t bar1_size = (uint64_t)g_pci->bar_size[1];
    if (!bar1_size) bar1_size = 16 * 1024 * 1024;

    void *bar1 = api->ioremap_guarded
               ? api->ioremap_guarded(bar1_phys, bar1_size)
               : api->ioremap(bar1_phys, bar1_size);
    if (!bar1) { com("[VMSVGA] BAR1 map failed\n"); return -1; }

    uint32_t fb_off = svga_in(SVGA_REG_FB_OFFSET);
    if ((uint64_t)fb_off >= bar1_size) {
        com("[VMSVGA] FB_OFFSET out of range\n"); return -1;
    }
    g_fb = (volatile uint32_t *)((uint8_t *)bar1 + fb_off);

    /* FIFO region: prefer MEM_START/MEM_SIZE, fall back to BAR scan. */
    uint32_t mem_start = svga_in(SVGA_REG_MEM_START);
    uint32_t mem_size  = svga_in(SVGA_REG_MEM_SIZE);

    auto int try_fifo(void *mf, uint64_t bytes, const char *tag) {
        if (!mf || bytes < 4096) return -1;
        g_fifo       = (volatile uint32_t *)mf;
        g_fifo_words = (uint32_t)(bytes / 4);
        if (fifo_init() != 0) { g_fifo = 0; g_fifo_words = 0; return -1; }
        com("[VMSVGA] FIFO via "); com(tag); com("\n");
        svga_out(SVGA_REG_CONFIG_DONE, 1);
        return 0;
    }

    if (mem_start && mem_size) {
        void *mf = api->ioremap_guarded
                 ? api->ioremap_guarded((uint64_t)mem_start, (uint64_t)mem_size)
                 : api->ioremap((uint64_t)mem_start, (uint64_t)mem_size);
        try_fifo(mf, mem_size, "MEM_START");
    }

    if (!g_fifo) {
        for (int bi = 0; bi < 6 && !g_fifo; bi++) {
            if (g_pci->bar_type[bi] == PCI_BAR_IO) continue;
            if (!g_pci->bar[bi] || bi == 1) continue;
            uint64_t phys = (uint64_t)(g_pci->bar[bi] & ~0xFULL);
            uint64_t size = (uint64_t)g_pci->bar_size[bi];
            if (!size) continue;
            void *mf = api->ioremap_guarded
                     ? api->ioremap_guarded(phys, size)
                     : api->ioremap(phys, size);
            char tag[6] = { 'B','A','R', (char)('0'+bi), 0 };
            try_fifo(mf, size, tag);
        }
    }

    if (!g_fifo) com("[VMSVGA] No FIFO — flush disabled\n");

    uint32_t caps  = svga_in(SVGA_REG_CAPABILITIES);
    int has_3d     = (caps & 0x04) ? 1 : 0;

    g_dev.flush              = g_fifo ? vmsvga_flush            : NULL;
    g_dev.fill_rect32_native = g_fifo ? vmsvga_fill_rect32_native : NULL;
    g_dev.blit_rect32        = g_fifo ? vmsvga_blit_rect32       : NULL;
    g_dev.blit_from_sg32     = vmsvga_blit_from_sg32;
    g_dev.enumerate_modes    = NULL;
    g_dev.set_mode           = NULL;
    g_dev.shutdown           = NULL;
    g_dev.caps               = g_fifo ? SQRM_GPU_CAP_2D_ACCEL : 0;

    if (has_3d && g_fifo) {
        g_dev.draw_triangle = draw_triangle_hw;
        g_dev.caps |= SQRM_GPU_CAP_3D_TRIANGLES;
    } else {
        g_dev.draw_triangle = NULL;
    }
    g_dev.draw_textured_triangle = NULL;

    if (set_mode_1024_768_32() != 0) {
        com("[VMSVGA] Mode set failed\n"); return -1;
    }

    if (!api->gfx_register_framebuffer) {
        com("[VMSVGA] Missing gfx_register_framebuffer\n"); return -1;
    }

    int rc = api->gfx_register_framebuffer(&g_dev);
    if (rc == 0) {
        /* Clear via a single hardware RECT_FILL — one FIFO command instead of
         * 786432 volatile dword writes through MMIO. */
        if (g_fifo) {
            vmsvga_fill_rect32_native(&g_dev.fb, 0, 0,
                                      g_dev.fb.width, g_dev.fb.height, 0);
            vmsvga_flush(&g_dev.fb, 0, 0, g_dev.fb.width, g_dev.fb.height);
            svga_wait_for_fifo();
        }
        com("[VMSVGA] Ready\n");
    } else {
        com("[VMSVGA] gfx_register_framebuffer failed\n");
    }

    return rc;
}