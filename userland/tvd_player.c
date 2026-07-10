/*
 * tvd_player.c — TVD (Tiny Video Delta) player for ModuOS
 * No audio — video only.
 *
 * TVD Format (magic 0x21445654 'TVD!'):
 *   Header:
 *     uint32_t magic           — 0x21445654
 *     uint16_t width
 *     uint16_t height
 *     uint16_t fps             — playback rate
 *     uint16_t total_frames
 *     uint16_t keyframe_interval
 *   Frames (total_frames entries):
 *     uint8_t  frame_type      — 0 = keyframe (full raw RGBA), 1 = delta (XOR map)
 *     uint32_t data_size       — byte count of compressed payload
 *     uint8_t  data[data_size] — zlib-compressed payload
 *       Keyframe payload: width*height*4 raw RGBA bytes
 *       Delta payload:    width*height*4 bytes — XOR mask; non-zero = changed pixel channel
 *
 * Controls:
 *   Space       — play / pause
 *   Left/Right  — step one frame backward / forward
 *   Home        — jump to first frame
 *   End         — jump to last frame
 *   +/-         — speed up / slow down (±5 fps, clamp 1..120)
 *   F           — fit-to-screen zoom toggle
 *   ESC         — quit
 *
 * Copyright © 2026 New Technologies Software — GPL v2.0
 */

#include "libc.h"
#include "NodGL.h"
#include "ilib.h"
#include "lib_fnt.h"
#include "../include/moduos/kernel/events/events.h"

/* ============================================================
   TVD format constants
   ============================================================ */
#define TVD_MAGIC       0x21445654u   /* 'TVD!' little-endian */
#define TVD_FRAME_KEY   0
#define TVD_FRAME_DELTA 1

/* ============================================================
   Layout / palette
   ============================================================ */
#define COL_BG         0xFF0D1117
#define COL_PANEL      0xFF161B22
#define COL_BORDER     0xFF30363D
#define COL_ACCENT     0xFF58A6FF
#define COL_ACCENT2    0xFF3FB950
#define COL_TEXT       0xFFC9D1D9
#define COL_MUTED      0xFF8B949E
#define COL_ERROR      0xFFF85149
#define COL_WARN       0xFFE3B341
#define COL_WHITE      0xFFFFFFFF
#define COL_SCRUB_BG   0xFF21262D
#define COL_SCRUB_FILL 0xFF388BFD
#define COL_SCRUB_HEAD 0xFFE6EDF3
#define COL_CHECKER_A  0xFF1A1A1A
#define COL_CHECKER_B  0xFF2A2A2A
#define COL_OVERLAY_BG 0xCC000000
#define COL_SEL_BG     0xFF1F6FEB

#define CONTROLS_H  56
#define HEADER_H    36
#define SCRUB_H     8
#define MAX_PATH    512
#define MAX_DIR     256

/* ============================================================
   Cursor — stored inline, no heap
   ============================================================ */
#define CURSOR_W 16
#define CURSOR_H 16

typedef struct {
    uint8_t rgba[CURSOR_W * CURSOR_H * 4];
    int     loaded;
} CursorImg;

/* ============================================================
   TVD frame index entry
   Bit-pack frame_type into bit 0 of data_size_and_type to
   shrink each entry from 9 bytes → 8 bytes and avoid the
   separate frame_type field padding byte.
   ============================================================ */
typedef struct {
    uint32_t file_offset;        /* byte offset in file to compressed payload */
    uint32_t data_size_and_type; /* bits 31..1 = data_size, bit 0 = frame_type */
} TVDFrameEntry;

#define TVD_ENTRY_TYPE(e)     ((uint8_t)((e).data_size_and_type & 1u))
#define TVD_ENTRY_SIZE(e)     ((e).data_size_and_type >> 1)
#define TVD_ENTRY_PACK(sz,ty) (((uint32_t)(sz) << 1) | ((uint32_t)(ty) & 1u))

/* ============================================================
   File browser — heap-allocated only while browser is open.
   Saves 65 KB of static AppState bloat.
   ============================================================ */
typedef struct {
    char    name[256];
    uint8_t is_dir;
} BrowserEntry;

typedef struct {
    BrowserEntry *entries;  /* malloc'd on open, freed on close */
    int           count;
    int           sel;
    int           scroll;
} FileBrowser;

/* ============================================================
   App state — trimmed to the bone
   ============================================================ */
typedef struct {
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

    /* Cursors — embedded bitmaps, no heap */
    CursorImg     cur_arrow;
    CursorImg     cur_hand;

    /* TVD file */
    char          filepath[MAX_PATH];
    int           fd;
    int           file_open;

    /* Video metadata */
    uint16_t      vid_w, vid_h;
    uint16_t      fps;
    uint16_t      total_frames;
    uint16_t      keyframe_interval;

    /* Frame index — heap, freed on close */
    TVDFrameEntry *frame_index;

    /* Decode buffers — allocated once at file-open, never again.
       All three are the same size: vid_w * vid_h * 4.
       Pointers are rotated between them; no memcpy of full frames
       except the one mandatory sync after a keyframe. */
    uint8_t *frame_buf;       /* currently displayed frame   */
    uint8_t *prev_frame_buf;  /* base for next delta (NULL if keyframe_interval==1) */
    uint8_t *decode_scratch;  /* decompression target        */

    /* Compressed-data read buffer — malloc'd once to max frame size */
    uint8_t  *cmp_scratch;
    uint32_t  cmp_scratch_sz;

    int       cur_frame;    /* 0-based index of displayed frame */
    int       last_decoded; /* last frame decoded into frame_buf */

    /* Playback */
    int      playing;
    int      fps_override;   /* 0 = use file fps */
    uint64_t last_frame_ms;

    /* Zoom / pan */
    int   fit_zoom;
    float zoom;
    int   pan_x, pan_y;

    /* Mouse */
    int32_t mouse_x, mouse_y;
    uint8_t mouse_btn, mouse_btn_prev;

    /* OSD — fade after 3 s inactivity */
    uint64_t last_input_ms;

    /* Status bar */
    char     status[256];
    uint32_t status_color;

    /* Scrubbing */
    int scrubbing;

    /* File browser */
    int         show_browser;
    char        cwd[MAX_PATH];
    FileBrowser browser;
} AppState;

static AppState app;

/* ============================================================
   Zero-allocation zlib decompressor
   Reuses ilib.h's ilib__deflate / ilib__adler32 internals.
   Writes directly into a caller-supplied buffer — no malloc.
   ============================================================ */
static int tvd__zlib_into(const uint8_t *src, size_t src_len,
                           uint8_t *out, size_t expected) {
    if (src_len < 6) return ILIB_ERR_ZLIB;
    uint8_t cmf = src[0], flg = src[1];
    if ((cmf & 0x0F) != 8)               return ILIB_ERR_ZLIB;
    if (((cmf * 256u + flg) % 31) != 0)  return ILIB_ERR_ZLIB;
    if (flg & 0x20)                       return ILIB_ERR_ZLIB; /* FDICT unsupported */

    ilib__bitstream_t bs;
    ilib__bs_init(&bs, src + 2, src_len - 6);
    size_t written = 0;
    int r = ilib__deflate(&bs, out, expected, &written);
    if (r != ILIB_OK)          return r;
    if (written != expected)   return ILIB_ERR_ZLIB;

    /* Verify Adler-32 trailer */
    uint32_t stored = ((uint32_t)src[src_len-4] << 24)
                    | ((uint32_t)src[src_len-3] << 16)
                    | ((uint32_t)src[src_len-2] <<  8)
                    |  (uint32_t)src[src_len-1];
    if (ilib__adler32(out, written) != stored) return ILIB_ERR_ZLIB;
    return ILIB_OK;
}

/* ============================================================
   Status helper
   ============================================================ */
static void set_status(const char *msg, uint32_t col) {
    size_t l = strlen(msg);
    if (l >= sizeof(app.status)) l = sizeof(app.status) - 1;
    memcpy(app.status, msg, l);
    app.status[l] = 0;
    app.status_color = col;
}

/* ============================================================
   Graphics primitives
   ============================================================ */
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
    uint8_t a = (col >> 24) & 0xFF;
    if (a == 0xFF) { gfx_fill(x, y, w, h, col); return; }
    if (!app.bb || w <= 0 || h <= 0 || a == 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > (int)app.screen_w) x1 = (int)app.screen_w;
    int y1 = y + h; if (y1 > (int)app.screen_h) y1 = (int)app.screen_h;
    uint8_t sr = (col >> 16) & 0xFF;
    uint8_t sg = (col >>  8) & 0xFF;
    uint8_t sb =  col        & 0xFF;
    uint32_t ia = 255 - a;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = (uint32_t *)(app.bb + (uint64_t)yy * app.bb_pitch);
        for (int xx = x0; xx < x1; xx++) {
            uint32_t d = row[xx];
            uint8_t nr = (uint8_t)((sr*a + ((d>>16)&0xFF)*ia)/255);
            uint8_t ng = (uint8_t)((sg*a + ((d>> 8)&0xFF)*ia)/255);
            uint8_t nb = (uint8_t)((sb*a + ( d     &0xFF)*ia)/255);
            row[xx] = 0xFF000000u | ((uint32_t)nr<<16) | ((uint32_t)ng<<8) | nb;
        }
    }
}

static void gfx_rect(int x, int y, int w, int h, uint32_t col) {
    gfx_fill(x,     y,         w, 1, col);
    gfx_fill(x,     y + h - 1, w, 1, col);
    gfx_fill(x,     y,         1, h, col);
    gfx_fill(x+w-1, y,         1, h, col);
}

static inline void gfx_blend_pixel(int x, int y,
                                    uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || y < 0 || (uint32_t)x >= app.screen_w ||
        (uint32_t)y >= app.screen_h || !app.bb) return;
    uint32_t *dst = (uint32_t *)(app.bb + (uint64_t)y * app.bb_pitch) + x;
    if (a == 255) {
        *dst = 0xFF000000u | ((uint32_t)r<<16) | ((uint32_t)g<<8) | b;
    } else if (a > 0) {
        uint32_t d = *dst, ia = 255 - a;
        uint8_t nr = (uint8_t)((r*a + ((d>>16)&0xFF)*ia)/255);
        uint8_t ng = (uint8_t)((g*a + ((d>> 8)&0xFF)*ia)/255);
        uint8_t nb = (uint8_t)((b*a + ( d     &0xFF)*ia)/255);
        *dst = 0xFF000000u | ((uint32_t)nr<<16) | ((uint32_t)ng<<8) | nb;
    }
}

static void gfx_char(int x, int y, char c, uint32_t col) {
    if (!app.font) return;
    fnt_glyph_t *g = fnt_get_glyph(app.font, (uint32_t)(unsigned char)c);
    if (!g) return;
    uint8_t r = (col>>16)&0xFF, gv = (col>>8)&0xFF, b = col&0xFF;
    for (int dy = 0; dy < g->bitmap_height; dy++)
        for (int dx = 0; dx < g->bitmap_width; dx++)
            if (fnt_get_pixel(g, dx, dy))
                gfx_blend_pixel(x+dx, y+dy, r, gv, b, 0xFF);
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

static void gfx_text_scaled(int x, int y, const char *s, uint32_t col, int scale) {
    if (scale <= 1) { gfx_text(x, y, s, col); return; }
    if (!app.font || !s) return;
    int cx = x;
    while (*s) {
        fnt_glyph_t *g = fnt_get_glyph(app.font, (uint32_t)(unsigned char)*s);
        if (g) {
            uint8_t r=(col>>16)&0xFF, gv=(col>>8)&0xFF, b=col&0xFF;
            for (int dy = 0; dy < g->bitmap_height; dy++)
                for (int dx = 0; dx < g->bitmap_width; dx++)
                    if (fnt_get_pixel(g, dx, dy))
                        gfx_fill(cx+dx*scale, y+dy*scale, scale, scale,
                                 0xFF000000u|((uint32_t)r<<16)|((uint32_t)gv<<8)|b);
            cx += g->width * scale;
        }
        s++;
    }
}

static void gfx_text_center(int cx, int y, const char *s, uint32_t col) {
    if (!app.font || !s) return;
    gfx_text(cx - fnt_string_width(app.font, s)/2, y, s, col);
}

static void gfx_text_right(int x, int y, const char *s, uint32_t col) {
    if (!app.font || !s) return;
    gfx_text(x - fnt_string_width(app.font, s), y, s, col);
}

static void gfx_text_clip(int x, int y, int max_w, const char *s, uint32_t col) {
    if (!app.font || !s) return;
    int cx = x;
    while (*s) {
        fnt_glyph_t *g = fnt_get_glyph(app.font, (uint32_t)(unsigned char)*s);
        if (!g) { s++; continue; }
        if (cx + g->width > x + max_w) {
            fnt_glyph_t *dot = fnt_get_glyph(app.font, '.');
            if (dot && cx + dot->width*3 <= x + max_w) {
                gfx_char(cx,y,'.',col); cx+=dot->width;
                gfx_char(cx,y,'.',col); cx+=dot->width;
                gfx_char(cx,y,'.',col);
            }
            return;
        }
        gfx_char(cx, y, *s, col);
        cx += g->width;
        s++;
    }
}

static void gfx_checker(int x, int y, int w, int h, int cell) {
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++) {
            int tx = (x+xx)/cell, ty = (y+yy)/cell;
            gfx_fill(x+xx, y+yy, 1, 1, ((tx+ty)&1) ? COL_CHECKER_B : COL_CHECKER_A);
        }
}

static void gfx_blit_rgba_zoom(int dx, int dy, int dw, int dh,
                                const uint8_t *px, int sw, int sh,
                                int cx, int cy, int cw, int ch) {
    if (dw<=0||dh<=0||sw<=0||sh<=0||!px) return;
    for (int y = 0; y < dh; y++) {
        int ay = dy + y;
        if (ay < cy || ay >= cy+ch) continue;
        int sy = y * sh / dh;
        for (int x = 0; x < dw; x++) {
            int ax = dx + x;
            if (ax < cx || ax >= cx+cw) continue;
            int sx2 = x * sw / dw;
            const uint8_t *p = px + (sy*sw + sx2)*4;
            gfx_blend_pixel(ax, ay, p[0], p[1], p[2], p[3]);
        }
    }
}

/* ============================================================
   Cursor
   ============================================================ */
static void cursor_blit(const CursorImg *cur, int32_t cx2, int32_t cy2) {
    if (!cur || !cur->loaded || !app.bb) return;
    for (int dy = 0; dy < CURSOR_H; dy++) {
        int py = cy2 + dy;
        if (py < 0 || (uint32_t)py >= app.screen_h) continue;
        uint32_t *row = (uint32_t *)(app.bb + (uint64_t)py * app.bb_pitch);
        for (int dx = 0; dx < CURSOR_W; dx++) {
            int px = cx2 + dx;
            if (px < 0 || (uint32_t)px >= app.screen_w) continue;
            uint32_t si = (uint32_t)(dy*CURSOR_W+dx)*4u;
            uint8_t sr=cur->rgba[si], sg=cur->rgba[si+1];
            uint8_t sb=cur->rgba[si+2], sa=cur->rgba[si+3];
            if (!sa) continue;
            if (sa == 255) {
                row[px] = 0xFF000000u|((uint32_t)sr<<16)|((uint32_t)sg<<8)|sb;
            } else {
                uint32_t d=row[px], ia=255u-sa;
                row[px] = 0xFF000000u
                    |(((sr*sa+((d>>16)&0xFF)*ia)/255u)<<16)
                    |(((sg*sa+((d>> 8)&0xFF)*ia)/255u)<< 8)
                    |  (sb*sa+( d     &0xFF)*ia)/255u;
            }
        }
    }
}

static void cursor_load_bmp(const char *path, CursorImg *out) {
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
        ssize_t n = read(fd, buf+got, (size_t)sz-got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got < 54 || buf[0] != 'B' || buf[1] != 'M') { free(buf); return; }
    uint32_t pixel_off = (uint32_t)buf[10]|((uint32_t)buf[11]<<8)|
                         ((uint32_t)buf[12]<<16)|((uint32_t)buf[13]<<24);
    int32_t  width  = (int32_t)((uint32_t)buf[18]|((uint32_t)buf[19]<<8)|
                                ((uint32_t)buf[20]<<16)|((uint32_t)buf[21]<<24));
    int32_t  height = (int32_t)((uint32_t)buf[22]|((uint32_t)buf[23]<<8)|
                                ((uint32_t)buf[24]<<16)|((uint32_t)buf[25]<<24));
    uint16_t bpp    = (uint16_t)(buf[28]|(buf[29]<<8));
    if (bpp != 32 || width != CURSOR_W) { free(buf); return; }
    int top_down = 0;
    if (height < 0) { top_down = 1; height = -height; }
    if (height != CURSOR_H) { free(buf); return; }
    uint32_t stride = (uint32_t)width * 4u;
    for (uint32_t y = 0; y < (uint32_t)CURSOR_H; y++) {
        uint32_t sy = top_down ? y : ((uint32_t)CURSOR_H - 1u - y);
        const uint8_t *row = buf + pixel_off + sy*stride;
        for (uint32_t x = 0; x < (uint32_t)CURSOR_W; x++) {
            uint32_t di = (y*(uint32_t)CURSOR_W+x)*4u;
            out->rgba[di+0] = row[x*4+2];
            out->rgba[di+1] = row[x*4+1];
            out->rgba[di+2] = row[x*4+0];
            out->rgba[di+3] = row[x*4+3];
        }
    }
    free(buf);
    out->loaded = 1;
}

/* ============================================================
   TVD decoder — ZERO heap allocation per frame
   ============================================================ */
static int tvd_read_exact(uint8_t *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(app.fd, buf+done, n-done);
        if (r <= 0) return -1;
        done += (size_t)r;
    }
    return 0;
}

static int tvd_decode_frame(int idx) {
    if (!app.file_open || !app.frame_index) return -1;
    if (idx < 0 || idx >= app.total_frames)  return -1;

    TVDFrameEntry *entry   = &app.frame_index[idx];
    uint32_t       dsz     = TVD_ENTRY_SIZE(*entry);
    uint8_t        ftype   = TVD_ENTRY_TYPE(*entry);
    size_t         raw_sz  = (size_t)app.vid_w * app.vid_h * 4;

    /* Guard: should never fire if cmp_scratch was sized correctly at open */
    if (dsz > app.cmp_scratch_sz) return -1;

    if (lseek(app.fd, (long)entry->file_offset, SEEK_SET) < 0) return -1;
    if (tvd_read_exact(app.cmp_scratch, dsz) != 0) return -1;

    /* Decompress into scratch — no malloc, no free */
    if (tvd__zlib_into(app.cmp_scratch, dsz, app.decode_scratch, raw_sz) != ILIB_OK)
        return -1;

    if (ftype == TVD_FRAME_KEY) {
        /* Rotate: decode_scratch becomes frame_buf; old frame_buf becomes new scratch.
           One memcpy to sync prev (mandatory for the next delta). */
        uint8_t *tmp       = app.frame_buf;
        app.frame_buf      = app.decode_scratch;
        app.decode_scratch = tmp;
        if (app.prev_frame_buf)
            memcpy(app.prev_frame_buf, app.frame_buf, raw_sz);
    } else {
        /* Delta: XOR decode_scratch (mask) onto prev → new frame.
           Rotate prev/frame pointers so old frame_buf becomes the new prev. */
        if (!app.prev_frame_buf) return -1;
        uint8_t *tmp       = app.prev_frame_buf;
        app.prev_frame_buf = app.frame_buf;
        app.frame_buf      = tmp;

        const uint8_t *prev = app.prev_frame_buf;
        const uint8_t *mask = app.decode_scratch;
        uint8_t       *dst  = app.frame_buf;

        /* Unrolled 8-at-a-time — helps compilers without auto-vectorisation */
        size_t i = 0;
        for (; i + 8 <= raw_sz; i += 8) {
            dst[i+0] = prev[i+0] ^ mask[i+0];
            dst[i+1] = prev[i+1] ^ mask[i+1];
            dst[i+2] = prev[i+2] ^ mask[i+2];
            dst[i+3] = prev[i+3] ^ mask[i+3];
            dst[i+4] = prev[i+4] ^ mask[i+4];
            dst[i+5] = prev[i+5] ^ mask[i+5];
            dst[i+6] = prev[i+6] ^ mask[i+6];
            dst[i+7] = prev[i+7] ^ mask[i+7];
        }
        for (; i < raw_sz; i++)
            dst[i] = prev[i] ^ mask[i];
    }

    app.last_decoded = idx;
    return 0;
}

static int tvd_seek_and_decode(int idx) {
    if (!app.file_open || !app.frame_index) return -1;
    if (idx < 0) idx = 0;
    if (idx >= app.total_frames) idx = app.total_frames - 1;

    if (idx == app.last_decoded) return 0;

    /* Fast path: simple forward step */
    if (idx == app.last_decoded + 1 &&
        TVD_ENTRY_TYPE(app.frame_index[idx]) == TVD_FRAME_DELTA)
        return tvd_decode_frame(idx);

    /* Find nearest keyframe at or before idx */
    int kf = idx;
    while (kf > 0 && TVD_ENTRY_TYPE(app.frame_index[kf]) != TVD_FRAME_KEY)
        kf--;

    /* Decode from keyframe forward */
    for (int i = kf; i <= idx; i++)
        if (tvd_decode_frame(i) != 0) return -1;

    return 0;
}

/* ============================================================
   TVD file open / close
   ============================================================ */
static void tvd_close_buffers(void) {
    if (app.file_open)      { close(app.fd); app.file_open = 0; }
    if (app.frame_index)    { free(app.frame_index);    app.frame_index    = NULL; }
    if (app.frame_buf)      { free(app.frame_buf);      app.frame_buf      = NULL; }
    if (app.prev_frame_buf) { free(app.prev_frame_buf); app.prev_frame_buf = NULL; }
    if (app.decode_scratch) { free(app.decode_scratch); app.decode_scratch = NULL; }
    if (app.cmp_scratch)    { free(app.cmp_scratch);    app.cmp_scratch    = NULL; }
    app.cmp_scratch_sz = 0;
}

static int tvd_open(const char *path) {
    tvd_close_buffers();

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) { set_status("Cannot open file", COL_ERROR); return -1; }

    /* Read 14-byte header */
    uint8_t hdr[14];
    size_t got = 0;
    while (got < 14) {
        ssize_t r = read(fd, hdr+got, 14-got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    if (got < 14) { close(fd); set_status("File too small", COL_ERROR); return -1; }

    uint32_t magic = (uint32_t)hdr[0]|((uint32_t)hdr[1]<<8)|
                     ((uint32_t)hdr[2]<<16)|((uint32_t)hdr[3]<<24);
    if (magic != TVD_MAGIC) { close(fd); set_status("Not a TVD file", COL_ERROR); return -1; }

    app.vid_w             = (uint16_t)(hdr[4] |(hdr[5] <<8));
    app.vid_h             = (uint16_t)(hdr[6] |(hdr[7] <<8));
    app.fps               = (uint16_t)(hdr[8] |(hdr[9] <<8));
    app.total_frames      = (uint16_t)(hdr[10]|(hdr[11]<<8));
    app.keyframe_interval = (uint16_t)(hdr[12]|(hdr[13]<<8));

    if (!app.vid_w || !app.vid_h || !app.total_frames || !app.fps) {
        close(fd); set_status("Invalid TVD header", COL_ERROR); return -1;
    }

    /* Build frame index, scanning max compressed size in one pass */
    app.frame_index = (TVDFrameEntry *)malloc(app.total_frames * sizeof(TVDFrameEntry));
    if (!app.frame_index) { close(fd); set_status("OOM: index", COL_ERROR); return -1; }

    uint32_t max_cmp = 0;
    long cur_off = 14;
    for (int i = 0; i < app.total_frames; i++) {
        uint8_t meta[5];
        got = 0;
        while (got < 5) {
            ssize_t r = read(fd, meta+got, 5-got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        if (got < 5) {
            free(app.frame_index); app.frame_index = NULL;
            close(fd); set_status("Truncated index", COL_ERROR); return -1;
        }
        uint8_t  ftype = meta[0];
        uint32_t dsz   = (uint32_t)meta[1]|((uint32_t)meta[2]<<8)|
                         ((uint32_t)meta[3]<<16)|((uint32_t)meta[4]<<24);

        app.frame_index[i].file_offset      = (uint32_t)(cur_off + 5);
        app.frame_index[i].data_size_and_type = TVD_ENTRY_PACK(dsz, ftype);

        if (dsz > max_cmp) max_cmp = dsz;
        cur_off += 5 + (long)dsz;

        if (lseek(fd, cur_off, SEEK_SET) < 0) {
            free(app.frame_index); app.frame_index = NULL;
            close(fd); set_status("Seek error", COL_ERROR); return -1;
        }
    }

    /* Allocate all decode buffers in one block of decisions */
    size_t fbsz = (size_t)app.vid_w * app.vid_h * 4;

    app.frame_buf      = (uint8_t *)malloc(fbsz);
    app.decode_scratch = (uint8_t *)malloc(fbsz);
    /* Only alloc prev if the file actually has delta frames */
    app.prev_frame_buf = (app.keyframe_interval > 1)
                         ? (uint8_t *)malloc(fbsz) : NULL;
    app.cmp_scratch    = (uint8_t *)malloc(max_cmp);

    if (!app.frame_buf || !app.decode_scratch || !app.cmp_scratch ||
        (app.keyframe_interval > 1 && !app.prev_frame_buf)) {
        tvd_close_buffers();
        close(fd);
        set_status("OOM: frame buffers", COL_ERROR);
        return -1;
    }
    app.cmp_scratch_sz = max_cmp;

    app.fd           = fd;
    app.file_open    = 1;
    app.cur_frame    = 0;
    app.last_decoded = -1;

    size_t pl = strlen(path);
    if (pl >= MAX_PATH) pl = MAX_PATH - 1;
    memcpy(app.filepath, path, pl);
    app.filepath[pl] = 0;

    /* Decode first frame immediately */
    tvd_seek_and_decode(0);
    app.cur_frame    = 0;
    app.last_decoded = 0;

    /* Fit zoom */
    float zx = (float)(int)app.screen_w / app.vid_w;
    float zy = (float)((int)app.screen_h - HEADER_H - CONTROLS_H) / app.vid_h;
    app.zoom        = zx < zy ? zx : zy;
    app.pan_x       = 0;
    app.pan_y       = 0;
    app.fit_zoom    = 1;
    app.fps_override = 0;

    set_status("Space=play/pause  +/-=speed  0=fit  1=100%  Arrows=seek", COL_MUTED);
    app.show_browser = 0;
    return 0;
}

/* ============================================================
   Effective FPS
   ============================================================ */
static int effective_fps(void) {
    return app.fps_override > 0 ? app.fps_override : (int)app.fps;
}

/* ============================================================
   Draw: Header
   ============================================================ */
static void draw_header(void) {
    gfx_fill(0, 0, (int)app.screen_w, HEADER_H, COL_PANEL);
    gfx_fill(0, HEADER_H-1, (int)app.screen_w, 1, COL_BORDER);

    gfx_text_scaled(8, 8, "TVD", COL_ACCENT, 2);
    gfx_text(8 + fnt_string_width_scaled(app.font,"TVD",2) + 6, 13, "Player", COL_TEXT);

    if (app.file_open) {
        const char *bn = app.filepath;
        for (const char *p = app.filepath; *p; p++) if (*p=='/') bn = p+1;
        gfx_text_clip(120, 8, (int)app.screen_w-280, bn, COL_TEXT);

        char info[48], wbuf[8], hbuf[8], fbuf[8];
        itoa(app.vid_w, wbuf, 10);
        itoa(app.vid_h, hbuf, 10);
        itoa(effective_fps(), fbuf, 10);
        memcpy(info, wbuf, strlen(wbuf)+1);
        strcat(info,"x"); strcat(info,hbuf);
        strcat(info,"  "); strcat(info,fbuf); strcat(info," fps");
        gfx_text_right((int)app.screen_w-8, 13, info, COL_MUTED);
    } else {
        gfx_text(120, 13, "No file loaded", COL_MUTED);
    }
}

/* ============================================================
   Draw: Controls bar
   ============================================================ */
static void draw_controls(void) {
    int cy = (int)app.screen_h - CONTROLS_H;
    gfx_fill(0, cy, (int)app.screen_w, CONTROLS_H, COL_PANEL);
    gfx_fill(0, cy, (int)app.screen_w, 1, COL_BORDER);

    if (!app.file_open) {
        gfx_text_center((int)app.screen_w/2, cy+20, "Open a .tvd file to begin", COL_MUTED);
        return;
    }

    int efps    = effective_fps();
    int sx      = 48;
    int sw      = (int)app.screen_w - 96;
    int scrub_y = cy + 8;

    /* Scrub bar background */
    gfx_fill(sx, scrub_y, sw, SCRUB_H, COL_SCRUB_BG);
    gfx_rect(sx, scrub_y, sw, SCRUB_H, COL_BORDER);

    /* Fill */
    int fill_w = (app.total_frames > 1)
        ? (int)((uint64_t)sw * (uint32_t)app.cur_frame / (app.total_frames-1))
        : sw;
    if (fill_w > 0) gfx_fill(sx, scrub_y, fill_w, SCRUB_H, COL_SCRUB_FILL);

    /* Keyframe ticks */
    for (int i = 0; i < app.total_frames; i++) {
        if (TVD_ENTRY_TYPE(app.frame_index[i]) == TVD_FRAME_KEY && i > 0) {
            int tx = sx + (int)((uint64_t)sw*i/(app.total_frames-1));
            gfx_fill(tx, scrub_y, 1, SCRUB_H, COL_ACCENT2);
        }
    }

    /* Scrub head */
    gfx_fill(sx+fill_w-2, scrub_y-2, 5, SCRUB_H+4, COL_SCRUB_HEAD);

    /* Frame counter */
    char fn[8], ft[8], fcbuf[20];
    itoa(app.cur_frame,      fn, 10);
    itoa(app.total_frames-1, ft, 10);
    memcpy(fcbuf, fn, strlen(fn)+1);
    strcat(fcbuf, "/"); strcat(fcbuf, ft);
    gfx_text_center(sx/2, scrub_y+1, fcbuf, COL_TEXT);

    /* Time display */
    {
        char tbuf[32], cmbuf[4], csbuf[4], tmbuf[4], tsbuf[4];
        int cur_sec = efps>0 ? app.cur_frame/efps : 0;
        int tot_sec = efps>0 ? (app.total_frames-1)/efps : 0;
        itoa(cur_sec/60, cmbuf, 10); itoa(cur_sec%60, csbuf, 10);
        itoa(tot_sec/60, tmbuf, 10); itoa(tot_sec%60, tsbuf, 10);
        memcpy(tbuf, cmbuf, strlen(cmbuf)+1);
        strcat(tbuf, ":");
        if (cur_sec%60 < 10) strcat(tbuf, "0");
        strcat(tbuf, csbuf); strcat(tbuf, " / ");
        strcat(tbuf, tmbuf); strcat(tbuf, ":");
        if (tot_sec%60 < 10) strcat(tbuf, "0");
        strcat(tbuf, tsbuf);
        gfx_text_right((int)app.screen_w-8, scrub_y+1, tbuf, COL_MUTED);
    }

    int by = scrub_y + SCRUB_H + 6;

    /* Play/Pause */
    {
        const char *lbl = app.playing ? "| |" : " > ";
        uint32_t bg = app.playing ? 0xFF2D3B4E : COL_SCRUB_FILL;
        int bx = (int)app.screen_w/2 - 24;
        gfx_fill(bx, by, 48, 18, bg);
        gfx_rect(bx, by, 48, 18, COL_BORDER);
        gfx_text_center(bx+24, by+5, lbl, COL_WHITE);
    }

    /* Step back */
    gfx_fill((int)app.screen_w/2-80, by, 32, 18, COL_SCRUB_BG);
    gfx_rect((int)app.screen_w/2-80, by, 32, 18, COL_BORDER);
    gfx_text_center((int)app.screen_w/2-64, by+5, "|<", COL_TEXT);

    /* Step fwd */
    gfx_fill((int)app.screen_w/2+48, by, 32, 18, COL_SCRUB_BG);
    gfx_rect((int)app.screen_w/2+48, by, 32, 18, COL_BORDER);
    gfx_text_center((int)app.screen_w/2+64, by+5, ">|", COL_TEXT);

    /* Speed */
    char spd[16], sfps[8];
    itoa(efps, sfps, 10);
    memcpy(spd, sfps, strlen(sfps)+1); strcat(spd, " fps");
    gfx_fill(8,  by, 64, 18, COL_SCRUB_BG);
    gfx_rect(8,  by, 64, 18, COL_BORDER);
    gfx_text_center(40, by+5, spd, COL_MUTED);

    /* Zoom */
    char zdsp[12], zpct[8];
    itoa((int)(app.zoom*100.0f), zpct, 10);
    memcpy(zdsp, zpct, strlen(zpct)+1); strcat(zdsp, "%");
    gfx_fill((int)app.screen_w-80, by, 72, 18, COL_SCRUB_BG);
    gfx_rect((int)app.screen_w-80, by, 72, 18, COL_BORDER);
    gfx_text_center((int)app.screen_w-44, by+5, zdsp, COL_MUTED);

    /* Open button */
    gfx_fill(80, by, 60, 18, COL_ACCENT);
    gfx_rect(80, by, 60, 18, COL_BORDER);
    gfx_text_center(110, by+5, "Open", COL_WHITE);

    /* Status */
    gfx_text_clip(8, cy+CONTROLS_H-14, (int)app.screen_w-16,
                  app.status, app.status_color);
}

/* ============================================================
   Draw: Video area
   ============================================================ */
static void draw_video(void) {
    int content_y = HEADER_H;
    int content_h = (int)app.screen_h - HEADER_H - CONTROLS_H;
    int content_w = (int)app.screen_w;

    if (!app.file_open || !app.frame_buf) {
        gfx_fill(0, content_y, content_w, content_h, COL_BG);
        gfx_text_center(content_w/2, content_y+content_h/2-10, "No video loaded",            COL_MUTED);
        gfx_text_center(content_w/2, content_y+content_h/2+ 8, "Press O or click Open",      COL_MUTED);
        return;
    }

    gfx_fill(0, content_y, content_w, content_h, 0xFF0A0A0A);

    if (app.fit_zoom) {
        float zx = (float)content_w / app.vid_w;
        float zy = (float)content_h / app.vid_h;
        app.zoom  = zx < zy ? zx : zy;
        app.pan_x = 0; app.pan_y = 0;
    }

    int dw = (int)(app.vid_w * app.zoom);
    int dh = (int)(app.vid_h * app.zoom);
    int ix = content_w/2 - dw/2 + app.pan_x;
    int iy = content_y + content_h/2 - dh/2 + app.pan_y;

    gfx_checker(ix, iy, dw, dh, 12);
    gfx_blit_rgba_zoom(ix, iy, dw, dh,
                       app.frame_buf, app.vid_w, app.vid_h,
                       0, content_y, content_w, content_h);
    gfx_rect(ix-1, iy-1, dw+2, dh+2, COL_BORDER);

    /* OSD badges — only while recently active */
    if ((time_ms() - app.last_input_ms) < 3000 && app.frame_index) {
        uint8_t     ft    = TVD_ENTRY_TYPE(app.frame_index[app.cur_frame]);
        const char *badge = (ft == TVD_FRAME_KEY) ? "KEY" : "DELTA";
        uint32_t    bc    = (ft == TVD_FRAME_KEY) ? COL_ACCENT2 : COL_WARN;
        int bw = fnt_string_width(app.font, badge) + 10;
        gfx_fill_alpha(8, content_y+8,  bw, 20, COL_OVERLAY_BG);
        gfx_text(12,      content_y+13, badge, bc);

        char zbuf[12], zpct[8];
        itoa((int)(app.zoom*100.0f), zpct, 10);
        memcpy(zbuf, zpct, strlen(zpct)+1); strcat(zbuf, "%");
        int zw = fnt_string_width(app.font, zbuf) + 10;
        gfx_fill_alpha(8, content_y+32, zw, 20, COL_OVERLAY_BG);
        gfx_text(12,      content_y+37, zbuf, COL_TEXT);

        const char *pb = app.playing ? "PLAYING" : "PAUSED";
        uint32_t    pc = app.playing ? COL_ACCENT2 : COL_MUTED;
        int pw = fnt_string_width(app.font, pb) + 10;
        gfx_fill_alpha((int)app.screen_w-pw-8, content_y+8, pw, 20, COL_OVERLAY_BG);
        gfx_text((int)app.screen_w-pw-3,       content_y+13, pb, pc);
    }
}

/* ============================================================
   File browser — heap lives only while browser is open
   ============================================================ */
#define BRITEM_H 22

static void browser_close(void) {
    if (app.browser.entries) {
        free(app.browser.entries);
        app.browser.entries = NULL;
    }
    app.browser.count  = 0;
    app.browser.sel    = 0;
    app.browser.scroll = 0;
    app.show_browser   = 0;
}

static void browser_reload(void) {
    /* Free old listing first */
    if (app.browser.entries) { free(app.browser.entries); app.browser.entries = NULL; }
    app.browser.count = 0; app.browser.sel = 0; app.browser.scroll = 0;

    app.browser.entries = (BrowserEntry *)malloc(MAX_DIR * sizeof(BrowserEntry));
    if (!app.browser.entries) { set_status("OOM: browser", COL_ERROR); return; }

    int fd = opendir(app.cwd);
    if (fd < 0) { set_status("Cannot open dir", COL_ERROR); return; }

    int n = 0;
    /* ".." entry if not root */
    if (!(app.cwd[0]=='/' && app.cwd[1]==0)) {
        memcpy(app.browser.entries[n].name, "..", 3);
        app.browser.entries[n].is_dir = 1;
        n++;
    }

    char nm[256]; int isd; uint32_t sz;
    while (n < MAX_DIR) {
        int rc = readdir(fd, nm, sizeof(nm), &isd, &sz);
        if (rc <= 0) break;
        if (nm[0]=='.' && (nm[1]==0||(nm[1]=='.'&&nm[2]==0))) continue;
        size_t nl = strlen(nm); if (nl > 255) nl = 254;
        memcpy(app.browser.entries[n].name, nm, nl+1);
        app.browser.entries[n].is_dir = (uint8_t)isd;
        n++;
    }
    closedir(fd);
    app.browser.count = n;

    /* Insertion sort: dirs first, then alpha */
    for (int i = 1; i < n; i++) {
        BrowserEntry tmp = app.browser.entries[i];
        int j = i - 1;
        while (j >= 0) {
            BrowserEntry *a = &app.browser.entries[j];
            int cmp = (a->is_dir != tmp.is_dir)
                      ? (tmp.is_dir ? -1 : 1)
                      : strcmp(a->name, tmp.name);
            if (cmp > 0) { app.browser.entries[j+1] = *a; j--; }
            else break;
        }
        app.browser.entries[j+1] = tmp;
    }
}

static void draw_browser_overlay(void) {
    gfx_fill_alpha(0, 0, (int)app.screen_w, (int)app.screen_h, 0xCC000000);

    int bx = (int)app.screen_w/2 - 260;
    int by = (int)app.screen_h/2 - 200;
    int bw = 520, bh = 400;

    gfx_fill(bx, by, bw, bh, COL_PANEL);
    gfx_rect(bx, by, bw, bh, COL_ACCENT);
    gfx_text(bx+12, by+10, "Open TVD File", COL_ACCENT);
    gfx_text_clip(bx+12, by+24, bw-24, app.cwd, COL_MUTED);
    gfx_fill(bx, by+38, bw, 1, COL_BORDER);

    int list_y = by + 42;
    int list_h = bh - 42 - 32;
    int visible = list_h / BRITEM_H;
    int ms = app.browser.count - visible;
    if (ms < 0) ms = 0;
    if (app.browser.scroll > ms) app.browser.scroll = ms;

    for (int i = app.browser.scroll; i < app.browser.count; i++) {
        int row = i - app.browser.scroll;
        int ry  = list_y + row * BRITEM_H;
        if (ry + BRITEM_H > list_y + list_h) break;

        int sel = (i == app.browser.sel);
        gfx_fill(bx, ry, bw, BRITEM_H, sel ? COL_SEL_BG : COL_PANEL);

        BrowserEntry *e = &app.browser.entries[i];
        uint32_t fc = e->is_dir ? COL_WARN : COL_ACCENT2;
        gfx_text(bx+8, ry+5, e->is_dir ? "[D]" : "[F]", fc);

        size_t nl = strlen(e->name);
        int is_tvd = (nl >= 4 && strcmp(e->name + nl - 4, ".tvd") == 0);
        uint32_t tc = sel ? COL_WHITE : (is_tvd ? COL_ACCENT : COL_TEXT);
        gfx_text_clip(bx+36, ry+5, bw-50, e->name, tc);
        gfx_fill(bx, ry+BRITEM_H-1, bw, 1, COL_BORDER);
    }

    int buty = by + bh - 28;
    gfx_fill(bx, buty, bw, 1, COL_BORDER);
    gfx_fill(bx+bw-120, buty+6, 55, 18, COL_ACCENT);
    gfx_text_center(bx+bw-92, buty+10, "Open",   COL_WHITE);
    gfx_fill(bx+bw-60, buty+6, 50, 18, COL_SCRUB_BG);
    gfx_rect(bx+bw-60, buty+6, 50, 18, COL_BORDER);
    gfx_text_center(bx+bw-35, buty+10, "Cancel", COL_MUTED);
}

/* ============================================================
   Draw frame (top level)
   ============================================================ */
static void draw_frame(void) {
    gfx_fill(0, 0, (int)app.screen_w, (int)app.screen_h, COL_BG);
    draw_video();
    draw_header();
    draw_controls();
    if (app.show_browser) draw_browser_overlay();

    /* Clamp mouse */
    if (app.mouse_x < 0) app.mouse_x = 0;
    if (app.mouse_y < 0) app.mouse_y = 0;
    if ((uint32_t)app.mouse_x >= app.screen_w) app.mouse_x = (int32_t)app.screen_w-1;
    if ((uint32_t)app.mouse_y >= app.screen_h) app.mouse_y = (int32_t)app.screen_h-1;

    int over_ui = (app.mouse_y < HEADER_H ||
                   app.mouse_y >= (int)app.screen_h - CONTROLS_H ||
                   app.show_browser);
    CursorImg *cur = (over_ui && app.cur_hand.loaded) ? &app.cur_hand : &app.cur_arrow;
    if (cur && cur->loaded) cursor_blit(cur, app.mouse_x, app.mouse_y);

    NodGL_DrawTexture(app.ctx, app.backbuf_tex, 0,0,0,0, app.screen_w, app.screen_h);
    NodGL_PresentContext(app.ctx, 0);
}

/* ============================================================
   Path helpers
   ============================================================ */
static void path_join(char *out, size_t sz, const char *dir, const char *name) {
    size_t dl = strlen(dir), nl = strlen(name);
    if (dl+1+nl+1 > sz) { out[0]=0; return; }
    memcpy(out, dir, dl);
    if (dl > 0 && dir[dl-1] != '/') out[dl++] = '/';
    memcpy(out+dl, name, nl+1);
}

static void path_parent(char *path) {
    size_t l = strlen(path);
    if (!l) return;
    if (l > 1 && path[l-1]=='/') path[--l]=0;
    for (int i=(int)l-1; i>=0; i--) {
        if (path[i]=='/') { path[i==0?1:i]=0; return; }
    }
}

/* ============================================================
   Browser open helper
   ============================================================ */
static void browser_open(void) {
    if (strlen(app.cwd)==0) memcpy(app.cwd, "/ModuOS", 8);
    app.show_browser = 1;
    browser_reload();
}

static void browser_try_open(void) {
    if (app.browser.sel < 0 || app.browser.sel >= app.browser.count) return;
    BrowserEntry *e = &app.browser.entries[app.browser.sel];
    if (e->is_dir) {
        char np[MAX_PATH];
        if (strcmp(e->name, "..") == 0) {
            memcpy(np, app.cwd, strlen(app.cwd)+1);
            path_parent(np);
        } else {
            path_join(np, sizeof(np), app.cwd, e->name);
        }
        memcpy(app.cwd, np, strlen(np)+1);
        browser_reload();
    } else {
        char fp[MAX_PATH];
        path_join(fp, sizeof(fp), app.cwd, e->name);
        tvd_open(fp);
        browser_close(); /* free browser heap immediately after open */
    }
}

/* ============================================================
   Input handlers
   ============================================================ */
static void handle_click(int x, int y) {
    app.last_input_ms = time_ms();

    if (app.show_browser) {
        int bx = (int)app.screen_w/2-260, by = (int)app.screen_h/2-200;
        int bw = 520, bh = 400;
        int list_y = by+42, list_h = bh-42-32;
        int buty = by+bh-28;

        if (x>=bx+bw-60  && x<bx+bw-10  && y>=buty+6 && y<buty+24) { browser_close(); return; }
        if (x>=bx+bw-120 && x<bx+bw-65  && y>=buty+6 && y<buty+24) { browser_try_open(); return; }
        if (x>=bx && x<bx+bw && y>=list_y && y<list_y+list_h) {
            int idx = (y-list_y)/BRITEM_H + app.browser.scroll;
            if (idx >= 0 && idx < app.browser.count) {
                if (idx == app.browser.sel) browser_try_open();
                else app.browser.sel = idx;
            }
        }
        return;
    }

    int cy   = (int)app.screen_h - CONTROLS_H;
    int by2  = cy + 8 + SCRUB_H + 6;
    int sx   = 48, sw = (int)app.screen_w-96;
    int scrub_y = cy + 8;

    /* Open button */
    if (x>=80 && x<140 && y>=by2 && y<by2+18) { browser_open(); return; }

    /* Scrub bar */
    if (y>=scrub_y-4 && y<scrub_y+SCRUB_H+4 && x>=sx && x<sx+sw && app.file_open) {
        int fr = (int)((uint64_t)(x-sx)*(app.total_frames-1)/sw);
        if (fr < 0) fr = 0;
        if (fr >= app.total_frames) fr = app.total_frames-1;
        app.playing = 0;
        tvd_seek_and_decode(fr);
        app.cur_frame = fr;
        app.scrubbing = 1;
        return;
    }

    /* Play/Pause */
    int pbx = (int)app.screen_w/2-24;
    if (x>=pbx && x<pbx+48 && y>=by2 && y<by2+18) {
        app.playing = !app.playing;
        app.last_frame_ms = time_ms();
        return;
    }
    /* Step back */
    if (x>=(int)app.screen_w/2-80 && x<(int)app.screen_w/2-48 && y>=by2 && y<by2+18) {
        app.playing = 0;
        if (app.cur_frame > 0) { tvd_seek_and_decode(app.cur_frame-1); app.cur_frame--; }
        return;
    }
    /* Step fwd */
    if (x>=(int)app.screen_w/2+48 && x<(int)app.screen_w/2+80 && y>=by2 && y<by2+18) {
        app.playing = 0;
        if (app.cur_frame < app.total_frames-1) { tvd_seek_and_decode(app.cur_frame+1); app.cur_frame++; }
        return;
    }
}

static void handle_key(KeyCode kc, char ch) {
    app.last_input_ms = time_ms();

    if (app.show_browser) {
        if (kc==KEY_ARROW_UP && app.browser.sel>0) {
            app.browser.sel--;
            if (app.browser.sel < app.browser.scroll)
                app.browser.scroll = app.browser.sel;
        } else if (kc==KEY_ARROW_DOWN && app.browser.sel<app.browser.count-1) {
            app.browser.sel++;
            int vis = (400-42-32)/BRITEM_H;
            if (app.browser.sel >= app.browser.scroll+vis)
                app.browser.scroll = app.browser.sel - vis + 1;
        } else if (kc==KEY_ENTER || ch=='\r' || ch=='\n') {
            browser_try_open();
        } else if (kc==KEY_ESCAPE || ch==0x1b) {
            browser_close();
        }
        return;
    }

    if (!app.file_open) {
        if (ch=='o'||ch=='O') browser_open();
        return;
    }

    if (ch==' ') {
        app.playing = !app.playing;
        app.last_frame_ms = time_ms();
    } else if (kc==KEY_ARROW_LEFT||ch=='j') {
        app.playing = 0;
        if (app.cur_frame>0) { tvd_seek_and_decode(app.cur_frame-1); app.cur_frame--; }
    } else if (kc==KEY_ARROW_RIGHT||ch=='l') {
        app.playing = 0;
        if (app.cur_frame<app.total_frames-1) { tvd_seek_and_decode(app.cur_frame+1); app.cur_frame++; }
    } else if (kc==KEY_HOME) {
        app.playing = 0; tvd_seek_and_decode(0); app.cur_frame = 0;
    } else if (kc==KEY_END) {
        int last = app.total_frames-1;
        app.playing = 0; tvd_seek_and_decode(last); app.cur_frame = last;
    } else if (ch=='+'||ch=='=') {
        int f = app.fps_override>0 ? app.fps_override : (int)app.fps;
        f += 5; if (f>120) f=120; app.fps_override = f;
    } else if (ch=='-') {
        int f = app.fps_override>0 ? app.fps_override : (int)app.fps;
        f -= 5; if (f<1) f=1; app.fps_override = f;
    } else if (ch=='f'||ch=='F') {
        app.fit_zoom = !app.fit_zoom;
    } else if (ch=='0') {
        app.fit_zoom=1; app.pan_x=0; app.pan_y=0;
    } else if (ch=='1') {
        app.fit_zoom=0; app.zoom=1.0f; app.pan_x=0; app.pan_y=0;
    } else if (ch=='o'||ch=='O') {
        browser_open();
    }
}

/* ============================================================
   Entry point
   ============================================================ */
int md_main(long argc, char **argv) {
    (void)argc;

    printf("tvd_player starting...\n");
    memset(&app, 0, sizeof(app));
    app.status_color = COL_MUTED;
    app.zoom         = 1.0f;
    app.fit_zoom     = 1;
    app.last_decoded = -1;
    memcpy(app.cwd, "/ModuOS", 8);
    set_status("Press O to open a .tvd file", COL_ACCENT);

    int efd = open("$/dev/input/event0", O_RDONLY|O_NONBLOCK, 0);
    if (efd < 0) { printf("tvd_player: no event device\n"); sleep(2); return 2; }

    if (NodGL_CreateDevice(NodGL_FEATURE_LEVEL_1_0, &app.device, &app.ctx, NULL) != NodGL_OK) {
        printf("tvd_player: NodGL failed\n"); close(efd); sleep(2); return 1;
    }
    NodGL_GetScreenResolution(app.device, &app.screen_w, &app.screen_h);

    NodGL_TextureDesc td; memset(&td, 0, sizeof(td));
    td.width=app.screen_w; td.height=app.screen_h;
    td.format=NodGL_FORMAT_R8G8B8A8_UNORM; td.mip_levels=1;

    if (NodGL_CreateTexture(app.device, &td, &app.backbuf_tex) != NodGL_OK) {
        printf("tvd_player: CreateTexture failed\n");
        NodGL_ReleaseDevice(app.device); close(efd); return 1;
    }
    if (NodGL_MapResource(app.ctx, app.backbuf_tex, (void **)&app.bb, &app.bb_pitch) != NodGL_OK) {
        printf("tvd_player: MapResource failed\n");
        NodGL_ReleaseResource(app.device, app.backbuf_tex);
        NodGL_ReleaseDevice(app.device); close(efd); return 1;
    }

    /* Font — load, parse, free the read buffer immediately */
    {
        int ffd = open("/ModuOS/shared/assets/fonts/Terminus.fnt", O_RDONLY, 0);
        if (ffd < 0) { printf("tvd_player: font not found\n"); goto cleanup; }
        long fsz = lseek(ffd, 0, SEEK_END); lseek(ffd, 0, SEEK_SET);
        if (fsz <= 0 || fsz > 8*1024*1024) { close(ffd); goto cleanup; }
        void *fdata = malloc((size_t)fsz);
        if (!fdata) { close(ffd); goto cleanup; }
        size_t r2 = 0;
        while (r2 < (size_t)fsz) {
            ssize_t n = read(ffd, (uint8_t *)fdata+r2, (size_t)fsz-r2);
            if (n <= 0) break; r2 += (size_t)n;
        }
        close(ffd);
        app.font = fnt_load_font(fdata, (size_t)fsz);
        free(fdata); /* free read buffer immediately — font has its own copy */
        if (!app.font) { printf("tvd_player: font parse failed\n"); goto cleanup; }
    }

    cursor_load_bmp("/ModuOS/shared/assets/mouse/arrow.bmp", &app.cur_arrow);
    cursor_load_bmp("/ModuOS/shared/assets/mouse/hand.bmp",  &app.cur_hand);

    /* Open file passed on command line, or show browser */
    if (argc >= 2 && argv && argv[1]) {
        tvd_open(argv[1]);
    } else {
        browser_open();
    }

    app.mouse_x       = (int32_t)(app.screen_w / 2);
    app.mouse_y       = (int32_t)(app.screen_h / 2);
    app.last_input_ms = time_ms();

    printf("Entering main loop...\n");
    int quit = 0;
    while (!quit) {
        Event ev;
        while (read(efd, &ev, sizeof(ev)) > 0) {
            if (ev.type == EVENT_KEY_PRESSED) {
                KeyCode kc = ev.data.keyboard.keycode;
                char    ch = ev.data.keyboard.ascii;
                if ((kc==KEY_ESCAPE||ch==0x1b) && !app.show_browser) { quit=1; break; }
                handle_key(kc, ch);
            } else if (ev.type == EVENT_MOUSE_MOVE) {
                app.mouse_x += (int32_t)ev.data.mouse.delta_x;
                app.mouse_y += (int32_t)ev.data.mouse.delta_y;
                if (app.mouse_x < 0) app.mouse_x = 0;
                if (app.mouse_y < 0) app.mouse_y = 0;
                if ((uint32_t)app.mouse_x >= app.screen_w) app.mouse_x=(int32_t)app.screen_w-1;
                if ((uint32_t)app.mouse_y >= app.screen_h) app.mouse_y=(int32_t)app.screen_h-1;
                if (app.scrubbing && (ev.data.mouse.buttons&1) && app.file_open) {
                    int sx2=48, sw2=(int)app.screen_w-96;
                    int fr = (int)((uint64_t)(app.mouse_x-sx2)*(app.total_frames-1)/sw2);
                    if (fr<0) fr=0;
                    if (fr>=app.total_frames) fr=app.total_frames-1;
                    tvd_seek_and_decode(fr);
                    app.cur_frame = fr;
                }
                app.mouse_btn = ev.data.mouse.buttons;
            } else if (ev.type == EVENT_MOUSE_BUTTON) {
                app.mouse_btn_prev = app.mouse_btn;
                app.mouse_btn      = ev.data.mouse.buttons;
                if ((app.mouse_btn&1) && !(app.mouse_btn_prev&1))
                    handle_click(app.mouse_x, app.mouse_y);
                if (!(app.mouse_btn&1)) app.scrubbing = 0;
            }
        }

        if (quit) break;

        /* Playback tick */
        if (app.playing && app.file_open) {
            uint64_t now = time_ms();
            int      efps = effective_fps();
            uint64_t ms_per_frame = (uint64_t)(1000 / (efps > 0 ? efps : 1));
            if (now - app.last_frame_ms >= ms_per_frame) {
                app.last_frame_ms = now;
                int next = app.cur_frame + 1;
                if (next >= app.total_frames) next = 0; /* loop */
                tvd_seek_and_decode(next);
                app.cur_frame = next;
            }
        }

        draw_frame();
        yield();
    }

    /* Cleanup — orderly, everything freed */
    browser_close();
    tvd_close_buffers();
    if (app.font) fnt_free_font(app.font);
    if (app.bb)   NodGL_UnmapResource(app.ctx, app.backbuf_tex);
    NodGL_ReleaseResource(app.device, app.backbuf_tex);
    NodGL_ReleaseDevice(app.device);
    close(efd);
    input_flush();
    return 0;

cleanup:
    if (app.bb) NodGL_UnmapResource(app.ctx, app.backbuf_tex);
    NodGL_ReleaseResource(app.device, app.backbuf_tex);
    NodGL_ReleaseDevice(app.device);
    close(efd);
    sleep(2);
    return 1;
}