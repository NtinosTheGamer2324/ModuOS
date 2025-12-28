#include "moduos/drivers/graphics/fb_console.h"
#include "moduos/drivers/graphics/utf8_decode.h"
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

void fbcon_get_cursor_pos(const fb_console_t *c, uint32_t *row, uint32_t *col) {
    if (!c || !c->ready) {
        if (row) *row = 0;
        if (col) *col = 0;
        return;
    }

    uint32_t rx = (c->x > c->margin_left) ? (c->x - c->margin_left) : 0;
    uint32_t ry = (c->y > c->margin_top)  ? (c->y - c->margin_top)  : 0;

    uint32_t r = (c->cell_h ? (ry / c->cell_h) : 0);
    uint32_t cl = (c->cell_w ? (rx / c->cell_w) : 0);

    if (row) *row = r;
    if (col) *col = cl;
}

void fbcon_set_cursor_pos(fb_console_t *c, uint32_t row, uint32_t col) {
    if (!c || !c->ready) return;

    fbcon_cursor_hide(c);

    uint32_t px = c->margin_left + col * (uint32_t)c->cell_w;
    uint32_t py = c->margin_top  + row * (uint32_t)c->cell_h;

    /* clamp to framebuffer */
    if (c->fb.width && px >= c->fb.width) {
        px = c->fb.width - (c->cell_w ? c->cell_w : 1);
    }
    if (c->fb.height && py >= c->fb.height) {
        py = c->fb.height - (c->cell_h ? c->cell_h : 1);
    }

    c->x = px;
    c->y = py;
    c->cursor_drawn = false;
    fbcon_cursor_show(c);
}

int fbcon_init(fb_console_t *c, const framebuffer_t *fb) {
    if (!c || !fb || !fb->addr || fb->width == 0 || fb->height == 0 || fb->pitch == 0) return -1;
    memset(c, 0, sizeof(*c));
    /* utf8_pending_* already zeroed */

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

static void fbcon_putc_raw(fb_console_t *c, char ch) {
    /* Byte-oriented output used after UTF-8 has been decoded/mapped to CP437/ASCII. */
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
    if (ch == '\b') {
        fbcon_backspace(c);
        return;
    }
    if (ch == '\t') {
        for (int i = 0; i < 4; i++) fbcon_putc_raw(c, ' ');
        return;
    }

    if (c->x + c->cell_w > c->fb.width) {
        fbcon_newline(c);
    }

    fbcon_draw_glyph(c, (uint8_t)ch);
    c->x += c->cell_w;
    fbcon_cursor_show(c);
}

/* Forward declarations for helpers used by UTF-8 emission */
static const char *fbcon_unicode_to_ascii_fallback(uint32_t cp);
static uint8_t fbcon_unicode_to_cp437(uint32_t cp);

static size_t fbcon_utf8_encode(uint32_t cp, char out[4]) {
    if (cp <= 0x7Fu) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7FFu) {
        out[0] = (char)(0xC0u | ((cp >> 6) & 0x1Fu));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp <= 0xFFFFu) {
        /* skip surrogates */
        if (cp >= 0xD800u && cp <= 0xDFFFu) {
            out[0] = '?';
            return 1;
        }
        out[0] = (char)(0xE0u | ((cp >> 12) & 0x0Fu));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cp <= 0x10FFFFu) {
        out[0] = (char)(0xF0u | ((cp >> 18) & 0x07u));
        out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[3] = (char)(0x80u | (cp & 0x3Fu));
        return 4;
    }
    out[0] = '?';
    return 1;
}

static void fbcon_emit_codepoint(fb_console_t *c, uint32_t cp) {
    if (!c) return;

    /* Skip variation selector */
    if (cp == 0xFE0F) return;

#if FBCON_DEBUG
    if (cp >= 0x80) {
        char enc[4] = {0,0,0,0};
        size_t elen = fbcon_utf8_encode(cp, enc);
        com_printf(COM1_PORT, "[FBCON][UTF8] cp=U+%04x bytes=", (unsigned)cp);
        for (size_t i = 0; i < elen; i++) {
            com_printf(COM1_PORT, "%02x", (unsigned)(uint8_t)enc[i]);
            if (i + 1 < elen) com_write_string(COM1_PORT, " ");
        }
        com_write_string(COM1_PORT, " char='");
        for (size_t i = 0; i < elen; i++) {
            char tmp[2] = {enc[i], 0};
            com_write_string(COM1_PORT, tmp);
        }
        com_write_string(COM1_PORT, "'\n");
    }
#endif

    const char *exp = fbcon_unicode_to_ascii_fallback(cp);
    if (exp) {
        while (*exp) fbcon_putc_raw(c, *exp++);
        return;
    }

    uint8_t ch = fbcon_unicode_to_cp437(cp);
    fbcon_putc_raw(c, (char)ch);
}

void fbcon_putc(fb_console_t *c, char ch) {
    if (!c || !c->ready) return;

    /* Fast path for ASCII control chars: flush pending UTF-8 and handle immediately. */
    if ((unsigned char)ch < 0x20 || ch == 0x7F) {
        c->utf8_pending_len = 0;
        c->utf8_pending_used = 0;
        fbcon_putc_raw(c, ch);
        return;
    }

    uint8_t b = (uint8_t)ch;

    /* If not currently collecting a UTF-8 sequence, decide what to do. */
    if (c->utf8_pending_len == 0) {
        if (b < 0x80) {
            fbcon_putc_raw(c, (char)b);
            return;
        }

        /* Start sequence */
        uint8_t need = 0;
        if ((b & 0xE0) == 0xC0) need = 2;
        else if ((b & 0xF0) == 0xE0) need = 3;
        else if ((b & 0xF8) == 0xF0) need = 4;
        else {
            fbcon_putc_raw(c, '?');
            return;
        }

        c->utf8_pending_len = need;
        c->utf8_pending_used = 0;
    }

    /* Collect bytes */
    if (c->utf8_pending_used < 4) {
        c->utf8_pending[c->utf8_pending_used++] = b;
    }

    /* Validate continuation bytes if we're past the first byte */
    if (c->utf8_pending_used > 1) {
        if ((b & 0xC0) != 0x80) {
            c->utf8_pending_len = 0;
            c->utf8_pending_used = 0;
            fbcon_putc_raw(c, '?');
            return;
        }
    }

    /* If complete, decode */
    if (c->utf8_pending_used >= c->utf8_pending_len) {
        uint32_t cp = 0;
        size_t seq_len = c->utf8_pending_len;
        uint8_t seq_bytes[4] = {0,0,0,0};
        for (size_t bi = 0; bi < seq_len && bi < 4; bi++) seq_bytes[bi] = c->utf8_pending[bi];

        size_t used = utf8_decode_one((const char*)seq_bytes, seq_len, &cp);
        c->utf8_pending_len = 0;
        c->utf8_pending_used = 0;

        if (used == 0) {
            fbcon_putc_raw(c, '?');
            return;
        }

        /* Debug logging is handled in fbcon_emit_codepoint so buffered and bytewise output behave the same. */

        fbcon_emit_codepoint(c, cp);
    }
}

/* Map a subset of Unicode codepoints to CP437 glyph indices.
 * This keeps rendering fast while supporting common UTF-8 text and box-drawing.
 */
/* If cp should expand to an ASCII string (emojis/checkmarks), return it; else NULL. */
static const char *fbcon_unicode_to_ascii_fallback(uint32_t cp) {
    switch (cp) {
        /* Checkmarks */
        case 0x2713: /* ✓ */ return "[OK]";
        case 0x2714: /* ✔ */ return "[OK]";
        case 0x2705: /* ✅ */ return "[OK]";
        case 0x2611: /* ☑ */ return "[x]";
        case 0x2610: /* ☐ */ return "[ ]";

        /* Common emojis (fallbacks) */
        case 0x1F600: /* 😀 */ return ":D";
        case 0x1F603: /* 😃 */ return ":D";
        case 0x1F604: /* 😄 */ return ":D";
        case 0x1F642: /* 🙂 */ return ":)";
        case 0x1F610: /* 😐 */ return ":|";
        case 0x1F622: /* 😢 */ return ":'(";
        case 0x1F62D: /* 😭 */ return ":'(";
        case 0x1F602: /* 😂 */ return "xD";
        case 0x1F525: /* 🔥 */ return "!!";
        case 0x1F44D: /* 👍 */ return "+1";
        case 0x1F44E: /* 👎 */ return "-1";
        case 0x1F4A9: /* 💩 */ return "[poop]";
        case 0x1F680: /* 🚀 */ return "->";
        case 0x1F389: /* 🎉 */ return "*";
        case 0x1F496: /* 💖 */ return "<3";
        case 0x1F499: /* 💙 */ return "<3";
        case 0x1F49A: /* 💚 */ return "<3";
        case 0x1F49B: /* 💛 */ return "<3";
        case 0x1F49C: /* 💜 */ return "<3";

        /* Heart (sometimes comes as U+2764 U+FE0F) */
        case 0x2764: /* ❤ */ return "<3";

        default: return NULL;
    }
}

static uint8_t fbcon_unicode_to_cp437(uint32_t cp) {
    if (cp < 0x80) return (uint8_t)cp;

    /* Latin-1 supplement (partial) */
    switch (cp) {
        case 0x00A0: return 0x20; /* NBSP -> space */
        case 0x00A9: return 0xA9; /* © */
        case 0x00AE: return 0xAE; /* ® */
        case 0x00B0: return 0xF8; /* ° */
        case 0x00B1: return 0xF1; /* ± */
        case 0x00B5: return 0xE6; /* µ */
        case 0x00D7: return 0xD9; /* × */
        case 0x00F7: return 0xF6; /* ÷ */
        default: break;
    }

    /* Box drawing (Unicode) -> CP437 */
    switch (cp) {
        case 0x2500: return 0xC4; /* ─ */
        case 0x2502: return 0xB3; /* │ */
        case 0x250C: return 0xDA; /* ┌ */
        case 0x2510: return 0xBF; /* ┐ */
        case 0x2514: return 0xC0; /* └ */
        case 0x2518: return 0xD9; /* ┘ */
        case 0x251C: return 0xC3; /* ├ */
        case 0x2524: return 0xB4; /* ┤ */
        case 0x252C: return 0xC2; /* ┬ */
        case 0x2534: return 0xC1; /* ┴ */
        case 0x253C: return 0xC5; /* ┼ */
        case 0x2588: return 0xDB; /* █ */
        case 0x2591: return 0xB0; /* ░ */
        case 0x2592: return 0xB1; /* ▒ */
        case 0x2593: return 0xB2; /* ▓ */
        default: break;
    }

    /* Fallback */
    return '?';
}

void fbcon_write(fb_console_t *c, const char *s) {
    if (!c || !s) return;

    /* Ensure we don't mix buffered UTF-8 decoding with an in-progress bytewise sequence. */
    c->utf8_pending_len = 0;
    c->utf8_pending_used = 0;

    size_t i = 0;
    size_t n = strlen(s);
    while (i < n) {
        uint32_t cp = 0;
        size_t used = utf8_decode_one(s + i, n - i, &cp);
        if (used == 0) {
            fbcon_putc_raw(c, '?');
            i++;
            continue;
        }

        fbcon_emit_codepoint(c, cp);
        i += used;
    }
}

void fbcon_write_n(fb_console_t *c, const char *s, size_t n) {
    if (!c || !s) return;

    c->utf8_pending_len = 0;
    c->utf8_pending_used = 0;

    size_t i = 0;
    while (i < n) {
        uint32_t cp = 0;
        size_t used = utf8_decode_one(s + i, n - i, &cp);
        if (used == 0) {
            fbcon_putc_raw(c, '?');
            i++;
            continue;
        }

        fbcon_emit_codepoint(c, cp);
        i += used;
    }
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
