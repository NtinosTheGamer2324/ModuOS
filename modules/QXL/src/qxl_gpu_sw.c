// SPDX-License-Identifier: GPL-2.0-only
//
// ModuOS SQRM QXL GPU module — software (CPU-side) rendering fallbacks.
// Drop this file into the same build unit as qxl_gpu.c, or #include it at
// the bottom of qxl_gpu.c just before sqrm_module_init().
// dirty rectangles to the host.

#include <stdint.h>
#include <stddef.h>
#include "../../sqrm_sdk.h"
#include "qxl_gpu_sw.h"

// ---------------------------------------------------------------------------
// Forward declarations for things defined in qxl_gpu.c that we need here.
// ---------------------------------------------------------------------------
extern framebuffer_t       g_fb;
extern void                qxl_flush(const framebuffer_t *fb,
                                      uint32_t x, uint32_t y,
                                      uint32_t w, uint32_t h);
extern int                 qxl_enumerate_modes(gfx_mode_t *out_modes,
                                                uint32_t    max_modes);

// Memory Functions
static void *memset(void *dst, int c, size_t n) {
    unsigned char *p = (unsigned char *)dst;
    unsigned char  v = (unsigned char)c;
    while (n--) *p++ = v;
    return dst;
}

static void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

static void *memmove(void *dst, const void *src, size_t n) {
    unsigned char       *d = (unsigned char *)dst;
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

static int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

static void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    unsigned char v = (unsigned char)c;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == v) return (void *)(p + i);
    }
    return (void *)0;
}



// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static inline uint32_t sw_alpha_blend(uint32_t dst_xrgb, uint32_t src_argb) {
    uint8_t a  = (uint8_t)(src_argb >> 24);
    if (a == 0)   return dst_xrgb;
    if (a == 255) return src_argb & 0x00FFFFFFu;

    uint8_t sr = (uint8_t)(src_argb >> 16);
    uint8_t sg = (uint8_t)(src_argb >>  8);
    uint8_t sb = (uint8_t)(src_argb >>  0);

    uint8_t dr = (uint8_t)(dst_xrgb >> 16);
    uint8_t dg = (uint8_t)(dst_xrgb >>  8);
    uint8_t db = (uint8_t)(dst_xrgb >>  0);

    uint8_t or_ = (uint8_t)((a * sr + (255 - a) * dr) / 255);
    uint8_t og  = (uint8_t)((a * sg + (255 - a) * dg) / 255);
    uint8_t ob  = (uint8_t)((a * sb + (255 - a) * db) / 255);

    return ((uint32_t)or_ << 16) | ((uint32_t)og << 8) | (uint32_t)ob;
}

// ---------------------------------------------------------------------------
// sw_fill_rect32_native
//
// Fills a rectangle with a pre-converted native pixel value (xRGB32) directly
// into the framebuffer, then flushes the dirty rect to the QXL host.
// ---------------------------------------------------------------------------
int sw_fill_rect32_native(const framebuffer_t *fb,
                                  uint32_t x, uint32_t y,
                                  uint32_t w, uint32_t h,
                                  uint32_t native_pixel)
{
    if (!fb || !fb->addr || fb->bpp != 32) return -1;
    if (w == 0 || h == 0)               return 0;
    if (x >= fb->width || y >= fb->height) return 0;

    // Clamp to surface bounds.
    if (x + w > fb->width)  w = fb->width  - x;
    if (y + h > fb->height) h = fb->height - y;

    uint8_t *base = (uint8_t *)fb->addr;

    for (uint32_t yy = 0; yy < h; yy++) {
        uint32_t *row = (uint32_t *)(base + (uint64_t)(y + yy) * fb->pitch);
        for (uint32_t xx = 0; xx < w; xx++) {
            row[x + xx] = native_pixel;
        }
    }

    qxl_flush(fb, x, y, w, h);
    return 0;
}

// ---------------------------------------------------------------------------
// sw_blit_rect32
//
// Copies a source rectangle to a destination rectangle within the same
// framebuffer (memmove semantics — handles overlapping regions correctly),
// then flushes the destination dirty rect.
// ---------------------------------------------------------------------------
int sw_blit_rect32(const framebuffer_t *fb,
                           uint32_t src_x, uint32_t src_y,
                           uint32_t dst_x, uint32_t dst_y,
                           uint32_t w, uint32_t h)
{
    if (!fb || !fb->addr || fb->bpp != 32) return -1;
    if (w == 0 || h == 0)                 return 0;

    // Clamp both regions to surface bounds.
    if (src_x >= fb->width  || src_y >= fb->height) return 0;
    if (dst_x >= fb->width  || dst_y >= fb->height) return 0;

    if (src_x + w > fb->width)  w = fb->width  - src_x;
    if (dst_x + w > fb->width)  w = fb->width  - dst_x;
    if (src_y + h > fb->height) h = fb->height - src_y;
    if (dst_y + h > fb->height) h = fb->height - dst_y;

    uint8_t  *base      = (uint8_t *)fb->addr;
    uint32_t  row_bytes = w * 4u;

    // Choose scan order to handle vertical overlap correctly.
    int forward = (dst_y < src_y) || (dst_y == src_y && dst_x <= src_x);

    if (forward) {
        for (uint32_t yy = 0; yy < h; yy++) {
            uint8_t *src_row = base + (uint64_t)(src_y + yy) * fb->pitch + (uint64_t)src_x * 4u;
            uint8_t *dst_row = base + (uint64_t)(dst_y + yy) * fb->pitch + (uint64_t)dst_x * 4u;
            memmove(dst_row, src_row, row_bytes);
        }
    } else {
        for (uint32_t yy = h; yy > 0; yy--) {
            uint32_t  y_      = yy - 1;
            uint8_t *src_row = base + (uint64_t)(src_y + y_) * fb->pitch + (uint64_t)src_x * 4u;
            uint8_t *dst_row = base + (uint64_t)(dst_y + y_) * fb->pitch + (uint64_t)dst_x * 4u;
            memmove(dst_row, src_row, row_bytes);
        }
    }

    qxl_flush(fb, dst_x, dst_y, w, h);
    return 0;
}

// ---------------------------------------------------------------------------
// Software cursor state
//
// We composite a cursor sprite into the framebuffer and restore the saved
// background when the cursor moves or is hidden.  Maximum sprite size: 64×64.
// ---------------------------------------------------------------------------
#define SW_CUR_MAX 64

static struct {
    // Sprite (ARGB32, source pixels).
    uint32_t  pixels[SW_CUR_MAX * SW_CUR_MAX];
    uint32_t  w, h;
    int32_t   hot_x, hot_y;

    // Where the cursor is currently drawn on-screen (top-left of sprite bbox).
    int32_t   drawn_x, drawn_y;
    uint32_t  drawn_w, drawn_h;   // clipped sprite dimensions actually drawn

    // Saved framebuffer background (xRGB32).
    uint32_t  bg[SW_CUR_MAX * SW_CUR_MAX];

    // Logical cursor position (hot-spot).
    int32_t   pos_x, pos_y;

    int       has_sprite;   // sprite is loaded
    int       visible;      // cursor should be shown
    int       on_screen;    // background is currently saved / cursor is composited
} sw_cur;

// Restore the saved background (erase the cursor from the framebuffer).
static void sw_cur_erase(void) {
    if (!sw_cur.on_screen || !g_fb.addr) return;

    int32_t  dx = sw_cur.drawn_x;
    int32_t  dy = sw_cur.drawn_y;
    uint32_t dw = sw_cur.drawn_w;
    uint32_t dh = sw_cur.drawn_h;

    uint8_t *base = (uint8_t *)g_fb.addr;
    for (uint32_t yy = 0; yy < dh; yy++) {
        uint32_t *dst = (uint32_t *)(base + (uint64_t)(dy + yy) * g_fb.pitch);
        const uint32_t *src = &sw_cur.bg[yy * SW_CUR_MAX];
        for (uint32_t xx = 0; xx < dw; xx++) {
            dst[dx + xx] = src[xx];
        }
    }

    qxl_flush(&g_fb, (uint32_t)dx, (uint32_t)dy, dw, dh);
    sw_cur.on_screen = 0;
}

// Save background and composite the sprite at the current logical position.
static void sw_cur_draw(void) {
    if (!sw_cur.has_sprite || !sw_cur.visible || !g_fb.addr) return;

    // Top-left corner of the sprite bbox on screen.
    int32_t sx = sw_cur.pos_x - sw_cur.hot_x;
    int32_t sy = sw_cur.pos_y - sw_cur.hot_y;

    // Source rect within sprite (handle off-screen clipping).
    int32_t  sprite_x0 = 0, sprite_y0 = 0;
    uint32_t draw_w = sw_cur.w, draw_h = sw_cur.h;

    if (sx < 0) { sprite_x0 = -sx; draw_w = (draw_w > (uint32_t)(-sx)) ? draw_w - (uint32_t)(-sx) : 0; sx = 0; }
    if (sy < 0) { sprite_y0 = -sy; draw_h = (draw_h > (uint32_t)(-sy)) ? draw_h - (uint32_t)(-sy) : 0; sy = 0; }

    if ((uint32_t)sx + draw_w > g_fb.width)  draw_w = (g_fb.width  > (uint32_t)sx) ? g_fb.width  - (uint32_t)sx : 0;
    if ((uint32_t)sy + draw_h > g_fb.height) draw_h = (g_fb.height > (uint32_t)sy) ? g_fb.height - (uint32_t)sy : 0;

    if (draw_w == 0 || draw_h == 0) return;

    uint8_t *base = (uint8_t *)g_fb.addr;

    // Save background.
    for (uint32_t yy = 0; yy < draw_h; yy++) {
        const uint32_t *src = (const uint32_t *)(base + (uint64_t)(sy + yy) * g_fb.pitch);
        uint32_t       *dst = &sw_cur.bg[yy * SW_CUR_MAX];
        for (uint32_t xx = 0; xx < draw_w; xx++) {
            dst[xx] = src[sx + xx];
        }
    }

    // Composite sprite (alpha-blend ARGB over saved background).
    for (uint32_t yy = 0; yy < draw_h; yy++) {
        uint32_t *dst_row = (uint32_t *)(base + (uint64_t)(sy + yy) * g_fb.pitch);
        for (uint32_t xx = 0; xx < draw_w; xx++) {
            uint32_t src_argb = sw_cur.pixels[(sprite_y0 + yy) * SW_CUR_MAX + (sprite_x0 + xx)];
            dst_row[sx + xx]  = sw_alpha_blend(dst_row[sx + xx], src_argb);
        }
    }

    sw_cur.drawn_x  = sx;
    sw_cur.drawn_y  = sy;
    sw_cur.drawn_w  = draw_w;
    sw_cur.drawn_h  = draw_h;
    sw_cur.on_screen = 1;

    qxl_flush(&g_fb, (uint32_t)sx, (uint32_t)sy, draw_w, draw_h);
}

// ---------------------------------------------------------------------------
// sw_cursor_set_argb32
// ---------------------------------------------------------------------------
int sw_cursor_set_argb32(uint32_t w, uint32_t h,
                                 int32_t hot_x, int32_t hot_y,
                                 const uint32_t *pixels_argb)
{
    if (!pixels_argb)          return -1;
    if (w == 0 || h == 0)     return -2;
    if (w > SW_CUR_MAX || h > SW_CUR_MAX) return -3;

    // Erase old cursor from screen before changing sprite.
    sw_cur_erase();

    sw_cur.w     = w;
    sw_cur.h     = h;
    sw_cur.hot_x = (hot_x < 0) ? 0 : hot_x;
    sw_cur.hot_y = (hot_y < 0) ? 0 : hot_y;

    // Copy pixels; pad unused rows/cols to transparent.
    memset(sw_cur.pixels, 0, sizeof(sw_cur.pixels));
    for (uint32_t yy = 0; yy < h; yy++) {
        memcpy(&sw_cur.pixels[yy * SW_CUR_MAX],
               &pixels_argb[yy * w],
               w * sizeof(uint32_t));
    }

    sw_cur.has_sprite = 1;
    sw_cur_draw();
    return 0;
}

// ---------------------------------------------------------------------------
// sw_cursor_move
// ---------------------------------------------------------------------------
int sw_cursor_move(int32_t x, int32_t y) {
    sw_cur_erase();
    sw_cur.pos_x = x;
    sw_cur.pos_y = y;
    sw_cur_draw();
    return 0;
}

// ---------------------------------------------------------------------------
// sw_cursor_show
// ---------------------------------------------------------------------------
int sw_cursor_show(int visible) {
    if (visible && !sw_cur.visible) {
        sw_cur.visible = 1;
        sw_cur_draw();
    } else if (!visible && sw_cur.visible) {
        sw_cur_erase();
        sw_cur.visible = 0;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// sw_set_mode
//
// Full mode switching requires tearing down and recreating the QXL primary
// surface, which belongs in the hardware path.  Return -1 to signal
// "not supported by software path" so the caller can fall back.
// ---------------------------------------------------------------------------
int sw_set_mode(uint32_t width, uint32_t height, uint32_t bpp) {
    (void)width; (void)height; (void)bpp;
    return -1; // Requires hardware; not implemented in SW path.
}
// ---------------------------------------------------------------------------
// sw_blit_buffer
//
// Copies an arbitrary external pixel buffer (src_pixels, src_pitch) into the
// framebuffer at (dst_x, dst_y) for a region of (w x h) pixels, then flushes
// the dirty rectangle via qxl_flush().
//
// src_pixels must already point at the first pixel to copy (i.e. src_x/src_y
// offsets have been applied by the caller — MVC3 does this before the call).
// src_pitch is the byte stride of the source buffer (may be wider than w*bpp).
// ---------------------------------------------------------------------------
int sw_blit_buffer(const framebuffer_t *fb,
                   uint32_t dst_x, uint32_t dst_y,
                   const void *src_pixels, uint32_t src_pitch,
                   uint32_t w, uint32_t h)
{
    if (!fb || !fb->addr || !src_pixels) return -1;
    if (w == 0 || h == 0)               return 0;

    // Clamp to framebuffer bounds.
    if (dst_x >= fb->width  || dst_y >= fb->height) return 0;
    if (dst_x + w > fb->width)  w = fb->width  - dst_x;
    if (dst_y + h > fb->height) h = fb->height - dst_y;

    uint32_t bpp_bytes = (fb->bpp + 7u) / 8u;
    uint32_t row_bytes = w * bpp_bytes;

    const uint8_t *src_row = (const uint8_t *)src_pixels;
          uint8_t *dst_row = (uint8_t *)fb->addr
                             + (uint64_t)dst_y * fb->pitch
                             + (uint64_t)dst_x * bpp_bytes;

    for (uint32_t y = 0; y < h; y++) {
        memcpy(dst_row, src_row, row_bytes);
        src_row += src_pitch;
        dst_row += fb->pitch;
    }

    qxl_flush(fb, dst_x, dst_y, w, h);
    return 0;
}