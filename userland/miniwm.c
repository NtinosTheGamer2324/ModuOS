#include "libc.h"
#include "NodGL.h"
#include "string.h"
#include "../include/moduos/kernel/events/events.h"

/*
 * miniwm.sqr
 *
 * Minimal window-manager-like demo (seed for X11-ish work):
 *  - gradient background
 *  - draggable windows with title bars
 *  - focus + raise-on-click (z-order)
 *  - transparent 16x16 cursor (arrow.bmp)
 */

#define CURSOR_W 16u
#define CURSOR_H 16u
#define TITLE_H  18
#define BORDER   2

typedef struct {
    uint32_t w;
    uint32_t h;
    uint8_t rgba[CURSOR_W * CURSOR_H * 4u];
} cursor_img_t;

typedef struct {
    int32_t x, y;
    int32_t w, h;
    uint32_t bg; /* stored as xrgb8888 */
    char title[24];
} Win;

/* Embedded 8x8 font (ASCII 32..127). Source: public domain font8x8.
 * Copied from sysmon.c (kept local so this stays a single-file app).
 */
static const uint8_t font8x8_basic[96][8] = {
    /* 0x20 ' ' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x21 '!' */ {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00},
    /* 0x22 '"' */ {0x36,0x36,0x24,0x00,0x00,0x00,0x00,0x00},
    /* 0x23 '#' */ {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},
    /* 0x24 '$' */ {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00},
    /* 0x25 '%' */ {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00},
    /* 0x '&' */ {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00},
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
    /* 0x7A 'z' */ {0x00,0x00,0x3F,0x19,0x0C,0x00,0x3F,0x00},
    /* 0x7B '{' */ {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00},
    /* 0x7C '|' */ {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},
    /* 0x7D '}' */ {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00},
    /* 0x7E '~' */ {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x7F DEL */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

static inline int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline uint32_t pack_xrgb8888(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline uint16_t pack_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t rr = (uint16_t)((r * 31u) / 255u);
    uint16_t gg = (uint16_t)((g * 63u) / 255u);
    uint16_t bb = (uint16_t)((b * 31u) / 255u);
    return (uint16_t)((rr << 11) | (gg << 5) | (bb));
}

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
    uint32_t comp = rd32(buf + 30);

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
            /* BI_RGB assumed: B,G,R,A */
            uint8_t b = (uint8_t)(px & 0xFF);
            uint8_t g = (uint8_t)((px >> 8) & 0xFF);
            uint8_t r = (uint8_t)((px >> 16) & 0xFF);
            uint8_t a = (uint8_t)((px >> 24) & 0xFF);
            (void)comp; /* ignore for now */

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

static void alpha_blit_cursor_rgb565(uint8_t *dst, uint32_t pitch, uint32_t w, uint32_t h,
                                     const cursor_img_t *cur, int32_t x, int32_t y) {
    for (uint32_t cy = 0; cy < cur->h; cy++) {
        int32_t dy = y + (int32_t)cy;
        if ((uint32_t)dy >= h) continue;
        uint16_t *row = (uint16_t *)(dst + (uint64_t)dy * pitch);

        for (uint32_t cx = 0; cx < cur->w; cx++) {
            int32_t dx = x + (int32_t)cx;
            if ((uint32_t)dx >= w) continue;

            uint32_t si = (cy * cur->w + cx) * 4u;
            uint8_t sr = cur->rgba[si + 0];
            uint8_t sg = cur->rgba[si + 1];
            uint8_t sb = cur->rgba[si + 2];
            uint8_t sa = cur->rgba[si + 3];
            if (sa == 0) continue;

            uint16_t dp = row[dx];
            uint8_t dr = (uint8_t)(((dp >> 11) & 31u) * 255u / 31u);
            uint8_t dg = (uint8_t)(((dp >> 5) & 63u) * 255u / 63u);
            uint8_t db = (uint8_t)((dp & 31u) * 255u / 31u);

            uint32_t a = sa;
            uint32_t ia = 255u - a;
            uint8_t or = (uint8_t)((sr * a + dr * ia) / 255u);
            uint8_t og = (uint8_t)((sg * a + dg * ia) / 255u);
            uint8_t ob = (uint8_t)((sb * a + db * ia) / 255u);

            row[dx] = pack_rgb565(or, og, ob);
        }
    }
}

static void fill_rect_xrgb8888(uint8_t *fb, uint32_t pitch, uint32_t sw, uint32_t sh,
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

static void fill_rect_rgb565(uint8_t *fb, uint32_t pitch, uint32_t sw, uint32_t sh,
                             int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c) {
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= (int32_t)sw || y >= (int32_t)sh) return;
    if (x + w > (int32_t)sw) w = (int32_t)sw - x;
    if (y + h > (int32_t)sh) h = (int32_t)sh - y;

    for (int32_t yy = 0; yy < h; yy++) {
        uint16_t *row = (uint16_t *)(fb + (uint64_t)(y + yy) * pitch);
        for (int32_t xx = 0; xx < w; xx++) row[x + xx] = c;
    }
}

static void put_px_xrgb8888(uint8_t *fb, uint32_t pitch, uint32_t sw, uint32_t sh,
                            int32_t x, int32_t y, uint32_t c) {
    if ((uint32_t)x >= sw || (uint32_t)y >= sh) return;
    uint32_t *row = (uint32_t *)(fb + (uint64_t)y * pitch);
    row[x] = c;
}

static void put_px_rgb565(uint8_t *fb, uint32_t pitch, uint32_t sw, uint32_t sh,
                          int32_t x, int32_t y, uint16_t c) {
    if ((uint32_t)x >= sw || (uint32_t)y >= sh) return;
    uint16_t *row = (uint16_t *)(fb + (uint64_t)y * pitch);
    row[x] = c;
}

static void draw_char8_xrgb8888(uint8_t *fb, uint32_t pitch, uint32_t sw, uint32_t sh,
                                int32_t x, int32_t y, char ch, uint32_t fg) {
    if (ch < 32 || ch > 127) ch = '?';
    const uint8_t *glyph = font8x8_basic[(int)ch - 32];
    for (int yy = 0; yy < 8; yy++) {
        uint8_t row = glyph[yy];
        for (int xx = 0; xx < 8; xx++) {
            if (row & (1u << xx)) {
                put_px_xrgb8888(fb, pitch, sw, sh, x + xx, y + yy, fg);
            }
        }
    }
}

static void draw_text_xrgb8888(uint8_t *fb, uint32_t pitch, uint32_t sw, uint32_t sh,
                               int32_t x, int32_t y, const char *s, uint32_t fg, int32_t clip_w) {
    int32_t cx = x;
    for (size_t i = 0; s && s[i]; i++) {
        if (clip_w > 0 && (cx - x + 8) > clip_w) break;
        draw_char8_xrgb8888(fb, pitch, sw, sh, cx, y, s[i], fg);
        cx += 8;
    }
}

static void draw_char8_rgb565(uint8_t *fb, uint32_t pitch, uint32_t sw, uint32_t sh,
                              int32_t x, int32_t y, char ch, uint16_t fg) {
    if (ch < 32 || ch > 127) ch = '?';
    const uint8_t *glyph = font8x8_basic[(int)ch - 32];
    for (int yy = 0; yy < 8; yy++) {
        uint8_t row = glyph[yy];
        for (int xx = 0; xx < 8; xx++) {
            if (row & (1u << xx)) {
                put_px_rgb565(fb, pitch, sw, sh, x + xx, y + yy, fg);
            }
        }
    }
}

static void draw_text_rgb565(uint8_t *fb, uint32_t pitch, uint32_t sw, uint32_t sh,
                             int32_t x, int32_t y, const char *s, uint16_t fg, int32_t clip_w) {
    int32_t cx = x;
    for (size_t i = 0; s && s[i]; i++) {
        if (clip_w > 0 && (cx - x + 8) > clip_w) break;
        draw_char8_rgb565(fb, pitch, sw, sh, cx, y, s[i], fg);
        cx += 8;
    }
}

static void draw_gradient_xrgb8888(uint8_t *fb, uint32_t pitch, uint32_t w, uint32_t h, uint32_t t) {
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(fb + (uint64_t)y * pitch);
        for (uint32_t x = 0; x < w; x++) {
            uint8_t r = (uint8_t)(((x + (t >> 2)) * 255u) / (w ? w : 1));
            uint8_t g = (uint8_t)(((y + (t >> 3)) * 255u) / (h ? h : 1));
            uint8_t b = (uint8_t)(((x ^ y ^ (t >> 1)) & 0xFF));
            row[x] = pack_xrgb8888(r, g, b);
        }
    }
}

static void draw_gradient_rgb565(uint8_t *fb, uint32_t pitch, uint32_t w, uint32_t h, uint32_t t) {
    for (uint32_t y = 0; y < h; y++) {
        uint16_t *row = (uint16_t *)(fb + (uint64_t)y * pitch);
        for (uint32_t x = 0; x < w; x++) {
            uint8_t r = (uint8_t)(((x + (t >> 2)) * 255u) / (w ? w : 1));
            uint8_t g = (uint8_t)(((y + (t >> 3)) * 255u) / (h ? h : 1));
            uint8_t b = (uint8_t)(((x ^ y ^ (t >> 1)) & 0xFF));
            row[x] = pack_rgb565(r, g, b);
        }
    }
}

static int win_hit(const Win *w, int32_t mx, int32_t my) {
    return mx >= w->x && my >= w->y && mx < (w->x + w->w) && my < (w->y + w->h);
}

static int win_hit_title(const Win *w, int32_t mx, int32_t my) {
    return mx >= w->x && my >= w->y && mx < (w->x + w->w) && my < (w->y + TITLE_H);
}

static int win_hit_resize_handle(const Win *w, int32_t mx, int32_t my) {
    /* bottom-right resize handle hit (8x8) */
    int32_t hx = w->x + w->w - 10;
    int32_t hy = w->y + w->h - 10;
    return (mx >= hx && my >= hy && mx < (hx + 8) && my < (hy + 8));
}

static void win_raise(Win *wins, int count, int idx) {
    if (idx < 0 || idx >= count) return;
    Win tmp = wins[idx];
    for (int i = idx; i < count - 1; i++) wins[i] = wins[i + 1];
    wins[count - 1] = tmp;
}

static void poll_events(int efd, int32_t *mx, int32_t *my, uint8_t *buttons,
                        int32_t *out_dx, int32_t *out_dy,
                        uint32_t sw, uint32_t sh, int *want_exit) {
    int32_t dx = 0;
    int32_t dy = 0;

    for (;;) {
        Event e;
        ssize_t n = read(efd, &e, sizeof(e));
        if (n != (ssize_t)sizeof(e)) break;

        if (e.type == EVENT_KEY_PRESSED && e.data.keyboard.keycode == KEY_ESCAPE) {
            *want_exit = 1;
        } else if (e.type == EVENT_MOUSE_MOVE) {
            dx += e.data.mouse.delta_x;
            dy += e.data.mouse.delta_y;
        } else if (e.type == EVENT_MOUSE_BUTTON) {
            *buttons = e.data.mouse.buttons;
        }
    }

    if (dx || dy) {
        *mx += dx;
        *my += dy;
        *mx = clamp_i32(*mx, 0, (int32_t)sw - 1);
        *my = clamp_i32(*my, 0, (int32_t)sh - 1);
    }

    if (out_dx) *out_dx = dx;
    if (out_dy) *out_dy = dy;
}

static void render_windows_xrgb8888(uint8_t *bb, uint32_t pitch, uint32_t sw, uint32_t sh, const Win *wins, int count) {
    for (int i = 0; i < count; i++) {
        const Win *w = &wins[i];
        int focused = (i == count - 1);

        uint32_t border = focused ? pack_xrgb8888(140, 170, 255) : pack_xrgb8888(10, 10, 12);
        uint32_t title  = focused ? pack_xrgb8888(55, 80, 130) : pack_xrgb8888(35, 45, 65);
        uint32_t body   = w->bg;

        /* Drop shadow */
        fill_rect_xrgb8888(bb, pitch, sw, sh, w->x + 6, w->y + 6, w->w, w->h, pack_xrgb8888(0, 0, 0));

        /* Window background */
        fill_rect_xrgb8888(bb, pitch, sw, sh, w->x, w->y, w->w, w->h, body);
        /* Title bar */
        fill_rect_xrgb8888(bb, pitch, sw, sh, w->x, w->y, w->w, TITLE_H, title);

        /* Title text */
        int32_t tx = w->x + 8;
        int32_t ty = w->y + (TITLE_H - 8) / 2;
        int32_t clip = w->w - 16;
        if (clip > 0) {
            draw_text_xrgb8888(bb, pitch, sw, sh, tx, ty, w->title, pack_xrgb8888(255, 255, 255), clip);
        }

        /* Resize handle (bottom-right) */
        fill_rect_xrgb8888(bb, pitch, sw, sh, w->x + w->w - 10, w->y + w->h - 10, 8, 8,
                           focused ? pack_xrgb8888(255, 220, 80) : pack_xrgb8888(80, 90, 120));

        /* Borders */
        fill_rect_xrgb8888(bb, pitch, sw, sh, w->x, w->y, w->w, BORDER, border);
        fill_rect_xrgb8888(bb, pitch, sw, sh, w->x, w->y + w->h - BORDER, w->w, BORDER, border);
        fill_rect_xrgb8888(bb, pitch, sw, sh, w->x, w->y, BORDER, w->h, border);
        fill_rect_xrgb8888(bb, pitch, sw, sh, w->x + w->w - BORDER, w->y, BORDER, w->h, border);

        /* Divider under title */
        fill_rect_xrgb8888(bb, pitch, sw, sh, w->x, w->y + TITLE_H, w->w, 1, pack_xrgb8888(80, 90, 120));
    }
}

static void render_windows_rgb565(uint8_t *bb, uint32_t pitch, uint32_t sw, uint32_t sh, const Win *wins, int count) {
    for (int i = 0; i < count; i++) {
        const Win *w = &wins[i];
        int focused = (i == count - 1);

        uint16_t border = focused ? pack_rgb565(140, 170, 255) : pack_rgb565(10, 10, 12);
        uint16_t title  = focused ? pack_rgb565(55, 80, 130) : pack_rgb565(35, 45, 65);
        uint16_t body   = pack_rgb565((uint8_t)((w->bg >> 16) & 0xFF), (uint8_t)((w->bg >> 8) & 0xFF), (uint8_t)(w->bg & 0xFF));

        fill_rect_rgb565(bb, pitch, sw, sh, w->x + 6, w->y + 6, w->w, w->h, pack_rgb565(0, 0, 0));
        fill_rect_rgb565(bb, pitch, sw, sh, w->x, w->y, w->w, w->h, body);
        fill_rect_rgb565(bb, pitch, sw, sh, w->x, w->y, w->w, TITLE_H, title);

        /* Title text */
        int32_t tx = w->x + 8;
        int32_t ty = w->y + (TITLE_H - 8) / 2;
        int32_t clip = w->w - 16;
        if (clip > 0) {
            draw_text_rgb565(bb, pitch, sw, sh, tx, ty, w->title, pack_rgb565(255, 255, 255), clip);
        }

        /* Resize handle (bottom-right) */
        fill_rect_rgb565(bb, pitch, sw, sh, w->x + w->w - 10, w->y + w->h - 10, 8, 8,
                         focused ? pack_rgb565(255, 220, 80) : pack_rgb565(80, 90, 120));

        fill_rect_rgb565(bb, pitch, sw, sh, w->x, w->y, w->w, BORDER, border);
        fill_rect_rgb565(bb, pitch, sw, sh, w->x, w->y + w->h - BORDER, w->w, BORDER, border);
        fill_rect_rgb565(bb, pitch, sw, sh, w->x, w->y, BORDER, w->h, border);
        fill_rect_rgb565(bb, pitch, sw, sh, w->x + w->w - BORDER, w->y, BORDER, w->h, border);

        fill_rect_rgb565(bb, pitch, sw, sh, w->x, w->y + TITLE_H, w->w, 1, pack_rgb565(80, 90, 120));
    }
}

int md_main(long argc, char **argv) {
    (void)argc;
    (void)argv;

    md64api_grp_video_info_t vi;
    memset(&vi, 0, sizeof(vi));
    if (md64api_grp_get_video0_info(&vi) != 0) {
        puts_raw("miniwm: cannot read video info\n");
        return 1;
    }

    if (vi.mode != MD64API_GRP_MODE_GRAPHICS || vi.width == 0 || vi.height == 0) {
        puts_raw("miniwm: not in graphics mode\n");
        return 0;
    }

    NodGL_Device device;
    NodGL_Context ctx;
    if (NodGL_CreateDevice(NodGL_FEATURE_LEVEL_1_0, &device, &ctx, NULL) != NodGL_OK) {
        puts_raw("miniwm: NodGL init failed\n");
        return 1;
    }

    NodGL_TextureDesc tex_desc = {
        .width = vi.width,
        .height = vi.height,
        .format = NodGL_FORMAT_R8G8B8A8_UNORM,
        .mip_levels = 1,
        .initial_data = NULL,
        .initial_data_size = 0
    };

    NodGL_Texture backbuffer_tex;
    if (NodGL_CreateTexture(device, &tex_desc, &backbuffer_tex) != NodGL_OK) {
        NodGL_ReleaseDevice(device);
        return 2;
    }

    uint8_t *bb;
    uint32_t pitch;
    if (NodGL_MapResource(ctx, backbuffer_tex, (void**)&bb, &pitch) != NodGL_OK) {
        NodGL_ReleaseResource(device, backbuffer_tex);
        NodGL_ReleaseDevice(device);
        return 2;
    }

    uint8_t fmt = MD64API_GRP_FMT_XRGB8888;

    int efd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    if (efd < 0) {
        NodGL_ReleaseResource(device, backbuffer_tex);
        NodGL_ReleaseDevice(device);
        puts_raw("miniwm: cannot open $/dev/input/event0\n");
        return 3;
    }

    cursor_img_t cursor;
    int have_cursor = (load_cursor_bmp_rgba8888("/ModuOS/shared/usr/assets/mouse/arrow.bmp", &cursor) == 0);

    Win wins[3];
    memset(wins, 0, sizeof(wins));
    wins[0] = (Win){ .x = 40,  .y = 50,  .w = 220, .h = 160, .bg = pack_xrgb8888(25, 25, 30) };
    strcpy(wins[0].title, "Terminal");
    wins[1] = (Win){ .x = 140, .y = 120, .w = 260, .h = 180, .bg = pack_xrgb8888(22, 28, 38) };
    strcpy(wins[1].title, "Files");
    wins[2] = (Win){ .x = 260, .y = 80,  .w = 240, .h = 140, .bg = pack_xrgb8888(28, 22, 36) };
    strcpy(wins[2].title, "Settings");

    int32_t mx = (int32_t)(vi.width / 2u);
    int32_t my = (int32_t)(vi.height / 2u);
    uint8_t buttons = 0;
    uint8_t prev_buttons = 0;

    /* Drag state */
    enum { DRAG_NONE = 0, DRAG_MOVE = 1, DRAG_RESIZE = 2 };
    int drag_mode = DRAG_NONE;
    int drag_idx = -1;
    int32_t drag_off_x = 0;
    int32_t drag_off_y = 0;
    int32_t drag_start_w = 0;
    int32_t drag_start_h = 0;

    puts_raw("miniwm: drag windows with left mouse. ESC exits.\n");

    int want_exit = 0;
    while (!want_exit) {
        int32_t dx = 0, dy = 0;
        poll_events(efd, &mx, &my, &buttons, &dx, &dy, vi.width, vi.height, &want_exit);

        int left_now = (buttons & 1) != 0;
        int left_prev = (prev_buttons & 1) != 0;
        int left_pressed = left_now && !left_prev;
        int left_released = !left_now && left_prev;

        if (left_pressed) {
            /* find topmost hit window */
            int hit = -1;
            for (int i = 2; i >= 0; i--) {
                if (win_hit(&wins[i], mx, my)) { hit = i; break; }
            }
            if (hit >= 0) {
                /* raise to top */
                win_raise(wins, 3, hit);
                hit = 2;

                if (win_hit_resize_handle(&wins[hit], mx, my)) {
                    drag_mode = DRAG_RESIZE;
                    drag_idx = hit;
                    drag_start_w = wins[hit].w;
                    drag_start_h = wins[hit].h;
                } else if (win_hit_title(&wins[hit], mx, my)) {
                    drag_mode = DRAG_MOVE;
                    drag_idx = hit;
                    drag_off_x = mx - wins[hit].x;
                    drag_off_y = my - wins[hit].y;
                }
            }
        }

        if (drag_mode == DRAG_MOVE && left_now) {
            Win *w = &wins[drag_idx];
            w->x = mx - drag_off_x;
            w->y = my - drag_off_y;
            w->x = clamp_i32(w->x, 0, (int32_t)vi.width - w->w);
            w->y = clamp_i32(w->y, 0, (int32_t)vi.height - w->h);
        } else if (drag_mode == DRAG_RESIZE && left_now) {
            Win *w = &wins[drag_idx];

            /* Resize uses mouse deltas since drag start; this is stable even if absolute drifts. */
            w->w += dx;
            w->h += dy;

            /* Minimum size (leave room for title bar + handle) */
            if (w->w < 96) w->w = 96;
            if (w->h < (TITLE_H + 40)) w->h = (TITLE_H + 40);

            /* Clamp so it stays within screen */
            if (w->x + w->w > (int32_t)vi.width)  w->w = (int32_t)vi.width - w->x;
            if (w->y + w->h > (int32_t)vi.height) w->h = (int32_t)vi.height - w->y;
        }

        if (left_released) {
            drag_mode = DRAG_NONE;
            drag_idx = -1;
        }

        uint32_t t = (uint32_t)time_ms();
        if (fmt == MD64API_GRP_FMT_XRGB8888 && vi.bpp == 32) {
            draw_gradient_xrgb8888(bb, pitch, vi.width, vi.height, t);
            render_windows_xrgb8888(bb, pitch, vi.width, vi.height, wins, 3);
            if (have_cursor) alpha_blit_cursor_xrgb8888(bb, pitch, vi.width, vi.height, &cursor, mx, my);
        } else if (fmt == MD64API_GRP_FMT_RGB565 && vi.bpp == 16) {
            draw_gradient_rgb565(bb, pitch, vi.width, vi.height, t);
            render_windows_rgb565(bb, pitch, vi.width, vi.height, wins, 3);
            if (have_cursor) alpha_blit_cursor_rgb565(bb, pitch, vi.width, vi.height, &cursor, mx, my);
        } else {
            puts_raw("miniwm: unsupported fmt\n");
            break;
        }

        NodGL_DrawTexture(ctx, backbuffer_tex, 0, 0, 0, 0, vi.width, vi.height);
        NodGL_PresentContext(ctx, 0);
        yield();

        /* Reset prev-buttons unless updated by a button event (simple edge detection). */
        prev_buttons = buttons;
    }

    close(efd);
    NodGL_ReleaseResource(device, backbuffer_tex);
    NodGL_ReleaseDevice(device);
    puts_raw("miniwm: exit\n");
    return 0;
}



