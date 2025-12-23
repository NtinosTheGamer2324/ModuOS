#include "moduos/drivers/graphics/fb_console.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/memory/paging.h"

/* Debug logging in fb console can stress stack/formatters; keep it off by default. */
#ifndef FBCON_DEBUG
#define FBCON_DEBUG 0
#endif

/* Forward declarations (used before their definitions) */
static void fbcon_cursor_hide(fb_console_t *c);
static void fbcon_cursor_show(fb_console_t *c);

static uint32_t fb_pack_rgb888(const framebuffer_t *fb, uint8_t r, uint8_t g, uint8_t b) {
    if (fb->red_mask_size && fb->green_mask_size && fb->blue_mask_size) {
        uint32_t rp = fb->red_pos;
        uint32_t gp = fb->green_pos;
        uint32_t bp = fb->blue_pos;

        uint32_t rm = (fb->red_mask_size >= 32) ? 0xFFFFFFFFu : ((1u << fb->red_mask_size) - 1u);
        uint32_t gm = (fb->green_mask_size >= 32) ? 0xFFFFFFFFu : ((1u << fb->green_mask_size) - 1u);
        uint32_t bm = (fb->blue_mask_size >= 32) ? 0xFFFFFFFFu : ((1u << fb->blue_mask_size) - 1u);

        uint32_t rv = ((uint32_t)r * rm) / 255u;
        uint32_t gv = ((uint32_t)g * gm) / 255u;
        uint32_t bv = ((uint32_t)b * bm) / 255u;

        return (rv << rp) | (gv << gp) | (bv << bp);
    }

    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void vga16_to_rgb(uint8_t idx, uint8_t *r, uint8_t *g, uint8_t *b) {
    /* Standard VGA 16-color palette approximation */
    static const uint8_t pal[16][3] = {
        {0x00,0x00,0x00}, {0x00,0x00,0xAA}, {0x00,0xAA,0x00}, {0x00,0xAA,0xAA},
        {0xAA,0x00,0x00}, {0xAA,0x00,0xAA}, {0xAA,0x55,0x00}, {0xAA,0xAA,0xAA},
        {0x55,0x55,0x55}, {0x55,0x55,0xFF}, {0x55,0xFF,0x55}, {0x55,0xFF,0xFF},
        {0xFF,0x55,0x55}, {0xFF,0x55,0xFF}, {0xFF,0xFF,0x55}, {0xFF,0xFF,0xFF},
    };
    idx &= 0x0F;
    *r = pal[idx][0];
    *g = pal[idx][1];
    *b = pal[idx][2];
}

static void fb_put_pixel(const framebuffer_t *fb, uint32_t x, uint32_t y, uint32_t px) {
    if (!fb->addr) return;
    if (x >= fb->width || y >= fb->height) return;
    uint8_t *base = (uint8_t*)fb->addr + (uint64_t)y * fb->pitch;

    if (fb->bpp == 32) {
        ((uint32_t*)base)[x] = px;
    } else if (fb->bpp == 24) {
        uint8_t *p = base + x * 3;
        p[0] = (uint8_t)(px & 0xFF);
        p[1] = (uint8_t)((px >> 8) & 0xFF);
        p[2] = (uint8_t)((px >> 16) & 0xFF);
    } else if (fb->bpp == 16) {
        ((uint16_t*)base)[x] = (uint16_t)px;
    }
}

static void fb_fill_rect(const framebuffer_t *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t px) {
    if (!fb->addr) return;
    if (x >= fb->width || y >= fb->height) return;

    if (x + w > fb->width) w = fb->width - x;
    if (y + h > fb->height) h = fb->height - y;

    for (uint32_t yy = 0; yy < h; yy++) {
        for (uint32_t xx = 0; xx < w; xx++) {
            fb_put_pixel(fb, x + xx, y + yy, px);
        }
    }
}

static void fb_scroll_up(fb_console_t *c) {
    /* Scroll by one text row (cell_h pixels) */
    uint32_t dy = c->cell_h;
    if (!c || !c->fb.addr || c->fb.pitch == 0) return;

    /* Debug: log scroll parameters (COM only, no VGA recursion). */
#if FBCON_DEBUG
    com_printf(COM1_PORT,
               "[FBCON] fb_scroll_up: fb.addr=0x%08x%08x pitch=%u height=%u dy=%u\n",
               (uint32_t)(((uint64_t)(uintptr_t)c->fb.addr) >> 32),
               (uint32_t)(((uint64_t)(uintptr_t)c->fb.addr) & 0xFFFFFFFFu),
               (unsigned)c->fb.pitch,
               (unsigned)c->fb.height,
               (unsigned)dy);
#endif

    /* Heuristic: warn if framebuffer lives in heap range (should usually be ioremap/MMIO). */
    if (((uint64_t)(uintptr_t)c->fb.addr) >= 0xFFFF800000000000ULL &&
        ((uint64_t)(uintptr_t)c->fb.addr) <  0xFFFF900000000000ULL) {
        com_write_string(COM1_PORT, "[FBCON] WARNING: fb.addr is inside KHEAP high-half range\n");
    }

    /* Ensure cursor isn't baked into the scrolled pixels */
    fbcon_cursor_hide(c);

    if (dy == 0 || dy >= c->fb.height) {
        c->x = c->y = 0;
        fbcon_clear(c);
        return;
    }

    uint8_t *p = (uint8_t*)c->fb.addr;
    uint64_t row_bytes = c->fb.pitch;

    /* Defensive: compute move size from the actually-mapped framebuffer byte size.
     * This avoids any accidental overrun if height/pitch are inconsistent.
     */
    uint64_t fb_size = (uint64_t)c->fb.height * row_bytes;
    uint64_t src_off = (uint64_t)dy * row_bytes;
    if (src_off >= fb_size) {
        fbcon_clear(c);
        return;
    }
    uint64_t move_bytes = fb_size - src_off;

#if FBCON_DEBUG
    com_printf(COM1_PORT,
               "[FBCON] fb_scroll_up: fb_size=%u src_off=%u move_bytes=%u\n",
               (uint32_t)fb_size, (uint32_t)src_off, (uint32_t)move_bytes);
#endif

    /* Extra mapping sanity: ensure framebuffer pages are mapped */
    uint64_t p_phys0 = paging_virt_to_phys((uint64_t)(uintptr_t)p);
    uint64_t p_phys1 = paging_virt_to_phys((uint64_t)(uintptr_t)(p + fb_size - 1));
    if (p_phys0 == 0 || p_phys1 == 0) {
        com_write_string(COM1_PORT, "[FBCON] FATAL: framebuffer not fully mapped\n");
        com_printf(COM1_PORT, "[FBCON]   p=0x%08x%08x p_phys0=0x%08x%08x p_phys_last=0x%08x%08x\n",
                   (uint32_t)(((uint64_t)(uintptr_t)p) >> 32), (uint32_t)(((uint64_t)(uintptr_t)p) & 0xFFFFFFFFu),
                   (uint32_t)(p_phys0 >> 32), (uint32_t)(p_phys0 & 0xFFFFFFFFu),
                   (uint32_t)(p_phys1 >> 32), (uint32_t)(p_phys1 & 0xFFFFFFFFu));
        for (;;) { __asm__ volatile("cli; hlt"); }
    }

    /* HARD BOUNDS CHECK (debug safety): ensure memmove stays within framebuffer */
    uintptr_t fb_start = (uintptr_t)p;
    uintptr_t fb_end   = fb_start + (uintptr_t)fb_size;

    uint8_t *dst = p;
    uint8_t *src = p + src_off;
#if FBCON_DEBUG
    com_printf(COM1_PORT, "[FBCON] memmove dst=0x%08x%08x src=0x%08x%08x\n",
               (uint32_t)(((uint64_t)(uintptr_t)dst) >> 32), (uint32_t)(((uint64_t)(uintptr_t)dst) & 0xFFFFFFFFu),
               (uint32_t)(((uint64_t)(uintptr_t)src) >> 32), (uint32_t)(((uint64_t)(uintptr_t)src) & 0xFFFFFFFFu));
#endif
    uintptr_t src_end = (uintptr_t)src + (uintptr_t)move_bytes;
    uintptr_t dst_end = (uintptr_t)dst + (uintptr_t)move_bytes;

    if (src_end > fb_end || dst_end > fb_end) {
        com_write_string(COM1_PORT, "[FBCON] FATAL: fb_scroll_up overflow prevented\n");
        com_printf(COM1_PORT, "[FBCON]   fb_start=0x%08x%08x fb_end=0x%08x%08x\n",
                   (uint32_t)(fb_start >> 32), (uint32_t)(fb_start & 0xFFFFFFFFu),
                   (uint32_t)(fb_end >> 32),   (uint32_t)(fb_end & 0xFFFFFFFFu));
        com_printf(COM1_PORT, "[FBCON]   src=0x%08x%08x dst=0x%08x%08x len=%u\n",
                   (uint32_t)(((uint64_t)(uintptr_t)src) >> 32), (uint32_t)(((uint64_t)(uintptr_t)src) & 0xFFFFFFFFu),
                   (uint32_t)(((uint64_t)(uintptr_t)dst) >> 32), (uint32_t)(((uint64_t)(uintptr_t)dst) & 0xFFFFFFFFu),
                   (uint32_t)move_bytes);
        com_printf(COM1_PORT, "[FBCON]   src_end=0x%08x%08x dst_end=0x%08x%08x\n",
                   (uint32_t)(src_end >> 32), (uint32_t)(src_end & 0xFFFFFFFFu),
                   (uint32_t)(dst_end >> 32), (uint32_t)(dst_end & 0xFFFFFFFFu));
        for (;;) { __asm__ volatile("cli; hlt"); }
    }

    memmove(dst, src, move_bytes);

    /* Clear bottom area */
    uint8_t r,g,b;
    vga16_to_rgb(c->bg, &r,&g,&b);
    uint32_t px = fb_pack_rgb888(&c->fb, r,g,b);
    fb_fill_rect(&c->fb, 0, c->fb.height - dy, c->fb.width, dy, px);

    if (c->y >= dy) c->y -= dy;
    else c->y = 0;

    /* Scrolling invalidates the drawn cursor position */
    c->cursor_drawn = false;
}

/* Invert a rectangle (used for cursor). */
static void fb_invert_rect(const framebuffer_t *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!fb || !fb->addr || fb->pitch == 0) return;
    if (x >= fb->width || y >= fb->height) return;

    if (x + w > fb->width) w = fb->width - x;
    if (y + h > fb->height) h = fb->height - y;

    uint8_t *p = (uint8_t*)fb->addr;

    if (fb->bpp == 32) {
        for (uint32_t yy = 0; yy < h; yy++) {
            uint32_t *row = (uint32_t*)(p + (uint64_t)(y + yy) * fb->pitch);
            for (uint32_t xx = 0; xx < w; xx++) {
                row[x + xx] ^= 0x00FFFFFFu;
            }
        }
    } else if (fb->bpp == 24) {
        for (uint32_t yy = 0; yy < h; yy++) {
            uint8_t *row = (uint8_t*)(p + (uint64_t)(y + yy) * fb->pitch);
            for (uint32_t xx = 0; xx < w; xx++) {
                uint8_t *px = row + (x + xx) * 3u;
                px[0] = (uint8_t)~px[0];
                px[1] = (uint8_t)~px[1];
                px[2] = (uint8_t)~px[2];
            }
        }
    } else if (fb->bpp == 16) {
        for (uint32_t yy = 0; yy < h; yy++) {
            uint16_t *row = (uint16_t*)(p + (uint64_t)(y + yy) * fb->pitch);
            for (uint32_t xx = 0; xx < w; xx++) {
                row[x + xx] ^= 0xFFFFu;
            }
        }
    }
}

static void fbcon_cursor_hide(fb_console_t *c) {
    if (!c || !c->ready) return;
    if (!c->cursor_enabled) return;
    if (!c->cursor_drawn) return;

    /* Remove underline cursor by inverting it back */
    uint32_t uy = (c->y + c->cell_h >= 2) ? (c->y + c->cell_h - 2) : c->y;
    fb_invert_rect(&c->fb, c->x, uy, c->cell_w, 2);
    c->cursor_drawn = false;
}

static void fbcon_cursor_show(fb_console_t *c) {
    if (!c || !c->ready) return;
    if (!c->cursor_enabled) return;
    if (c->cursor_drawn) return;

    /* Draw underline cursor */
    uint32_t uy = (c->y + c->cell_h >= 2) ? (c->y + c->cell_h - 2) : c->y;
    fb_invert_rect(&c->fb, c->x, uy, c->cell_w, 2);
    c->cursor_drawn = true;
}

void fbcon_set_cursor_enabled(fb_console_t *c, bool enabled) {
    if (!c) return;
    if (!enabled) {
        fbcon_cursor_hide(c);
        c->cursor_enabled = false;
        c->cursor_drawn = false;
        return;
    }
    c->cursor_enabled = true;
    c->cursor_drawn = false;
    fbcon_cursor_show(c);
}

int fbcon_init(fb_console_t *c, const framebuffer_t *fb) {
    if (!c || !fb || !fb->addr || fb->width == 0 || fb->height == 0 || fb->pitch == 0) return -1;
    memset(c, 0, sizeof(*c));

    c->fb = *fb;
    c->ready = true;

    /* Cursor enabled by default (static underline cursor). */
    c->cursor_enabled = true;
    c->cursor_drawn = false;

    c->fg = 0x07;
    c->bg = 0x00;

    c->bmp_font_ready = 0;
    c->bmp_render_w = 0;
    c->bmp_render_h = 0;

    /* Small margin helps avoid any top-edge glitches */
    c->margin_top = 2;
    c->margin_left = 2;

    c->cell_w = BITMAP_FONT_W;
    c->cell_h = BITMAP_FONT_H;

    fbcon_clear(c);
    return 0;
}

int fbcon_set_bmp_font_moduosdef(fb_console_t *c, const void *bmp_buf, size_t bmp_size) {
    if (!c || !bmp_buf || bmp_size < 64) return -1;
    int r = bmp_font_init_moduosdef(&c->bmp_font, bmp_buf, bmp_size);
    if (r != 0) {
        c->bmp_font_ready = 0;
        return r;
    }
    /* Sanity: reject obviously broken atlas layouts */
    if (c->bmp_font.cols == 0 || c->bmp_font.rows == 0 || c->bmp_font.cell_w == 0 || c->bmp_font.cell_h == 0) {
        c->bmp_font_ready = 0;
        return -2;
    }

    c->bmp_font_ready = 1;

    /* Render downscaled for usability; source cells are 30x30.
     * 12x16 is readable and closer to a classic terminal size.
     */
    c->bmp_render_w = 12;
    c->bmp_render_h = 16;

    c->cell_w = c->bmp_render_w;
    c->cell_h = c->bmp_render_h;
    return 0;
}

void fbcon_set_text_color(fb_console_t *c, uint8_t fg, uint8_t bg) {
    if (!c) return;
    c->fg = (uint8_t)(fg & 0x0F);
    c->bg = (uint8_t)(bg & 0x0F);
}

void fbcon_clear(fb_console_t *c) {
    if (!c || !c->ready) return;
    fbcon_cursor_hide(c);
    uint8_t r,g,b;
    vga16_to_rgb(c->bg, &r,&g,&b);
    uint32_t px = fb_pack_rgb888(&c->fb, r,g,b);
    fb_fill_rect(&c->fb, 0, 0, c->fb.width, c->fb.height, px);
    c->x = c->margin_left;
    c->y = c->margin_top;
    fbcon_cursor_show(c);
}

static void fbcon_newline(fb_console_t *c) {
    fbcon_cursor_hide(c);
    c->x = c->margin_left;
    c->y += c->cell_h;
    if (c->y + c->cell_h > c->fb.height) {
        fb_scroll_up(c);
        /* keep cursor on last row */
        if (c->fb.height >= c->cell_h) c->y = c->fb.height - c->cell_h;
        else c->y = 0;
    }
    fbcon_cursor_show(c);
}

static void fbcon_draw_glyph(fb_console_t *c, uint8_t ch) {
    uint8_t fr,fg,fb, br,bg,bb;
    vga16_to_rgb(c->fg, &fr,&fg,&fb);
    vga16_to_rgb(c->bg, &br,&bg,&bb);
    uint32_t fpx = fb_pack_rgb888(&c->fb, fr,fg,fb);
    uint32_t bpx = fb_pack_rgb888(&c->fb, br,bg,bb);

    /* Fill cell background */
    fb_fill_rect(&c->fb, c->x, c->y, c->cell_w, c->cell_h, bpx);

    if (c->bmp_font_ready) {
        /* Downscale glyph by sampling from the 30x30 source cell */
        uint32_t dst_w = c->bmp_render_w ? c->bmp_render_w : c->cell_w;
        uint32_t dst_h = c->bmp_render_h ? c->bmp_render_h : c->cell_h;

        /* Hard safety: avoid divide-by-zero even if configuration/state is corrupted. */
        if (dst_w == 0 || dst_h == 0 || c->bmp_font.cell_w == 0 || c->bmp_font.cell_h == 0) {
            return;
        }

        for (uint32_t yy = 0; yy < dst_h; yy++) {
            uint32_t sy = (yy * (uint32_t)c->bmp_font.cell_h) / dst_h;
            for (uint32_t xx = 0; xx < dst_w; xx++) {
                uint32_t sx = (xx * (uint32_t)c->bmp_font.cell_w) / dst_w;
                if (bmp_font_glyph_pixel_on(&c->bmp_font, ch, (uint16_t)sx, (uint16_t)sy)) {
                    fb_put_pixel(&c->fb, c->x + xx, c->y + yy, fpx);
                }
            }
        }
        return;
    }

    const uint8_t *g = bitmap_font_glyph8x16(ch);
    if (!g) return;

    for (uint32_t yy = 0; yy < BITMAP_FONT_H; yy++) {
        uint8_t row = g[yy];
        for (uint32_t xx = 0; xx < BITMAP_FONT_W; xx++) {
            uint8_t bit = (uint8_t)(0x80u >> xx);
            if (row & bit) {
                fb_put_pixel(&c->fb, c->x + xx, c->y + yy, fpx);
            }
        }
    }
}

void fbcon_putc(fb_console_t *c, char ch) {
    if (!c || !c->ready) return;

    fbcon_cursor_hide(c);

    if (ch == '\n') {
        fbcon_newline(c);
        return;
    }
    if (ch == '\r') {
        c->x = c->margin_left;
        fbcon_cursor_show(c);
        return;
    }

    if (c->x + c->cell_w > c->fb.width) {
        fbcon_newline(c);
    }

    fbcon_draw_glyph(c, (uint8_t)ch);
    c->x += c->cell_w;
    fbcon_cursor_show(c);
}

void fbcon_write(fb_console_t *c, const char *s) {
    if (!c || !s) return;
    while (*s) fbcon_putc(c, *s++);
}

void fbcon_write_n(fb_console_t *c, const char *s, size_t n) {
    if (!c || !s) return;
    for (size_t i = 0; i < n; i++) fbcon_putc(c, s[i]);
}

void fbcon_write_at(fb_console_t *c, uint32_t row, uint32_t col, const char *s) {
    if (!c || !c->ready || !s) return;

    /* Save cursor (pixels) */
    uint32_t saved_x = c->x;
    uint32_t saved_y = c->y;

    /* Convert cell coords to pixels */
    uint32_t px = c->margin_left + col * c->cell_w;
    uint32_t py = c->margin_top + row * c->cell_h;

    c->x = px;
    c->y = py;
    fbcon_write(c, s);

    /* Restore cursor */
    c->x = saved_x;
    c->y = saved_y;
}

void fbcon_backspace(fb_console_t *c) {
    if (!c || !c->ready) return;
    fbcon_cursor_hide(c);
    /* Don’t backspace into the margin area */
    if (c->y == c->margin_top && c->x <= c->margin_left) return;

    if (c->x >= c->cell_w) c->x -= c->cell_w;
    else {
        /* move to previous line */
        if (c->y >= c->cell_h) {
            c->y -= c->cell_h;
            c->x = (c->fb.width >= c->cell_w) ? (c->fb.width - c->cell_w) : 0;
        } else {
            c->x = 0;
            c->y = 0;
        }
    }

    uint8_t r,g,b;
    vga16_to_rgb(c->bg, &r,&g,&b);
    uint32_t px = fb_pack_rgb888(&c->fb, r,g,b);
    fb_fill_rect(&c->fb, c->x, c->y, c->cell_w, c->cell_h, px);
    fbcon_cursor_show(c);
}
