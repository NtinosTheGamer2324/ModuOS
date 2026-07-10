/*
 * imgview.c — Standalone image viewer for ModuOS
 *
 * Supported formats:
 *   - BMP  (1/4/8/24/32-bit, uncompressed + RLE-8)
 *   - PPM  (P6 binary, P3 ASCII)
 *   - QOI  (Quite OK Image format)
 *   - TGA  (24/32-bit uncompressed, bottom-up & top-down)
 *   - farbfeld (ff)
 *
 * Controls:
 *   Arrow keys    — pan image
 *   + / -         — zoom in / out
 *   0             — fit to window
 *   1             — 100% zoom
 *   N / PageDown  — next file in directory
 *   P / PageUp    — previous file in directory
 *   R             — reload current file
 *   F             — flip channel order (BGR <-> RGB debug aid)
 *   I             — toggle info overlay
 *   ESC / Q       — quit
 *
 * Copyright © 2026 New Technologies Software — GPL v2.0
 */

#include "libc.h"
#include "NodGL.h"
#include "lib_fnt.h"
#include "../include/moduos/kernel/events/events.h"

/* ============================================================
   Compile-time tunables
   ============================================================ */

#define APP_TITLE        "Image Viewer"
#define FONT_PATH        "/ModuOS/shared/assets/fonts/Terminus.fnt"
#define CURSOR_ARROW_PATH "/ModuOS/shared/assets/mouse/arrow.bmp"
#define CURSOR_HAND_PATH  "/ModuOS/shared/assets/mouse/hand.bmp"
#define CURSOR_W          16
#define CURSOR_H          16

#define MAX_PATH          512
#define MAX_DIR_ENTRIES   512
#define INFO_LINES        8

#define ZOOM_MIN          0.05f
#define ZOOM_MAX          32.0f
#define ZOOM_STEP         0.25f
#define PAN_STEP          32

#define FOOTER_H          22
#define HEADER_H          32

/* Palette */
#define COL_BG            0xFF0D1117
#define COL_PANEL         0xFF161B22
#define COL_BORDER        0xFF30363D
#define COL_TEXT          0xFFC9D1D9
#define COL_MUTED         0xFF8B949E
#define COL_ACCENT        0xFF58A6FF
#define COL_ACCENT2       0xFF3FB950
#define COL_ERROR         0xFFF85149
#define COL_WARN          0xFFE3B341
#define COL_WHITE         0xFFFFFFFF
#define COL_BLACK         0xFF000000
#define COL_CHECKER_A     0xFF1C1C1C
#define COL_CHECKER_B     0xFF282828
#define COL_OVERLAY_BG    0xCC000000   /* info overlay background */

/* ============================================================
   Image formats
   ============================================================ */

typedef enum {
    FMT_UNKNOWN = 0,
    FMT_BMP,
    FMT_PPM,
    FMT_QOI,
    FMT_TGA,
    FMT_FF,     /* farbfeld */
} ImgFormat;

/* Decoded image — always RGBA8888 in host byte order 0xAARRGGBB */
typedef struct {
    uint8_t  *pixels;   /* malloc'd; NULL if not loaded */
    uint32_t  width;
    uint32_t  height;
    ImgFormat fmt;
    char      path[MAX_PATH];
    /* Metadata for info overlay */
    uint32_t  file_size;
    int       bpp_src;  /* source bits-per-pixel (informational) */
} Image;

/* ============================================================
   File list
   ============================================================ */

typedef struct {
    char name[256];
} FileEntry;

/* ============================================================
   Cursor
   ============================================================ */

typedef struct {
    uint8_t rgba[CURSOR_W * CURSOR_H * 4];
    int     loaded;
} CursorImg;

/* ============================================================
   App state
   ============================================================ */

static struct {
    /* Graphics */
    NodGL_Device  device;
    NodGL_Context ctx;
    NodGL_Texture backbuf_tex;
    uint8_t      *bb;
    uint32_t      bb_pitch;
    uint32_t      screen_w;
    uint32_t      screen_h;

    /* Font */
    fnt_font_t   *font;

    /* Cursors */
    CursorImg     cur_arrow;
    CursorImg     cur_hand;

    /* Current image */
    Image         img;
    int           img_failed;
    char          fail_msg[256];

    /* Viewport */
    float         zoom;
    int           pan_x;
    int           pan_y;
    int           show_info;
    int           flip_channels; /* BGR debug */

    /* Directory listing */
    char          dir[MAX_PATH];
    FileEntry    *files;
    int           file_count;
    int           file_idx;

    /* Mouse */
    int32_t       mouse_x;
    int32_t       mouse_y;
    uint8_t       mouse_btn;
    uint8_t       mouse_btn_prev;
    int32_t       drag_start_x;
    int32_t       drag_start_y;
    int           dragging;
    int           pan_drag_x;
    int           pan_drag_y;

    /* Status */
    char          status[256];
    uint32_t      status_col;
} app;

/* ============================================================
   Helpers: set_status
   ============================================================ */
static void set_status(const char *msg, uint32_t col) {
    size_t l = strlen(msg);
    if (l >= sizeof(app.status)) l = sizeof(app.status) - 1;
    memcpy(app.status, msg, l);
    app.status[l] = 0;
    app.status_col = col;
}

/* ============================================================
   Graphics primitives (software rasteriser into backbuffer)
   ============================================================ */

static inline void gfx_put(int x, int y, uint32_t col) {
    if (!app.bb) return;
    if ((uint32_t)x >= app.screen_w || (uint32_t)y >= app.screen_h) return;
    ((uint32_t *)(app.bb + (uint64_t)y * app.bb_pitch))[x] = col;
}

static void gfx_fill(int x, int y, int w, int h, uint32_t col) {
    if (!app.bb || w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > (int)app.screen_w) x1 = (int)app.screen_w;
    int y1 = y + h; if (y1 > (int)app.screen_h) y1 = (int)app.screen_h;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = (uint32_t *)(app.bb + (uint64_t)yy * app.bb_pitch);
        for (int xx = x0; xx < x1; xx++) row[xx] = col;
    }
}

static void gfx_fill_alpha(int x, int y, int w, int h, uint32_t col) {
    /* col = 0xAARRGGBB */
    uint8_t a  = (col >> 24) & 0xFF;
    if (a == 0xFF) { gfx_fill(x, y, w, h, col); return; }
    uint8_t sr = (col >> 16) & 0xFF;
    uint8_t sg = (col >>  8) & 0xFF;
    uint8_t sb = (col      ) & 0xFF;
    uint8_t ia = 255 - a;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > (int)app.screen_w) x1 = (int)app.screen_w;
    int y1 = y + h; if (y1 > (int)app.screen_h) y1 = (int)app.screen_h;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = (uint32_t *)(app.bb + (uint64_t)yy * app.bb_pitch);
        for (int xx = x0; xx < x1; xx++) {
            uint32_t d = row[xx];
            uint8_t nr = (uint8_t)((sr * a + ((d >> 16) & 0xFF) * ia) / 255);
            uint8_t ng = (uint8_t)((sg * a + ((d >>  8) & 0xFF) * ia) / 255);
            uint8_t nb = (uint8_t)((sb * a + ( d        & 0xFF) * ia) / 255);
            row[xx] = 0xFF000000u | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | nb;
        }
    }
}

static void gfx_rect_outline(int x, int y, int w, int h, uint32_t col) {
    gfx_fill(x,         y,         w, 1, col);
    gfx_fill(x,         y + h - 1, w, 1, col);
    gfx_fill(x,         y,         1, h, col);
    gfx_fill(x + w - 1, y,         1, h, col);
}

static inline void gfx_blend_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if ((uint32_t)x >= app.screen_w || (uint32_t)y >= app.screen_h) return;
    if (!app.bb) return;
    uint32_t *dst = (uint32_t *)(app.bb + (uint64_t)y * app.bb_pitch) + x;
    if (a == 255) {
        *dst = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    } else if (a > 0) {
        uint32_t d = *dst;
        uint8_t dr = (d >> 16) & 0xFF;
        uint8_t dg = (d >>  8) & 0xFF;
        uint8_t db = (d      ) & 0xFF;
        uint32_t ia = 255 - a;
        *dst = 0xFF000000u
             | ((uint32_t)((r * a + dr * ia) / 255) << 16)
             | ((uint32_t)((g * a + dg * ia) / 255) <<  8)
             | ((uint32_t)((b * a + db * ia) / 255)      );
    }
}

/* Checkerboard for transparency */
static void gfx_checker(int x, int y, int w, int h, int cell) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > (int)app.screen_w) x1 = (int)app.screen_w;
    int y1 = y + h; if (y1 > (int)app.screen_h) y1 = (int)app.screen_h;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = (uint32_t *)(app.bb + (uint64_t)yy * app.bb_pitch);
        for (int xx = x0; xx < x1; xx++) {
            int tx = xx / cell, ty = yy / cell;
            row[xx] = ((tx + ty) & 1) ? COL_CHECKER_B : COL_CHECKER_A;
        }
    }
}

/* Font glyph rendering */
static void gfx_char(int x, int y, char c, uint32_t col) {
    if (!app.font) return;
    fnt_glyph_t *g = fnt_get_glyph(app.font, (uint32_t)(unsigned char)c);
    if (!g) return;
    uint8_t r = (col >> 16) & 0xFF;
    uint8_t gv = (col >> 8) & 0xFF;
    uint8_t b  = col & 0xFF;
    for (int dy = 0; dy < g->bitmap_height; dy++)
        for (int dx = 0; dx < g->bitmap_width; dx++)
            if (fnt_get_pixel(g, dx, dy))
                gfx_blend_pixel(x + dx, y + dy, r, gv, b, 0xFF);
}

static void gfx_text(int x, int y, const char *s, uint32_t col) {
    if (!app.font || !s) return;
    int cx = x;
    while (*s) {
        fnt_glyph_t *g = fnt_get_glyph(app.font, (uint32_t)(unsigned char)*s);
        if (g) { gfx_char(cx, y, *s, col); cx += g->width; }
        s++;
    }
}

static void gfx_text_clip(int x, int y, int max_w, const char *s, uint32_t col) {
    if (!app.font || !s) return;
    int cx = x;
    while (*s) {
        fnt_glyph_t *g = fnt_get_glyph(app.font, (uint32_t)(unsigned char)*s);
        if (!g) { s++; continue; }
        if (cx + g->width > x + max_w) {
            fnt_glyph_t *dot = fnt_get_glyph(app.font, '.');
            if (dot && cx + dot->width * 3 <= x + max_w) {
                gfx_char(cx, y, '.', col); cx += dot->width;
                gfx_char(cx, y, '.', col); cx += dot->width;
                gfx_char(cx, y, '.', col);
            }
            return;
        }
        gfx_char(cx, y, *s, col); cx += g->width; s++;
    }
}

static void gfx_text_right(int x, int y, const char *s, uint32_t col) {
    int w = fnt_string_width(app.font, s);
    gfx_text(x - w, y, s, col);
}

/* Draw number as decimal */
static void gfx_num(int x, int y, long n, uint32_t col) {
    char buf[32];
    /* Simple itoa */
    int neg = 0;
    if (n < 0) { neg = 1; n = -n; }
    int i = 0;
    char tmp[32];
    if (n == 0) { tmp[i++] = '0'; }
    while (n > 0) { tmp[i++] = '0' + (n % 10); n /= 10; }
    if (neg) tmp[i++] = '-';
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;
    gfx_text(x, y, buf, col);
}

/* Draw floating-point (1 decimal place) */
static void gfx_float1(int x, int y, float v, uint32_t col) {
    char buf[32];
    int whole = (int)v;
    int frac  = (int)((v - (float)whole) * 10.0f + 0.5f);
    if (frac >= 10) { frac = 0; whole++; }
    /* Build string: whole.frac */
    char tmp[16];
    int i = 0, neg = 0;
    if (whole < 0) { neg = 1; whole = -whole; }
    if (whole == 0) { tmp[i++] = '0'; }
    while (whole > 0) { tmp[i++] = '0' + (whole % 10); whole /= 10; }
    if (neg) tmp[i++] = '-';
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j++] = '.';
    buf[j++] = '0' + frac;
    buf[j] = 0;
    gfx_text(x, y, buf, col);
}

/* ============================================================
   Software cursor
   ============================================================ */

static void cursor_load(const char *path, CursorImg *out) {
    out->loaded = 0;
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return;
    long sz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (sz < 54 || sz > 64*1024) { close(fd); return; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { close(fd); return; }
    size_t got = 0;
    while (got < (size_t)sz) {
        ssize_t n = read(fd, buf + got, (size_t)sz - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got < 54 || buf[0] != 'B' || buf[1] != 'M') { free(buf); return; }
    uint32_t px_off = (uint32_t)buf[10] | ((uint32_t)buf[11]<<8)
                    | ((uint32_t)buf[12]<<16) | ((uint32_t)buf[13]<<24);
    int32_t  bw = (int32_t)((uint32_t)buf[18] | ((uint32_t)buf[19]<<8)
                           | ((uint32_t)buf[20]<<16) | ((uint32_t)buf[21]<<24));
    int32_t  bh = (int32_t)((uint32_t)buf[22] | ((uint32_t)buf[23]<<8)
                           | ((uint32_t)buf[24]<<16) | ((uint32_t)buf[25]<<24));
    uint16_t bpp = (uint16_t)(buf[28] | (buf[29]<<8));
    if (bpp != 32 || bw != CURSOR_W) { free(buf); return; }
    int top_down = 0;
    if (bh < 0) { top_down = 1; bh = -bh; }
    if (bh != CURSOR_H) { free(buf); return; }
    uint32_t stride = (uint32_t)bw * 4u;
    if (px_off + stride * (uint32_t)bh > (uint32_t)sz) { free(buf); return; }
    for (uint32_t y = 0; y < (uint32_t)CURSOR_H; y++) {
        uint32_t sy = top_down ? y : ((uint32_t)CURSOR_H - 1u - y);
        const uint8_t *row = buf + px_off + sy * stride;
        for (uint32_t x = 0; x < (uint32_t)CURSOR_W; x++) {
            uint32_t di = (y * (uint32_t)CURSOR_W + x) * 4u;
            out->rgba[di+0] = row[x*4+2]; /* R */
            out->rgba[di+1] = row[x*4+1]; /* G */
            out->rgba[di+2] = row[x*4+0]; /* B */
            out->rgba[di+3] = row[x*4+3]; /* A */
        }
    }
    free(buf);
    out->loaded = 1;
}

static void cursor_blit(const CursorImg *cur, int32_t cx, int32_t cy) {
    if (!cur || !cur->loaded || !app.bb) return;
    for (int dy = 0; dy < CURSOR_H; dy++) {
        int py = cy + dy;
        if (py < 0 || (uint32_t)py >= app.screen_h) continue;
        uint32_t *row = (uint32_t *)(app.bb + (uint64_t)py * app.bb_pitch);
        for (int dx = 0; dx < CURSOR_W; dx++) {
            int px = cx + dx;
            if (px < 0 || (uint32_t)px >= app.screen_w) continue;
            uint32_t si = (uint32_t)(dy * CURSOR_W + dx) * 4u;
            uint8_t sr = cur->rgba[si+0], sg = cur->rgba[si+1];
            uint8_t sb = cur->rgba[si+2], sa = cur->rgba[si+3];
            if (sa == 0) continue;
            if (sa == 255) {
                row[px] = 0xFF000000u | ((uint32_t)sr<<16) | ((uint32_t)sg<<8) | sb;
            } else {
                uint32_t d = row[px], ia = 255u - sa;
                row[px] = 0xFF000000u
                    | (((uint32_t)((sr*sa + ((d>>16)&0xFF)*ia)/255u))<<16)
                    | (((uint32_t)((sg*sa + ((d>> 8)&0xFF)*ia)/255u))<< 8)
                    | (((uint32_t)((sb*sa + ( d     &0xFF)*ia)/255u))    );
            }
        }
    }
}

/* ============================================================
   Image blitter — RGBA nearest-neighbour zoom with clipping
   ============================================================ */

static void blit_rgba_zoom(const uint8_t *pixels, int src_w, int src_h,
                            int dst_x, int dst_y, int dst_w, int dst_h,
                            int clip_x, int clip_y, int clip_w, int clip_h,
                            int flip_ch)
{
    if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) return;
    int cx0 = clip_x, cy0 = clip_y;
    int cx1 = clip_x + clip_w, cy1 = clip_y + clip_h;

    for (int dy = 0; dy < dst_h; dy++) {
        int abs_dy = dst_y + dy;
        if (abs_dy < cy0 || abs_dy >= cy1) continue;
        int sy = dy * src_h / dst_h;
        for (int dx = 0; dx < dst_w; dx++) {
            int abs_dx = dst_x + dx;
            if (abs_dx < cx0 || abs_dx >= cx1) continue;
            int sx = dx * src_w / dst_w;
            const uint8_t *p = pixels + (sy * src_w + sx) * 4;
            uint8_t r = flip_ch ? p[2] : p[0];
            uint8_t g = p[1];
            uint8_t b = flip_ch ? p[0] : p[2];
            uint8_t a = p[3];
            gfx_blend_pixel(abs_dx, abs_dy, r, g, b, a);
        }
    }
}

/* ============================================================
   Format detection
   ============================================================ */

static ImgFormat detect_format(const uint8_t *hdr, size_t hlen,
                                const char *path)
{
    /* BMP */
    if (hlen >= 2 && hdr[0] == 'B' && hdr[1] == 'M') return FMT_BMP;
    /* QOI */
    if (hlen >= 4 && hdr[0]=='q' && hdr[1]=='o' && hdr[2]=='i' && hdr[3]=='f')
        return FMT_QOI;
    /* farbfeld */
    if (hlen >= 8 && memcmp(hdr, "farbfeld", 8) == 0) return FMT_FF;
    /* PPM binary (P6) or ASCII (P3) */
    if (hlen >= 2 && hdr[0] == 'P' && (hdr[1] == '6' || hdr[1] == '3'))
        return FMT_PPM;
    /* TGA — no magic; detect by extension */
    if (path) {
        size_t l = strlen(path);
        if (l >= 4) {
            const char *e = path + l - 4;
            if ((e[0]=='.' || e[1]=='.') &&
                ((e[1]=='t'||e[1]=='T') && (e[2]=='g'||e[2]=='G') && (e[3]=='a'||e[3]=='A')))
                return FMT_TGA;
            /* also .tga at end */
            if (l >= 4 && (path[l-4]=='.' &&
                (path[l-3]=='t'||path[l-3]=='T') &&
                (path[l-2]=='g'||path[l-2]=='G') &&
                (path[l-1]=='a'||path[l-1]=='A')))
                return FMT_TGA;
        }
    }
    return FMT_UNKNOWN;
}

/* ============================================================
   BMP decoder — supports 1/4/8/24/32-bit + RLE-8
   Output: malloc'd RGBA pixels (R,G,B,A order), width, height
   ============================================================ */

static inline uint16_t rd16le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static inline uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0]<<24) | ((uint32_t)p[1]<<16) | ((uint32_t)p[2]<<8) | (uint32_t)p[3];
}

static int bmp_decode(const uint8_t *data, size_t size,
                      uint8_t **out_pixels, uint32_t *out_w, uint32_t *out_h,
                      int *out_bpp)
{
    if (size < 54) return -1;
    if (data[0] != 'B' || data[1] != 'M') return -1;

    uint32_t px_off    = rd32le(data + 10);
    uint32_t dib_size  = rd32le(data + 14);
    int32_t  bmp_w     = (int32_t)rd32le(data + 18);
    int32_t  bmp_h     = (int32_t)rd32le(data + 22);
    uint16_t bpp       = rd16le(data + 28);
    uint32_t compress  = rd32le(data + 30);
    uint32_t clr_used  = (dib_size >= 36) ? rd32le(data + 46) : 0;

    if (bmp_w <= 0 || bmp_w > 65536) return -1;
    if (bmp_h == 0 || bmp_h > 65536 || bmp_h < -65536) return -1;

    int top_down = (bmp_h < 0);
    uint32_t w = (uint32_t)bmp_w;
    uint32_t h = (uint32_t)(bmp_h < 0 ? -bmp_h : bmp_h);

    if (out_bpp) *out_bpp = bpp;

    /* Palette */
    uint32_t pal[256];
    memset(pal, 0, sizeof(pal));
    if (bpp <= 8) {
        uint32_t pal_count = clr_used ? clr_used : (1u << bpp);
        if (pal_count > 256) pal_count = 256;
        uint32_t pal_off = 14 + dib_size;
        for (uint32_t i = 0; i < pal_count; i++) {
            uint32_t o = pal_off + i * 4;
            if (o + 3 >= size) break;
            uint8_t b = data[o+0], g = data[o+1], r = data[o+2];
            pal[i] = 0xFF000000u | ((uint32_t)r<<16) | ((uint32_t)g<<8) | b;
        }
    }

    uint8_t *pixels = (uint8_t *)calloc(w * h, 4);
    if (!pixels) return -1;

    /* RLE-8 decoder */
    if (compress == 1 && bpp == 8) {
        int32_t cx = 0, cy = 0;
        const uint8_t *p = data + px_off;
        const uint8_t *end = data + size;
        while (p + 1 < end) {
            uint8_t cnt = p[0], val = p[1]; p += 2;
            if (cnt > 0) {
                /* Encoded run */
                for (uint8_t i = 0; i < cnt && cx < (int32_t)w; i++, cx++) {
                    int32_t ty = top_down ? cy : (int32_t)(h-1) - cy;
                    if (ty >= 0 && (uint32_t)ty < h) {
                        uint32_t *dst = (uint32_t *)(pixels + (ty * w + cx) * 4);
                        uint32_t c = pal[val];
                        dst[0] = c;
                    }
                }
            } else {
                /* Escape codes */
                if (val == 0) { cx = 0; cy++; }
                else if (val == 1) { break; }
                else if (val == 2) {
                    if (p + 1 >= end) break;
                    cx += p[0]; cy += p[1]; p += 2;
                } else {
                    /* Absolute run */
                    for (uint8_t i = 0; i < val && p < end; i++, cx++) {
                        uint8_t idx = *p++;
                        int32_t ty = top_down ? cy : (int32_t)(h-1) - cy;
                        if (ty >= 0 && (uint32_t)ty < h && cx >= 0 && (uint32_t)cx < w) {
                            uint32_t *dst = (uint32_t *)(pixels + (ty * w + cx) * 4);
                            dst[0] = pal[idx];
                        }
                    }
                    /* Align to word */
                    if (val & 1) p++;
                }
            }
        }
    } else {
        /* Uncompressed */
        for (uint32_t row = 0; row < h; row++) {
            uint32_t src_row = top_down ? row : (h - 1 - row);
            uint8_t *dst_row = pixels + row * w * 4;

            if (bpp == 24) {
                uint32_t stride = ((w * 3 + 3) & ~3u);
                uint32_t off = px_off + src_row * stride;
                if (off + stride > size) break;
                const uint8_t *s = data + off;
                for (uint32_t x = 0; x < w; x++) {
                    dst_row[x*4+0] = s[x*3+2]; /* R */
                    dst_row[x*4+1] = s[x*3+1]; /* G */
                    dst_row[x*4+2] = s[x*3+0]; /* B */
                    dst_row[x*4+3] = 0xFF;
                }
            } else if (bpp == 32) {
                uint32_t stride = w * 4;
                uint32_t off = px_off + src_row * stride;
                if (off + stride > size) break;
                const uint8_t *s = data + off;
                for (uint32_t x = 0; x < w; x++) {
                    dst_row[x*4+0] = s[x*4+2]; /* R */
                    dst_row[x*4+1] = s[x*4+1]; /* G */
                    dst_row[x*4+2] = s[x*4+0]; /* B */
                    dst_row[x*4+3] = s[x*4+3]; /* A */
                }
            } else if (bpp == 8) {
                uint32_t stride = ((w + 3) & ~3u);
                uint32_t off = px_off + src_row * stride;
                if (off + stride > size) break;
                const uint8_t *s = data + off;
                for (uint32_t x = 0; x < w; x++) {
                    uint32_t c = pal[s[x]];
                    dst_row[x*4+0] = (c >> 16) & 0xFF;
                    dst_row[x*4+1] = (c >>  8) & 0xFF;
                    dst_row[x*4+2] = (c      ) & 0xFF;
                    dst_row[x*4+3] = 0xFF;
                }
            } else if (bpp == 4) {
                uint32_t stride = (((w+1)/2 + 3) & ~3u);
                uint32_t off = px_off + src_row * stride;
                if (off + stride > size) break;
                const uint8_t *s = data + off;
                for (uint32_t x = 0; x < w; x++) {
                    uint8_t byte = s[x/2];
                    uint8_t idx  = (x & 1) ? (byte & 0x0F) : (byte >> 4);
                    uint32_t c = pal[idx & 0x0F];
                    dst_row[x*4+0] = (c >> 16) & 0xFF;
                    dst_row[x*4+1] = (c >>  8) & 0xFF;
                    dst_row[x*4+2] = (c      ) & 0xFF;
                    dst_row[x*4+3] = 0xFF;
                }
            } else if (bpp == 1) {
                uint32_t stride = (((w+7)/8 + 3) & ~3u);
                uint32_t off = px_off + src_row * stride;
                if (off + stride > size) break;
                const uint8_t *s = data + off;
                for (uint32_t x = 0; x < w; x++) {
                    uint8_t bit = (s[x/8] >> (7 - (x & 7))) & 1;
                    uint32_t c = pal[bit];
                    dst_row[x*4+0] = (c >> 16) & 0xFF;
                    dst_row[x*4+1] = (c >>  8) & 0xFF;
                    dst_row[x*4+2] = (c      ) & 0xFF;
                    dst_row[x*4+3] = 0xFF;
                }
            } else {
                /* Unsupported bpp — leave row black */
            }
        }
    }

    *out_pixels = pixels;
    *out_w = w;
    *out_h = h;
    return 0;
}

/* ============================================================
   PPM decoder (P3 ASCII + P6 binary)
   ============================================================ */

static int ppm_skip_whitespace_and_comments(const uint8_t *data, size_t size, size_t *pos) {
    while (*pos < size) {
        if (data[*pos] == '#') {
            while (*pos < size && data[*pos] != '\n') (*pos)++;
        } else if (data[*pos] == ' '  || data[*pos] == '\t' ||
                   data[*pos] == '\n' || data[*pos] == '\r') {
            (*pos)++;
        } else {
            return 0;
        }
    }
    return -1; /* EOF */
}

static int ppm_read_uint(const uint8_t *data, size_t size, size_t *pos, uint32_t *out) {
    ppm_skip_whitespace_and_comments(data, size, pos);
    if (*pos >= size) return -1;
    if (data[*pos] < '0' || data[*pos] > '9') return -1;
    uint32_t v = 0;
    while (*pos < size && data[*pos] >= '0' && data[*pos] <= '9') {
        v = v * 10 + (data[(*pos)++] - '0');
    }
    *out = v;
    return 0;
}

static int ppm_decode(const uint8_t *data, size_t size,
                      uint8_t **out_pixels, uint32_t *out_w, uint32_t *out_h,
                      int *out_bpp)
{
    if (size < 3) return -1;
    int is_binary = (data[1] == '6');
    if (data[0] != 'P' || (data[1] != '6' && data[1] != '3')) return -1;
    if (out_bpp) *out_bpp = 24;

    size_t pos = 2;
    uint32_t w, h, maxval;
    if (ppm_read_uint(data, size, &pos, &w)      < 0) return -1;
    if (ppm_read_uint(data, size, &pos, &h)      < 0) return -1;
    if (ppm_read_uint(data, size, &pos, &maxval) < 0) return -1;
    if (w == 0 || h == 0 || w > 65536 || h > 65536) return -1;
    if (maxval == 0 || maxval > 65535) return -1;

    /* Skip exactly one whitespace byte after maxval */
    if (pos < size && (data[pos] == '\n' || data[pos] == '\r' || data[pos] == ' '))
        pos++;

    uint8_t *pixels = (uint8_t *)malloc(w * h * 4);
    if (!pixels) return -1;

    if (is_binary) {
        int bytes_per_sample = (maxval > 255) ? 2 : 1;
        size_t needed = (size_t)w * h * 3 * (size_t)bytes_per_sample;
        if (pos + needed > size) { free(pixels); return -1; }
        const uint8_t *p = data + pos;
        for (uint32_t i = 0; i < w * h; i++) {
            uint8_t r, g, b;
            if (bytes_per_sample == 2) {
                r = (uint8_t)((((uint32_t)p[0]<<8)|p[1]) * 255 / maxval); p += 2;
                g = (uint8_t)((((uint32_t)p[0]<<8)|p[1]) * 255 / maxval); p += 2;
                b = (uint8_t)((((uint32_t)p[0]<<8)|p[1]) * 255 / maxval); p += 2;
            } else {
                r = (uint8_t)(*p++ * 255 / maxval);
                g = (uint8_t)(*p++ * 255 / maxval);
                b = (uint8_t)(*p++ * 255 / maxval);
            }
            pixels[i*4+0] = r;
            pixels[i*4+1] = g;
            pixels[i*4+2] = b;
            pixels[i*4+3] = 0xFF;
        }
    } else {
        /* P3 ASCII */
        for (uint32_t i = 0; i < w * h; i++) {
            uint32_t r, g, b;
            if (ppm_read_uint(data, size, &pos, &r) < 0 ||
                ppm_read_uint(data, size, &pos, &g) < 0 ||
                ppm_read_uint(data, size, &pos, &b) < 0) {
                free(pixels); return -1;
            }
            pixels[i*4+0] = (uint8_t)(r * 255 / maxval);
            pixels[i*4+1] = (uint8_t)(g * 255 / maxval);
            pixels[i*4+2] = (uint8_t)(b * 255 / maxval);
            pixels[i*4+3] = 0xFF;
        }
    }

    *out_pixels = pixels;
    *out_w = w;
    *out_h = h;
    return 0;
}

/* ============================================================
   QOI decoder
   ============================================================ */

#define QOI_OP_INDEX  0x00u
#define QOI_OP_DIFF   0x40u
#define QOI_OP_LUMA   0x80u
#define QOI_OP_RUN    0xC0u
#define QOI_OP_RGB    0xFEu
#define QOI_OP_RGBA   0xFFu

static int qoi_decode(const uint8_t *data, size_t size,
                      uint8_t **out_pixels, uint32_t *out_w, uint32_t *out_h,
                      int *out_bpp)
{
    if (size < 14) return -1;
    if (memcmp(data, "qoif", 4) != 0) return -1;

    uint32_t w = rd32be(data + 4);
    uint32_t h = rd32be(data + 8);
    uint8_t  channels = data[12];
    /* colorspace = data[13]; ignored */

    if (w == 0 || h == 0 || w > 65536 || h > 65536) return -1;
    if (channels < 3 || channels > 4) return -1;
    if (out_bpp) *out_bpp = channels * 8;

    uint8_t *pixels = (uint8_t *)malloc(w * h * 4);
    if (!pixels) return -1;

    uint8_t index[64][4];
    memset(index, 0, sizeof(index));

    uint8_t px[4] = {0, 0, 0, 255};
    size_t  pos = 14;
    uint32_t done = 0;
    uint32_t total = w * h;

    while (done < total && pos < size) {
        uint8_t tag = data[pos++];

        if (tag == QOI_OP_RGB) {
            if (pos + 2 >= size) break;
            px[0] = data[pos++];
            px[1] = data[pos++];
            px[2] = data[pos++];
        } else if (tag == QOI_OP_RGBA) {
            if (pos + 3 >= size) break;
            px[0] = data[pos++];
            px[1] = data[pos++];
            px[2] = data[pos++];
            px[3] = data[pos++];
        } else if ((tag & 0xC0u) == QOI_OP_INDEX) {
            memcpy(px, index[tag & 0x3Fu], 4);
        } else if ((tag & 0xC0u) == QOI_OP_DIFF) {
            px[0] += (uint8_t)(((tag >> 4) & 3) - 2);
            px[1] += (uint8_t)(((tag >> 2) & 3) - 2);
            px[2] += (uint8_t)(( tag       & 3) - 2);
        } else if ((tag & 0xC0u) == QOI_OP_LUMA) {
            if (pos >= size) break;
            uint8_t b2 = data[pos++];
            int vg = (int)(tag & 0x3Fu) - 32;
            px[0] += (uint8_t)(vg - 8 + ((b2 >> 4) & 0x0Fu));
            px[1] += (uint8_t)(vg);
            px[2] += (uint8_t)(vg - 8 + ( b2       & 0x0Fu));
        } else if ((tag & 0xC0u) == QOI_OP_RUN) {
            uint32_t run = (uint32_t)(tag & 0x3Fu) + 1;
            while (run-- && done < total) {
                uint8_t *d = pixels + done * 4;
                d[0] = px[0]; d[1] = px[1]; d[2] = px[2]; d[3] = px[3];
                done++;
            }
            uint32_t hash = ((uint32_t)px[0]*3 + (uint32_t)px[1]*5 + (uint32_t)px[2]*7 + (uint32_t)px[3]*11) % 64;
            memcpy(index[hash], px, 4);
            continue;
        }

        uint32_t hash = ((uint32_t)px[0]*3 + (uint32_t)px[1]*5 + (uint32_t)px[2]*7 + (uint32_t)px[3]*11) % 64;
        memcpy(index[hash], px, 4);

        uint8_t *d = pixels + done * 4;
        d[0] = px[0]; d[1] = px[1]; d[2] = px[2]; d[3] = px[3];
        done++;
    }

    *out_pixels = pixels;
    *out_w = w;
    *out_h = h;
    return 0;
}

/* ============================================================
   TGA decoder — 24-bit and 32-bit uncompressed
   ============================================================ */

static int tga_decode(const uint8_t *data, size_t size,
                      uint8_t **out_pixels, uint32_t *out_w, uint32_t *out_h,
                      int *out_bpp)
{
    if (size < 18) return -1;

    uint8_t  id_len    = data[0];
    uint8_t  color_map = data[1];
    uint8_t  img_type  = data[2];
    /* color map fields: 3-7 */
    uint16_t w          = rd16le(data + 12);
    uint16_t h          = rd16le(data + 14);
    uint8_t  bpp        = data[16];
    uint8_t  descriptor = data[17];

    (void)color_map;

    /* Only handle uncompressed true-colour (type 2) and rle true-colour (10) */
    if (img_type != 2 && img_type != 10) return -1;
    if (bpp != 24 && bpp != 32) return -1;
    if (w == 0 || h == 0) return -1;
    if (out_bpp) *out_bpp = bpp;

    int top_down  = (descriptor >> 5) & 1;
    /* int right_left = (descriptor >> 4) & 1; */ /* ignore for now */

    uint32_t pixel_offset = 18 + id_len;
    /* Skip color map */

    uint8_t *pixels = (uint8_t *)malloc((uint32_t)w * h * 4);
    if (!pixels) return -1;

    if (img_type == 2) {
        /* Uncompressed */
        uint32_t bytes_pp = bpp / 8u;
        uint32_t stride = (uint32_t)w * bytes_pp;
        for (uint32_t row = 0; row < (uint32_t)h; row++) {
            uint32_t src_row = top_down ? row : ((uint32_t)h - 1 - row);
            uint32_t off = pixel_offset + src_row * stride;
            if (off + stride > size) break;
            const uint8_t *s = data + off;
            uint8_t *d = pixels + row * (uint32_t)w * 4;
            for (uint32_t x = 0; x < (uint32_t)w; x++) {
                d[x*4+0] = s[x*bytes_pp+2]; /* R (BGR in file) */
                d[x*4+1] = s[x*bytes_pp+1]; /* G */
                d[x*4+2] = s[x*bytes_pp+0]; /* B */
                d[x*4+3] = (bytes_pp == 4) ? s[x*4+3] : 0xFF;
            }
        }
    } else {
        /* RLE true-colour */
        uint32_t bytes_pp = bpp / 8u;
        const uint8_t *p = data + pixel_offset;
        const uint8_t *end = data + size;
        uint32_t done = 0, total = (uint32_t)w * h;
        while (done < total && p < end) {
            uint8_t rep = *p++;
            uint32_t count = (uint32_t)(rep & 0x7Fu) + 1u;
            if (rep & 0x80u) {
                /* RLE run */
                if (p + bytes_pp > end) break;
                uint8_t sr = p[2], sg = p[1], sb = p[0], sa = (bytes_pp==4)?p[3]:0xFF;
                p += bytes_pp;
                for (uint32_t i = 0; i < count && done < total; i++, done++) {
                    uint32_t row = top_down ? done / w : (total - 1 - done) / w;
                    uint32_t col = done % w;
                    uint8_t *d = pixels + (row * w + col) * 4;
                    d[0]=sr; d[1]=sg; d[2]=sb; d[3]=sa;
                }
            } else {
                /* Raw packet */
                for (uint32_t i = 0; i < count && done < total && p + bytes_pp <= end; i++, done++) {
                    uint32_t row = top_down ? done / w : (total - 1 - done) / w;
                    uint32_t col = done % w;
                    uint8_t *d = pixels + (row * w + col) * 4;
                    d[0]=p[2]; d[1]=p[1]; d[2]=p[0];
                    d[3]=(bytes_pp==4)?p[3]:0xFF;
                    p += bytes_pp;
                }
            }
        }
    }

    *out_pixels = pixels;
    *out_w = (uint32_t)w;
    *out_h = (uint32_t)h;
    return 0;
}

/* ============================================================
   farbfeld decoder — ff (RGBA 16bpc big-endian, 8-byte header "farbfeld")
   ============================================================ */

static int ff_decode(const uint8_t *data, size_t size,
                     uint8_t **out_pixels, uint32_t *out_w, uint32_t *out_h,
                     int *out_bpp)
{
    if (size < 16) return -1;
    if (memcmp(data, "farbfeld", 8) != 0) return -1;
    uint32_t w = rd32be(data + 8);
    uint32_t h = rd32be(data + 12);
    if (w == 0 || h == 0 || w > 65536 || h > 65536) return -1;
    if (out_bpp) *out_bpp = 64;

    size_t needed = 16 + (size_t)w * h * 8;
    if (size < needed) return -1;

    uint8_t *pixels = (uint8_t *)malloc(w * h * 4);
    if (!pixels) return -1;

    const uint8_t *p = data + 16;
    for (uint32_t i = 0; i < w * h; i++) {
        /* Each component is 16-bit big-endian; we take high byte */
        pixels[i*4+0] = p[0]; /* R hi */
        pixels[i*4+1] = p[2]; /* G hi */
        pixels[i*4+2] = p[4]; /* B hi */
        pixels[i*4+3] = p[6]; /* A hi */
        p += 8;
    }

    *out_pixels = pixels;
    *out_w = w;
    *out_h = h;
    return 0;
}

/* ============================================================
   Generic image loader — reads file, dispatches to decoder
   ============================================================ */

static int img_load(const char *path, Image *img) {
    img->pixels = NULL;
    img->width  = 0;
    img->height = 0;
    img->fmt    = FMT_UNKNOWN;
    img->bpp_src = 0;
    img->file_size = 0;

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;

    long sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0 || sz > 256 * 1024 * 1024) { close(fd); return -1; }
    lseek(fd, 0, SEEK_SET);

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { close(fd); return -1; }

    size_t got = 0;
    while (got < (size_t)sz) {
        ssize_t n = read(fd, buf + got, (size_t)sz - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);

    if (got < 4) { free(buf); return -1; }

    img->file_size = (uint32_t)got;

    ImgFormat fmt = detect_format(buf, got, path);
    img->fmt = fmt;

    int rc = -1;
    switch (fmt) {
        case FMT_BMP: rc = bmp_decode(buf, got, &img->pixels, &img->width, &img->height, &img->bpp_src); break;
        case FMT_PPM: rc = ppm_decode(buf, got, &img->pixels, &img->width, &img->height, &img->bpp_src); break;
        case FMT_QOI: rc = qoi_decode(buf, got, &img->pixels, &img->width, &img->height, &img->bpp_src); break;
        case FMT_TGA: rc = tga_decode(buf, got, &img->pixels, &img->width, &img->height, &img->bpp_src); break;
        case FMT_FF:  rc = ff_decode (buf, got, &img->pixels, &img->width, &img->height, &img->bpp_src); break;
        default:      rc = -1; break;
    }

    free(buf);

    if (rc == 0) {
        size_t plen = strlen(path);
        if (plen >= MAX_PATH) plen = MAX_PATH - 1;
        memcpy(img->path, path, plen);
        img->path[plen] = 0;
    }

    return rc;
}

static void img_free(Image *img) {
    if (img->pixels) { free(img->pixels); img->pixels = NULL; }
    img->width = 0;
    img->height = 0;
}

/* ============================================================
   Directory scan
   ============================================================ */

/* Returns 1 if path has an image extension we support */
static int is_image_file(const char *name) {
    size_t l = strlen(name);
    if (l < 4) return 0;
    const char *e = name + l - 4;
    /* .bmp .ppm .qoi .tga .ff_ */
    if (e[0] == '.') {
        if ((e[1]=='b'||e[1]=='B') && (e[2]=='m'||e[2]=='M') && (e[3]=='p'||e[3]=='P')) return 1;
        if ((e[1]=='p'||e[1]=='P') && (e[2]=='p'||e[2]=='P') && (e[3]=='m'||e[3]=='M')) return 1;
        if ((e[1]=='q'||e[1]=='Q') && (e[2]=='o'||e[2]=='O') && (e[3]=='i'||e[3]=='I')) return 1;
        if ((e[1]=='t'||e[1]=='T') && (e[2]=='g'||e[2]=='G') && (e[3]=='a'||e[3]=='A')) return 1;
    }
    if (l >= 3 && name[l-3]=='.' && (name[l-2]=='f'||name[l-2]=='F') && (name[l-1]=='f'||name[l-1]=='F'))
        return 1;
    return 0;
}

static void scan_dir(const char *dir_path) {
    if (app.files) { free(app.files); app.files = NULL; }
    app.file_count = 0;

    int fd = opendir(dir_path);
    if (fd < 0) return;

    /* Count first pass */
    char name[256];
    int is_dir; uint32_t sz;
    int count = 0;
    while (readdir(fd, name, sizeof(name), &is_dir, &sz) > 0) {
        if (!is_dir && is_image_file(name)) count++;
    }
    closedir(fd);

    if (count == 0) return;

    app.files = (FileEntry *)malloc(count * sizeof(FileEntry));
    if (!app.files) return;

    fd = opendir(dir_path);
    if (fd < 0) { free(app.files); app.files = NULL; return; }

    int i = 0;
    while (i < count && readdir(fd, name, sizeof(name), &is_dir, &sz) > 0) {
        if (!is_dir && is_image_file(name)) {
            size_t nl = strlen(name);
            if (nl >= 256) nl = 255;
            memcpy(app.files[i].name, name, nl);
            app.files[i].name[nl] = 0;
            i++;
        }
    }
    closedir(fd);
    app.file_count = i;

    /* Sort alphabetically */
    for (int a = 1; a < app.file_count; a++) {
        FileEntry tmp = app.files[a];
        int b = a - 1;
        while (b >= 0 && strcmp(app.files[b].name, tmp.name) > 0) {
            app.files[b+1] = app.files[b]; b--;
        }
        app.files[b+1] = tmp;
    }
}

/* Build full path from dir + filename */
static void make_path(char *out, size_t outsz, const char *dir, const char *name) {
    size_t dl = strlen(dir), nl = strlen(name);
    if (dl + 1 + nl + 1 > outsz) { out[0] = 0; return; }
    memcpy(out, dir, dl);
    if (dl > 0 && dir[dl-1] != '/') out[dl++] = '/';
    memcpy(out + dl, name, nl + 1);
}

/* ============================================================
   Viewport helpers
   ============================================================ */

static void zoom_fit(void) {
    if (!app.img.pixels || app.img.width == 0 || app.img.height == 0) return;
    int aw = (int)app.screen_w;
    int ah = (int)app.screen_h - HEADER_H - FOOTER_H;
    float zx = (float)aw / (float)app.img.width;
    float zy = (float)ah / (float)app.img.height;
    app.zoom = (zx < zy) ? zx : zy;
    if (app.zoom < ZOOM_MIN) app.zoom = ZOOM_MIN;
    if (app.zoom > ZOOM_MAX) app.zoom = ZOOM_MAX;
    app.pan_x = 0;
    app.pan_y = 0;
}

/* ============================================================
   Load image by file_idx
   ============================================================ */

static void load_current(void) {
    img_free(&app.img);
    app.img_failed = 0;
    app.fail_msg[0] = 0;

    if (app.file_count == 0) {
        set_status("No image files found in directory", COL_WARN);
        return;
    }
    if (app.file_idx < 0) app.file_idx = 0;
    if (app.file_idx >= app.file_count) app.file_idx = app.file_count - 1;

    char path[MAX_PATH];
    make_path(path, sizeof(path), app.dir, app.files[app.file_idx].name);

    int rc = img_load(path, &app.img);
    if (rc != 0) {
        app.img_failed = 1;
        memcpy(app.fail_msg, "Failed to decode: ", 18);
        size_t nl = strlen(app.files[app.file_idx].name);
        if (nl + 18 >= sizeof(app.fail_msg)) nl = sizeof(app.fail_msg) - 19;
        memcpy(app.fail_msg + 18, app.files[app.file_idx].name, nl);
        app.fail_msg[18 + nl] = 0;
        set_status(app.fail_msg, COL_ERROR);
        return;
    }

    zoom_fit();

    /* Build status string */
    char msg[256];
    char num[32];
    int i = 0;
    /* name */
    size_t nn = strlen(app.files[app.file_idx].name);
    if (nn > 40) nn = 40;
    memcpy(msg, app.files[app.file_idx].name, nn); i = (int)nn;
    msg[i++] = ' '; msg[i++] = '(';
    /* format */
    const char *fmtname[] = {"?", "BMP", "PPM", "QOI", "TGA", "FF"};
    const char *fn = fmtname[app.img.fmt];
    size_t fl = strlen(fn);
    memcpy(msg + i, fn, fl); i += (int)fl;
    msg[i++] = ' ';
    /* dimensions */
    sprintf(num, "%ux%u", app.img.width, app.img.height);
    size_t dl = strlen(num);
    memcpy(msg + i, num, dl); i += (int)dl;
    msg[i++] = ' ';
    /* bpp */
    sprintf(num, "%dbpp", app.img.bpp_src);
    dl = strlen(num);
    memcpy(msg + i, num, dl); i += (int)dl;
    msg[i++] = ')';
    msg[i] = 0;
    set_status(msg, COL_ACCENT2);
}

/* ============================================================
   Drawing
   ============================================================ */

static void draw_header(void) {
    gfx_fill(0, 0, (int)app.screen_w, HEADER_H, COL_PANEL);
    gfx_fill(0, HEADER_H - 1, (int)app.screen_w, 1, COL_BORDER);

    /* Title */
    gfx_text(8, 9, APP_TITLE, COL_ACCENT);

    if (app.file_count > 0) {
        /* "N / Total" */
        char buf[64];
        char a[16], b[16];
        sprintf(a, "%d", app.file_idx + 1);
        sprintf(b, "%d", app.file_count);
        memcpy(buf, a, strlen(a)+1);
        strcat(buf, " / ");
        strcat(buf, b);
        int bw = fnt_string_width(app.font, buf);
        gfx_text((int)app.screen_w / 2 - bw / 2, 9, buf, COL_MUTED);

        /* Zoom */
        if (app.img.pixels) {
            char zb[32];
            int zpct = (int)(app.zoom * 100.0f + 0.5f);
            sprintf(zb, "%d%%", zpct);
            gfx_text_right((int)app.screen_w - 8, 9, zb, COL_TEXT);
        }
    }
}

static void draw_footer(void) {
    int y = (int)app.screen_h - FOOTER_H;
    gfx_fill(0, y, (int)app.screen_w, FOOTER_H, COL_PANEL);
    gfx_fill(0, y, (int)app.screen_w, 1, COL_BORDER);
    gfx_text_clip(8, y + 5, (int)app.screen_w - 200, app.status, app.status_col);

    /* Key hints */
    gfx_text_right((int)app.screen_w - 8, y + 5,
                   "+/- zoom  0 fit  I info  N/P nav  Q quit",
                   COL_MUTED);
}

static void draw_image(void) {
    int content_y = HEADER_H;
    int content_h = (int)app.screen_h - HEADER_H - FOOTER_H;
    int content_w = (int)app.screen_w;

    if (!app.img.pixels) {
        gfx_fill(0, content_y, content_w, content_h, COL_BG);
        if (app.img_failed) {
            int tw = fnt_string_width(app.font, app.fail_msg);
            gfx_text(content_w/2 - tw/2,
                     content_y + content_h/2 - 8,
                     app.fail_msg, COL_ERROR);
        } else {
            const char *hint = "Drop an image file path as argument, or press N";
            int tw = fnt_string_width(app.font, hint);
            gfx_text(content_w/2 - tw/2,
                     content_y + content_h/2 - 8,
                     hint, COL_MUTED);
        }
        return;
    }

    /* Checkerboard background */
    gfx_checker(0, content_y, content_w, content_h, 12);

    int disp_w = (int)(app.img.width  * app.zoom + 0.5f);
    int disp_h = (int)(app.img.height * app.zoom + 0.5f);
    if (disp_w < 1) disp_w = 1;
    if (disp_h < 1) disp_h = 1;

    int img_x = content_w  / 2 - disp_w / 2 + app.pan_x;
    int img_y = content_y + content_h / 2 - disp_h / 2 + app.pan_y;

    /* Drop shadow */
    gfx_fill_alpha(img_x + 4, img_y + 4, disp_w, disp_h, 0x66000000);

    /* Image */
    blit_rgba_zoom(app.img.pixels,
                   (int)app.img.width, (int)app.img.height,
                   img_x, img_y, disp_w, disp_h,
                   0, content_y, content_w, content_h,
                   app.flip_channels);

    /* Thin border */
    gfx_rect_outline(img_x - 1, img_y - 1, disp_w + 2, disp_h + 2, COL_BORDER);

    /* Pixel inspector: show RGBA under mouse */
    int rel_mx = app.mouse_x - img_x;
    int rel_my = app.mouse_y - img_y;
    if (disp_w > 0 && disp_h > 0 &&
        rel_mx >= 0 && rel_my >= 0 &&
        rel_mx < disp_w && rel_my < disp_h &&
        app.mouse_y >= content_y) {
        int px = rel_mx * (int)app.img.width  / disp_w;
        int py = rel_my * (int)app.img.height / disp_h;
        if ((uint32_t)px < app.img.width && (uint32_t)py < app.img.height) {
            const uint8_t *p = app.img.pixels + (py * app.img.width + px) * 4;
            char pbuf[80];
            char tmp[16];
            memcpy(pbuf, "px(", 3); int j = 3;
            sprintf(tmp, "%d", px); memcpy(pbuf+j, tmp, strlen(tmp)); j+=strlen(tmp);
            pbuf[j++]=',';
            sprintf(tmp, "%d", py); memcpy(pbuf+j, tmp, strlen(tmp)); j+=strlen(tmp);
            memcpy(pbuf+j, ") R:", 4); j+=4;
            sprintf(tmp, "%d", p[0]); memcpy(pbuf+j,tmp,strlen(tmp)); j+=strlen(tmp);
            memcpy(pbuf+j, " G:", 3); j+=3;
            sprintf(tmp, "%d", p[1]); memcpy(pbuf+j,tmp,strlen(tmp)); j+=strlen(tmp);
            memcpy(pbuf+j, " B:", 3); j+=3;
            sprintf(tmp, "%d", p[2]); memcpy(pbuf+j,tmp,strlen(tmp)); j+=strlen(tmp);
            memcpy(pbuf+j, " A:", 3); j+=3;
            sprintf(tmp, "%d", p[3]); memcpy(pbuf+j,tmp,strlen(tmp)); j+=strlen(tmp);
            pbuf[j] = 0;

            int pw = fnt_string_width(app.font, pbuf) + 12;
            gfx_fill_alpha(8, content_y + content_h - 28, pw, 20, COL_OVERLAY_BG);
            gfx_text(12, content_y + content_h - 22, pbuf, COL_TEXT);

            /* Colour swatch */
            uint32_t swatch = 0xFF000000u
                | ((uint32_t)p[0]<<16) | ((uint32_t)p[1]<<8) | p[2];
            gfx_fill(pw + 16, content_y + content_h - 28, 18, 18, swatch);
            gfx_rect_outline(pw + 16, content_y + content_h - 28, 18, 18, COL_BORDER);
        }
    }

    /* Info overlay */
    if (app.show_info && app.img.pixels) {
        int oy = content_y + 8, ox = 8;
        const char *fmtname[] = {"Unknown", "BMP", "PPM", "QOI", "TGA", "FF"};
        char lines[INFO_LINES][80];
        char tmp[32];

        int nl = 0;
        memcpy(lines[nl], "File: ", 6);
        /* basename */
        const char *bn = app.img.path;
        for (const char *pp = app.img.path; *pp; pp++) if (*pp == '/') bn = pp+1;
        size_t bnl = strlen(bn); if (bnl > 60) bnl = 60;
        memcpy(lines[nl]+6, bn, bnl); lines[nl][6+bnl] = 0; nl++;

        memcpy(lines[nl], "Format: ", 8);
        const char *fn = fmtname[app.img.fmt];
        memcpy(lines[nl]+8, fn, strlen(fn)+1); nl++;

        sprintf(tmp, "%u x %u", app.img.width, app.img.height);
        memcpy(lines[nl], "Size: ", 6);
        memcpy(lines[nl]+6, tmp, strlen(tmp)+1); nl++;

        sprintf(tmp, "%d bpp (source)", app.img.bpp_src);
        memcpy(lines[nl], "Depth: ", 7);
        memcpy(lines[nl]+7, tmp, strlen(tmp)+1); nl++;

        /* File size in KB */
        uint32_t fk = app.img.file_size / 1024;
        sprintf(tmp, "%u KB (%u bytes)", fk, app.img.file_size);
        memcpy(lines[nl], "File size: ", 11);
        memcpy(lines[nl]+11, tmp, strlen(tmp)+1); nl++;

        /* Memory: w*h*4 bytes */
        uint64_t mem = (uint64_t)app.img.width * app.img.height * 4;
        sprintf(tmp, "%u KB decoded", (uint32_t)(mem / 1024));
        memcpy(lines[nl], "Memory: ", 8);
        memcpy(lines[nl]+8, tmp, strlen(tmp)+1); nl++;

        sprintf(tmp, "%.2fx", app.zoom);
        memcpy(lines[nl], "Zoom: ", 6);
        /* Simple float format */
        int zpct = (int)(app.zoom * 100 + 0.5f);
        sprintf(tmp, "%d%%", zpct);
        memcpy(lines[nl]+6, tmp, strlen(tmp)+1); nl++;

        sprintf(tmp, "%d / %d", app.file_idx+1, app.file_count);
        memcpy(lines[nl], "Index: ", 7);
        memcpy(lines[nl]+7, tmp, strlen(tmp)+1); nl++;

        /* Measure panel width */
        int pw = 0;
        for (int li = 0; li < nl; li++) {
            int lw = fnt_string_width(app.font, lines[li]);
            if (lw > pw) pw = lw;
        }
        pw += 20;

        int ph = nl * 16 + 12;
        gfx_fill_alpha(ox, oy, pw, ph, COL_OVERLAY_BG);
        gfx_rect_outline(ox, oy, pw, ph, COL_BORDER);
        for (int li = 0; li < nl; li++) {
            gfx_text(ox + 8, oy + 6 + li * 16, lines[li], COL_TEXT);
        }
    }
}

static void draw_frame(void) {
    gfx_fill(0, 0, (int)app.screen_w, (int)app.screen_h, COL_BG);
    draw_image();
    draw_header();
    draw_footer();

    /* Cursor */
    CursorImg *cur = app.cur_arrow.loaded ? &app.cur_arrow : NULL;
    if (cur) cursor_blit(cur, app.mouse_x, app.mouse_y);

    NodGL_DrawTexture(app.ctx, app.backbuf_tex, 0, 0, 0, 0,
                      app.screen_w, app.screen_h);
    NodGL_PresentContext(app.ctx, 0);
}

/* ============================================================
   Input
   ============================================================ */

static void navigate(int delta) {
    if (app.file_count == 0) return;
    app.file_idx += delta;
    if (app.file_idx < 0) app.file_idx = app.file_count - 1;
    if (app.file_idx >= app.file_count) app.file_idx = 0;
    load_current();
}

static void handle_key(KeyCode kc, char ch) {
    switch (kc) {
        case KEY_ARROW_LEFT:  app.pan_x -= PAN_STEP; return;
        case KEY_ARROW_RIGHT: app.pan_x += PAN_STEP; return;
        case KEY_ARROW_UP:    app.pan_y -= PAN_STEP; return;
        case KEY_ARROW_DOWN:  app.pan_y += PAN_STEP; return;
        case KEY_PAGE_UP:     navigate(-1); return;
        case KEY_PAGE_DOWN:   navigate( 1); return;
        default: break;
    }

    switch (ch) {
        case '+': case '=':
            app.zoom += ZOOM_STEP;
            if (app.zoom > ZOOM_MAX) app.zoom = ZOOM_MAX;
            break;
        case '-':
            app.zoom -= ZOOM_STEP;
            if (app.zoom < ZOOM_MIN) app.zoom = ZOOM_MIN;
            break;
        case '0':
            zoom_fit();
            break;
        case '1':
            app.zoom = 1.0f;
            app.pan_x = app.pan_y = 0;
            break;
        case '2':
            app.zoom = 2.0f;
            app.pan_x = app.pan_y = 0;
            break;
        case 'n': case 'N':
            navigate(1);
            break;
        case 'p': case 'P':
            navigate(-1);
            break;
        case 'r': case 'R':
            load_current();
            break;
        case 'f': case 'F':
            app.flip_channels ^= 1;
            set_status(app.flip_channels ? "Channel flip ON (BGR)" : "Channel flip OFF", COL_WARN);
            break;
        case 'i': case 'I':
            app.show_info ^= 1;
            break;
        case 'q': case 'Q': case 27:
            /* handled in main loop as quit */
            break;
        default: break;
    }
}

/* ============================================================
   Entry point
   ============================================================ */

int md_main(long argc, char **argv) {
    memset(&app, 0, sizeof(app));
    app.zoom        = 1.0f;
    app.status_col  = COL_MUTED;
    app.show_info   = 0;

    printf("imgview starting...\n");

    /* Determine starting directory + optional initial file from argv */
    const char *initial_file = NULL;
    if (argc >= 2 && argv[1]) {
        /* Check if argument is a file or directory */
        fs_file_info_t fi;
        if (stat(argv[1], &fi) == 0) {
            if (fi.is_directory) {
                size_t l = strlen(argv[1]);
                if (l >= MAX_PATH) l = MAX_PATH - 1;
                memcpy(app.dir, argv[1], l);
                app.dir[l] = 0;
            } else {
                /* It's a file: extract directory */
                size_t l = strlen(argv[1]);
                if (l >= MAX_PATH) l = MAX_PATH - 1;
                memcpy(app.dir, argv[1], l);
                app.dir[l] = 0;
                /* Find last slash */
                int last = -1;
                for (int i = (int)l - 1; i >= 0; i--)
                    if (app.dir[i] == '/') { last = i; break; }
                if (last >= 0) {
                    app.dir[last] = 0;
                    initial_file = argv[1] + last + 1;
                } else {
                    app.dir[0] = '/'; app.dir[1] = 0;
                    initial_file = argv[1];
                }
            }
        } else {
            /* Treat as directory path anyway */
            size_t l = strlen(argv[1]);
            if (l >= MAX_PATH) l = MAX_PATH - 1;
            memcpy(app.dir, argv[1], l);
            app.dir[l] = 0;
        }
    } else {
        /* Default: current working directory */
        getcwd(app.dir, sizeof(app.dir));
        if (app.dir[0] == 0) { app.dir[0] = '/'; app.dir[1] = 0; }
    }

    /* Open event device */
    int efd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    if (efd < 0) {
        printf("imgview: cannot open event device\n");
        sleep(2);
        return 2;
    }

    /* Init NodGL */
    if (NodGL_CreateDevice(NodGL_FEATURE_LEVEL_1_0,
                           &app.device, &app.ctx, NULL) != NodGL_OK) {
        printf("imgview: NodGL_CreateDevice failed\n");
        close(efd);
        return 1;
    }
    NodGL_GetScreenResolution(app.device, &app.screen_w, &app.screen_h);
    printf("imgview: screen %ux%u\n", app.screen_w, app.screen_h);

    /* Backbuffer texture */
    NodGL_TextureDesc td;
    memset(&td, 0, sizeof(td));
    td.width = app.screen_w; td.height = app.screen_h;
    td.format = NodGL_FORMAT_R8G8B8A8_UNORM; td.mip_levels = 1;
    if (NodGL_CreateTexture(app.device, &td, &app.backbuf_tex) != NodGL_OK) {
        printf("imgview: CreateTexture failed\n");
        NodGL_ReleaseDevice(app.device);
        close(efd); return 1;
    }
    if (NodGL_MapResource(app.ctx, app.backbuf_tex,
                          (void **)&app.bb, &app.bb_pitch) != NodGL_OK) {
        printf("imgview: MapResource failed\n");
        NodGL_ReleaseResource(app.device, app.backbuf_tex);
        NodGL_ReleaseDevice(app.device);
        close(efd); return 1;
    }

    /* Font */
    {
        int fd = open(FONT_PATH, O_RDONLY, 0);
        if (fd < 0) {
            printf("imgview: font not found: %s\n", FONT_PATH);
            NodGL_ReleaseResource(app.device, app.backbuf_tex);
            NodGL_ReleaseDevice(app.device);
            close(efd); return 1;
        }
        long fsz = lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
        void *fdata = malloc((size_t)fsz);
        if (!fdata) { close(fd); NodGL_ReleaseDevice(app.device); close(efd); return 1; }
        size_t fgot = 0;
        while (fgot < (size_t)fsz) {
            ssize_t r = read(fd, (uint8_t *)fdata + fgot, (size_t)fsz - fgot);
            if (r <= 0) break; fgot += (size_t)r;
        }
        close(fd);
        app.font = fnt_load_font(fdata, fgot);
        free(fdata);
        if (!app.font) {
            printf("imgview: font parse failed\n");
            NodGL_ReleaseDevice(app.device);
            close(efd); return 1;
        }
    }
    printf("imgview: font OK (%u glyphs)\n", app.font->header.glyph_count);

    /* Cursors */
    cursor_load(CURSOR_ARROW_PATH, &app.cur_arrow);
    cursor_load(CURSOR_HAND_PATH,  &app.cur_hand);

    /* Initial mouse position */
    app.mouse_x = (int32_t)(app.screen_w  / 2);
    app.mouse_y = (int32_t)(app.screen_h  / 2);

    /* Scan directory */
    scan_dir(app.dir);
    printf("imgview: found %d image(s) in %s\n", app.file_count, app.dir);

    /* Find initial file index if specified */
    if (initial_file && app.file_count > 0) {
        for (int i = 0; i < app.file_count; i++) {
            if (strcmp(app.files[i].name, initial_file) == 0) {
                app.file_idx = i;
                break;
            }
        }
    }

    /* Load first image */
    if (app.file_count > 0) {
        load_current();
    } else {
        set_status("No supported images found. Formats: BMP, PPM, QOI, TGA, FF", COL_WARN);
    }

    printf("imgview: entering main loop\n");
    int quit = 0;

    while (!quit) {
        Event ev;
        while (read(efd, &ev, sizeof(ev)) > 0) {
            if (ev.type == EVENT_KEY_PRESSED) {
                KeyCode kc = ev.data.keyboard.keycode;
                char    ch = ev.data.keyboard.ascii;

                if (kc == KEY_ESCAPE || ch == 'q' || ch == 'Q') {
                    quit = 1; break;
                }
                handle_key(kc, ch);

            } else if (ev.type == EVENT_MOUSE_MOVE) {
                /* Track drag for panning */
                if (app.dragging && (app.mouse_btn & 1)) {
                    app.pan_x = app.pan_drag_x + (app.mouse_x - app.drag_start_x);
                    app.pan_y = app.pan_drag_y + (app.mouse_y - app.drag_start_y);
                }

                app.mouse_x += (int32_t)ev.data.mouse.delta_x;
                app.mouse_y += (int32_t)ev.data.mouse.delta_y;
                if (app.mouse_x < 0) app.mouse_x = 0;
                if (app.mouse_y < 0) app.mouse_y = 0;
                if ((uint32_t)app.mouse_x >= app.screen_w) app.mouse_x = (int32_t)app.screen_w - 1;
                if ((uint32_t)app.mouse_y >= app.screen_h) app.mouse_y = (int32_t)app.screen_h - 1;
                app.mouse_btn = ev.data.mouse.buttons;

            } else if (ev.type == EVENT_MOUSE_BUTTON) {
                app.mouse_btn_prev = app.mouse_btn;
                app.mouse_btn      = ev.data.mouse.buttons;

                if ((app.mouse_btn & 1) && !(app.mouse_btn_prev & 1)) {
                    /* Left button pressed — start drag */
                    app.dragging     = 1;
                    app.drag_start_x = app.mouse_x;
                    app.drag_start_y = app.mouse_y;
                    app.pan_drag_x   = app.pan_x;
                    app.pan_drag_y   = app.pan_y;
                }
                if (!(app.mouse_btn & 1) && (app.mouse_btn_prev & 1)) {
                    app.dragging = 0;
                }

                /* Right click: next image */
                if ((app.mouse_btn & 2) && !(app.mouse_btn_prev & 2)) {
                    navigate(1);
                }
            }
        }

        if (quit) break;

        draw_frame();
        yield();
    }

    /* Cleanup */
    printf("imgview: shutting down\n");
    img_free(&app.img);
    if (app.files) free(app.files);
    if (app.font)  fnt_free_font(app.font);
    if (app.bb)    NodGL_UnmapResource(app.ctx, app.backbuf_tex);
    NodGL_ReleaseResource(app.device, app.backbuf_tex);
    NodGL_ReleaseDevice(app.device);
    close(efd);
    input_flush();
    return 0;
}