// GPU capability syscalls
#include "moduos/kernel/syscall/gfx_syscall.h"
#include "moduos/kernel/gfx.h"
#include "moduos/kernel/sqrm.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/drivers/graphics/VGA.h"

uint32_t sys_gfx_get_caps(void) {
    return gfx_get_gpu_caps();
}

int sys_gfx_get_info(gfx_info_t *out) {
    if (!out) return -1;

    memset(out, 0, sizeof(*out));

    const char *driver = gfx_get_sqrm_gpu_driver_name();
    if (driver) {
        size_t i;
        for (i = 0; i < 63 && driver[i]; i++) {
            out->driver_name[i] = driver[i];
        }
        out->driver_name[i] = 0;
    }

    out->caps = gfx_get_gpu_caps();

    const sqrm_gpu_device_t *gpu = gfx_get_sqrm_gpu_device();
    if (gpu) {
        out->width = gpu->fb.width;
        out->height = gpu->fb.height;
        out->bpp = gpu->fb.bpp;
    }

    return 0;
}

/* Software 3D triangle rasterizer (Bresenham-based scanline) */
static inline int tri_edge(int ax, int ay, int bx, int by, int cx, int cy) {
    return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax);
}

static void draw_triangle_software(const framebuffer_t *fb,
                                  int32_t x0, int32_t y0, uint32_t color0,
                                  int32_t x1, int32_t y1, uint32_t color1,
                                  int32_t x2, int32_t y2, uint32_t color2) {
    if (!fb || !fb->addr || fb->bpp != 32) return;

    /* Bounding box */
    int32_t min_x = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int32_t max_x = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int32_t min_y = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int32_t max_y = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);

    /* Clamp to screen */
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= (int32_t)fb->width) max_x = (int32_t)fb->width - 1;
    if (max_y >= (int32_t)fb->height) max_y = (int32_t)fb->height - 1;

    /* For now, use flat shading (use color0) */
    (void)color1;
    (void)color2;
    uint32_t color = color0;

    uint8_t *base = (uint8_t*)fb->addr;

    /* Scanline rasterization with edge functions */
    for (int32_t y = min_y; y <= max_y; y++) {
        uint32_t *row = (uint32_t*)(base + (uint64_t)y * fb->pitch);
        for (int32_t x = min_x; x <= max_x; x++) {
            /* Check if point is inside triangle using edge functions */
            int e0 = tri_edge(x1, y1, x2, y2, x, y);
            int e1 = tri_edge(x2, y2, x0, y0, x, y);
            int e2 = tri_edge(x0, y0, x1, y1, x, y);

            /* All edges must have same sign for point to be inside */
            if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || 
                (e0 <= 0 && e1 <= 0 && e2 <= 0)) {
                row[x] = color;
            }
        }
    }
}

int sys_gfx_draw_triangle(int32_t x0, int32_t y0, uint32_t color0,
                          int32_t x1, int32_t y1, uint32_t color1,
                          int32_t x2, int32_t y2, uint32_t color2) {
    const sqrm_gpu_device_t *gpu = gfx_get_sqrm_gpu_device();
    if (!gpu) return -1;

    /* Try hardware acceleration first */
    if (gpu->draw_triangle && (gpu->caps & SQRM_GPU_CAP_3D_TRIANGLES)) {
        int rc = gpu->draw_triangle(&gpu->fb, x0, y0, color0, x1, y1, color1, x2, y2, color2);
        if (rc == 0) {
            /* Flush the triangle to screen */
            if (gpu->flush) {
                /* Compute bounding box for dirty rect */
                int32_t min_x = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
                int32_t max_x = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
                int32_t min_y = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
                int32_t max_y = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
                
                if (min_x < 0) min_x = 0;
                if (min_y < 0) min_y = 0;
                if (max_x >= (int32_t)gpu->fb.width) max_x = gpu->fb.width - 1;
                if (max_y >= (int32_t)gpu->fb.height) max_y = gpu->fb.height - 1;
                
                uint32_t w = (uint32_t)(max_x - min_x + 1);
                uint32_t h = (uint32_t)(max_y - min_y + 1);
                gpu->flush(&gpu->fb, (uint32_t)min_x, (uint32_t)min_y, w, h);
            }
            return 0;
        }
    }

    /* Software fallback */
    draw_triangle_software(&gpu->fb, x0, y0, color0, x1, y1, color1, x2, y2, color2);
    
    /* Flush */
    if (gpu->flush) {
        int32_t min_x = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
        int32_t max_x = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
        int32_t min_y = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
        int32_t max_y = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
        
        if (min_x < 0) min_x = 0;
        if (min_y < 0) min_y = 0;
        if (max_x >= (int32_t)gpu->fb.width) max_x = gpu->fb.width - 1;
        if (max_y >= (int32_t)gpu->fb.height) max_y = gpu->fb.height - 1;
        
        uint32_t w = (uint32_t)(max_x - min_x + 1);
        uint32_t h = (uint32_t)(max_y - min_y + 1);
        gpu->flush(&gpu->fb, (uint32_t)min_x, (uint32_t)min_y, w, h);
    }

    return 0;
}
