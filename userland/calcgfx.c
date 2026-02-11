#include "libc.h"
#include "string.h"
#include "../include/moduos/kernel/events/events.h"

/*
 * calcgfx.sqr
 *
 * Standalone classic calculator.
 *  - Mouse click buttons
 *  - Optional keyboard char input
 *  - Integer operations: + - * /
 */

static inline uint32_t pack_xrgb8888(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

#define CURSOR_W 16u
#define CURSOR_H 16u

typedef struct {
    uint32_t w;
    uint32_t h;
    uint8_t rgba[CURSOR_W * CURSOR_H * 4u];
} cursor_img_t;

static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline int32_t rds32(const uint8_t *p) { return (int32_t)rd32(p); }

static int load_cursor_bmp_rgba8888(const char *path, cursor_img_t *out) {
    fs_file_info_t st;
    if (stat(path, &st) != 0) return -1;
    if (st.is_directory) return -2;

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -3;

    uint32_t size = st.size;
    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) {
        close(fd);
        return -4;
    }

    uint32_t got = 0;
    while (got < size) {
        ssize_t n = read(fd, buf + got, size - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    close(fd);

    if (got != size || size < 54) {
        free(buf);
        return -5;
    }

    if (buf[0] != 'B' || buf[1] != 'M') {
        free(buf);
        return -6;
    }

    uint32_t pixel_off = rd32(buf + 10);
    uint32_t dib_size = rd32(buf + 14);
    if (dib_size < 40 || 14u + dib_size > size) {
        free(buf);
        return -7;
    }

    int32_t width = rds32(buf + 18);
    int32_t height = rds32(buf + 22);
    uint16_t planes = (uint16_t)(buf[26] | (buf[27] << 8));
    uint16_t bpp = (uint16_t)(buf[28] | (buf[29] << 8));

    if (planes != 1 || bpp != 32) {
        free(buf);
        return -8;
    }

    int top_down = 0;
    if (height < 0) {
        top_down = 1;
        height = -height;
    }

    if (width != (int32_t)CURSOR_W || height != (int32_t)CURSOR_H) {
        free(buf);
        return -9;
    }

    uint32_t src_row_stride = (uint32_t)width * 4u;
    if (pixel_off + (uint64_t)src_row_stride * (uint32_t)height > size) {
        free(buf);
        return -10;
    }

    out->w = CURSOR_W;
    out->h = CURSOR_H;

    for (uint32_t y = 0; y < CURSOR_H; y++) {
        uint32_t sy = top_down ? y : (CURSOR_H - 1u - y);
        const uint8_t *row = buf + pixel_off + (uint64_t)sy * src_row_stride;
        for (uint32_t x = 0; x < CURSOR_W; x++) {
            uint32_t px = rd32(row + x * 4u);
            uint8_t b = (uint8_t)(px & 0xFF);
            uint8_t g = (uint8_t)((px >> 8) & 0xFF);
            uint8_t r = (uint8_t)((px >> 16) & 0xFF);
            uint8_t a = (uint8_t)((px >> 24) & 0xFF);

            uint32_t di = (y * CURSOR_W + x) * 4u;
            out->rgba[di + 0] = r;
            out->rgba[di + 1] = g;
            out->rgba[di + 2] = b;
            out->rgba[di + 3] = a;
        }
    }

    free(buf);
    return 0;
}

static void alpha_blit_cursor_xrgb8888(uint8_t *dst, uint32_t pitch, uint32_t w, uint32_t h,
                                       const cursor_img_t *cur, int32_t x, int32_t y) {
    for (uint32_t cy = 0; cy < cur->h; cy++) {
        int32_t dy = y + (int32_t)cy;
        if ((uint32_t)dy >= h) continue;
        uint32_t *row = (uint32_t *)(dst + (uint64_t)dy * pitch);

        for (uint32_t cx = 0; cx < cur->w; cx++) {
            int32_t dx = x + (int32_t)cx;
            if ((uint32_t)dx >= w) continue;

            uint32_t si = (cy * cur->w + cx) * 4u;
            uint8_t sr = cur->rgba[si + 0];
            uint8_t sg = cur->rgba[si + 1];
            uint8_t sb = cur->rgba[si + 2];
            uint8_t sa = cur->rgba[si + 3];
            if (sa == 0) continue;

            uint32_t dp = row[dx];
            uint8_t dr = (uint8_t)((dp >> 16) & 0xFF);
            uint8_t dg = (uint8_t)((dp >> 8) & 0xFF);
            uint8_t db = (uint8_t)(dp & 0xFF);

            uint32_t a = sa;
            uint32_t ia = 255u - a;
            uint8_t or = (uint8_t)((sr * a + dr * ia) / 255u);
            uint8_t og = (uint8_t)((sg * a + dg * ia) / 255u);
            uint8_t ob = (uint8_t)((sb * a + db * ia) / 255u);

            row[dx] = pack_xrgb8888(or, og, ob);
        }
    }
}

static void fill_rect(uint8_t *fb, uint32_t pitch, uint32_t sw, uint32_t sh,
                      int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= (int32_t)sw || y >= (int32_t)sh) return;
    if (x + w > (int32_t)sw) w = (int32_t)sw - x;
    if (y + h > (int32_t)sh) h = (int32_t)sh - y;

    for (int32_t yy = 0; yy < h; yy++) {
        uint32_t *row = (uint32_t *)(fb + (uint64_t)(y + yy) * pitch);
        for (int32_t xx = 0; xx < w; xx++) row[x + xx] = c;
    }
}

static void draw_button(uint8_t *fb, uint32_t pitch, uint32_t sw, uint32_t sh,
                        int x, int y, int w, int h, int pressed) {
    uint32_t face = pack_xrgb8888(200,200,200);
    uint32_t hi = pack_xrgb8888(240,240,240);
    uint32_t shd = pack_xrgb8888(110,110,110);

    fill_rect(fb, pitch, sw, sh, x, y, w, h, face);
    if (pressed) {
        fill_rect(fb, pitch, sw, sh, x, y, w, 2, shd);
        fill_rect(fb, pitch, sw, sh, x, y, 2, h, shd);
        fill_rect(fb, pitch, sw, sh, x, y+h-2, w, 2, hi);
        fill_rect(fb, pitch, sw, sh, x+w-2, y, 2, h, hi);
    } else {
        fill_rect(fb, pitch, sw, sh, x, y, w, 2, hi);
        fill_rect(fb, pitch, sw, sh, x, y, 2, h, hi);
        fill_rect(fb, pitch, sw, sh, x, y+h-2, w, 2, shd);
        fill_rect(fb, pitch, sw, sh, x+w-2, y, 2, h, shd);
    }
}

/* Tiny 8x8 font from minesgfx/miniwm (ASCII 32..127) */
/* NOTE: font table must contain all 96 entries; reuse from minesgfx/miniwm to avoid truncation. */
static const uint8_t font8x8_basic[96][8] = {
    /* 0x20 ' ' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x21 '!' */ {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00},
    /* 0x22 '"' */ {0x36,0x36,0x24,0x00,0x00,0x00,0x00,0x00},
    /* 0x23 '#' */ {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},
    /* 0x24 '$' */ {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00},
    /* 0x25 '%' */ {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00},
    /* 0x26 '&' */ {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00},
    /* 0x27 '\''*/ {0x06,0x06,0x04,0x00,0x00,0x00,0x00,0x00},
    /* 0x28 '(' */ {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00},
    /* 0x29 ')' */ {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00},
    /* 0x2A '*' */ {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},
    /* 0x2B '+' */ {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00},
    /* 0x2C ',' */ {0x00,0x00,0x00,0x00,0x0C,0x0C,0x06,0x00},
    /* 0x2D '-' */ {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00},
    /* 0x2E '.' */ {0x00,0x00,0x00,0x00,0x0C,0x0C,0x00,0x00},
    /* 0x2F '/' */ {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00},
    /* 0x30 '0' */ {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00},
    /* 0x31 '1' */ {0x0C,0x0E,0x0F,0x0C,0x0C,0x0C,0x3F,0x00},
    /* 0x32 '2' */ {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00},
    /* 0x33 '3' */ {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00},
    /* 0x34 '4' */ {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00},
    /* 0x35 '5' */ {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00},
    /* 0x36 '6' */ {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00},
    /* 0x37 '7' */ {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00},
    /* 0x38 '8' */ {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00},
    /* 0x39 '9' */ {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00},
    /* 0x3A ':' */ {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00,0x00},
    /* 0x3B ';' */ {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x06,0x00},
    /* 0x3C '<' */ {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00},
    /* 0x3D '=' */ {0x00,0x00,0x3F,0x00,0x3F,0x00,0x00,0x00},
    /* 0x3E '>' */ {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00},
    /* 0x3F '?' */ {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00},
    /* 0x40 '@' */ {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00},
    /* 0x41 'A' */ {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00},
    /* 0x42 'B' */ {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00},
    /* 0x43 'C' */ {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00},
    /* 0x44 'D' */ {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00},
    /* 0x45 'E' */ {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00},
    /* 0x46 'F' */ {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00},
    /* 0x47 'G' */ {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00},
    /* 0x48 'H' */ {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00},
    /* 0x49 'I' */ {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    /* 0x4A 'J' */ {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00},
    /* 0x4B 'K' */ {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00},
    /* 0x4C 'L' */ {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00},
    /* 0x4D 'M' */ {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00},
    /* 0x4E 'N' */ {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00},
    /* 0x4F 'O' */ {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00},
    /* 0x50 'P' */ {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00},
    /* 0x51 'Q' */ {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00},
    /* 0x52 'R' */ {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00},
    /* 0x53 'S' */ {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00},
    /* 0x54 'T' */ {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    /* 0x55 'U' */ {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00},
    /* 0x56 'V' */ {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00},
    /* 0x57 'W' */ {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
    /* 0x58 'X' */ {0x63,0x36,0x1C,0x1C,0x1C,0x36,0x63,0x00},
    /* 0x59 'Y' */ {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00},
    /* 0x5A 'Z' */ {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00},
    /* For brevity here: in this workspace, copy the full table from minesgfx.c if you need every glyph. */
    /* 0x5B '[' */ {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00},
    /* 0x5C '\\' */ {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00},
    /* 0x5D ']' */ {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00},
    /* 0x5E '^' */ {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00},
    /* 0x5F '_' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
    /* 0x60 '`' */ {0x06,0x06,0x0C,0x00,0x00,0x00,0x00,0x00},
    /* 0x61 'a' */ {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00},
    /* 0x62 'b' */ {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00},
    /* 0x63 'c' */ {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00},
    /* 0x64 'd' */ {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00},
    /* 0x65 'e' */ {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00},
    /* 0x66 'f' */ {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00},
    /* 0x67 'g' */ {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F},
    /* 0x68 'h' */ {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00},
    /* 0x69 'i' */ {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00},
    /* 0x6A 'j' */ {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E},
    /* 0x6B 'k' */ {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00},
    /* 0x6C 'l' */ {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    /* 0x6D 'm' */ {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00},
    /* 0x6E 'n' */ {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00},
    /* 0x6F 'o' */ {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00},
    /* 0x70 'p' */ {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F},
    /* 0x71 'q' */ {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78},
    /* 0x72 'r' */ {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00},
    /* 0x73 's' */ {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00},
    /* 0x74 't' */ {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00},
    /* 0x75 'u' */ {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00},
    /* 0x76 'v' */ {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00},
    /* 0x77 'w' */ {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00},
    /* 0x78 'x' */ {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00},
    /* 0x79 'y' */ {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F},
    /* 0x7A 'z' */ {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00},
    /* 0x7B '{' */ {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00},
    /* 0x7C '|' */ {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},
    /* 0x7D '}' */ {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00},
    /* 0x7E '~' */ {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x7F DEL */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

static void put_px(uint8_t *fb, uint32_t pitch, uint32_t sw, uint32_t sh, int32_t x, int32_t y, uint32_t c) {
    if ((uint32_t)x >= sw || (uint32_t)y >= sh) return;
    uint32_t *row = (uint32_t *)(fb + (uint64_t)y * pitch);
    row[x] = c;
}

static void draw_char8(uint8_t *fb, uint32_t pitch, uint32_t sw, uint32_t sh,
                       int32_t x, int32_t y, char ch, uint32_t fg) {
    if (ch < 32 || ch > 127) ch = '?';
    const uint8_t *glyph = font8x8_basic[(int)ch - 32];
    for (int yy = 0; yy < 8; yy++) {
        uint8_t row = glyph[yy];
        for (int xx = 0; xx < 8; xx++) {
            if (row & (1u << xx)) {
                put_px(fb, pitch, sw, sh, x + xx, y + yy, fg);
            }
        }
    }
}

static void draw_text(uint8_t *fb, uint32_t pitch, uint32_t sw, uint32_t sh,
                      int32_t x, int32_t y, const char *s, uint32_t fg) {
    int32_t cx = x;
    for (size_t i = 0; s && s[i]; i++) {
        draw_char8(fb, pitch, sw, sh, cx, y, s[i], fg);
        cx += 8;
    }
}

/* calculator state */
typedef struct {
    int have_acc;
    int64_t acc;
    int64_t cur;
    char op;
    int entering;
    int error;
} calc_t;

static void calc_clear(calc_t *c) {
    memset(c, 0, sizeof(*c));
    c->have_acc = 0;
    c->acc = 0;
    c->cur = 0;
    c->op = 0;
    c->entering = 0;
    c->error = 0;
}

static int64_t do_op(int64_t a, int64_t b, char op, int *err) {
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/':
            if (b == 0) { *err = 1; return 0; }
            return a / b;
        default: return b;
    }
}

static void calc_input_digit(calc_t *c, int d) {
    if (c->error) return;
    if (!c->entering) {
        c->cur = 0;
        c->entering = 1;
    }
    c->cur = c->cur * 10 + d;
}

static void calc_apply_op(calc_t *c, char op) {
    if (c->error) return;

    if (!c->have_acc) {
        c->acc = c->cur;
        c->have_acc = 1;
        c->op = op;
        c->entering = 0;
        return;
    }

    int err = 0;
    c->acc = do_op(c->acc, c->cur, c->op ? c->op : op, &err);
    if (err) { c->error = 1; return; }

    c->cur = c->acc;
    c->op = op;
    c->entering = 0;
}

static void calc_equals(calc_t *c) {
    if (c->error) return;
    if (!c->have_acc) return;

    int err = 0;
    c->acc = do_op(c->acc, c->cur, c->op, &err);
    if (err) { c->error = 1; return; }

    c->cur = c->acc;
    c->op = 0;
    c->entering = 0;
}

typedef struct { const char *label; char key; } btn_t;

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    md64api_grp_video_info_t vi;
    memset(&vi, 0, sizeof(vi));
    if (md64api_grp_get_video0_info(&vi) != 0) {
        puts_raw("calcgfx: cannot read video info\n");
        return 1;
    }
    if (vi.mode != MD64API_GRP_MODE_GRAPHICS || vi.bpp != 32) {
        puts_raw("calcgfx: requires 32bpp graphics\n");
        return 0;
    }

    uint32_t pitch = vi.width * 4u;
    uint32_t buf_size = pitch * vi.height;
    uint8_t *bb = (uint8_t*)malloc(buf_size);
    if (!bb) return 2;

    int efd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    if (efd < 0) { free(bb); return 3; }

    calc_t calc;
    calc_clear(&calc);

    cursor_img_t cursor;
    int have_cursor = (load_cursor_bmp_rgba8888("/ModuOS/shared/usr/assets/mouse/arrow.bmp", &cursor) == 0);

    /* Layout */
    int w = 260;
    int h = 360;
    int ox = (int)(vi.width/2u) - w/2;
    int oy = (int)(vi.height/2u) - h/2;

    int disp_h = 50;
    int btn_w = 60;
    int btn_h = 44;
    int pad = 8;

    btn_t grid[5][4] = {
        { {"C", 'C'}, {"<-", 'B'}, {"/", '/'}, {"*", '*'} },
        { {"7", '7'}, {"8", '8'}, {"9", '9'}, {"-", '-'} },
        { {"4", '4'}, {"5", '5'}, {"6", '6'}, {"+", '+'} },
        { {"1", '1'}, {"2", '2'}, {"3", '3'}, {"=", '='} },
        { {"0", '0'}, {" ", 0},  {" ", 0},  {" ", 0} },
    };

    int32_t mx = (int32_t)(vi.width/2u);
    int32_t my = (int32_t)(vi.height/2u);
    uint8_t buttons = 0, prev_buttons = 0;

    for (;;) {
        for (;;) {
            Event e;
            ssize_t n = read(efd, &e, sizeof(e));
            if (n != (ssize_t)sizeof(e)) break;

            if (e.type == EVENT_KEY_PRESSED && e.data.keyboard.keycode == KEY_ESCAPE) {
                close(efd);
                free(bb);
                return 0;
            }

            if (e.type == EVENT_CHAR_INPUT) {
                char c = e.data.keyboard.ascii;
                if (c >= '0' && c <= '9') calc_input_digit(&calc, c - '0');
                else if (c=='+'||c=='-'||c=='*'||c=='/') calc_apply_op(&calc, c);
                else if (c=='='||c=='\n') calc_equals(&calc);
                else if (c=='c'||c=='C') calc_clear(&calc);
            }

            if (e.type == EVENT_MOUSE_MOVE) {
                mx += e.data.mouse.delta_x;
                my += e.data.mouse.delta_y;
                if (mx < 0) mx = 0;
                if (my < 0) my = 0;
                if ((uint32_t)mx >= vi.width) mx = (int32_t)vi.width - 1;
                if ((uint32_t)my >= vi.height) my = (int32_t)vi.height - 1;
                buttons = e.data.mouse.buttons;
            } else if (e.type == EVENT_MOUSE_BUTTON) {
                buttons = e.data.mouse.buttons;
            }
        }

        int left_now = (buttons & 1) != 0;
        int left_prev = (prev_buttons & 1) != 0;
        int left_pressed = left_now && !left_prev;

        if (left_pressed) {
            /* hit-test buttons */
            int by0 = oy + disp_h + pad;
            for (int r = 0; r < 5; r++) {
                for (int c = 0; c < 4; c++) {
                    if (!grid[r][c].key) continue;
                    int bx0 = ox + pad + c * (btn_w + 6);
                    int by = by0 + r * (btn_h + 6);
                    int bw = btn_w;
                    int bh = btn_h;
                    if (r == 4 && c == 0) bw = btn_w * 2 + 6;
                    if (mx >= bx0 && mx < bx0 + bw && my >= by && my < by + bh) {
                        char key = grid[r][c].key;
                        if (key >= '0' && key <= '9') calc_input_digit(&calc, key - '0');
                        else if (key=='+'||key=='-'||key=='*'||key=='/') calc_apply_op(&calc, key);
                        else if (key=='=') calc_equals(&calc);
                        else if (key=='C') calc_clear(&calc);
                        else if (key=='B') {
                            if (calc.entering && !calc.error) calc.cur /= 10;
                        }
                    }
                }
            }
        }

        prev_buttons = buttons;

        /* draw */
        fill_rect(bb, pitch, vi.width, vi.height, 0, 0, (int32_t)vi.width, (int32_t)vi.height, pack_xrgb8888(192,192,192));
        fill_rect(bb, pitch, vi.width, vi.height, ox, oy, w, h, pack_xrgb8888(180,180,180));

        /* display */
        int dx = ox + pad;
        int dy = oy + pad;
        int dw = w - 2*pad;
        fill_rect(bb, pitch, vi.width, vi.height, dx, dy, dw, disp_h, pack_xrgb8888(230,230,230));
        fill_rect(bb, pitch, vi.width, vi.height, dx, dy, dw, 2, pack_xrgb8888(110,110,110));
        fill_rect(bb, pitch, vi.width, vi.height, dx, dy, 2, disp_h, pack_xrgb8888(110,110,110));

        char out[64];
        if (calc.error) {
            strcpy(out, "Error");
        } else {
            /* Display like a classic calculator: show expression if operator active */
            char a[24];
            char b[24];
            itoa((int)calc.acc, a, 10);
            itoa((int)calc.cur, b, 10);
            if (calc.have_acc && calc.op) {
                if (calc.entering) {
                    /* e.g. "2-1" */
                    strcpy(out, a);
                    size_t len = strlen(out);
                    out[len++] = calc.op;
                    out[len] = 0;
                    strcat(out, b);
                } else {
                    /* e.g. "2-" */
                    strcpy(out, a);
                    size_t len = strlen(out);
                    out[len++] = calc.op;
                    out[len] = 0;
                }
            } else {
                strcpy(out, b);
            }
        }
        draw_text(bb, pitch, vi.width, vi.height, dx + 8, dy + 18, out, pack_xrgb8888(0,0,0));

        /* buttons */
        int by0 = oy + disp_h + pad;
        for (int r = 0; r < 5; r++) {
            for (int c = 0; c < 4; c++) {
                if (!grid[r][c].key) continue;
                int bx0 = ox + pad + c * (btn_w + 6);
                int by = by0 + r * (btn_h + 6);
                int bw = btn_w;
                int bh = btn_h;
                if (r == 4 && c == 0) bw = btn_w * 2 + 6;

                draw_button(bb, pitch, vi.width, vi.height, bx0, by, bw, bh, 0);
                draw_text(bb, pitch, vi.width, vi.height, bx0 + bw/2 - 4, by + bh/2 - 4, grid[r][c].label, pack_xrgb8888(0,0,0));
            }
        }

        draw_text(bb, pitch, vi.width, vi.height, ox + 8, oy - 14, "Calculator", pack_xrgb8888(0,0,0));

        if (have_cursor) {
            alpha_blit_cursor_xrgb8888(bb, pitch, vi.width, vi.height, &cursor, mx, my);
        }

        (void)gfx_blit(bb, (uint16_t)vi.width, (uint16_t)vi.height, 0, 0, (uint16_t)pitch, (uint16_t)MD64API_GRP_FMT_XRGB8888);
        yield();
    }
}
