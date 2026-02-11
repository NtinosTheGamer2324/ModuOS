#include "libc.h"
#include "string.h"
#include "../include/moduos/kernel/events/events.h"
#include "gfx2d.h"

/*
 * paintgfx.sqr
 *
 * Standalone classic MS-Paint-like app (paint-only MVP).
 *  - pencil + eraser + fill
 *  - color palette
 *  - brush sizes
 *
 * Controls:
 *  - Left click/drag: draw
 *  - Palette: left sets primary, right sets secondary
 *  - Eyedropper tool: click canvas to pick color (left->primary, right->secondary)
 *  - ESC: exit
 */

static inline uint32_t pack_xrgb8888(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

#define CURSOR_W 16u
#define CURSOR_H 16u

typedef struct {
    int32_t x, y, w, h;
} rect_i32_t;

static inline rect_i32_t rect_empty(void) {
    rect_i32_t r; r.x = 0; r.y = 0; r.w = 0; r.h = 0; return r;
}

static inline int rect_is_empty(rect_i32_t r) { return r.w <= 0 || r.h <= 0; }

static inline rect_i32_t rect_union(rect_i32_t a, rect_i32_t b) {
    if (rect_is_empty(a)) return b;
    if (rect_is_empty(b)) return a;
    int32_t x0 = a.x < b.x ? a.x : b.x;
    int32_t y0 = a.y < b.y ? a.y : b.y;
    int32_t x1 = (a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int32_t y1 = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    rect_i32_t r; r.x = x0; r.y = y0; r.w = x1 - x0; r.h = y1 - y0; return r;
}

static inline rect_i32_t rect_clip(rect_i32_t r, int32_t sw, int32_t sh) {
    if (rect_is_empty(r)) return rect_empty();
    int32_t x0 = r.x;
    int32_t y0 = r.y;
    int32_t x1 = r.x + r.w;
    int32_t y1 = r.y + r.h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > sw) x1 = sw;
    if (y1 > sh) y1 = sh;
    rect_i32_t out; out.x = x0; out.y = y0; out.w = x1 - x0; out.h = y1 - y0;
    if (out.w <= 0 || out.h <= 0) return rect_empty();
    return out;
}

static void copy_rect(uint8_t *dst, const uint8_t *src, uint32_t pitch, int32_t sw, int32_t sh, rect_i32_t r) {
    r = rect_clip(r, sw, sh);
    if (rect_is_empty(r)) return;
    uint32_t row_bytes = (uint32_t)r.w * 4u;
    for (int32_t yy = 0; yy < r.h; yy++) {
        uint8_t *drow = dst + (uint64_t)(r.y + yy) * pitch + (uint64_t)r.x * 4u;
        const uint8_t *srow = src + (uint64_t)(r.y + yy) * pitch + (uint64_t)r.x * 4u;
        memcpy(drow, srow, row_bytes);
    }
}

static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline int32_t rds32(const uint8_t *p) { return (int32_t)rd32(p); }

static inline uint32_t ctz32(uint32_t v) {
    uint32_t n = 0;
    if (!v) return 0;
    while ((v & 1u) == 0u) { v >>= 1u; n++; }
    return n;
}

static inline uint32_t popcnt32(uint32_t v) {
    uint32_t c = 0;
    while (v) { c += (v & 1u); v >>= 1u; }
    return c;
}

static inline uint8_t bf_to_u8(uint32_t v, uint32_t mask) {
    if (!mask) return 255;
    uint32_t s = ctz32(mask);
    uint32_t bits = popcnt32(mask);
    uint32_t x = (v & mask) >> s;
    if (bits == 0) return 0;
    if (bits >= 8) return (uint8_t)(x >> (bits - 8));
    return (uint8_t)((x * 255u + (((1u << bits) - 1u) / 2u)) / ((1u << bits) - 1u));
}

typedef struct {
    uint32_t w;
    uint32_t h;
    uint8_t rgba[CURSOR_W * CURSOR_H * 4u];
} cursor_img_t;

static void cursor_rgba_to_argb8888(const cursor_img_t *cur, uint32_t *out_argb /* CURSOR_W*CURSOR_H */) {
    for (uint32_t y = 0; y < cur->h; y++) {
        for (uint32_t x = 0; x < cur->w; x++) {
            uint32_t si = (y * cur->w + x) * 4u;
            uint8_t r = cur->rgba[si + 0];
            uint8_t g = cur->rgba[si + 1];
            uint8_t b = cur->rgba[si + 2];
            uint8_t a = cur->rgba[si + 3];
            out_argb[y * cur->w + x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}


typedef struct {
    uint32_t w;
    uint32_t h;
    uint32_t *argb; /* w*h pixels, A in top byte */
} icon_img_t;

static int load_icon_bmp_argb8888(const char *path, icon_img_t *out) {
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

    /* Paint.NET commonly writes 32bpp BMP as BI_BITFIELDS (comp=3) with explicit channel masks. */
    if (planes != 1 || (bpp != 24 && bpp != 32) || !((comp == 0) || (comp == 3 && bpp == 32))) {
        free(buf);
        return -8;
    }

    uint32_t mask_r = 0x00FF0000u;
    uint32_t mask_g = 0x0000FF00u;
    uint32_t mask_b = 0x000000FFu;
    uint32_t mask_a = 0xFF000000u;

    if (comp == 3) {
        /* BITFIELDS masks are either stored right after the BITMAPINFOHEADER (40 bytes) or inside v4/v5 headers.
           For both, offsets 40..56 from the DIB header start contain RGBA masks. */
        uint32_t m_off = 14u + 40u;
        if (m_off + 12u <= size) {
            mask_r = rd32(buf + m_off + 0);
            mask_g = rd32(buf + m_off + 4);
            mask_b = rd32(buf + m_off + 8);
            if (m_off + 16u <= size) mask_a = rd32(buf + m_off + 12);
            else mask_a = 0xFF000000u;
        }
    }


    int top_down = 0;
    if (height < 0) {
        top_down = 1;
        height = -height;
    }

    if (width <= 0 || height <= 0) {
        free(buf);
        return -9;
    }

    uint32_t bytes_per_px = (uint32_t)bpp / 8u;
    uint32_t src_row_stride = (uint32_t)width * bytes_per_px;
    /* BMP rows are padded to 4 bytes */
    uint32_t src_row_padded = (src_row_stride + 3u) & ~3u;

    if (pixel_off + (uint64_t)src_row_padded * (uint32_t)height > size) {
        free(buf);
        return -10;
    }

    out->w = (uint32_t)width;
    out->h = (uint32_t)height;
    out->argb = (uint32_t *)malloc((size_t)out->w * (size_t)out->h * 4u);
    if (!out->argb) {
        free(buf);
        return -11;
    }

    /* Heuristic: many 32bpp BMPs are stored with alpha bytes all 0 even though they should be opaque.
       Detect that and treat alpha as 255 in that case, while still honoring magenta-key transparency. */
    uint8_t max_a = 0;
    if (bytes_per_px == 4u) {
        for (uint32_t y = 0; y < out->h; y++) {
            uint32_t sy = top_down ? y : (out->h - 1u - y);
            const uint8_t *row = buf + pixel_off + (uint64_t)sy * src_row_padded;
            for (uint32_t x = 0; x < out->w; x++) {
                const uint8_t *p = row + (uint64_t)x * bytes_per_px;
                if (p[3] > max_a) max_a = p[3];
            }
        }
    }

    for (uint32_t y = 0; y < out->h; y++) {
        uint32_t sy = top_down ? y : (out->h - 1u - y);
        const uint8_t *row = buf + pixel_off + (uint64_t)sy * src_row_padded;
        for (uint32_t x = 0; x < out->w; x++) {
            const uint8_t *p = row + (uint64_t)x * bytes_per_px;

            uint8_t r = 0, g = 0, b = 0, a = 255;
            if (bytes_per_px == 3u) {
                b = p[0]; g = p[1]; r = p[2];
                a = 255;
            } else {
                uint32_t v = rd32(p);
                if (comp == 3) {
                    r = bf_to_u8(v, mask_r);
                    g = bf_to_u8(v, mask_g);
                    b = bf_to_u8(v, mask_b);
                    a = mask_a ? bf_to_u8(v, mask_a) : p[3];
                } else {
                    /* default BGRA */
                    b = p[0]; g = p[1]; r = p[2]; a = p[3];
                }
                if (max_a == 0) a = 255;
            }

            /* Treat magenta as transparent for classic UI assets */
            if (a == 0 || (r == 255 && g == 0 && b == 255)) {
                out->argb[y * out->w + x] = 0x00000000u;
            } else {
                out->argb[y * out->w + x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
    }

    free(buf);
    return 0;
}

static inline uint8_t blend8(uint8_t src, uint8_t dst, uint8_t a) {
    /* out = src*a + dst*(1-a) */
    return (uint8_t)((src * (uint32_t)a + dst * (uint32_t)(255u - a) + 127u) / 255u);
}

static void blit_icon_argb8888(uint8_t *dst, uint32_t pitch, uint32_t sw, uint32_t sh,
                               const icon_img_t *ico, int32_t x, int32_t y) {
    if (!ico || !ico->argb) return;
    for (uint32_t iy = 0; iy < ico->h; iy++) {
        int32_t dy = y + (int32_t)iy;
        if ((uint32_t)dy >= sh) continue;
        uint32_t *row = (uint32_t *)(dst + (uint64_t)dy * pitch);
        for (uint32_t ix = 0; ix < ico->w; ix++) {
            int32_t dx = x + (int32_t)ix;
            if ((uint32_t)dx >= sw) continue;

            uint32_t argb = ico->argb[iy * ico->w + ix];
            uint8_t a = (uint8_t)(argb >> 24);
            if (a == 0) continue;

            uint8_t sr = (uint8_t)(argb >> 16);
            uint8_t sg = (uint8_t)(argb >> 8);
            uint8_t sb = (uint8_t)(argb);

            if (a == 255) {
                row[dx] = pack_xrgb8888(sr, sg, sb);
            } else {
                uint32_t d = row[dx];
                uint8_t dr = (uint8_t)(d >> 16);
                uint8_t dg = (uint8_t)(d >> 8);
                uint8_t db = (uint8_t)(d);

                uint8_t or_ = blend8(sr, dr, a);
                uint8_t og  = blend8(sg, dg, a);
                uint8_t ob  = blend8(sb, db, a);
                row[dx] = pack_xrgb8888(or_, og, ob);
            }
        }
    }
}

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

static inline uint32_t *px_ptr(uint8_t *fb, uint32_t pitch, int32_t x, int32_t y) {
    return (uint32_t *)(fb + (uint64_t)y * pitch) + x;
}

static inline uint32_t *canvas_px(uint8_t *canvas, uint32_t canvas_pitch, int32_t x, int32_t y) {
    return (uint32_t *)(canvas + (uint64_t)y * canvas_pitch) + x;
}

static void brush_dot(uint8_t *fb, uint32_t pitch, uint32_t sw, uint32_t sh,
                      int32_t x, int32_t y, int radius, uint32_t col) {
    for (int yy = -radius; yy <= radius; yy++) {
        int32_t py = y + yy;
        if ((uint32_t)py >= sh) continue;
        for (int xx = -radius; xx <= radius; xx++) {
            int32_t px = x + xx;
            if ((uint32_t)px >= sw) continue;
            *px_ptr(fb, pitch, px, py) = col;
        }
    }
}

/* Simple flood fill (bounded) */
static void flood_fill(uint8_t *fb, uint32_t pitch, uint32_t sw, uint32_t sh,
                       int32_t sx, int32_t sy, uint32_t from, uint32_t to) {
    if (from == to) return;
    if ((uint32_t)sx >= sw || (uint32_t)sy >= sh) return;
    if (*px_ptr(fb, pitch, sx, sy) != from) return;

    typedef struct { int16_t x,y; } P;

    /* Use heap for the fill stack to avoid overflowing the user stack. */
    static P *stack = NULL;
    static int stack_cap = 0;
    if (!stack) {
        stack_cap = 32768; /* 128KB of points */
        stack = (P*)malloc((size_t)stack_cap * sizeof(P));
        if (!stack) return;
    }

    int sp = 0;
    stack[sp++] = (P){(int16_t)sx,(int16_t)sy};

    while (sp > 0) {
        P p = stack[--sp];
        int32_t x = p.x, y = p.y;
        if ((uint32_t)x >= sw || (uint32_t)y >= sh) continue;
        uint32_t *pp = px_ptr(fb, pitch, x, y);
        if (*pp != from) continue;
        *pp = to;

        if (sp < stack_cap - 4) {
            stack[sp++] = (P){(int16_t)(x+1),(int16_t)y};
            stack[sp++] = (P){(int16_t)(x-1),(int16_t)y};
            stack[sp++] = (P){(int16_t)x,(int16_t)(y+1)};
            stack[sp++] = (P){(int16_t)x,(int16_t)(y-1)};
        }
    }
}

/* Embedded 8x8 font (ASCII 32..127). Source: public domain font8x8.
 * Copied from minesgfx.c.
 */
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

static void draw_char8_style(uint8_t *fb, uint32_t pitch, uint32_t sw, uint32_t sh,
                              int32_t x, int32_t y, char ch, uint32_t fg,
                              int scale, int bold, int italic) {
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    if (ch < 32 || ch > 127) ch = '?';
    const uint8_t *glyph = font8x8_basic[(int)ch - 32];

    for (int yy = 0; yy < 8; yy++) {
        uint8_t row = glyph[yy];
        int slant = italic ? ((7 - yy) / 2) : 0;
        for (int xx = 0; xx < 8; xx++) {
            if (row & (1u << xx)) {
                int px = x + (xx + slant) * scale;
                int py = y + yy * scale;
                fill_rect(fb, pitch, sw, sh, px, py, scale, scale, fg);
                if (bold) fill_rect(fb, pitch, sw, sh, px + 1, py, scale, scale, fg);
            }
        }
    }
}

static void draw_text_style(uint8_t *fb, uint32_t pitch, uint32_t sw, uint32_t sh,
                            int32_t x, int32_t y, const char *s, uint32_t fg,
                            int scale, int bold, int italic) {
    int32_t cx = x;
    int adv = 8 * (scale < 1 ? 1 : scale);
    for (size_t i = 0; s && s[i]; i++) {
        draw_char8_style(fb, pitch, sw, sh, cx, y, s[i], fg, scale, bold, italic);
        cx += adv;
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

typedef enum {
    TOOL_PENCIL=0,
    TOOL_ERASER=1,
    TOOL_FILL=2,
    TOOL_EYEDROPPER=3,
    TOOL_BRUSH=4,
    TOOL_SPRAY=5,
    TOOL_LINE=6,
    TOOL_CURVE=7,
    TOOL_TEXT=8,
    TOOL_ZOOM=9,
    TOOL_SELECT_RECT=10,
    TOOL_MOVE=11,
    TOOL_MAGIC_SELECT=12,
} tool_t;

static const char *tool_name(tool_t t) {
    switch (t) {
        case TOOL_PENCIL: return "Pencil";
        case TOOL_ERASER: return "Eraser";
        case TOOL_FILL: return "Fill";
        case TOOL_EYEDROPPER: return "Pick";
        case TOOL_BRUSH: return "Brush";
        case TOOL_SPRAY: return "Spray";
        case TOOL_LINE: return "Line";
        case TOOL_CURVE: return "Curve";
        case TOOL_TEXT: return "Text";
        case TOOL_ZOOM: return "Zoom";
        case TOOL_SELECT_RECT: return "Select";
        case TOOL_MOVE: return "Move";
        case TOOL_MAGIC_SELECT: return "Magic";
    }
    return "Tool";
}

static uint32_t rng_state = 0xC001D00Du; //I am very cool
static inline uint32_t xorshift32(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}


static void draw_bevel_button(uint8_t *fb, uint32_t pitch, uint32_t sw, uint32_t sh,
                              int32_t x, int32_t y, int32_t w, int32_t h, int pressed) {
    uint32_t face = pack_xrgb8888(200,200,200);
    uint32_t hi = pack_xrgb8888(240,240,240);
    uint32_t lo = pack_xrgb8888(110,110,110);
    uint32_t mid = pack_xrgb8888(160,160,160);

    fill_rect(fb, pitch, sw, sh, x, y, w, h, face);

    /* classic Win9x-ish bevel */
    if (!pressed) {
        fill_rect(fb, pitch, sw, sh, x, y, w, 2, hi);
        fill_rect(fb, pitch, sw, sh, x, y, 2, h, hi);
        fill_rect(fb, pitch, sw, sh, x, y + h - 2, w, 2, lo);
        fill_rect(fb, pitch, sw, sh, x + w - 2, y, 2, h, lo);
    } else {
        fill_rect(fb, pitch, sw, sh, x, y, w, 2, lo);
        fill_rect(fb, pitch, sw, sh, x, y, 2, h, lo);
        fill_rect(fb, pitch, sw, sh, x, y + h - 2, w, 2, hi);
        fill_rect(fb, pitch, sw, sh, x + w - 2, y, 2, h, hi);
        /* inner shadow */
        fill_rect(fb, pitch, sw, sh, x + 2, y + 2, w - 4, 1, mid);
        fill_rect(fb, pitch, sw, sh, x + 2, y + 2, 1, h - 4, mid);
    }
}

static void draw_tool_icon(uint8_t *fb, uint32_t pitch, uint32_t sw, uint32_t sh,
                           int32_t x, int32_t y, tool_t tool) {
    uint32_t ink = pack_xrgb8888(0,0,0);
    uint32_t dark = pack_xrgb8888(60,60,60);
    uint32_t blue = pack_xrgb8888(0,0,180);

    /* 16x16-ish pixel icons inside a 26x26 button */
    int32_t ix = x + 5;
    int32_t iy = y + 5;

    switch (tool) {
        case TOOL_PENCIL:
            /* diagonal pencil */
            for (int i = 0; i < 10; i++) {
                put_px(fb, pitch, sw, sh, ix + i, iy + 9 - i, ink);
                put_px(fb, pitch, sw, sh, ix + i, iy + 10 - i, dark);
            }
            fill_rect(fb, pitch, sw, sh, ix + 9, iy + 0, 3, 3, blue);
            break;
        case TOOL_ERASER:
            fill_rect(fb, pitch, sw, sh, ix + 2, iy + 6, 10, 6, pack_xrgb8888(255,255,255));
            fill_rect(fb, pitch, sw, sh, ix + 2, iy + 6, 10, 1, ink);
            fill_rect(fb, pitch, sw, sh, ix + 2, iy + 6, 1, 6, ink);
            fill_rect(fb, pitch, sw, sh, ix + 11, iy + 6, 1, 6, ink);
            fill_rect(fb, pitch, sw, sh, ix + 2, iy + 11, 10, 1, ink);
            break;
        case TOOL_FILL:
            /* simple bucket */
            fill_rect(fb, pitch, sw, sh, ix + 3, iy + 2, 7, 6, ink);
            fill_rect(fb, pitch, sw, sh, ix + 4, iy + 3, 5, 4, pack_xrgb8888(200,200,200));
            fill_rect(fb, pitch, sw, sh, ix + 5, iy + 8, 5, 5, blue);
            break;
        case TOOL_EYEDROPPER:
            /* pipette-ish */
            for (int i = 0; i < 10; i++) put_px(fb, pitch, sw, sh, ix + 2 + i, iy + 10 - i, ink);
            fill_rect(fb, pitch, sw, sh, ix + 8, iy + 1, 3, 3, blue);
            break;
    }
}

static void draw_color_swatch_ui(uint8_t *fb, uint32_t pitch, uint32_t sw, uint32_t sh,
                                 int32_t x, int32_t y, uint32_t primary, uint32_t secondary) {
    /* MS Paint-ish overlapping swatches */
    uint32_t border = pack_xrgb8888(0,0,0);
    fill_rect(fb, pitch, sw, sh, x + 12, y + 12, 22, 22, secondary);
    fill_rect(fb, pitch, sw, sh, x + 12, y + 12, 22, 1, border);
    fill_rect(fb, pitch, sw, sh, x + 12, y + 12, 1, 22, border);
    fill_rect(fb, pitch, sw, sh, x + 33, y + 12, 1, 22, border);
    fill_rect(fb, pitch, sw, sh, x + 12, y + 33, 22, 1, border);

    fill_rect(fb, pitch, sw, sh, x + 2, y + 2, 22, 22, primary);
    fill_rect(fb, pitch, sw, sh, x + 2, y + 2, 22, 1, border);
    fill_rect(fb, pitch, sw, sh, x + 2, y + 2, 1, 22, border);
    fill_rect(fb, pitch, sw, sh, x + 23, y + 2, 1, 22, border);
    fill_rect(fb, pitch, sw, sh, x + 2, y + 23, 22, 1, border);
}


int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    md64api_grp_video_info_t vi;
    memset(&vi, 0, sizeof(vi));
    if (md64api_grp_get_video0_info(&vi) != 0) {
        puts_raw("paintgfx: cannot read video info\n");
        return 1;
    }
    if (vi.mode != MD64API_GRP_MODE_GRAPHICS || vi.bpp != 32) {
        puts_raw("paintgfx: requires 32bpp graphics\n");
        return 0;
    }

    /* Use gfx2d buffer handles for presentation.
     * We keep a CPU backbuffer (mapped buffer) but present via BLIT_BUF+FLUSH.
     */
    gfx2d_t g;
    g.fd = -1;
    int rc = gfx2d_open(&g);
    if (rc != 0) {
        printf("paintgfx: gfx2d_open rc=%d\n", rc);
        return 2;
    }

    uint32_t pitch = vi.width * 4u;
    uint32_t buf_size = pitch * vi.height;

    uint32_t bb_handle = 0;
    uint32_t bb_pitch = 0;
    rc = gfx2d_alloc_buf(&g, buf_size, (uint32_t)MD64API_GRP_FMT_XRGB8888, &bb_handle, &bb_pitch);
    if (rc != 0) {
        printf("paintgfx: alloc_buf rc=%d size=%u\n", rc, buf_size);
        gfx2d_close(&g);
        return 2;
    }

    void *bbv = NULL;
    uint32_t bb_size2 = 0, bb_pitch2 = 0, bb_fmt2 = 0;
    rc = gfx2d_map_buf(&g, bb_handle, &bbv, &bb_size2, &bb_pitch2, &bb_fmt2);
    if (rc != 0 || !bbv) {
        printf("paintgfx: map_buf rc=%d handle=%u\n", rc, bb_handle);
        gfx2d_close(&g);
        return 2;
    }

    /* Keep pitch consistent with how we draw into bb. If the kernel returns a different
     * pitch for the mapped buffer, use it and recompute buf_size accordingly.
     */
    if (bb_pitch2 && bb_pitch2 != pitch) {
        pitch = bb_pitch2;
        buf_size = pitch * vi.height;
    }

    uint8_t *bb = (uint8_t*)bbv;
    (void)bb_pitch; (void)bb_size2; (void)bb_fmt2;

    /* Cached base frame (UI + canvas but without cursor). We copy from this into bb
     * for dirty regions, then draw transient overlays (cursor, selection marquee, previews)
     * on top.
     */
    uint8_t *base = (uint8_t*)malloc(buf_size);
    if (!base) { gfx2d_close(&g); return 3; }

    /* Canvas buffer (only canvas area, not full screen) */
    int top_h = 44;
    int left_w = 72;
    int pal_h = 40;

    int canvas_x = left_w + 8;
    int canvas_y = top_h + 8;
    int canvas_w = (int)vi.width - canvas_x - 8;
    int canvas_h = (int)vi.height - canvas_y - pal_h - 16;
    if (canvas_w < 16) canvas_w = 16;
    if (canvas_h < 16) canvas_h = 16;

    uint32_t canvas_pitch = (uint32_t)canvas_w * 4u;
    uint32_t canvas_size = canvas_pitch * (uint32_t)canvas_h;

    uint8_t *canvas = (uint8_t*)malloc(canvas_size);
    if (!canvas) { free(base); gfx2d_close(&g); return 3; }
    memset(canvas, 0xFF, canvas_size); /* white */
    memset(bb, 0, buf_size);

    int efd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    if (efd < 0) {
        free(canvas);
        free(base);
        gfx2d_close(&g);
        return 4;
    }

    /* UI layout already computed above; init canvas already white */

    tool_t tool = TOOL_PENCIL;
    int brush = 1;
    uint32_t col_primary = pack_xrgb8888(0,0,0);
    uint32_t col_secondary = pack_xrgb8888(255,255,255);

    /* Selection state (rect selection MVP) */
    int sel_active = 0;
    int sel_dragging = 0;
    int sel_sx = 0, sel_sy = 0;
    int sel_x = 0, sel_y = 0, sel_w = 0, sel_h = 0;

    /* Move selection state */
    int move_dragging = 0;
    int move_off_x = 0, move_off_y = 0;
    int move_src_x = 0, move_src_y = 0; /* original selection position when starting move */
    uint32_t *sel_buf = NULL; /* sel_w*sel_h */

    /* Text tool state */
    int text_active = 0;
    int text_x = 0, text_y = 0; /* canvas coords */
    char text_buf[256];
    int text_len = 0;
    int text_bold = 0;
    int text_italic = 0;
    int text_scale = 1; /* 1..4 */

    cursor_img_t cur_arrow, cur_pencil, cur_eraser, cur_hand;
    int have_arrow = (load_cursor_bmp_rgba8888("/ModuOS/shared/usr/assets/mouse/arrow.bmp", &cur_arrow) == 0);
    int have_pencil = (load_cursor_bmp_rgba8888("/ModuOS/shared/usr/assets/mouse/pencil.bmp", &cur_pencil) == 0);
    int have_eraser = (load_cursor_bmp_rgba8888("/ModuOS/shared/usr/assets/mouse/eraser.bmp", &cur_eraser) == 0);
    int have_hand = (load_cursor_bmp_rgba8888("/ModuOS/shared/usr/assets/mouse/hand.bmp", &cur_hand) == 0);

    /* Top-bar utilities (16x16 BMP icons) */
    icon_img_t ico_undo = {0}, ico_redo = {0};
    int have_undo = (load_icon_bmp_argb8888("$/appdata/md-paint/assets/undo_util.bmp", &ico_undo) == 0) ||
                    (load_icon_bmp_argb8888("/appdata/md-paint/assets/undo_util.bmp", &ico_undo) == 0);
    int have_redo = (load_icon_bmp_argb8888("$/appdata/md-paint/assets/redo_util.bmp", &ico_redo) == 0) ||
                    (load_icon_bmp_argb8888("/appdata/md-paint/assets/redo_util.bmp", &ico_redo) == 0);

    /* Tool icons */
    icon_img_t ico_pencil = {0}, ico_eraser = {0}, ico_fill = {0}, ico_pick = {0};
    icon_img_t ico_brush = {0}, ico_spray = {0}, ico_line = {0}, ico_curve = {0};
    icon_img_t ico_text = {0}, ico_zoom = {0}, ico_select = {0}, ico_move = {0}, ico_magic = {0};

    int have_ico_pencil = (load_icon_bmp_argb8888("$/appdata/md-paint/assets/pencil_tool.bmp", &ico_pencil) == 0) ||
                          (load_icon_bmp_argb8888("/appdata/md-paint/assets/pencil_tool.bmp", &ico_pencil) == 0);
    int have_ico_eraser = (load_icon_bmp_argb8888("$/appdata/md-paint/assets/eraser_tool.bmp", &ico_eraser) == 0) ||
                          (load_icon_bmp_argb8888("/appdata/md-paint/assets/eraser_tool.bmp", &ico_eraser) == 0);
    int have_ico_fill   = (load_icon_bmp_argb8888("$/appdata/md-paint/assets/fill_tool.bmp", &ico_fill) == 0) ||
                          (load_icon_bmp_argb8888("/appdata/md-paint/assets/fill_tool.bmp", &ico_fill) == 0);
    int have_ico_pick   = (load_icon_bmp_argb8888("$/appdata/md-paint/assets/pick_tool.bmp", &ico_pick) == 0) ||
                          (load_icon_bmp_argb8888("/appdata/md-paint/assets/pick_tool.bmp", &ico_pick) == 0);

    int have_ico_brush  = (load_icon_bmp_argb8888("$/appdata/md-paint/assets/brush_tool.bmp", &ico_brush) == 0) ||
                          (load_icon_bmp_argb8888("/appdata/md-paint/assets/brush_tool.bmp", &ico_brush) == 0);
    int have_ico_spray  = (load_icon_bmp_argb8888("$/appdata/md-paint/assets/spray_tool.bmp", &ico_spray) == 0) ||
                          (load_icon_bmp_argb8888("/appdata/md-paint/assets/spray_tool.bmp", &ico_spray) == 0);
    int have_ico_line   = (load_icon_bmp_argb8888("$/appdata/md-paint/assets/line_tool.bmp", &ico_line) == 0) ||
                          (load_icon_bmp_argb8888("/appdata/md-paint/assets/line_tool.bmp", &ico_line) == 0);
    int have_ico_curve  = (load_icon_bmp_argb8888("$/appdata/md-paint/assets/curve_tool.bmp", &ico_curve) == 0) ||
                          (load_icon_bmp_argb8888("/appdata/md-paint/assets/curve_tool.bmp", &ico_curve) == 0);

    int have_ico_text   = (load_icon_bmp_argb8888("$/appdata/md-paint/assets/text_tool.bmp", &ico_text) == 0) ||
                          (load_icon_bmp_argb8888("/appdata/md-paint/assets/text_tool.bmp", &ico_text) == 0);
    int have_ico_zoom   = (load_icon_bmp_argb8888("$/appdata/md-paint/assets/zoom_tool.bmp", &ico_zoom) == 0) ||
                          (load_icon_bmp_argb8888("/appdata/md-paint/assets/zoom_tool.bmp", &ico_zoom) == 0);
    int have_ico_select = (load_icon_bmp_argb8888("$/appdata/md-paint/assets/select_rect_tool.bmp", &ico_select) == 0) ||
                          (load_icon_bmp_argb8888("/appdata/md-paint/assets/select_rect_tool.bmp", &ico_select) == 0);
    int have_ico_move   = (load_icon_bmp_argb8888("$/appdata/md-paint/assets/move_tool.bmp", &ico_move) == 0) ||
                          (load_icon_bmp_argb8888("/appdata/md-paint/assets/move_tool.bmp", &ico_move) == 0);
    int have_ico_magic  = (load_icon_bmp_argb8888("$/appdata/md-paint/assets/magic_select_tool.bmp", &ico_magic) == 0) ||
                          (load_icon_bmp_argb8888("/appdata/md-paint/assets/magic_select_tool.bmp", &ico_magic) == 0);

    uint32_t palette[12] = {
        pack_xrgb8888(0,0,0), pack_xrgb8888(255,255,255), pack_xrgb8888(128,128,128), pack_xrgb8888(255,0,0),
        pack_xrgb8888(0,255,0), pack_xrgb8888(0,0,255), pack_xrgb8888(255,255,0), pack_xrgb8888(255,0,255),
        pack_xrgb8888(0,255,255), pack_xrgb8888(128,0,0), pack_xrgb8888(0,128,0), pack_xrgb8888(0,0,128),
    };

    int32_t mx = (int32_t)(vi.width/2u), my = (int32_t)(vi.height/2u);
    uint8_t buttons = 0, prev_buttons = 0;

    int base_valid = 0;

    /* Hardware cursor tracking */
    int hwcur_enabled = 0;
    const cursor_img_t *hwcur_last = NULL;
    int32_t hwcur_last_x = -1;
    int32_t hwcur_last_y = -1;

    /* Try enable kernel-composited cursor ("hardware cursor" equivalent) */
    {
        gfx2d_info_t gi;
        memset(&gi, 0, sizeof(gi));
        if (gfx2d_get_info(&g, &gi) == 0 && (gi.caps & VIDEOCTL2_CAP_HW_CURSOR)) {
            uint32_t argb[CURSOR_W * CURSOR_H];
            if (have_arrow) {
                cursor_rgba_to_argb8888(&cur_arrow, argb);
                if (gfx2d_cursor_set(&g, CURSOR_W, CURSOR_H, 0, 0, argb) == 0) {
                    (void)gfx2d_cursor_show(&g, 1);
                    hwcur_enabled = 1;
                    hwcur_last = &cur_arrow;
                    hwcur_last_x = mx;
                    hwcur_last_y = my;
                    (void)gfx2d_cursor_move(&g, mx, my);
                }
            }
        }
    }

    /* UI geometry (used for hit testing & redraw decisions) */
    const int ui_gx0 = 10;
    const int ui_gy0 = 10;
    const int ui_bw = 26;
    const int ui_bh = 26;
    const int ui_gap = 6;
    const int ui_cols = 2;
    const int ui_tool_count = 13;
    const int ui_rows = (ui_tool_count + ui_cols - 1) / ui_cols;
    const int ui_grid_h = ui_bh * ui_rows + ui_gap * (ui_rows - 1);
    const int ui_sw_y = ui_gy0 + ui_grid_h + 10;
    const int ui_brush_bx = 10;
    const int ui_brush_by = ui_sw_y + 44 + 12;

    for (;;) {
        int had_input = 0;
        int text_committed = 0;

        /* input */
        for (;;) {
            Event e;
            ssize_t n = read(efd, &e, sizeof(e));
            if (n != (ssize_t)sizeof(e)) break;
            had_input = 1;

            if (e.type == EVENT_KEY_PRESSED && e.data.keyboard.keycode == KEY_ESCAPE) {
                if (tool == TOOL_TEXT && text_active) {
                    /* cancel text edit */
                    text_active = 0;
                    text_len = 0;
                    text_buf[0] = 0;
                } else {
                    if (hwcur_enabled) {
                        (void)gfx2d_cursor_show(&g, 0);
                    }
                    close(efd);
                    free(sel_buf);
                    free(canvas);
                    free(base);
                    gfx2d_close(&g);
                    return 0;
                }
            }

            /* Text tool keyboard input */
            if (tool == TOOL_TEXT && text_active) {
                if (e.type == EVENT_CHAR_INPUT) {
                    /* Some input backends may emit EVENT_CHAR_INPUT */
                    char c = e.data.keyboard.ascii;
                    if (c >= 32 && c < 127 && text_len < (int)sizeof(text_buf) - 1) {
                        text_buf[text_len++] = c;
                        text_buf[text_len] = 0;
                    }
                } else if (e.type == EVENT_KEY_PRESSED) {
                    /* USB keyboard currently sends KEY_PRESSED with ascii filled (no EVENT_CHAR_INPUT) */
                    if (e.data.keyboard.keycode == KEY_BACKSPACE) {
                        if (text_len > 0) {
                            text_buf[--text_len] = 0;
                        }
                    } else if (e.data.keyboard.keycode == KEY_ENTER) {
                        /* commit text to canvas */
                        draw_text_style(canvas, canvas_pitch, (uint32_t)canvas_w, (uint32_t)canvas_h,
                                        text_x, text_y, text_buf, col_primary,
                                        text_scale, text_bold, text_italic);
                        text_active = 0;
                        text_len = 0;
                        text_buf[0] = 0;
                        text_committed = 1;
                    } else {
                        char c = e.data.keyboard.ascii;
                        if (c >= 32 && c < 127 && text_len < (int)sizeof(text_buf) - 1) {
                            text_buf[text_len++] = c;
                            text_buf[text_len] = 0;
                        }
                    }
                }
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
        int right_now = (buttons & 2) != 0;
        int left_prev = (prev_buttons & 1) != 0;
        int right_prev = (prev_buttons & 2) != 0;
        int left_pressed = left_now && !left_prev;
        int right_pressed = right_now && !right_prev;

        /* active color for drawing depends on the button */
        uint32_t active_col = left_now ? col_primary : col_secondary;

        /* UI clicks */
        {
            /* Win98-ish tool grid (2 columns) */
            int gx0 = ui_gx0;
            int gy0 = ui_gy0;
            int bw = ui_bw;
            int bh = ui_bh;
            int gap = ui_gap;

            if (left_pressed) {
                /* top-bar utility buttons */
                int ub_y = 8;
                int ub_w = 22;
                int ub_h = 22;
                int undo_x = left_w + 8;
                int redo_x = undo_x + ub_w + 6;

                if (my >= ub_y && my < ub_y + ub_h) {
                    if (mx >= undo_x && mx < undo_x + ub_w) {
                        /* TODO: implement undo stack */
                        /* placeholder: no-op */
                    } else if (mx >= redo_x && mx < redo_x + ub_w) {
                        /* TODO: implement redo stack */
                        /* placeholder: no-op */
                    }
                }

                /* Text tool utilities (only when text tool selected) */
                if (tool == TOOL_TEXT) {
                    int tx = redo_x + ub_w + 10;
                    int ty = ub_y;
                    int bw2 = 22, bh2 = 22;

                    int b_x = tx + 70;
                    int i_x = b_x + bw2 + 6;
                    int minus_x = i_x + bw2 + 14;
                    int plus_x = minus_x + bw2 + 6;

                    if (my >= ty && my < ty + bh2) {
                        if (mx >= b_x && mx < b_x + bw2) text_bold = !text_bold;
                        else if (mx >= i_x && mx < i_x + bw2) text_italic = !text_italic;
                        else if (mx >= minus_x && mx < minus_x + bw2) { if (text_scale > 1) text_scale--; }
                        else if (mx >= plus_x && mx < plus_x + bw2) { if (text_scale < 4) text_scale++; }
                    }
                }

                /* tool grid */
                tool_t tools[] = {
                    TOOL_PENCIL, TOOL_ERASER,
                    TOOL_BRUSH,  TOOL_SPRAY,
                    TOOL_FILL,   TOOL_EYEDROPPER,
                    TOOL_LINE,   TOOL_CURVE,
                    TOOL_TEXT,   TOOL_ZOOM,
                    TOOL_SELECT_RECT, TOOL_MOVE,
                    TOOL_MAGIC_SELECT,
                };
                int tool_count = (int)(sizeof(tools) / sizeof(tools[0]));

                int cols = 2;
                int rows = (tool_count + cols - 1) / cols;

                int relx = mx - gx0;
                int rely = my - gy0;
                if (relx >= 0 && rely >= 0 && relx < (bw*cols + gap*(cols-1)) && rely < (bh*rows + gap*(rows-1))) {
                    int col = (relx >= bw + gap) ? 1 : 0;
                    int row = rely / (bh + gap);
                    int idx = row*cols + col;
                    if (idx >= 0 && idx < tool_count) tool = tools[idx];
                }

                /* brush size (below tool grid + swatches) */
                int grid_h = bh * rows + gap * (rows - 1);
                int sw_y = gy0 + grid_h + 10;
                int bx = 10;
                int by = sw_y + 44 + 12;
                if (mx >= bx && mx < bx + 52 && my >= by && my < by + 60) {
                    int bidx = (my - by) / 20;
                    brush = (bidx == 0) ? 1 : (bidx == 1) ? 2 : 3;
                }

                /* palette: left click sets primary */
                int px0 = canvas_x;
                int py0 = (int)vi.height - pal_h - 8;
                int swatch = 24;
                for (int i = 0; i < 12; i++) {
                    int px = px0 + i * (swatch + 4);
                    if (mx >= px && mx < px + swatch && my >= py0 && my < py0 + swatch) {
                        col_primary = palette[i];
                    }
                }
            }

            if (right_pressed) {
                /* palette: right click sets secondary */
                int px0 = canvas_x;
                int py0 = (int)vi.height - pal_h - 8;
                int swatch = 24;
                for (int i = 0; i < 12; i++) {
                    int px = px0 + i * (swatch + 4);
                    if (mx >= px && mx < px + swatch && my >= py0 && my < py0 + swatch) {
                        col_secondary = palette[i];
                    }
                }
            }
        }

        /* continuous drawing: interpolate between mouse samples while holding left/right */
        static int have_last = 0;
        static int32_t last_cx = 0, last_cy = 0;

        /* Line tool state (preview + commit on release) */
        static int line_active = 0;
        static int32_t line_sx = 0, line_sy = 0;
        static uint32_t line_col = 0;

        /* Curve tool state (Win98-ish):
           stage 1: drag endpoints
           stage 2: first bend (control 1)
           stage 3: second bend (control 2)
        */
        static int curve_stage = 0; /* 0=idle, 1=endpoints, 2=control1, 3=control2 */
        static int curve_dragging = 0;
        static int32_t curve_x0 = 0, curve_y0 = 0;
        static int32_t curve_x1 = 0, curve_y1 = 0;
        static int32_t curve_c1x = 0, curve_c1y = 0;
        static int32_t curve_c2x = 0, curve_c2y = 0;
        static uint32_t curve_col = 0;
        static int curve_radius = 0;

        int drawing_button = (left_now || right_now);
        int pressed_any = (left_pressed || right_pressed);
        int released_any = ((!left_now && left_prev) || (!right_now && right_prev));

        int draw_now = drawing_button && tool != TOOL_EYEDROPPER && tool != TOOL_LINE;
        int pick_now = pressed_any && tool == TOOL_EYEDROPPER;

        if (!draw_now) {
            have_last = 0;
        }

        /* Text tool: click to start editing */
        if (tool == TOOL_TEXT) {
            if (left_pressed && mx >= canvas_x && mx < canvas_x + canvas_w && my >= canvas_y && my < canvas_y + canvas_h) {
                text_active = 1;
                text_x = mx - canvas_x;
                text_y = my - canvas_y;
                text_len = 0;
                text_buf[0] = 0;
            }
        } else {
            /* leaving text tool cancels current edit (does not commit) */
            text_active = 0;
            text_len = 0;
            text_buf[0] = 0;
        }

        /* Selection tools */
        if (tool == TOOL_SELECT_RECT) {
            if (left_pressed && mx >= canvas_x && mx < canvas_x + canvas_w && my >= canvas_y && my < canvas_y + canvas_h) {
                sel_dragging = 1;
                sel_sx = mx - canvas_x;
                sel_sy = my - canvas_y;
                sel_active = 0;
            }
            if (sel_dragging && left_now) {
                int ex = mx - canvas_x;
                int ey = my - canvas_y;
                if (ex < 0) ex = 0; if (ey < 0) ey = 0;
                if (ex >= canvas_w) ex = canvas_w - 1;
                if (ey >= canvas_h) ey = canvas_h - 1;
                int x0 = sel_sx < ex ? sel_sx : ex;
                int y0 = sel_sy < ey ? sel_sy : ey;
                int x1 = sel_sx > ex ? sel_sx : ex;
                int y1 = sel_sy > ey ? sel_sy : ey;
                sel_x = x0; sel_y = y0; sel_w = x1 - x0 + 1; sel_h = y1 - y0 + 1;
            }
            if (sel_dragging && !left_now && left_prev) {
                sel_dragging = 0;
                if (sel_w > 0 && sel_h > 0) {
                    sel_active = 1;
                    /* capture selection into buffer */
                    free(sel_buf);
                    sel_buf = (uint32_t*)malloc((size_t)sel_w * (size_t)sel_h * 4u);
                    if (sel_buf) {
                        for (int y = 0; y < sel_h; y++) {
                            for (int x = 0; x < sel_w; x++) {
                                sel_buf[y*sel_w + x] = *canvas_px(canvas, canvas_pitch, sel_x + x, sel_y + y);
                            }
                        }
                    } else {
                        sel_active = 0;
                    }
                }
            }
        }

        if (tool == TOOL_MOVE) {
            if (sel_active && sel_buf) {
                int cx = mx - canvas_x;
                int cy = my - canvas_y;
                int inside = (cx >= sel_x && cx < sel_x + sel_w && cy >= sel_y && cy < sel_y + sel_h);
                if (left_pressed && inside) {
                    move_dragging = 1;
                    move_off_x = cx - sel_x;
                    move_off_y = cy - sel_y;
                    move_src_x = sel_x;
                    move_src_y = sel_y;

                    /* cut selection from canvas (fill old area with white) */
                    for (int y = 0; y < sel_h; y++) {
                        for (int x = 0; x < sel_w; x++) {
                            *canvas_px(canvas, canvas_pitch, move_src_x + x, move_src_y + y) = pack_xrgb8888(255,255,255);
                        }
                    }
                }
                if (move_dragging && left_now) {
                    int nx = cx - move_off_x;
                    int ny = cy - move_off_y;
                    if (nx < 0) nx = 0;
                    if (ny < 0) ny = 0;
                    if (nx + sel_w > canvas_w) nx = canvas_w - sel_w;
                    if (ny + sel_h > canvas_h) ny = canvas_h - sel_h;
                    sel_x = nx;
                    sel_y = ny;
                }
                if (move_dragging && !left_now && left_prev) {
                    move_dragging = 0;
                    /* commit selection buffer into canvas at new location */
                    for (int y = 0; y < sel_h; y++) {
                        for (int x = 0; x < sel_w; x++) {
                            *canvas_px(canvas, canvas_pitch, sel_x + x, sel_y + y) = sel_buf[y*sel_w + x];
                        }
                    }
                }
            }
        } else {
            move_dragging = 0;
        }

        /* Eyedropper: click canvas to pick into primary/secondary */
        if (pick_now) {
            if (mx >= canvas_x && mx < canvas_x + canvas_w && my >= canvas_y && my < canvas_y + canvas_h) {
                int32_t cx = mx - canvas_x;
                int32_t cy = my - canvas_y;
                uint32_t picked = *canvas_px(canvas, canvas_pitch, cx, cy);
                if (left_pressed) col_primary = picked;
                if (right_pressed) col_secondary = picked;
            }
        }

        /* Curve tool: Stage 1 endpoints, Stage 2 bend1, Stage 3 bend2, commit */
        if (tool == TOOL_CURVE) {
            if (pressed_any && mx >= canvas_x && mx < canvas_x + canvas_w && my >= canvas_y && my < canvas_y + canvas_h) {
                int32_t cx = mx - canvas_x;
                int32_t cy = my - canvas_y;

                if (curve_stage == 0) {
                    curve_stage = 1;
                    curve_dragging = 1;
                    curve_x0 = cx;
                    curve_y0 = cy;
                    curve_x1 = cx;
                    curve_y1 = cy;
                    curve_c1x = cx;
                    curve_c1y = cy;
                    curve_c2x = cx;
                    curve_c2y = cy;
                    curve_col = (left_pressed ? col_primary : col_secondary);
                    curve_radius = (brush > 0 ? brush : 0);
                } else if (curve_stage == 2) {
                    /* start dragging first bend */
                    curve_dragging = 1;
                    curve_c1x = cx;
                    curve_c1y = cy;
                } else if (curve_stage == 3) {
                    /* start dragging second bend */
                    curve_dragging = 1;
                    curve_c2x = cx;
                    curve_c2y = cy;
                }
            }

            if (curve_dragging && drawing_button) {
                int32_t cx = mx - canvas_x;
                int32_t cy = my - canvas_y;
                if (cx < 0) cx = 0; if (cy < 0) cy = 0;
                if (cx >= canvas_w) cx = canvas_w - 1;
                if (cy >= canvas_h) cy = canvas_h - 1;

                if (curve_stage == 1) {
                    curve_x1 = cx;
                    curve_y1 = cy;
                } else if (curve_stage == 2) {
                    curve_c1x = cx;
                    curve_c1y = cy;
                } else if (curve_stage == 3) {
                    curve_c2x = cx;
                    curve_c2y = cy;
                }
            }

            if (curve_dragging && released_any) {
                curve_dragging = 0;
                if (curve_stage == 1) {
                    /* advance to first bend stage */
                    curve_stage = 2;
                    curve_dragging = 0;
                } else if (curve_stage == 2) {
                    /* advance to second bend stage */
                    curve_stage = 3;
                    curve_dragging = 0;
                } else if (curve_stage == 3) {
                    /* commit cubic bezier (two bends) */
                    int steps = 96;
                    for (int i = 0; i <= steps; i++) {
                        int t = (i * 1024) / steps;
                        int omt = 1024 - t;
                        /* cubic: P(t)= (1-t)^3 P0 + 3(1-t)^2 t C1 + 3(1-t) t^2 C2 + t^3 P1 */
                        int64_t omt2 = (int64_t)omt * omt;
                        int64_t t2 = (int64_t)t * t;
                        int64_t omt3 = omt2 * omt;
                        int64_t t3 = t2 * t;
                        int64_t w0 = omt3;
                        int64_t w1 = 3 * omt2 * t;
                        int64_t w2 = 3 * omt * t2;
                        int64_t w3 = t3;
                        int64_t denom = (int64_t)1024 * 1024 * 1024;

                        int32_t px = (int32_t)((w0*curve_x0 + w1*curve_c1x + w2*curve_c2x + w3*curve_x1) / denom);
                        int32_t py = (int32_t)((w0*curve_y0 + w1*curve_c1y + w2*curve_c2y + w3*curve_y1) / denom);
                        brush_dot(canvas, canvas_pitch, (uint32_t)canvas_w, (uint32_t)canvas_h, px, py, curve_radius, curve_col);
                    }
                    curve_stage = 0;
                }
            }
        } else {
            curve_stage = 0;
            curve_dragging = 0;
        }

        /* Line tool: start on press, preview while held, commit on release */
        if (tool == TOOL_LINE) {
            if (pressed_any && mx >= canvas_x && mx < canvas_x + canvas_w && my >= canvas_y && my < canvas_y + canvas_h) {
                line_active = 1;
                line_sx = mx - canvas_x;
                line_sy = my - canvas_y;
                line_col = (left_pressed ? col_primary : col_secondary);
            }
            if (line_active && released_any) {
                /* commit the line into the canvas */
                int32_t ex = mx - canvas_x;
                int32_t ey = my - canvas_y;

                int32_t dx = ex - line_sx;
                int32_t dy = ey - line_sy;
                int32_t steps = dx < 0 ? -dx : dx;
                int32_t ay = dy < 0 ? -dy : dy;
                if (ay > steps) steps = ay;
                if (steps < 1) steps = 1;

                for (int32_t i = 0; i <= steps; i++) {
                    int32_t px = line_sx + (dx * i) / steps;
                    int32_t py = line_sy + (dy * i) / steps;
                    brush_dot(canvas, canvas_pitch, (uint32_t)canvas_w, (uint32_t)canvas_h, px, py, 0, line_col);
                }
                line_active = 0;
            }
        } else {
            line_active = 0;
        }

        if (draw_now) {
            if (mx >= canvas_x && mx < canvas_x + canvas_w && my >= canvas_y && my < canvas_y + canvas_h) {
                int32_t cx = mx - canvas_x;
                int32_t cy = my - canvas_y;

                if (!have_last) {
                    have_last = 1;
                    last_cx = cx;
                    last_cy = cy;
                }

                /* DDA stepping */
                int32_t dx = cx - last_cx;
                int32_t dy = cy - last_cy;
                int32_t steps = dx < 0 ? -dx : dx;
                int32_t ay = dy < 0 ? -dy : dy;
                if (ay > steps) steps = ay;
                if (steps < 1) steps = 1;

                for (int32_t i = 0; i <= steps; i++) {
                    int32_t px = last_cx + (dx * i) / steps;
                    int32_t py = last_cy + (dy * i) / steps;

                    if (tool == TOOL_PENCIL) {
                        brush_dot(canvas, canvas_pitch, (uint32_t)canvas_w, (uint32_t)canvas_h, px, py, 0, active_col);
                    } else if (tool == TOOL_BRUSH) {
                        brush_dot(canvas, canvas_pitch, (uint32_t)canvas_w, (uint32_t)canvas_h, px, py, brush + 1, active_col);
                    } else if (tool == TOOL_SPRAY) {
                        /* spray: random dots in a radius */
                        int r = (brush + 2) * 2;
                        for (int k = 0; k < 12 + brush * 6; k++) {
                            uint32_t rv = xorshift32();
                            int ox = (int)((rv & 0xFF) % (2 * r + 1)) - r;
                            int oy = (int)(((rv >> 8) & 0xFF) % (2 * r + 1)) - r;
                            if (ox*ox + oy*oy <= r*r) {
                                int32_t sx = px + ox;
                                int32_t sy = py + oy;
                                if ((uint32_t)sx < (uint32_t)canvas_w && (uint32_t)sy < (uint32_t)canvas_h) {
                                    *canvas_px(canvas, canvas_pitch, sx, sy) = active_col;
                                }
                            }
                        }
                    } else if (tool == TOOL_ERASER) {
                        brush_dot(canvas, canvas_pitch, (uint32_t)canvas_w, (uint32_t)canvas_h, px, py, brush + 2, pack_xrgb8888(255,255,255));
                    }
                }

                /* Fill: click (either button) to fill using that button's color */
                if (tool == TOOL_FILL && (left_pressed || right_pressed)) {
                    uint32_t from = *canvas_px(canvas, canvas_pitch, cx, cy);
                    flood_fill(canvas, canvas_pitch, (uint32_t)canvas_w, (uint32_t)canvas_h, cx, cy, from, active_col);
                }

                last_cx = cx;
                last_cy = cy;
            } else {
                /* held but outside canvas */
                have_last = 0;
            }
        }

        prev_buttons = buttons;

        /* Rendering strategy:
         * - Maintain a cached "base" frame (UI + canvas), no cursor, no transient overlays.
         * - Only rebuild the base when something changes (tool/brush/color/canvas).
         * - If only the cursor moved (and no overlays active), update just the cursor dirty rects.
         */

        /* Determine whether we have transient overlays that are not part of the base. */
        int overlay_active = 0;
        overlay_active |= (tool == TOOL_TEXT && text_active);
        overlay_active |= (sel_dragging || sel_active);
        overlay_active |= (tool == TOOL_CURVE && curve_stage != 0);
        overlay_active |= (tool == TOOL_LINE && line_active);

        /* Track input activity this iteration (read loop is non-blocking). */
        /* NOTE: we conservatively treat any button change as requiring a redraw. */

        /* Determine cursor type (affects dirty-rect updates). */
        const cursor_img_t *cur = NULL;
        {
            int over_tool = (mx >= ui_gx0 && my >= ui_gy0 && mx < ui_gx0 + (ui_bw * ui_cols + ui_gap * (ui_cols - 1)) &&
                             my < ui_gy0 + (ui_bh * ui_rows + ui_gap * (ui_rows - 1)));
            int over_brush = (mx >= ui_brush_bx && mx < ui_brush_bx + 52 && my >= ui_brush_by && my < ui_brush_by + 60);
            int over_palette = (my >= (int)vi.height - pal_h - 8 && my < (int)vi.height - pal_h - 8 + 24 &&
                                mx >= canvas_x && mx < canvas_x + 12 * (24 + 4));

            int over_utils = 0;
            {
                int ub_y = 8;
                int ub_w = 22;
                int ub_h = 22;
                int undo_x = left_w + 8;
                int redo_x = undo_x + ub_w + 6;
                over_utils = (my >= ub_y && my < ub_y + ub_h) &&
                             ((mx >= undo_x && mx < undo_x + ub_w) || (mx >= redo_x && mx < redo_x + ub_w));
            }

            if ((over_tool || over_brush || over_palette || over_utils) && have_hand) {
                cur = &cur_hand;
            } else if (mx >= canvas_x && mx < canvas_x + canvas_w && my >= canvas_y && my < canvas_y + canvas_h) {
                if (tool == TOOL_PENCIL && have_pencil) cur = &cur_pencil;
                else if (tool == TOOL_ERASER && have_eraser) cur = &cur_eraser;
                else if (have_arrow) cur = &cur_arrow;
            } else {
                if (have_arrow) cur = &cur_arrow;
            }
        }

        /* Determine if we should rebuild the base. */
        static tool_t last_tool = (tool_t)-1;
        static int last_brush = -1;
        static uint32_t last_col_primary = 0;
        static uint32_t last_col_secondary = 0;

        int ui_state_changed = 0;
        if (tool != last_tool) ui_state_changed = 1;
        if (brush != last_brush) ui_state_changed = 1;
        if (col_primary != last_col_primary) ui_state_changed = 1;
        if (col_secondary != last_col_secondary) ui_state_changed = 1;

        int canvas_changed = 0;
        canvas_changed |= draw_now; /* pencil/brush/eraser/spray while held */
        canvas_changed |= (tool == TOOL_FILL && (left_pressed || right_pressed));
        canvas_changed |= (tool == TOOL_LINE && released_any); /* commit on release */
        canvas_changed |= (tool == TOOL_CURVE && curve_stage == 0 && released_any); /* commit ends stage 3 */
        canvas_changed |= text_committed;
        canvas_changed |= (tool == TOOL_MOVE && (move_dragging && !left_now && left_prev));

        if (!base_valid || ui_state_changed || canvas_changed) {
            base_valid = 0;
        }

        /* Cursor rectangles are only used for software cursor path. */
        rect_i32_t r_cur_prev = rect_empty();
        rect_i32_t r_cur_now  = rect_empty();
        rect_i32_t dirty = rect_empty();

        if (!hwcur_enabled) {
            if (hwcur_last && hwcur_last_x >= 0 && hwcur_last_y >= 0) {
                r_cur_prev.x = hwcur_last_x;
                r_cur_prev.y = hwcur_last_y;
                r_cur_prev.w = (int32_t)CURSOR_W;
                r_cur_prev.h = (int32_t)CURSOR_H;
            }
            if (cur && mx >= 0 && my >= 0) {
                r_cur_now.x = mx;
                r_cur_now.y = my;
                r_cur_now.w = (int32_t)CURSOR_W;
                r_cur_now.h = (int32_t)CURSOR_H;
            }
        }

        if (!base_valid) {
            /* (Re)build base frame into 'base' */
            /* background */
            fill_rect(base, pitch, vi.width, vi.height, 0, 0, (int32_t)vi.width, (int32_t)vi.height, pack_xrgb8888(192,192,192));

            /* canvas */
            for (int y = 0; y < canvas_h; y++) {
                uint32_t *dstrow = (uint32_t *)(base + (uint64_t)(canvas_y + y) * pitch) + canvas_x;
                const uint32_t *srcrow = (const uint32_t *)(canvas + (uint64_t)y * canvas_pitch);
                memcpy(dstrow, srcrow, (size_t)canvas_w * 4u);
            }

            /* chrome */
            fill_rect(base, pitch, vi.width, vi.height, 0, 0, (int32_t)vi.width, top_h, pack_xrgb8888(160,160,160));
            fill_rect(base, pitch, vi.width, vi.height, 0, 0, (int32_t)vi.width, 2, pack_xrgb8888(240,240,240));
            fill_rect(base, pitch, vi.width, vi.height, 0, 0, 2, top_h, pack_xrgb8888(240,240,240));
            fill_rect(base, pitch, vi.width, vi.height, 0, top_h-2, (int32_t)vi.width, 2, pack_xrgb8888(110,110,110));

            /* top-bar utilities (Undo/Redo) */
            {
                int ub_y = 8;
                int ub_w = 22;
                int ub_h = 22;
                int undo_x = left_w + 8;
                int redo_x = undo_x + ub_w + 6;

                draw_bevel_button(base, pitch, vi.width, vi.height, undo_x, ub_y, ub_w, ub_h, 0);
                draw_bevel_button(base, pitch, vi.width, vi.height, redo_x, ub_y, ub_w, ub_h, 0);
                if (have_undo) blit_icon_argb8888(base, pitch, vi.width, vi.height, &ico_undo, undo_x + 3, ub_y + 3);
                if (have_redo) blit_icon_argb8888(base, pitch, vi.width, vi.height, &ico_redo, redo_x + 3, ub_y + 3);

                int status_x = redo_x + ub_w + 10;
                if (tool == TOOL_TEXT) status_x += 140;
                draw_text(base, pitch, vi.width, vi.height, status_x, ub_y + 7, tool_name(tool), pack_xrgb8888(0,0,0));

                if (tool == TOOL_TEXT) {
                    int tx = redo_x + ub_w + 10;
                    int ty = ub_y;
                    char sz[8];
                    sz[0] = 'x';
                    sz[1] = '0' + (char)text_scale;
                    sz[2] = 0;
                    draw_text(base, pitch, vi.width, vi.height, tx + 36, ty + 7, sz, pack_xrgb8888(0,0,0));

                    int bw2 = 22, bh2 = 22;
                    int b_x = tx + 70;
                    int i_x = b_x + bw2 + 6;
                    int minus_x = i_x + bw2 + 14;
                    int plus_x = minus_x + bw2 + 6;

                    draw_bevel_button(base, pitch, vi.width, vi.height, b_x, ty, bw2, bh2, text_bold);
                    draw_bevel_button(base, pitch, vi.width, vi.height, i_x, ty, bw2, bh2, text_italic);
                    draw_bevel_button(base, pitch, vi.width, vi.height, minus_x, ty, bw2, bh2, 0);
                    draw_bevel_button(base, pitch, vi.width, vi.height, plus_x, ty, bw2, bh2, 0);

                    draw_text(base, pitch, vi.width, vi.height, b_x + 8, ty + 7, "B", pack_xrgb8888(0,0,0));
                    draw_text(base, pitch, vi.width, vi.height, i_x + 8, ty + 7, "I", pack_xrgb8888(0,0,0));
                    draw_text(base, pitch, vi.width, vi.height, minus_x + 8, ty + 7, "-", pack_xrgb8888(0,0,0));
                    draw_text(base, pitch, vi.width, vi.height, plus_x + 8, ty + 7, "+", pack_xrgb8888(0,0,0));
                }
            }

            fill_rect(base, pitch, vi.width, vi.height, 0, 0, left_w, (int32_t)vi.height, pack_xrgb8888(180,180,180));

            /* tool buttons */
            int gx0 = ui_gx0;
            int gy0 = ui_gy0;
            int bw = ui_bw;
            int bh = ui_bh;
            int gap = ui_gap;

            tool_t tools[] = {
                TOOL_PENCIL, TOOL_ERASER,
                TOOL_BRUSH,  TOOL_SPRAY,
                TOOL_FILL,   TOOL_EYEDROPPER,
                TOOL_LINE,   TOOL_CURVE,
                TOOL_TEXT,   TOOL_ZOOM,
                TOOL_SELECT_RECT, TOOL_MOVE,
                TOOL_MAGIC_SELECT,
            };
            int tool_count = (int)(sizeof(tools) / sizeof(tools[0]));
            int cols = 2;

            for (int i = 0; i < tool_count; i++) {
                int row = i / cols;
                int col = i % cols;
                int x = gx0 + col * (bw + gap);
                int y = gy0 + row * (bh + gap);
                int pressed = (tool == tools[i]);
                draw_bevel_button(base, pitch, vi.width, vi.height, x, y, bw, bh, pressed);

                const icon_img_t *ico = NULL;
                int have_ico = 0;
                switch (tools[i]) {
                    case TOOL_PENCIL: ico = &ico_pencil; have_ico = have_ico_pencil; break;
                    case TOOL_ERASER: ico = &ico_eraser; have_ico = have_ico_eraser; break;
                    case TOOL_FILL: ico = &ico_fill; have_ico = have_ico_fill; break;
                    case TOOL_EYEDROPPER: ico = &ico_pick; have_ico = have_ico_pick; break;
                    case TOOL_BRUSH: ico = &ico_brush; have_ico = have_ico_brush; break;
                    case TOOL_SPRAY: ico = &ico_spray; have_ico = have_ico_spray; break;
                    case TOOL_LINE: ico = &ico_line; have_ico = have_ico_line; break;
                    case TOOL_CURVE: ico = &ico_curve; have_ico = have_ico_curve; break;
                    case TOOL_TEXT: ico = &ico_text; have_ico = have_ico_text; break;
                    case TOOL_ZOOM: ico = &ico_zoom; have_ico = have_ico_zoom; break;
                    case TOOL_SELECT_RECT: ico = &ico_select; have_ico = have_ico_select; break;
                    case TOOL_MOVE: ico = &ico_move; have_ico = have_ico_move; break;
                    case TOOL_MAGIC_SELECT: ico = &ico_magic; have_ico = have_ico_magic; break;
                }

                int ox = x + 5 + (pressed ? 1 : 0);
                int oy = y + 5 + (pressed ? 1 : 0);
                if (have_ico && ico) {
                    blit_icon_argb8888(base, pitch, vi.width, vi.height, ico, ox, oy);
                } else {
                    draw_tool_icon(base, pitch, vi.width, vi.height, x + (pressed ? 1 : 0), y + (pressed ? 1 : 0), tools[i]);
                }
            }

            int rows = (tool_count + cols - 1) / cols;
            int grid_h = bh * rows + gap * (rows - 1);

            int sw_y = gy0 + grid_h + 10;
            fill_rect(base, pitch, vi.width, vi.height, 6, sw_y - 6, left_w - 12, 2, pack_xrgb8888(150,150,150));
            draw_color_swatch_ui(base, pitch, vi.width, vi.height, 10, sw_y, col_primary, col_secondary);

            fill_rect(base, pitch, vi.width, vi.height, 6, sw_y + 44 + 4, left_w - 12, 2, pack_xrgb8888(150,150,150));
            int bx = 10;
            int by = sw_y + 44 + 12;
            fill_rect(base, pitch, vi.width, vi.height, bx, by, 52, 60, pack_xrgb8888(200,200,200));
            fill_rect(base, pitch, vi.width, vi.height, bx+6, by+6, brush*2, brush*2, col_primary);

            int px0 = canvas_x;
            int py0 = (int)vi.height - pal_h - 8;
            int swatch = 24;
            for (int i = 0; i < 12; i++) {
                int px = px0 + i * (swatch + 4);
                fill_rect(base, pitch, vi.width, vi.height, px, py0, swatch, swatch, palette[i]);

                if (palette[i] == col_primary) {
                    fill_rect(base, pitch, vi.width, vi.height, px, py0, swatch, 2, pack_xrgb8888(0,0,0));
                    fill_rect(base, pitch, vi.width, vi.height, px, py0, 2, swatch, pack_xrgb8888(0,0,0));
                }
                if (palette[i] == col_secondary) {
                    fill_rect(base, pitch, vi.width, vi.height, px, py0 + swatch - 2, swatch, 2, pack_xrgb8888(0,0,0));
                    fill_rect(base, pitch, vi.width, vi.height, px + swatch - 2, py0, 2, swatch, pack_xrgb8888(0,0,0));
                }
            }

            fill_rect(base, pitch, vi.width, vi.height, canvas_x-2, canvas_y-2, canvas_w+4, 2, pack_xrgb8888(110,110,110));
            fill_rect(base, pitch, vi.width, vi.height, canvas_x-2, canvas_y-2, 2, canvas_h+4, pack_xrgb8888(110,110,110));
            fill_rect(base, pitch, vi.width, vi.height, canvas_x-2, canvas_y+canvas_h+2, canvas_w+4, 2, pack_xrgb8888(240,240,240));
            fill_rect(base, pitch, vi.width, vi.height, canvas_x+canvas_w+2, canvas_y-2, 2, canvas_h+4, pack_xrgb8888(240,240,240));

            base_valid = 1;
            last_tool = tool;
            last_brush = brush;
            last_col_primary = col_primary;
            last_col_secondary = col_secondary;

            memcpy(bb, base, buf_size);
            dirty.x = 0; dirty.y = 0; dirty.w = (int32_t)vi.width; dirty.h = (int32_t)vi.height;
        } else {
            /* Base is valid. Only redraw if needed. */
            int cursor_only = (!hwcur_enabled && !overlay_active && cur && hwcur_last && (mx != hwcur_last_x || my != hwcur_last_y || cur != hwcur_last));
            if (cursor_only) {
                dirty = rect_union(r_cur_prev, r_cur_now);
                copy_rect(bb, base, pitch, (int32_t)vi.width, (int32_t)vi.height, dirty);
            } else {
                /* Fallback: copy full base (still far cheaper than fully re-rendering every loop). */
                memcpy(bb, base, buf_size);
                dirty.x = 0; dirty.y = 0; dirty.w = (int32_t)vi.width; dirty.h = (int32_t)vi.height;
            }
        }

        /* If we have overlays, they must be drawn after copying the base. */

        /* text preview (non-destructive) */
        if (tool == TOOL_TEXT && text_active) {
            draw_text_style(bb, pitch, vi.width, vi.height, canvas_x + text_x, canvas_y + text_y, text_buf, col_primary,
                            text_scale, text_bold, text_italic);
            /* caret */
            int adv = 8 * (text_scale < 1 ? 1 : text_scale);
            int cx = canvas_x + text_x + text_len * adv;
            int cy = canvas_y + text_y;
            for (int yy = 0; yy < 8 * text_scale; yy++) put_px(bb, pitch, vi.width, vi.height, cx, cy + yy, pack_xrgb8888(0,0,0));
        }

        /* selection preview: draw buffer + marquee */
        if ((sel_dragging || sel_active) && sel_w > 0 && sel_h > 0) {
            /* if we have a captured buffer, draw it (useful for move tool drag preview) */
            if (sel_active && sel_buf) {
                for (int y = 0; y < sel_h; y++) {
                    for (int x = 0; x < sel_w; x++) {
                        put_px(bb, pitch, vi.width, vi.height, canvas_x + sel_x + x, canvas_y + sel_y + y, sel_buf[y*sel_w + x]);
                    }
                }
            }

            /* dashed marquee */
            uint32_t dash = pack_xrgb8888(0,0,0);
            int x0 = canvas_x + sel_x;
            int y0 = canvas_y + sel_y;
            int x1 = x0 + sel_w - 1;
            int y1 = y0 + sel_h - 1;
            for (int x = x0; x <= x1; x++) {
                if (((x - x0) & 3) < 2) { put_px(bb, pitch, vi.width, vi.height, x, y0, dash); put_px(bb, pitch, vi.width, vi.height, x, y1, dash); }
            }
            for (int y = y0; y <= y1; y++) {
                if (((y - y0) & 3) < 2) { put_px(bb, pitch, vi.width, vi.height, x0, y, dash); put_px(bb, pitch, vi.width, vi.height, x1, y, dash); }
            }
        }

        if (tool == TOOL_CURVE && curve_stage != 0) {
            if (curve_stage == 1) {
                /* preview straight endpoints line */
                int32_t dx = curve_x1 - curve_x0;
                int32_t dy = curve_y1 - curve_y0;
                int32_t steps = dx < 0 ? -dx : dx;
                int32_t ay = dy < 0 ? -dy : dy;
                if (ay > steps) steps = ay;
                if (steps < 1) steps = 1;
                for (int32_t i = 0; i <= steps; i++) {
                    int32_t px = curve_x0 + (dx * i) / steps;
                    int32_t py = curve_y0 + (dy * i) / steps;
                    brush_dot(bb, pitch, vi.width, vi.height, canvas_x + px, canvas_y + py, curve_radius, curve_col);
                }
            } else if (curve_stage == 2) {
                /* preview after first bend: use quadratic with C1 as control */
                int steps = 64;
                for (int i = 0; i <= steps; i++) {
                    int t = (i * 1024) / steps;
                    int omt = 1024 - t;
                    int32_t px = (omt*omt*curve_x0 + 2*omt*t*curve_c1x + t*t*curve_x1) / (1024*1024);
                    int32_t py = (omt*omt*curve_y0 + 2*omt*t*curve_c1y + t*t*curve_y1) / (1024*1024);
                    brush_dot(bb, pitch, vi.width, vi.height, canvas_x + px, canvas_y + py, curve_radius, curve_col);
                }
            } else if (curve_stage == 3) {
                /* preview final curve (cubic) */
                int steps = 96;
                for (int i = 0; i <= steps; i++) {
                    int t = (i * 1024) / steps;
                    int omt = 1024 - t;
                    int64_t omt2 = (int64_t)omt * omt;
                    int64_t t2 = (int64_t)t * t;
                    int64_t omt3 = omt2 * omt;
                    int64_t t3 = t2 * t;
                    int64_t w0 = omt3;
                    int64_t w1 = 3 * omt2 * t;
                    int64_t w2 = 3 * omt * t2;
                    int64_t w3 = t3;
                    int64_t denom = (int64_t)1024 * 1024 * 1024;

                    int32_t px = (int32_t)((w0*curve_x0 + w1*curve_c1x + w2*curve_c2x + w3*curve_x1) / denom);
                    int32_t py = (int32_t)((w0*curve_y0 + w1*curve_c1y + w2*curve_c2y + w3*curve_y1) / denom);
                    brush_dot(bb, pitch, vi.width, vi.height, canvas_x + px, canvas_y + py, curve_radius, curve_col);
                }
            }
        }

        if (tool == TOOL_LINE && line_active) {
            int32_t ex = mx - canvas_x;
            int32_t ey = my - canvas_y;

            /* clamp end to canvas */
            if (ex < 0) ex = 0;
            if (ey < 0) ey = 0;
            if (ex >= canvas_w) ex = canvas_w - 1;
            if (ey >= canvas_h) ey = canvas_h - 1;

            int32_t dx = ex - line_sx;
            int32_t dy = ey - line_sy;
            int32_t steps = dx < 0 ? -dx : dx;
            int32_t ay = dy < 0 ? -dy : dy;
            if (ay > steps) steps = ay;
            if (steps < 1) steps = 1;

            for (int32_t i = 0; i <= steps; i++) {
                int32_t px = line_sx + (dx * i) / steps;
                int32_t py = line_sy + (dy * i) / steps;
                /* draw onto backbuffer at canvas offset */
                put_px(bb, pitch, vi.width, vi.height, canvas_x + px, canvas_y + py, line_col);
            }
        }

        /* UI is part of the cached base frame; do not redraw it here. */
        if (!hwcur_enabled && cur) {
            alpha_blit_cursor_xrgb8888(bb, pitch, vi.width, vi.height, cur, mx, my);
            /* Cursor drawn after base copy: include it in dirty region. */
            dirty = rect_union(dirty, r_cur_now);
        }

        /* Present:
         * Using video0 ENQUEUE+FLUSH for every mouse move is syscall-heavy and can lag.
         * Prefer the kernel fast-path blit syscall for dirty rectangles.
         */
        if (!rect_is_empty(dirty)) {
            rect_i32_t dr = rect_clip(dirty, (int32_t)vi.width, (int32_t)vi.height);
            if (!rect_is_empty(dr)) {
                const uint8_t *src = (const uint8_t *)bb + (uint64_t)dr.y * pitch + (uint64_t)dr.x * 4u;
                (void)gfx_blit(src,
                               (uint16_t)dr.w, (uint16_t)dr.h,
                               (uint16_t)dr.x, (uint16_t)dr.y,
                               (uint16_t)pitch,
                               (uint16_t)MD64API_GRP_FMT_XRGB8888);
            }
        } else if (!had_input) {
            /* Idle: yield CPU if nothing changed and no pending input. */
            yield();
        }

        /* Hardware cursor: update cursor image and position in kernel. */
        if (hwcur_enabled) {
            if (cur && cur != hwcur_last) {
                uint32_t argb[CURSOR_W * CURSOR_H];
                cursor_rgba_to_argb8888(cur, argb);
                (void)gfx2d_cursor_set(&g, CURSOR_W, CURSOR_H, 0, 0, argb);
                hwcur_last = cur;
            }
            if (mx != hwcur_last_x || my != hwcur_last_y) {
                (void)gfx2d_cursor_move(&g, mx, my);
                hwcur_last_x = mx;
                hwcur_last_y = my;
            }
        } else {
            /* Software cursor tracking for dirty rect updates */
            hwcur_last = cur;
            hwcur_last_x = mx;
            hwcur_last_y = my;
        }
    }
}
