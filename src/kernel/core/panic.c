#include "moduos/kernel/tty/tty.h"
#include "moduos/kernel/panic.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/drivers/power/ACPI.h"
#include "moduos/drivers/Time/RTC.h"
#include <stdint.h>
#include <stddef.h>

// Shared boilerplate
static void panic_header(const char* title)
{
    (void)title;

    /* Ensure the panic screen starts from a known visible state */
    if (VGA_GetFrameBufferMode() == FB_MODE_GRAPHICS) {
        VGA_ClearFrameBuffer(0x00000000);
    } else {
        VGA_Clear();
    }
}

/* ---------------- Framebuffer Panic Screen `BLUESCREEN` ---------------- */

#include "moduos/drivers/graphics/framebuffer.h"
#include "moduos/drivers/graphics/bitmap_font.h"
#include "moduos/kernel/memory/string.h"

static uint32_t fb_pack_rgb(const framebuffer_t *fb, uint8_t r, uint8_t g, uint8_t b) {
    if (!fb) return 0;

    if (fb->bpp == 16) {
        /* Assume RGB565 */
        uint16_t rr = (uint16_t)((r * 31u) / 255u);
        uint16_t gg = (uint16_t)((g * 63u) / 255u);
        uint16_t bb = (uint16_t)((b * 31u) / 255u);
        return (uint32_t)((rr << 11) | (gg << 5) | bb);
    }

    /* If the bootloader didn't provide mask fields, fall back to XRGB8888.
     * This fixes "GUI panic only shows [PANIC]" because otherwise all packed
     * colors become 0 (black-on-black).
     */
    if (fb->bpp == 32 && (!fb->red_mask_size || !fb->green_mask_size || !fb->blue_mask_size)) {
        return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }

    /* 32bpp RGB layout via multiboot positions */
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

static void fb_put_pixel(const framebuffer_t *fb, uint32_t x, uint32_t y, uint32_t px) {
    if (!fb || !fb->addr) return;
    if (x >= fb->width || y >= fb->height) return;

    uint8_t *row = (uint8_t*)fb->addr + (uint64_t)y * fb->pitch;
    if (fb->bpp == 32) {
        ((uint32_t*)row)[x] = px;
    } else if (fb->bpp == 16) {
        ((uint16_t*)row)[x] = (uint16_t)px;
    }
}

static void fb_fill_rect(const framebuffer_t *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t px) {
    if (!fb || !fb->addr) return;
    if (x >= fb->width || y >= fb->height) return;
    if (x + w > fb->width) w = fb->width - x;
    if (y + h > fb->height) h = fb->height - y;

    for (uint32_t yy = 0; yy < h; yy++) {
        uint32_t ry = y + yy;
        uint8_t *row = (uint8_t*)fb->addr + (uint64_t)ry * fb->pitch;
        if (fb->bpp == 32) {
            uint32_t *p = (uint32_t*)row;
            for (uint32_t xx = 0; xx < w; xx++) p[x + xx] = px;
        } else if (fb->bpp == 16) {
            uint16_t *p = (uint16_t*)row;
            for (uint32_t xx = 0; xx < w; xx++) p[x + xx] = (uint16_t)px;
        }
    }
}

static void fb_draw_gradient_bg(const framebuffer_t *fb) {
    if (!fb || !fb->addr || fb->height == 0) return;
    
    for (uint32_t y = 0; y < fb->height; y++) {
        /* vertical gradient: deep navy -> near-black */
        uint8_t t = (uint8_t)((y * 255u) / fb->height);
        uint8_t r = (uint8_t)(10 + (t * 8u) / 255u);
        uint8_t g = (uint8_t)(16 + (t * 10u) / 255u);
        uint8_t b = (uint8_t)(35 + (t * 20u) / 255u);
        uint32_t px = fb_pack_rgb(fb, r, g, b);
        fb_fill_rect(fb, 0, y, fb->width, 1, px);
    }
}

static void fb_draw_glyph_scaled(const framebuffer_t *fb, uint32_t x, uint32_t y, uint8_t ch, uint32_t fg, uint32_t scale) {
    if (!fb || !fb->addr || scale == 0) return;
    
    const uint8_t *g = bitmap_font_glyph8x16(ch);
    if (!g) return;
    
    for (uint32_t yy = 0; yy < 16; yy++) {
        uint8_t row = g[yy];
        for (uint32_t xx = 0; xx < 8; xx++) {
            if (row & (0x80u >> xx)) {
                fb_fill_rect(fb, x + xx*scale, y + yy*scale, scale, scale, fg);
            }
        }
    }
}

static void fb_draw_text(const framebuffer_t *fb, uint32_t x, uint32_t y, const char *s, uint32_t fg, uint32_t scale, uint32_t max_w_px) {
    if (!fb || !fb->addr || !s || scale == 0) return;
    
    uint32_t cx = x;
    uint32_t cy = y;
    uint32_t cell_w = 8 * scale;
    uint32_t cell_h = 16 * scale;

    while (*s) {
        char ch = *s++;
        if (ch == '\n') {
            cx = x;
            cy += cell_h;
            continue;
        }
        if (max_w_px && (cx + cell_w > x + max_w_px)) {
            cx = x;
            cy += cell_h;
        }
        fb_draw_glyph_scaled(fb, cx, cy, (uint8_t)ch, fg, scale);
        cx += cell_w;
    }
}

/* ---------------- QR code (byte mode, ECC L, versions 1-6) ----------------
 *
 * The payload is plain text (category, code, title), so the code is useful
 * with no external site. Versions 1-6 need no version-info blocks, have a
 * single alignment pattern and at most two equal-sized RS blocks, which keeps
 * the encoder small. Only static and stack storage is used because the panic
 * path must not depend on the heap. The code is encoded once in panic(), not
 * on every countdown redraw.
 */

#define PANIC_QR_MAX_VERSION 6
#define PANIC_QR_MAX_SIZE    (17 + 4 * PANIC_QR_MAX_VERSION)
#define PANIC_QR_MAX_TOTAL   172
#define PANIC_QR_MAX_DATA    136
#define PANIC_QR_MAX_BYTES   134
#define PANIC_QR_MAX_EC      26
#define PANIC_QR_MAX_BLOCKS  2
#define PANIC_QR_QUIET       4u
#define PANIC_QR_TARGET_PX   200u

static const uint16_t panic_qr_total_cw[PANIC_QR_MAX_VERSION] = {
    26, 44, 70, 100, 134, 172
};
static const uint8_t panic_qr_ec_cw[PANIC_QR_MAX_VERSION] = {
    7, 10, 15, 20, 26, 18
};
static const uint8_t panic_qr_blocks[PANIC_QR_MAX_VERSION] = {
    1, 1, 1, 1, 1, 2
};
static const uint8_t panic_qr_pat_a[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0};
static const uint8_t panic_qr_pat_b[11] = {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};

static uint8_t panic_qr_mod[PANIC_QR_MAX_SIZE][PANIC_QR_MAX_SIZE];
static uint8_t panic_qr_fn[PANIC_QR_MAX_SIZE][PANIC_QR_MAX_SIZE];
static int panic_qr_size;   /* 0 = no code available */

static uint8_t panic_qr_gf_mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1D : 0));
        b >>= 1;
    }
    return r;
}

static void panic_qr_rs_ec(const uint8_t *data, int len, uint8_t *ec,
                           int ec_len) {
    uint8_t gen[PANIC_QR_MAX_EC];
    uint8_t root = 1;

    for (int i = 0; i < ec_len; i++) gen[i] = 0;
    gen[ec_len - 1] = 1;
    for (int i = 0; i < ec_len; i++) {
        for (int j = 0; j < ec_len; j++) {
            gen[j] = panic_qr_gf_mul(gen[j], root);
            if (j + 1 < ec_len) gen[j] ^= gen[j + 1];
        }
        root = panic_qr_gf_mul(root, 0x02);
    }

    for (int i = 0; i < ec_len; i++) ec[i] = 0;
    for (int i = 0; i < len; i++) {
        uint8_t f = data[i] ^ ec[0];
        for (int j = 0; j + 1 < ec_len; j++) ec[j] = ec[j + 1];
        ec[ec_len - 1] = 0;
        for (int j = 0; j < ec_len; j++)
            ec[j] ^= panic_qr_gf_mul(gen[j], f);
    }
}

static void panic_qr_set_fn(int x, int y, int dark) {
    panic_qr_mod[y][x] = (uint8_t)(dark != 0);
    panic_qr_fn[y][x] = 1;
}

static int panic_qr_abs(int v) { return v < 0 ? -v : v; }

/* Draws the 7x7 finder plus its separator ring, clipped to the symbol. */
static void panic_qr_draw_finder(int cx, int cy) {
    for (int dy = -4; dy <= 4; dy++) {
        for (int dx = -4; dx <= 4; dx++) {
            int x = cx + dx;
            int y = cy + dy;
            int ax = panic_qr_abs(dx);
            int ay = panic_qr_abs(dy);
            int d = ax > ay ? ax : ay;

            if (x < 0 || y < 0 || x >= panic_qr_size || y >= panic_qr_size)
                continue;
            panic_qr_set_fn(x, y, d != 2 && d != 4);
        }
    }
}

static void panic_qr_draw_alignment(int cx, int cy) {
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            int ax = panic_qr_abs(dx);
            int ay = panic_qr_abs(dy);
            int d = ax > ay ? ax : ay;
            panic_qr_set_fn(cx + dx, cy + dy, d != 1);
        }
    }
}

/* Doubles as the format-area reservation: called once with mask 0 before data
 * placement so those modules are excluded from it, then again per mask. */
static void panic_qr_draw_format(int mask) {
    int size = panic_qr_size;
    uint32_t data = (1u << 3) | (uint32_t)mask;   /* ECC L = 0b01 */
    uint32_t rem = data;

    for (int i = 0; i < 10; i++)
        rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    uint32_t bits = ((data << 10) | rem) ^ 0x5412;

    for (int i = 0; i <= 5; i++)
        panic_qr_set_fn(8, i, (bits >> i) & 1);
    panic_qr_set_fn(8, 7, (bits >> 6) & 1);
    panic_qr_set_fn(8, 8, (bits >> 7) & 1);
    panic_qr_set_fn(7, 8, (bits >> 8) & 1);
    for (int i = 9; i < 15; i++)
        panic_qr_set_fn(14 - i, 8, (bits >> i) & 1);

    for (int i = 0; i < 8; i++)
        panic_qr_set_fn(size - 1 - i, 8, (bits >> i) & 1);
    for (int i = 8; i < 15; i++)
        panic_qr_set_fn(8, size - 15 + i, (bits >> i) & 1);
    panic_qr_set_fn(8, size - 8, 1);
}

static void panic_qr_draw_function_patterns(int version) {
    int size = panic_qr_size;

    for (int y = 0; y < PANIC_QR_MAX_SIZE; y++) {
        for (int x = 0; x < PANIC_QR_MAX_SIZE; x++) {
            panic_qr_mod[y][x] = 0;
            panic_qr_fn[y][x] = 0;
        }
    }

    for (int i = 0; i < size; i++) {
        panic_qr_set_fn(6, i, i % 2 == 0);
        panic_qr_set_fn(i, 6, i % 2 == 0);
    }
    panic_qr_draw_finder(3, 3);
    panic_qr_draw_finder(size - 4, 3);
    panic_qr_draw_finder(3, size - 4);

    /* Versions 2-6 have exactly one alignment pattern, at (ap, ap). */
    if (version >= 2) {
        int ap = 4 * version + 10;
        panic_qr_draw_alignment(ap, ap);
    }
    panic_qr_draw_format(0);
}

static void panic_qr_place_codewords(const uint8_t *cw, int count) {
    int size = panic_qr_size;
    int i = 0;

    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < size; vert++) {
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                int up = ((right + 1) & 2) == 0;
                int y = up ? size - 1 - vert : vert;

                if (!panic_qr_fn[y][x] && i < count * 8) {
                    panic_qr_mod[y][x] = (cw[i >> 3] >> (7 - (i & 7))) & 1;
                    i++;
                }
            }
        }
    }
}

static int panic_qr_mask_hit(int mask, int x, int y) {
    switch (mask) {
    case 0: return (x + y) % 2 == 0;
    case 1: return y % 2 == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return (x / 3 + y / 2) % 2 == 0;
    case 5: return x * y % 2 + x * y % 3 == 0;
    case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
    default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

/* XOR is its own inverse, so one call both applies and removes a mask. */
static void panic_qr_apply_mask(int mask) {
    for (int y = 0; y < panic_qr_size; y++)
        for (int x = 0; x < panic_qr_size; x++)
            if (!panic_qr_fn[y][x] && panic_qr_mask_hit(mask, x, y))
                panic_qr_mod[y][x] ^= 1;
}

static int panic_qr_cell(int pass, int a, int b) {
    return pass ? panic_qr_mod[b][a] : panic_qr_mod[a][b];
}

static int panic_qr_penalty(void) {
    int size = panic_qr_size;
    int score = 0;
    int dark = 0;

    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
            dark += panic_qr_mod[y][x];

    /* Rows (pass 0) and columns (pass 1): runs and finder-like patterns. */
    for (int pass = 0; pass < 2; pass++) {
        for (int a = 0; a < size; a++) {
            int run = 1;

            for (int b = 1; b < size; b++) {
                if (panic_qr_cell(pass, a, b) == panic_qr_cell(pass, a, b - 1)) {
                    run++;
                } else {
                    if (run >= 5) score += run - 2;
                    run = 1;
                }
            }
            if (run >= 5) score += run - 2;

            for (int s = 0; s + 11 <= size; s++) {
                int ma = 1;
                int mb = 1;
                for (int k = 0; k < 11; k++) {
                    int c = panic_qr_cell(pass, a, s + k);
                    if (c != panic_qr_pat_a[k]) ma = 0;
                    if (c != panic_qr_pat_b[k]) mb = 0;
                }
                if (ma || mb) score += 40;
            }
        }
    }

    for (int y = 0; y + 1 < size; y++) {
        for (int x = 0; x + 1 < size; x++) {
            int c = panic_qr_mod[y][x];
            if (c == panic_qr_mod[y][x + 1] && c == panic_qr_mod[y + 1][x] &&
                c == panic_qr_mod[y + 1][x + 1])
                score += 3;
        }
    }

    int total = size * size;
    int k = (panic_qr_abs(dark * 20 - total * 10) + total - 1) / total - 1;
    if (k > 0) score += k * 10;

    return score;
}

static void panic_qr_put_bits(uint8_t *buf, int *pos, uint32_t val, int n) {
    for (int i = n - 1; i >= 0; i--) {
        if ((val >> i) & 1)
            buf[*pos >> 3] |= (uint8_t)(0x80 >> (*pos & 7));
        (*pos)++;
    }
}

/* Appends s to buf, replacing anything outside printable ASCII (except '\n')
 * so a corrupted string can't produce an unscannable payload. */
static int panic_qr_append(char *buf, int pos, int cap, const char *s) {
    if (!s) return pos;
    while (*s && pos < cap) {
        char c = *s++;
        if (c != '\n' && (c < 0x20 || c > 0x7E)) c = '?';
        buf[pos++] = c;
    }
    return pos;
}

static void panic_qr_build(const char *title, const char *cat,
                           const char *code) {
    char text[PANIC_QR_MAX_BYTES];
    uint8_t data[PANIC_QR_MAX_DATA];
    uint8_t ec[PANIC_QR_MAX_BLOCKS][PANIC_QR_MAX_EC];
    uint8_t cw[PANIC_QR_MAX_TOTAL];
    int len = 0;

    len = panic_qr_append(text, len, PANIC_QR_MAX_BYTES, "ModuOS panic\nCAT: ");
    len = panic_qr_append(text, len, PANIC_QR_MAX_BYTES, cat ? cat : "UNKNOWN");
    len = panic_qr_append(text, len, PANIC_QR_MAX_BYTES, "\nCODE: ");
    len = panic_qr_append(text, len, PANIC_QR_MAX_BYTES,
                          code ? code : "UNKNOWN");
    len = panic_qr_append(text, len, PANIC_QR_MAX_BYTES, "\n");
    len = panic_qr_append(text, len, PANIC_QR_MAX_BYTES, title);

    int version = 1;
    while (version < PANIC_QR_MAX_VERSION) {
        int idx = version - 1;
        int cap = panic_qr_total_cw[idx] -
                  panic_qr_ec_cw[idx] * panic_qr_blocks[idx] - 2;
        if (cap >= len) break;
        version++;
    }

    int idx = version - 1;
    int nb = panic_qr_blocks[idx];
    int ec_len = panic_qr_ec_cw[idx];
    int total = panic_qr_total_cw[idx];
    int data_cw = total - ec_len * nb;
    int dpb = data_cw / nb;

    for (int i = 0; i < data_cw; i++) data[i] = 0;

    int pos = 0;
    panic_qr_put_bits(data, &pos, 0x4, 4);
    panic_qr_put_bits(data, &pos, (uint32_t)len, 8);
    for (int i = 0; i < len; i++)
        panic_qr_put_bits(data, &pos, (uint8_t)text[i], 8);

    int term = data_cw * 8 - pos;
    if (term > 4) term = 4;
    pos += term;
    pos = (pos + 7) & ~7;

    uint8_t pad = 0xEC;
    for (int i = pos >> 3; i < data_cw; i++) {
        data[i] = pad;
        pad ^= (uint8_t)(0xEC ^ 0x11);
    }

    for (int b = 0; b < nb; b++)
        panic_qr_rs_ec(data + b * dpb, dpb, ec[b], ec_len);

    int n = 0;
    for (int i = 0; i < dpb; i++)
        for (int b = 0; b < nb; b++)
            cw[n++] = data[b * dpb + i];
    for (int i = 0; i < ec_len; i++)
        for (int b = 0; b < nb; b++)
            cw[n++] = ec[b][i];

    panic_qr_size = 17 + 4 * version;
    panic_qr_draw_function_patterns(version);
    panic_qr_place_codewords(cw, total);

    int best = 0;
    int best_score = 0x7FFFFFFF;
    for (int m = 0; m < 8; m++) {
        panic_qr_apply_mask(m);
        panic_qr_draw_format(m);
        int s = panic_qr_penalty();
        panic_qr_apply_mask(m);
        if (s < best_score) {
            best_score = s;
            best = m;
        }
    }
    panic_qr_apply_mask(best);
    panic_qr_draw_format(best);
}

static void panic_draw_qr(const framebuffer_t *fb, uint32_t x, uint32_t y,
                          uint32_t scale) {
    uint32_t modules = (uint32_t)panic_qr_size + 2 * PANIC_QR_QUIET;
    uint32_t light = fb_pack_rgb(fb, 255, 255, 255);
    uint32_t dark = fb_pack_rgb(fb, 0, 0, 0);

    /* Light panel includes the quiet zone; scanners need it on dark screens. */
    fb_fill_rect(fb, x, y, modules * scale, modules * scale, light);
    for (int my = 0; my < panic_qr_size; my++) {
        for (int mx = 0; mx < panic_qr_size; mx++) {
            if (!panic_qr_mod[my][mx]) continue;
            fb_fill_rect(fb, x + (PANIC_QR_QUIET + (uint32_t)mx) * scale,
                         y + (PANIC_QR_QUIET + (uint32_t)my) * scale,
                         scale, scale, dark);
        }
    }
}

static void panic_draw_gui(const char* title, const char* message, const char* tips, const char* err_cat, const char* err_code, int seconds_left) {
    framebuffer_t fb;
    if (VGA_GetFrameBufferMode() != FB_MODE_GRAPHICS) {
        /* fallback */
        VGA_Clear();
        VGA_Write(title);
        VGA_Write("\n\n");
        VGA_Write(message);
        VGA_Write("\n\n");
        VGA_Writef("ERR_CODE_CAT: %s | ERR_CODE: %s\n", err_cat, err_code);
        VGA_Writef("Rebooting in %d seconds...\n", seconds_left);
        return;
    }
    if (VGA_GetFrameBuffer(&fb) != 0 || !fb.addr) {
        /* fallback */
        VGA_Clear();
        VGA_Write(title);
        VGA_Write("\n");
        VGA_Write(message);
        return;
    }

    /* Always clear first so we never end up with a blank/unchanged screen */
    fb_draw_gradient_bg(&fb);

    /* card */
    uint32_t card_w = (fb.width > 900) ? 900 : (fb.width > 40 ? fb.width - 40 : fb.width);
    uint32_t card_h = (fb.height > 520) ? 520 : (fb.height > 40 ? fb.height - 40 : fb.height);
    uint32_t card_x = (fb.width - card_w) / 2;
    uint32_t card_y = (fb.height - card_h) / 2;
    if (card_y < 180) card_y = 180; /* leave room for big top-left sad face */

    uint32_t card_bg = fb_pack_rgb(&fb, 20, 24, 38);
    uint32_t card_edge = fb_pack_rgb(&fb, 40, 50, 80);

    /* pseudo-rounded: draw border + inset */
    fb_fill_rect(&fb, card_x, card_y, card_w, card_h, card_edge);
    fb_fill_rect(&fb, card_x + 2, card_y + 2, card_w - 4, card_h - 4, card_bg);

    uint32_t fg1 = fb_pack_rgb(&fb, 240, 245, 255);
    uint32_t fg2 = fb_pack_rgb(&fb, 170, 185, 210);

    /* The QR sits top-right of the card; the title and message wrap to a
     * narrower column so they never run under it. Small screens skip it. */
    int show_qr = panic_qr_size > 0 && card_w >= 600 && card_h >= 400;
    uint32_t qr_scale = 0;
    uint32_t qr_panel = 0;
    uint32_t text_w = card_w - 56;

    if (show_qr) {
        uint32_t modules = (uint32_t)panic_qr_size + 2 * PANIC_QR_QUIET;
        qr_scale = PANIC_QR_TARGET_PX / modules;
        if (qr_scale == 0) qr_scale = 1;
        qr_panel = modules * qr_scale;
        text_w -= qr_panel + 16;
    }

    /* Sad face (classic BSOD vibe) - top-left */
    fb_draw_text(&fb, 40, 30, ":(", fg1, 6, 0);

    /* Title (old panic style, but HD) */
    fb_draw_text(&fb, card_x + 28, card_y + 24, title, fg1, 2, text_w);

    /* Guaranteed fallback text (fb console), in case direct fb drawing isn't visible */
    VGA_WriteTextAtPosition(0, 0, "[PANIC]");

    /* message */
    fb_draw_text(&fb, card_x + 28, card_y + 80, message, fg1, 1, text_w);

    if (show_qr) {
        uint32_t qx = card_x + card_w - 28 - qr_panel;
        uint32_t qy = card_y + 24;
        panic_draw_qr(&fb, qx, qy, qr_scale);
        fb_draw_text(&fb, qx, qy + qr_panel + 4, "Scan for error info", fg2, 1, 0);
    }

    uint32_t y = card_y + 260;

    /* tips */
    if (tips && tips[0]) {
        fb_draw_text(&fb, card_x + 28, y, "Troubleshooting Tips:", fg2, 1, card_w - 56);
        y += 22;
        fb_draw_text(&fb, card_x + 28, y, tips, fg1, 1, card_w - 56);
        y += 86;
    }

    /* error code + footer with safe bounds checking */
    char code_line[256];
    size_t pos = 0;
    const char* prefix1 = "ERR_CODE_CAT: ";
    const char* sep = " | ERR_CODE: ";
    
    /* Safely build code_line */
    while (*prefix1 && pos < sizeof(code_line) - 1) {
        code_line[pos++] = *prefix1++;
    }
    const char* cat_ptr = err_cat ? err_cat : "UNKNOWN";
    while (*cat_ptr && pos < sizeof(code_line) - 1) {
        code_line[pos++] = *cat_ptr++;
    }
    while (*sep && pos < sizeof(code_line) - 1) {
        code_line[pos++] = *sep++;
    }
    const char* code_ptr = err_code ? err_code : "UNKNOWN";
    while (*code_ptr && pos < sizeof(code_line) - 1) {
        code_line[pos++] = *code_ptr++;
    }
    code_line[pos] = '\0';

    fb_draw_text(&fb, card_x + 28, card_y + card_h - 140, code_line, fg2, 1, card_w - 56);
    /* Safely build reboot message */
    char reboot_line[128];
    pos = 0;
    const char* prefix2 = "The system will reboot shortly. Rebooting in ";
    while (*prefix2 && pos < sizeof(reboot_line) - 20) {
        reboot_line[pos++] = *prefix2++;
    }
    
    /* Convert seconds to string safely */
    char num[16];
    int n = seconds_left;
    int i = 0;
    if (n < 0) {
        num[i++] = '-';
        n = -n;
    }
    if (n == 0) {
        num[i++] = '0';
    } else {
        char tmp[16];
        int j = 0;
        while (n > 0 && j < 15) {
            tmp[j++] = '0' + (n % 10);
            n /= 10;
        }
        while (j > 0 && i < 15) {
            num[i++] = tmp[--j];
        }
    }
    num[i] = '\0';
    
    /* Append number */
    i = 0;
    while (num[i] && pos < sizeof(reboot_line) - 15) {
        reboot_line[pos++] = num[i++];
    }
    
    /* Append suffix */
    const char* suffix = " seconds...";
    while (*suffix && pos < sizeof(reboot_line) - 1) {
        reboot_line[pos++] = *suffix++;
    }
    reboot_line[pos] = '\0';
    
    fb_draw_text(&fb, card_x + 28, card_y + card_h - 80, reboot_line, fg2, 1, card_w - 56);
}

/**
 * Generic panic function
 * @param title        The panic title
 * @param message      Detailed message explaining the problem
 * @param tips         Optional troubleshooting tips (can be NULL)
 * @param err_cat      Error category string (e.g., "DEV")
 * @param err_code     Specific error code string (e.g., "ATA_DEV_NONE")
 * @param reboot_delay Seconds to wait before reboot
 */
void panic(const char* title, const char* message, const char* tips, const char* err_cat, const char* err_code, int reboot_delay)
{
    __asm__ volatile("cli" ::: "memory");

    /* Hand the display back to the raw console before drawing. */
    tty_panic_release();

    /* Encoded once up front: the redraw loop below only blits the result. */
    panic_qr_build(title, err_cat, err_code);

    for (int i = reboot_delay; i >= 0; i--) {
        panic_header(title);
        panic_draw_gui(title, message, tips, err_cat, err_code, i);
        rtc_wait_seconds(1);
    }

    acpi_reboot();
    for (;;) { __asm__("hlt"); }
}

// Zenith4 deprecated, this is now just for init.
void trigger_no_shell_panic() {
    panic(
        "init process has stopped responding",
        "The system cannot continue without the init process running.\n"
        "This may be due to memory corruption.",
        " - Check if your RAM is properly connected and not loose.\n"
        " - Try a different RAM stick.\n"
        " - If using automan, check what automan version you are using.",
        "SYS_PROCESS",
        "INIT_PROCESS_NOT_RUNNING",
        6
    );
}

void trigger_panic_dodev() {
    panic(
        "No hard disks were detected during boot.",
        "The system cannot continue without at least one storage device.\n"
        "This may be due to missing drivers, hardware failure, or misconfiguration.",
        " - Check if your storage devices are properly connected.\n"
        " - Try a different hardware configuration if available.",
        "HW_DEVICE",
        "NO_MEDIUM_FOUND",
        6
    );
}

void trigger_panic_doata() {
    panic(
        "The ATA Controller did not respond during boot.",
        "The system cannot continue without a functional ATA controller.\n"
        "This may be due to missing drivers, hardware failure, or misconfiguration.",
        " - Ensure your storage controller is enabled in BIOS/UEFI.\n"
        " - Verify that drives are properly connected.\n"
        " - Try a different hardware or emulator configuration if available.",
        "HW_DEVICE",
        "ATA_CONTROLLER_UNRESPONSIVE",
        6
    );
}

void trigger_panic_dops2() {
    panic(
        "The PS/2 keyboard did not respond during boot.",
        "The system cannot continue without a keyboard device.\n"
        "This may be due to missing drivers, hardware failure, or misconfiguration.",
        " - Check if your PS/2 device is properly connected.\n"
        " - Try a different hardware configuration if available.",
        "HW_TIMEOUT",
        "PS2_DEVICE_TIMEOUT",
        6
    );
}

void trigger_panic_dofs() {
    panic(
        "No FAT32 or ISO9660 filesystem was detected during boot.",
        "The system cannot continue without a valid filesystem.\n"
        "This may be due to:\n"
        " - Missing or corrupted partition/boot sector.\n"
        " - Unsupported filesystem type.\n"
        " - Drive not properly formatted.",
        " - Verify that your disk is formatted with FAT32.\n"
        " - Ensure the drive is properly connected and detected.\n"
        " - If using an image, confirm it contains a valid ISO9660 volume.",
        "FS_LAYER",
        "FS_INIT_NO_VALID_FS",
        6
    );
}

void trigger_panic_unknown() {
    panic(
        "An unexpected system crash has occurred.",
        "The system encountered a fatal error and cannot continue.\n"
        "Please restart your computer or contact a developer.",
        "If this error persists, report it with the steps to reproduce.",
        "UNKNOWN",
        "UNKNOWN_ERROR",
        6
    );
}