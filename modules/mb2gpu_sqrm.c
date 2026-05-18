#include "sqrm_sdk.h"
#include "moduos/kernel/multiboot2.h"
#include <stddef.h>

// ====================== Memory Helpers ======================
static void *memset(void *dst, int c, size_t n) {
    unsigned char *p = (unsigned char *)dst;
    while (n--) *p++ = (unsigned char)c;
    return dst;
}

static void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

static void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

// ====================== Global State ======================
static sqrm_gpu_device_t g_gpu_dev;
static const void *g_mb2_root;

static uint32_t *g_cursor_buffer = NULL;
static uint32_t  g_cursor_w = 0, g_cursor_h = 0;
static int32_t   g_cursor_hot_x = 0, g_cursor_hot_y = 0;
static int32_t   g_cursor_x = 0, g_cursor_y = 0;
static int       g_cursor_visible = 0;

// ====================== Helpers ======================
static struct multiboot_tag *find_tag(uint32_t type) {
    return multiboot2_find_tag((void *)g_mb2_root, type);
}

// ====================== Software 2D Acceleration ======================

static int sw_fill_rect32_native(const framebuffer_t *fb, uint32_t x, uint32_t y,
                                 uint32_t w, uint32_t h, uint32_t color)
{
    if (x + w > fb->width || y + h > fb->height) return -1;

    uint32_t *line = (uint32_t*)((uint8_t*)fb->addr + y * fb->pitch + x * 4);

    for (uint32_t yy = 0; yy < h; ++yy) {
        for (uint32_t xx = 0; xx < w; ++xx) {
            line[xx] = color;
        }
        line = (uint32_t*)((uint8_t*)line + fb->pitch);
    }
    return 0;
}

static int sw_blit_rect32(const framebuffer_t *fb,
                          uint32_t src_x, uint32_t src_y,
                          uint32_t dst_x, uint32_t dst_y,
                          uint32_t w, uint32_t h)
{
    if (src_x + w > fb->width || src_y + h > fb->height ||
        dst_x + w > fb->width || dst_y + h > fb->height)
        return -1;

    const uint8_t *src_line = (uint8_t*)fb->addr + src_y * fb->pitch + src_x * 4;
    uint8_t *dst_line       = (uint8_t*)fb->addr + dst_y * fb->pitch + dst_x * 4;

    if ((dst_y > src_y) || (dst_y == src_y && dst_x >= src_x)) {
        // Overlapping - copy backwards
        src_line += (h-1) * fb->pitch;
        dst_line += (h-1) * fb->pitch;
        for (uint32_t i = 0; i < h; ++i) {
            memmove(dst_line, src_line, w * 4);
            src_line -= fb->pitch;
            dst_line -= fb->pitch;
        }
    } else {
        for (uint32_t i = 0; i < h; ++i) {
            memcpy(dst_line, src_line, w * 4);
            src_line += fb->pitch;
            dst_line += fb->pitch;
        }
    }
    return 0;
}

static int sw_blit_from_sg32(const framebuffer_t *fb, const gfx_src_sg_t *src,
                             uint32_t src_x, uint32_t src_y,
                             uint32_t dst_x, uint32_t dst_y,
                             uint32_t w, uint32_t h)
{
    if (!src || src_x + w > src->width || src_y + h > src->height ||
        dst_x + w > fb->width || dst_y + h > fb->height)
        return -1;

    const uint8_t *src_line = (uint8_t*)src->addr + src_y * src->pitch + src_x * 4;
    uint8_t *dst_line = (uint8_t*)fb->addr + dst_y * fb->pitch + dst_x * 4;

    for (uint32_t i = 0; i < h; ++i) {
        memcpy(dst_line, src_line, w * 4);
        src_line += src->pitch;
        dst_line += fb->pitch;
    }
    return 0;
}

// ====================== Proper Flush with Software Cursor ======================

static void sw_flush(const framebuffer_t *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    (void)x; (void)y; (void)w; (void)h; // We can ignore partial for simplicity, or optimize later

    if (!g_cursor_visible || !g_cursor_buffer) return;

    // Composite software cursor
    int32_t cx = g_cursor_x - g_cursor_hot_x;
    int32_t cy = g_cursor_y - g_cursor_hot_y;

    int32_t start_x = cx < 0 ? 0 : cx;
    int32_t start_y = cy < 0 ? 0 : cy;
    int32_t end_x = (cx + (int32_t)g_cursor_w) > (int32_t)fb->width ? fb->width : cx + g_cursor_w;
    int32_t end_y = (cy + (int32_t)g_cursor_h) > (int32_t)fb->height ? fb->height : cy + g_cursor_h;

    if (start_x >= (int32_t)fb->width || start_y >= (int32_t)fb->height) return;

    for (int32_t yy = start_y; yy < end_y; ++yy) {
        uint32_t *dst = (uint32_t*)((uint8_t*)fb->addr + yy * fb->pitch + start_x * 4);
        const uint32_t *src = g_cursor_buffer + (yy - cy) * g_cursor_w + (start_x - cx);

        for (int32_t xx = start_x; xx < end_x; ++xx) {
            uint32_t pixel = *src++;
            if (pixel & 0xFF000000) {           // Alpha test (non-transparent)
                *dst = pixel;                   // Simple replace (you can do proper blending)
            }
            dst++;
        }
    }
}

// ====================== Software Triangle Rasterizer (Optional but included) ======================

static int sw_draw_triangle(const framebuffer_t *fb,
                            int32_t x0, int32_t y0, uint32_t c0,
                            int32_t x1, int32_t y1, uint32_t c1,
                            int32_t x2, int32_t y2, uint32_t c2)
{
    int32_t minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int32_t miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int32_t maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int32_t maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);

    minx = minx < 0 ? 0 : minx;
    miny = miny < 0 ? 0 : miny;
    maxx = maxx > (int32_t)fb->width  - 1 ? (int32_t)fb->width  - 1 : maxx;
    maxy = maxy > (int32_t)fb->height - 1 ? (int32_t)fb->height - 1 : maxy;

    // Integer cross product for edge function
    // area2 = (x1-x0)*(y2-y0) - (x2-x0)*(y1-y0)  [2x triangle area, signed]
    int32_t area2 = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (area2 == 0) return 0;

    for (int32_t y = miny; y <= maxy; ++y) {
        for (int32_t x = minx; x <= maxx; ++x) {
            // Edge functions (all integer)
            int32_t w0 = (x1 - x0) * (y - y0) - (x  - x0) * (y1 - y0);
            int32_t w1 = (x2 - x1) * (y - y1) - (x  - x1) * (y2 - y1);
            int32_t w2 = (x0 - x2) * (y - y2) - (x  - x2) * (y0 - y2);

            // Flip for CW winding
            if (area2 < 0) { w0 = -w0; w1 = -w1; w2 = -w2; area2 = -area2; }

            if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
                // Interpolate color channels with integer weights
                uint32_t r = ((uint32_t)w0 * ((c0 >> 16) & 0xFF) +
                              (uint32_t)w1 * ((c1 >> 16) & 0xFF) +
                              (uint32_t)w2 * ((c2 >> 16) & 0xFF)) / (uint32_t)area2;
                uint32_t g = ((uint32_t)w0 * ((c0 >>  8) & 0xFF) +
                              (uint32_t)w1 * ((c1 >>  8) & 0xFF) +
                              (uint32_t)w2 * ((c2 >>  8) & 0xFF)) / (uint32_t)area2;
                uint32_t b = ((uint32_t)w0 * (c0 & 0xFF) +
                              (uint32_t)w1 * (c1 & 0xFF) +
                              (uint32_t)w2 * (c2 & 0xFF)) / (uint32_t)area2;

                *((uint32_t*)((uint8_t*)fb->addr + y * fb->pitch + x * 4)) =
                    0xFF000000 | (r << 16) | (g << 8) | b;
            }
        }
    }
    return 0;
}

// ====================== Cursor ======================

static int sw_cursor_set_argb32(uint32_t w, uint32_t h, int32_t hot_x, int32_t hot_y,
                                const uint32_t *pixels_argb)
{
    if (w > 256 || h > 256) return -1;
    g_cursor_w = w;
    g_cursor_h = h;
    g_cursor_hot_x = hot_x;
    g_cursor_hot_y = hot_y;
    g_cursor_buffer = (uint32_t*)pixels_argb;   // Pointer to user data (caller must keep alive)
    return 0;
}

static int sw_cursor_move(int32_t x, int32_t y) {
    g_cursor_x = x;
    g_cursor_y = y;
    return 0;
}

static int sw_cursor_show(int visible) {
    g_cursor_visible = visible;
    return 0;
}

// ====================== Module Entry ======================

SQRM_DEFINE_MODULE(SQRM_TYPE_GPU, "mb2gpu");

int sqrm_module_init(const sqrm_kernel_api_t *api)
{
    g_mb2_root = api->multiboot2_header;
    if (!g_mb2_root) return -1;

    struct multiboot_tag_framebuffer *fb_tag =
        (struct multiboot_tag_framebuffer *)find_tag(MULTIBOOT_TAG_TYPE_FRAMEBUFFER);

    if (!fb_tag || fb_tag->framebuffer_type != MULTIBOOT_FRAMEBUFFER_TYPE_RGB ||
        fb_tag->framebuffer_bpp != 32)
        return -1;

    uint64_t phys = fb_tag->framebuffer_addr;
    uint32_t size = fb_tag->framebuffer_pitch * fb_tag->framebuffer_height;

    void *virt = api->ioremap_guarded(phys, size);
    if (!virt) virt = api->ioremap(phys, size);
    if (!virt) return -1;

    // Setup framebuffer
    g_gpu_dev.fb.addr           = virt;
    g_gpu_dev.fb.phys_addr      = phys;
    g_gpu_dev.fb.size_bytes     = size;
    g_gpu_dev.fb.width          = fb_tag->framebuffer_width;
    g_gpu_dev.fb.height         = fb_tag->framebuffer_height;
    g_gpu_dev.fb.pitch          = fb_tag->framebuffer_pitch;
    g_gpu_dev.fb.bpp            = 32;
    g_gpu_dev.fb.fmt            = FB_FMT_XRGB8888;

    g_gpu_dev.fb.red_pos        = fb_tag->framebuffer_red_field_position;
    g_gpu_dev.fb.red_mask_size  = fb_tag->framebuffer_red_mask_size;
    g_gpu_dev.fb.green_pos      = fb_tag->framebuffer_green_field_position;
    g_gpu_dev.fb.green_mask_size= fb_tag->framebuffer_green_mask_size;
    g_gpu_dev.fb.blue_pos       = fb_tag->framebuffer_blue_field_position;
    g_gpu_dev.fb.blue_mask_size = fb_tag->framebuffer_blue_mask_size;

    // Hook software functions
    g_gpu_dev.flush                  = sw_flush;

    g_gpu_dev.fill_rect32_native     = sw_fill_rect32_native;
    g_gpu_dev.blit_rect32            = sw_blit_rect32;
    g_gpu_dev.blit_from_sg32         = sw_blit_from_sg32;

    g_gpu_dev.cursor_set_argb32      = sw_cursor_set_argb32;
    g_gpu_dev.cursor_move            = sw_cursor_move;
    g_gpu_dev.cursor_show            = sw_cursor_show;

    g_gpu_dev.draw_triangle          = sw_draw_triangle;

    g_gpu_dev.caps = SQRM_GPU_CAP_2D_ACCEL | SQRM_GPU_CAP_HW_CURSOR | SQRM_GPU_CAP_3D_TRIANGLES;

    return api->gfx_register_framebuffer(&g_gpu_dev);
}