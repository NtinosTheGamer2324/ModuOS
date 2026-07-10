/*
 * ilib_viewer.c — GUI viewer for .ilib image library files
 * ModuOS userland application
 *
 * Features:
 *   - Browse and open .ilib files from the filesystem
 *   - Thumbnail grid view of all images in a library
 *   - Zoom in / full-screen view of individual images
 *   - Keyboard and mouse navigation
 *   - Image metadata display (ID, dimensions, compressed size)
 *
 * Copyright © 2026 New Technologies Software — GPL v2.0
 */

#include "libc.h"
#include "NodGL.h"
#include "ilib.h"
#include "lib_fnt.h"
#include "../include/moduos/kernel/events/events.h"

/* ============================================================
   Constants & layout
   ============================================================ */

#define APP_TITLE       "ILIB Viewer"
#define FONT_PATH       "/ModuOS/shared/assets/fonts/Terminus.fnt"

/* Colour palette */
#define COL_BG          0xFF0D1117   /* dark background */
#define COL_PANEL       0xFF161B22   /* side-panel / header */
#define COL_BORDER      0xFF30363D   /* subtle border */
#define COL_ACCENT      0xFF58A6FF   /* blue accent */
#define COL_ACCENT2     0xFF3FB950   /* green accent */
#define COL_TEXT        0xFFC9D1D9   /* primary text */
#define COL_MUTED       0xFF8B949E   /* muted text */
#define COL_ERROR       0xFFF85149   /* error red */
#define COL_WARN        0xFFE3B341   /* warning yellow */
#define COL_SEL_BG      0xFF1F6FEB   /* selection highlight */
#define COL_THUMB_BG    0xFF21262D   /* thumbnail cell background */
#define COL_THUMB_BOR   0xFF30363D
#define COL_THUMB_HOV   0xFF388BFD   /* hovered thumbnail border */
#define COL_CHECKER_A   0xFF1A1A1A   /* transparency checker dark */
#define COL_CHECKER_B   0xFF2A2A2A   /* transparency checker light */
#define COL_BLACK       0xFF000000
#define COL_WHITE       0xFFFFFFFF

/* Thumbnail grid */
#define THUMB_PAD       8
#define THUMB_SIZE      48   /* image area inside cell */
#define THUMB_CELL      (THUMB_SIZE + THUMB_PAD * 2 + 20)  /* +20 for label */
#define THUMB_COLS_DEF  4    /* default columns (recalculated at runtime) */

/* Header / footer heights */
#define HEADER_H        40
#define FOOTER_H        24
#define SIDEBAR_W       220

/* Zoom step */
#define ZOOM_MIN        0.1f
#define ZOOM_MAX        8.0f
#define ZOOM_STEP       0.25f

/* Cursor */
#define CURSOR_W 16
#define CURSOR_H 16

/* Icon IDs from /ModuOS/shared/assets/icons.ilib */
#define ICON_THIS_PC          0
#define ICON_ATA_DRIVE        1
#define ICON_SCSI_DRIVE       2
#define ICON_CD_ROM           3
#define ICON_FOLDER_OPEN      4
#define ICON_FOLDER_CLOSED    5
#define ICON_FOLDER_FULL      6
#define ICON_FILE_EXPLORER    7
#define ICON_TERMINAL         8
#define ICON_TRASH            9
#define ICON_TRASH_FULL       10
#define ICON_RICH_DOC         11
#define ICON_BIN              12
#define ICON_UNKNOWN_FILE     13
#define ICON_MUSIC            14
#define ICON_NETWORK          15
#define ICON_TASK_MANAGER     16
#define ICON_GLOBE            17
#define ICON_STOPWATCH        18
#define ICON_PERM_REQ         19
#define ICON_LOCKED_FOLDER    20
#define ICON_ARCHIVE          21
#define ICON_FOLDER_PERM      22
#define ICON_IE               23
#define ICON_WORD             24
#define ICON_COUNT            25
#define ICON_SIZE             16  /* display size for icons */

typedef struct {
    uint8_t rgba[CURSOR_W * CURSOR_H * 4];
    int     loaded;
} CursorImg;

#define MAX_PATH        512

/* ============================================================
   State
   ============================================================ */

typedef enum {
    VIEW_FILE_BROWSER,
    VIEW_THUMB_GRID,
    VIEW_IMAGE,
} ViewMode;

typedef struct {
    uint8_t  *pixels;
    int       loaded;
    int       failed;
} ThumbCache;

typedef struct {
    char  name[256];
    int   is_dir;
} DirEntry;

#define MAX_DIR_ENTRIES 256

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

    /* Current view */
    ViewMode      view;

    /* File browser */
    char          cwd[MAX_PATH];
    DirEntry      dir_entries[MAX_DIR_ENTRIES];
    int           dir_count;
    int           browser_sel;
    int           browser_scroll;

    /* Loaded library */
    char          lib_path[MAX_PATH];
    ilib_t       *lib;
    ThumbCache   *thumbs;
    int           thumb_scroll;
    int           thumb_hover;
    int           thumb_sel;

    /* Image view */
    int           view_id;
    ilib_image_t  view_img;
    int           view_img_loaded;
    float         zoom;
    int           pan_x, pan_y;

    /* Mouse — absolute position, updated via deltas */
    int32_t       mouse_x, mouse_y;
    uint8_t       mouse_btn;
    uint8_t       mouse_btn_prev;

    /* Software cursor images */
    CursorImg     cur_arrow;
    CursorImg     cur_hand;

    /* Icon cache — loaded from icons.ilib at startup */
    ilib_t       *icons_lib;
    ilib_image_t  icons[ICON_COUNT];   /* loaded on demand */
    int           icons_loaded[ICON_COUNT];

    /* Status bar */
    char          status[256];
    uint32_t      status_color;
} app;

/* ============================================================
   Helper: set_status
   ============================================================ */
static void set_status(const char *msg, uint32_t color) {
    size_t len = strlen(msg);
    if (len >= sizeof(app.status)) len = sizeof(app.status) - 1;
    memcpy(app.status, msg, len);
    app.status[len] = 0;
    app.status_color = color;
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

static void gfx_rect(int x, int y, int w, int h, uint32_t col) {
    gfx_fill(x, y, w, 1, col);
    gfx_fill(x, y + h - 1, w, 1, col);
    gfx_fill(x, y, 1, h, col);
    gfx_fill(x + w - 1, y, 1, h, col);
}

/* Alpha-blend a single RGBA pixel onto the backbuffer */
static inline void gfx_blend_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || y < 0 || (uint32_t)x >= app.screen_w || (uint32_t)y >= app.screen_h) return;
    if (!app.bb) return;
    uint32_t *dst = (uint32_t *)(app.bb + (uint64_t)y * app.bb_pitch) + x;
    if (a == 255) {
        *dst = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    } else if (a == 0) {
        /* no-op */
    } else {
        uint32_t d = *dst;
        uint8_t dr = (d >> 16) & 0xFF;
        uint8_t dg = (d >>  8) & 0xFF;
        uint8_t db = (d      ) & 0xFF;
        uint32_t ia = 255 - a;
        uint8_t nr = (uint8_t)((r * a + dr * ia) / 255);
        uint8_t ng = (uint8_t)((g * a + dg * ia) / 255);
        uint8_t nb = (uint8_t)((b * a + db * ia) / 255);
        *dst = 0xFF000000 | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | nb;
    }
}

/* Draw a text char at pixel position */
static void gfx_char(int x, int y, char c, uint32_t col) {
    if (!app.font) return;
    fnt_glyph_t *g = fnt_get_glyph(app.font, (uint32_t)(unsigned char)c);
    if (!g) return;
    uint8_t r = (col >> 16) & 0xFF;
    uint8_t gv = (col >> 8) & 0xFF;
    uint8_t b = col & 0xFF;
    for (int dy = 0; dy < g->bitmap_height; dy++)
        for (int dx = 0; dx < g->bitmap_width; dx++)
            if (fnt_get_pixel(g, dx, dy))
                gfx_blend_pixel(x + dx, y + dy, r, gv, b, 0xFF);
}

static void gfx_char_scaled(int x, int y, char c, uint32_t col, int scale) {
    if (!app.font || scale <= 1) { gfx_char(x, y, c, col); return; }
    fnt_glyph_t *g = fnt_get_glyph(app.font, (uint32_t)(unsigned char)c);
    if (!g) return;
    uint8_t r = (col >> 16) & 0xFF;
    uint8_t gv = (col >> 8) & 0xFF;
    uint8_t b = col & 0xFF;
    for (int dy = 0; dy < g->bitmap_height; dy++)
        for (int dx = 0; dx < g->bitmap_width; dx++)
            if (fnt_get_pixel(g, dx, dy))
                gfx_fill(x + dx * scale, y + dy * scale, scale, scale,
                          0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)gv << 8) | b);
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
    if (!app.font || !s || scale <= 1) { gfx_text(x, y, s, col); return; }
    int cx = x;
    while (*s) {
        fnt_glyph_t *g = fnt_get_glyph(app.font, (uint32_t)(unsigned char)*s);
        if (g) { gfx_char_scaled(cx, y, *s, col, scale); cx += g->width * scale; }
        s++;
    }
}

/* Draw number */
static void gfx_num(int x, int y, int n, uint32_t col) {
    char buf[24]; itoa(n, buf, 10); gfx_text(x, y, buf, col);
}

/* Right-align text ending at x */
static void gfx_text_right(int x, int y, const char *s, uint32_t col) {
    int w = fnt_string_width(app.font, s);
    gfx_text(x - w, y, s, col);
}

/* Centered text within [x, x+w) */
static void gfx_text_center(int cx, int y, const char *s, uint32_t col) {
    int w = fnt_string_width(app.font, s);
    gfx_text(cx - w / 2, y, s, col);
}

/* Horizontally clipped text */
static void gfx_text_clip(int x, int y, int max_w, const char *s, uint32_t col) {
    if (!app.font || !s) return;
    int cx = x;
    while (*s) {
        fnt_glyph_t *g = fnt_get_glyph(app.font, (uint32_t)(unsigned char)*s);
        if (!g) { s++; continue; }
        if (cx + g->width > x + max_w) {
            /* Draw "…" if space allows */
            fnt_glyph_t *ell = fnt_get_glyph(app.font, '.');
            if (ell && cx + ell->width * 3 <= x + max_w) {
                gfx_char(cx, y, '.', col); cx += ell->width;
                gfx_char(cx, y, '.', col); cx += ell->width;
                gfx_char(cx, y, '.', col);
            }
            return;
        }
        gfx_char(cx, y, *s, col);
        cx += g->width;
        s++;
    }
}

/* ============================================================
   Checkerboard (transparency indicator)
   ============================================================ */
static void gfx_checker(int x, int y, int w, int h, int cell) {
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++) {
            int tx = (x + xx) / cell;
            int ty = (y + yy) / cell;
            gfx_fill(x + xx, y + yy, 1, 1,
                ((tx + ty) & 1) ? COL_CHECKER_B : COL_CHECKER_A);
        }
}

/* ============================================================
   Blit RGBA image to backbuffer with alpha blending
   ============================================================ */
static void gfx_blit_rgba(int dst_x, int dst_y,
                            const uint8_t *pixels, int src_w, int src_h,
                            int clip_x, int clip_y, int clip_w, int clip_h)
{
    for (int sy = 0; sy < src_h; sy++) {
        int dy = dst_y + sy;
        if (dy < clip_y || dy >= clip_y + clip_h) continue;
        for (int sx = 0; sx < src_w; sx++) {
            int dx = dst_x + sx;
            if (dx < clip_x || dx >= clip_x + clip_w) continue;
            const uint8_t *p = pixels + (sy * src_w + sx) * 4;
            gfx_blend_pixel(dx, dy, p[0], p[1], p[2], p[3]);
        }
    }
}

/* Blit RGBA with nearest-neighbour zoom */
static void gfx_blit_rgba_zoom(int dst_x, int dst_y, int dst_w, int dst_h,
                                 const uint8_t *pixels, int src_w, int src_h,
                                 int clip_x, int clip_y, int clip_w, int clip_h)
{
    if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) return;
    for (int dy = 0; dy < dst_h; dy++) {
        int abs_dy = dst_y + dy;
        if (abs_dy < clip_y || abs_dy >= clip_y + clip_h) continue;
        int sy = dy * src_h / dst_h;
        for (int dx = 0; dx < dst_w; dx++) {
            int abs_dx = dst_x + dx;
            if (abs_dx < clip_x || abs_dx >= clip_x + clip_w) continue;
            int sx = dx * src_w / dst_w;
            const uint8_t *p = pixels + (sy * src_w + sx) * 4;
            gfx_blend_pixel(abs_dx, abs_dy, p[0], p[1], p[2], p[3]);
        }
    }
}

/* Scale RGBA image to THUMB_SIZE×THUMB_SIZE with letterboxing (writes to dst[THUMB_SIZE*THUMB_SIZE*4]) */
static void rgba_make_thumb(const uint8_t *src, int sw, int sh, uint8_t *dst) {
    /* Clear to transparent */
    memset(dst, 0, THUMB_SIZE * THUMB_SIZE * 4);

    if (sw <= 0 || sh <= 0) return;

    /* Keep aspect ratio */
    int tw = THUMB_SIZE, th = THUMB_SIZE;
    if (sw * THUMB_SIZE > sh * THUMB_SIZE) {
        /* wider */
        th = sh * THUMB_SIZE / sw;
    } else {
        tw = sw * THUMB_SIZE / sh;
    }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;

    int ox = (THUMB_SIZE - tw) / 2;
    int oy = (THUMB_SIZE - th) / 2;

    for (int dy = 0; dy < th; dy++) {
        int sy = dy * sh / th;
        for (int dx = 0; dx < tw; dx++) {
            int sx = dx * sw / tw;
            const uint8_t *s = src + (sy * sw + sx) * 4;
            uint8_t *d = dst + ((oy + dy) * THUMB_SIZE + (ox + dx)) * 4;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
    }
}

/* ============================================================
   Cursor loading (32-bit BMP BGRA, 16x16)
   Mirrors the approach from paintgfx
   ============================================================ */

static inline uint32_t cur__rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
           ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

static void cursor_load_bmp(const char *path, CursorImg *out) {
    out->loaded = 0;
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return;
    long sz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (sz < 54 || sz > 64 * 1024) { close(fd); return; }
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
    uint32_t pixel_off = cur__rd32(buf + 10);
    int32_t  width     = (int32_t)cur__rd32(buf + 18);
    int32_t  height    = (int32_t)cur__rd32(buf + 22);
    uint16_t bpp       = (uint16_t)(buf[28] | (buf[29] << 8));
    if (bpp != 32 || width != CURSOR_W) { free(buf); return; }
    int top_down = 0;
    if (height < 0) { top_down = 1; height = -height; }
    if (height != CURSOR_H) { free(buf); return; }
    uint32_t row_stride = (uint32_t)width * 4u;
    if (pixel_off + row_stride * (uint32_t)height > (uint32_t)sz) { free(buf); return; }
    for (uint32_t y = 0; y < (uint32_t)CURSOR_H; y++) {
        uint32_t sy = top_down ? y : ((uint32_t)CURSOR_H - 1u - y);
        const uint8_t *row = buf + pixel_off + sy * row_stride;
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

/* Software-blit cursor RGBA onto the backbuffer with alpha blending */
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
                uint8_t nr = (uint8_t)((sr*sa + ((d>>16)&0xFF)*ia)/255u);
                uint8_t ng = (uint8_t)((sg*sa + ((d>> 8)&0xFF)*ia)/255u);
                uint8_t nb = (uint8_t)((sb*sa + ( d     &0xFF)*ia)/255u);
                row[px] = 0xFF000000u | ((uint32_t)nr<<16) | ((uint32_t)ng<<8) | nb;
            }
        }
    }
}

/* ============================================================
   Icon helpers — must come after gfx_blit_rgba_zoom
   ============================================================ */

static uint8_t *icon_get(int id) {
    if (!app.icons_lib || id < 0 || id >= ICON_COUNT) return NULL;
    if (app.icons_loaded[id] ==  1) return app.icons[id].pixels;
    if (app.icons_loaded[id] == -1) return NULL; /* previously failed */
    int r = ilib_load_image(app.icons_lib, (uint16_t)id, &app.icons[id]);
    if (r != ILIB_OK) { app.icons_loaded[id] = -1; return NULL; }
    app.icons_loaded[id] = 1;
    return app.icons[id].pixels;
}

static void gfx_blit_icon(int id, int dst_x, int dst_y, int size) {
    uint8_t *px = icon_get(id);
    if (!px) return;
    ilib_image_t *img = &app.icons[id];
    gfx_blit_rgba_zoom(dst_x, dst_y, size, size,
                        px, (int)img->width, (int)img->height,
                        0, 0, (int)app.screen_w, (int)app.screen_h);
}

/* ============================================================
   File browser helpers
   ============================================================ */
static void browser_load_dir(const char *path) {
    app.dir_count = 0;
    app.browser_sel = 0;
    app.browser_scroll = 0;

    int fd = opendir(path);
    if (fd < 0) {
        set_status("Cannot open directory", COL_ERROR);
        return;
    }

    /* Add ".." entry unless at root */
    if (!(path[0] == '$' && path[1] == '/' && path[2] == 0) &&
        !(path[0] == '/' && path[1] == 0)) {
        memcpy(app.dir_entries[app.dir_count].name, "..", 3);
        app.dir_entries[app.dir_count].is_dir = 1;
        app.dir_count++;
    }

    char name[256];
    int is_dir;
    uint32_t sz;
    while (app.dir_count < MAX_DIR_ENTRIES) {
        int rc = readdir(fd, name, sizeof(name), &is_dir, &sz);
        if (rc == 0) break;   /* end of directory */
        if (rc < 0) break;    /* error */

        /* Skip hidden dot-entries (. and ..) — we add ".." manually above */
        if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
            continue;

        /* Show everything — dirs to navigate into, any file the user might want */
        /* Highlight .ilib files specially but don't hide other files */
        size_t nl = strlen(name);
        if (nl >= 255) nl = 254;
        memcpy(app.dir_entries[app.dir_count].name, name, nl);
        app.dir_entries[app.dir_count].name[nl] = 0;
        app.dir_entries[app.dir_count].is_dir = is_dir;
        app.dir_count++;
    }
    closedir(fd);

    /* Simple insertion sort: dirs first, then files, alphabetical within each */
    for (int i = 1; i < app.dir_count; i++) {
        DirEntry tmp = app.dir_entries[i];
        int j = i - 1;
        while (j >= 0) {
            int cmp;
            if (app.dir_entries[j].is_dir != tmp.is_dir)
                cmp = tmp.is_dir ? -1 : 1;
            else
                cmp = strcmp(app.dir_entries[j].name, tmp.name);
            if (cmp > 0) { app.dir_entries[j + 1] = app.dir_entries[j]; j--; }
            else break;
        }
        app.dir_entries[j + 1] = tmp;
    }

    size_t plen = strlen(path);
    if (plen >= MAX_PATH) plen = MAX_PATH - 1;
    memcpy(app.cwd, path, plen);
    app.cwd[plen] = 0;

    set_status("Browse and select an .ilib file", COL_MUTED);
}

/* Build full path: cwd + "/" + name */
static void path_join(char *out, size_t outsz, const char *dir, const char *name) {
    size_t dl = strlen(dir);
    size_t nl = strlen(name);
    if (dl + 1 + nl + 1 > outsz) { out[0] = 0; return; }
    memcpy(out, dir, dl);
    if (dl > 0 && dir[dl - 1] != '/') { out[dl++] = '/'; }
    memcpy(out + dl, name, nl + 1);
}

/* Go up one directory */
static void path_parent(char *path) {
    size_t l = strlen(path);
    if (l == 0) return;
    /* Remove trailing slash */
    if (l > 1 && path[l - 1] == '/') { path[--l] = 0; }
    /* Find last slash */
    for (int i = (int)l - 1; i >= 0; i--) {
        if (path[i] == '/') {
            if (i == 0) path[1] = 0;
            else path[i] = 0;
            return;
        }
    }
}

/* ============================================================
   Library loading
   ============================================================ */
static void lib_open_file(const char *path) {
    /* Close previous */
    if (app.lib) {
        ilib_close(app.lib);
        app.lib = NULL;
    }
    if (app.thumbs) {
        for (int i = 0; i < 0; i++) { /* count unknown — free was per-entry */ }
        free(app.thumbs);
        app.thumbs = NULL;
    }
    if (app.view_img_loaded) {
        ilib_free_image(&app.view_img);
        app.view_img_loaded = 0;
    }

    app.lib = ilib_open(path);
    if (!app.lib) {
        set_status("Failed to open .ilib file", COL_ERROR);
        return;
    }

    size_t plen = strlen(path);
    if (plen >= MAX_PATH) plen = MAX_PATH - 1;
    memcpy(app.lib_path, path, plen);
    app.lib_path[plen] = 0;

    app.thumbs = (ThumbCache *)calloc(app.lib->count, sizeof(ThumbCache));
    if (!app.thumbs) {
        ilib_close(app.lib);
        app.lib = NULL;
        set_status("Out of memory loading thumbnails", COL_ERROR);
        return;
    }

    app.thumb_scroll = 0;
    app.thumb_hover  = -1;
    app.thumb_sel    = -1;
    app.view = VIEW_THUMB_GRID;

    char msg[256];
    char num_buf[16];
    itoa(app.lib->count, num_buf, 10);
    memcpy(msg, "Loaded: ", 8);
    size_t ml = 8;
    size_t nl = strlen(num_buf);
    memcpy(msg + ml, num_buf, nl); ml += nl;
    memcpy(msg + ml, " image(s)", 9); ml += 9;
    msg[ml] = 0;
    set_status(msg, COL_ACCENT2);
}

/* Load (or return cached) thumbnail for entry index i */
static uint8_t *thumb_get(int i) {
    if (i < 0 || !app.lib || i >= app.lib->count) return NULL;
    ThumbCache *tc = &app.thumbs[i];
    if (tc->loaded) return tc->pixels;
    if (tc->failed) return NULL;

    ilib_image_t img;
    int r = ilib_load_image(app.lib, app.lib->entries[i].image_id, &img);
    if (r != ILIB_OK) { tc->failed = 1; return NULL; }

    tc->pixels = (uint8_t *)malloc(THUMB_SIZE * THUMB_SIZE * 4);
    if (!tc->pixels) { ilib_free_image(&img); tc->failed = 1; return NULL; }

    rgba_make_thumb(img.pixels, img.width, img.height, tc->pixels);
    ilib_free_image(&img);
    tc->loaded = 1;
    return tc->pixels;
}

/* Open full-view for entry index i */
static void view_open_image(int i) {
    if (!app.lib || i < 0 || i >= app.lib->count) return;

    if (app.view_img_loaded) {
        ilib_free_image(&app.view_img);
        app.view_img_loaded = 0;
    }

    int r = ilib_load_image(app.lib, app.lib->entries[i].image_id, &app.view_img);
    if (r != ILIB_OK) {
        set_status("Failed to load image", COL_ERROR);
        return;
    }
    app.view_img_loaded = 1;
    app.view_id = i;

    /* Fit to screen */
    int avail_w = (int)app.screen_w - SIDEBAR_W;
    int avail_h = (int)app.screen_h - HEADER_H - FOOTER_H;
    float zx = (float)avail_w / (float)app.view_img.width;
    float zy = (float)avail_h / (float)app.view_img.height;
    app.zoom = zx < zy ? zx : zy;
    if (app.zoom < ZOOM_MIN) app.zoom = ZOOM_MIN;
    if (app.zoom > ZOOM_MAX) app.zoom = ZOOM_MAX;
    app.pan_x = 0;
    app.pan_y = 0;

    app.view = VIEW_IMAGE;

    set_status("Arrow keys to pan  +/- to zoom  Backspace to go back", COL_MUTED);
}

/* ============================================================
   Drawing: Header
   ============================================================ */
static void draw_header(void) {
    gfx_fill(0, 0, (int)app.screen_w, HEADER_H, COL_PANEL);
    gfx_fill(0, HEADER_H - 1, (int)app.screen_w, 1, COL_BORDER);

    /* Logo — icon + text */
    if (app.icons_lib && icon_get(ICON_FILE_EXPLORER)) {
        gfx_blit_icon(ICON_FILE_EXPLORER, 6, (HEADER_H - ICON_SIZE) / 2, ICON_SIZE);
        gfx_text_scaled(8 + ICON_SIZE + 4, 6, "ILIB", COL_ACCENT, 2);
    } else {
        gfx_text_scaled(8, 6, "ILIB", COL_ACCENT, 2);
    }
    gfx_text(8 + ICON_SIZE + 4 + fnt_string_width_scaled(app.font, "ILIB", 2) + 6,
             13, "Viewer", COL_TEXT);

    /* Path / info */
    if (app.view == VIEW_FILE_BROWSER) {
        gfx_text_clip(160, 13, (int)app.screen_w - 170, app.cwd, COL_MUTED);
    } else if (app.view == VIEW_THUMB_GRID || app.view == VIEW_IMAGE) {
        /* File name */
        const char *bn = app.lib_path;
        for (const char *p = app.lib_path; *p; p++) if (*p == '/') bn = p + 1;
        gfx_text_clip(160, 8, (int)app.screen_w - 170, bn, COL_TEXT);
        if (app.lib) {
            char buf[64];
            char cnt[16];
            itoa(app.lib->count, cnt, 10);
            memcpy(buf, cnt, strlen(cnt) + 1);
            strcat(buf, " images");
            gfx_text_clip(160, 22, (int)app.screen_w - 170, buf, COL_MUTED);
        }
    }

    /* Tab buttons */
    if (app.lib) {
        int bx = (int)app.screen_w - 300;
        const char *tabs[] = { "Browse", "Grid", "Image" };
        ViewMode modes[] = { VIEW_FILE_BROWSER, VIEW_THUMB_GRID, VIEW_IMAGE };
        for (int t = 0; t < 3; t++) {
            int bw = 80;
            int active = (app.view == modes[t]);
            uint32_t bg = active ? COL_ACCENT : COL_BORDER;
            uint32_t fg = active ? COL_WHITE  : COL_MUTED;
            gfx_fill(bx, 6, bw, HEADER_H - 12, bg);
            gfx_text_center(bx + bw / 2, 14, tabs[t], fg);
            bx += bw + 4;
        }
    } else {
        int bx = (int)app.screen_w - 180;
        gfx_fill(bx, 6, 80, HEADER_H - 12, COL_SEL_BG);
        gfx_text_center(bx + 40, 14, "Browse", COL_WHITE);
    }
}

/* ============================================================
   Drawing: Footer / status bar
   ============================================================ */
static void draw_footer(void) {
    int y = (int)app.screen_h - FOOTER_H;
    gfx_fill(0, y, (int)app.screen_w, FOOTER_H, COL_PANEL);
    gfx_fill(0, y, (int)app.screen_w, 1, COL_BORDER);
    gfx_text_clip(8, y + 6, (int)app.screen_w - 200, app.status, app.status_color);

    /* Clamp mouse display coords defensively */
    int32_t disp_x = app.mouse_x;
    int32_t disp_y = app.mouse_y;
    if (disp_x < 0) disp_x = 0;
    if (disp_y < 0) disp_y = 0;
    if ((uint32_t)disp_x >= app.screen_w) disp_x = (int32_t)app.screen_w - 1;
    if ((uint32_t)disp_y >= app.screen_h) disp_y = (int32_t)app.screen_h - 1;

    char mbuf[32];
    char xb[12], yb[12];
    itoa((int)disp_x, xb, 10);
    itoa((int)disp_y, yb, 10);
    memcpy(mbuf, "x:", 2);
    memcpy(mbuf + 2, xb, strlen(xb) + 1);
    strcat(mbuf, " y:");
    strcat(mbuf, yb);
    gfx_text_right((int)app.screen_w - 8, y + 6, mbuf, COL_MUTED);
}

/* ============================================================
   Drawing: File Browser
   ============================================================ */
#define BROWSER_ITEM_H  22
#define BROWSER_ICON_W  16

static void draw_browser(void) {
    int content_y = HEADER_H;
    int content_h = (int)app.screen_h - HEADER_H - FOOTER_H;

    gfx_fill(0, content_y, (int)app.screen_w, content_h, COL_BG);

    /* Breadcrumb bar */
    gfx_fill(0, content_y, (int)app.screen_w, 24, COL_PANEL);
    gfx_fill(0, content_y + 23, (int)app.screen_w, 1, COL_BORDER);
    gfx_text(8, content_y + 6, app.cwd, COL_TEXT);
    content_y += 24;
    content_h -= 24;

    /* Entry list */
    int visible = content_h / BROWSER_ITEM_H;
    int max_scroll = app.dir_count - visible;
    if (max_scroll < 0) max_scroll = 0;
    if (app.browser_scroll > max_scroll) app.browser_scroll = max_scroll;

    for (int i = app.browser_scroll; i < app.dir_count; i++) {
        int row = i - app.browser_scroll;
        int iy = content_y + row * BROWSER_ITEM_H;
        if (iy + BROWSER_ITEM_H > (int)app.screen_h - FOOTER_H) break;

        int hovered  = (app.mouse_y >= iy && app.mouse_y < iy + BROWSER_ITEM_H &&
                        app.mouse_x < (int)app.screen_w);
        int selected = (i == app.browser_sel);

        uint32_t bg = selected ? COL_SEL_BG : (hovered ? 0xFF1C2128 : COL_BG);
        gfx_fill(0, iy, (int)app.screen_w, BROWSER_ITEM_H, bg);

        /* Icon — use real icons from icons.ilib */
        {
            int icon_id;
            if (strcmp(app.dir_entries[i].name, "..") == 0) {
                icon_id = ICON_FOLDER_OPEN;
            } else if (app.dir_entries[i].is_dir) {
                icon_id = ICON_FOLDER_CLOSED;
            } else {
                /* Check if .ilib file */
                const char *n = app.dir_entries[i].name;
                size_t nl = strlen(n);
                int is_ilib = (nl >= 5 && strcmp(n + nl - 5, ".ilib") == 0);
                icon_id = is_ilib ? ICON_FILE_EXPLORER : ICON_UNKNOWN_FILE;
            }

            if (app.icons_lib && icon_get(icon_id)) {
                gfx_blit_icon(icon_id, 6, iy + (BROWSER_ITEM_H - ICON_SIZE) / 2, ICON_SIZE);
            } else {
                /* Fallback text tag */
                uint32_t fc = app.dir_entries[i].is_dir ? COL_WARN : COL_ACCENT2;
                gfx_text(8, iy + (BROWSER_ITEM_H - app.font->header.glyph_height) / 2,
                         app.dir_entries[i].is_dir ? "[D]" : "[F]", fc);
            }
        }

        /* Name — offset right of icon */
        uint32_t text_col = selected ? COL_WHITE : COL_TEXT;
        gfx_text_clip(8 + ICON_SIZE + 6, iy + (BROWSER_ITEM_H - app.font->header.glyph_height) / 2,
                      (int)app.screen_w - (8 + ICON_SIZE + 6) - 12,
                      app.dir_entries[i].name, text_col);

        /* Thin separator */
        gfx_fill(0, iy + BROWSER_ITEM_H - 1, (int)app.screen_w, 1, COL_BORDER);
    }

    /* Scroll indicator */
    if (app.dir_count > visible && visible > 0) {
        int track_h = content_h;
        int thumb_h = track_h * visible / app.dir_count;
        if (thumb_h < 8) thumb_h = 8;
        int thumb_y = content_y + track_h * app.browser_scroll / app.dir_count;
        gfx_fill((int)app.screen_w - 6, content_y, 6, track_h, COL_PANEL);
        gfx_fill((int)app.screen_w - 5, thumb_y, 4, thumb_h, COL_BORDER);
    }

    /* Help text at bottom of content if empty */
    if (app.dir_count == 0) {
        gfx_text_center((int)app.screen_w / 2,
                        content_y + content_h / 2 - 10,
                        "Empty directory", COL_MUTED);
    }
}

/* ============================================================
   Drawing: Sidebar (shared between grid and image view)
   ============================================================ */
static void draw_sidebar(int sel_entry) {
    int x = (int)app.screen_w - SIDEBAR_W;
    int y = HEADER_H;
    int h = (int)app.screen_h - HEADER_H - FOOTER_H;

    gfx_fill(x, y, SIDEBAR_W, h, COL_PANEL);
    gfx_fill(x, y, 1, h, COL_BORDER);

    int cy = y + 8;

    gfx_text(x + 8, cy, "Library Info", COL_ACCENT); cy += 18;
    gfx_fill(x + 8, cy, SIDEBAR_W - 16, 1, COL_BORDER); cy += 8;

    /* File name */
    const char *bn = app.lib_path;
    for (const char *p = app.lib_path; *p; p++) if (*p == '/') bn = p + 1;
    gfx_text(x + 8, cy, "File:", COL_MUTED); cy += 14;
    gfx_text_clip(x + 8, cy, SIDEBAR_W - 16, bn, COL_TEXT); cy += 18;

    /* Count */
    char buf[32];
    gfx_text(x + 8, cy, "Images:", COL_MUTED);
    itoa(app.lib->count, buf, 10);
    gfx_text_right(x + SIDEBAR_W - 8, cy, buf, COL_TEXT);
    cy += 18;

    if (sel_entry >= 0 && sel_entry < app.lib->count) {
        cy += 8;
        gfx_fill(x + 8, cy, SIDEBAR_W - 16, 1, COL_BORDER); cy += 8;
        gfx_text(x + 8, cy, "Selected Image", COL_ACCENT); cy += 18;
        gfx_fill(x + 8, cy, SIDEBAR_W - 16, 1, COL_BORDER); cy += 8;

        ilib_entry_t *en = &app.lib->entries[sel_entry];

        gfx_text(x + 8, cy, "ID:", COL_MUTED);
        itoa(en->image_id, buf, 10);
        gfx_text_right(x + SIDEBAR_W - 8, cy, buf, COL_TEXT); cy += 14;

        gfx_text(x + 8, cy, "Width:", COL_MUTED);
        itoa(en->width, buf, 10);
        gfx_text_right(x + SIDEBAR_W - 8, cy, buf, COL_TEXT); cy += 14;

        gfx_text(x + 8, cy, "Height:", COL_MUTED);
        itoa(en->height, buf, 10);
        gfx_text_right(x + SIDEBAR_W - 8, cy, buf, COL_TEXT); cy += 14;

        gfx_text(x + 8, cy, "Raw bytes:", COL_MUTED);
        itoa((int)en->raw_size, buf, 10);
        gfx_text_right(x + SIDEBAR_W - 8, cy, buf, COL_TEXT); cy += 14;

        gfx_text(x + 8, cy, "Cmp bytes:", COL_MUTED);
        itoa((int)en->cmp_size, buf, 10);
        gfx_text_right(x + SIDEBAR_W - 8, cy, buf, COL_TEXT); cy += 14;

        /* Ratio */
        if (en->raw_size > 0) {
            int pct = (int)((uint64_t)en->cmp_size * 100 / en->raw_size);
            gfx_text(x + 8, cy, "Ratio:", COL_MUTED);
            itoa(pct, buf, 10);
            strcat(buf, "%");
            gfx_text_right(x + SIDEBAR_W - 8, cy, buf,
                           pct < 50 ? COL_ACCENT2 : COL_WARN);
            cy += 14;
        }

        gfx_text(x + 8, cy, "Offset:", COL_MUTED);
        itoa((int)en->file_offset, buf, 16);
        char hex[24]; memcpy(hex, "0x", 2); memcpy(hex + 2, buf, strlen(buf) + 1);
        gfx_text_right(x + SIDEBAR_W - 8, cy, hex, COL_TEXT); cy += 14;

        /* Thumb status */
        cy += 8;
        if (sel_entry < app.lib->count) {
            ThumbCache *tc = &app.thumbs[sel_entry];
            if (tc->failed) {
                gfx_text(x + 8, cy, "Load: FAILED", COL_ERROR);
            } else if (tc->loaded) {
                gfx_text(x + 8, cy, "Load: OK", COL_ACCENT2);
            } else {
                gfx_text(x + 8, cy, "Load: pending", COL_MUTED);
            }
        }
    }

    /* Help */
    int help_y = (int)app.screen_h - FOOTER_H - 80;
    gfx_fill(x + 8, help_y, SIDEBAR_W - 16, 1, COL_BORDER); help_y += 8;
    gfx_text(x + 8, help_y, "Enter - Open image", COL_MUTED); help_y += 14;
    gfx_text(x + 8, help_y, "Backspace - Back",   COL_MUTED); help_y += 14;
    gfx_text(x + 8, help_y, "ESC - File browser", COL_MUTED);
}

/* ============================================================
   Drawing: Thumbnail Grid
   ============================================================ */
static void draw_thumb_grid(void) {
    if (!app.lib) return;

    int content_x = 0;
    int content_y = HEADER_H;
    int content_w = (int)app.screen_w - SIDEBAR_W;
    int content_h = (int)app.screen_h - HEADER_H - FOOTER_H;

    gfx_fill(content_x, content_y, content_w, content_h, COL_BG);

    /* Calculate columns */
    int cols = content_w / THUMB_CELL;
    if (cols < 1) cols = 1;

    int rows_total = (app.lib->count + cols - 1) / cols;
    int visible_rows = content_h / THUMB_CELL;
    int max_scroll = rows_total - visible_rows;
    if (max_scroll < 0) max_scroll = 0;
    if (app.thumb_scroll > max_scroll) app.thumb_scroll = max_scroll;

    int first_row = app.thumb_scroll;
    int last_row  = first_row + visible_rows + 1;

    app.thumb_hover = -1;

    for (int row = first_row; row < last_row && row < rows_total; row++) {
        for (int col = 0; col < cols; col++) {
            int idx = row * cols + col;
            if (idx >= app.lib->count) break;

            int cx = content_x + col * THUMB_CELL + THUMB_PAD;
            int cy = content_y + (row - first_row) * THUMB_CELL + THUMB_PAD;

            /* Hover detection */
            int hovered = (app.mouse_x >= cx - THUMB_PAD &&
                           app.mouse_x < cx + THUMB_CELL - THUMB_PAD &&
                           app.mouse_y >= cy - THUMB_PAD &&
                           app.mouse_y < cy + THUMB_CELL - THUMB_PAD);
            if (hovered) app.thumb_hover = idx;

            int selected = (idx == app.thumb_sel);

            /* Cell background */
            gfx_fill(cx - THUMB_PAD, cy - THUMB_PAD, THUMB_CELL, THUMB_CELL,
                     selected ? 0xFF1C2128 : COL_THUMB_BG);

            /* Checker for transparency */
            gfx_checker(cx, cy, THUMB_SIZE, THUMB_SIZE, 8);

            /* Load and blit thumbnail */
            uint8_t *thumb = thumb_get(idx);
            if (thumb) {
                gfx_blit_rgba(cx, cy, thumb, THUMB_SIZE, THUMB_SIZE,
                              cx, cy, THUMB_SIZE, THUMB_SIZE);
            } else if (app.thumbs[idx].failed) {
                gfx_fill(cx, cy, THUMB_SIZE, THUMB_SIZE, 0xFF200000);
                gfx_text_center(cx + THUMB_SIZE / 2, cy + THUMB_SIZE / 2 - 6,
                                "ERR", COL_ERROR);
            }

            /* Border */
            uint32_t bor = selected ? COL_ACCENT :
                           (hovered ? COL_THUMB_HOV : COL_THUMB_BOR);
            gfx_rect(cx - 1, cy - 1, THUMB_SIZE + 2, THUMB_SIZE + 2, bor);

            /* Label: image ID */
            char id_buf[16];
            itoa(app.lib->entries[idx].image_id, id_buf, 10);
            gfx_text_center(cx + THUMB_SIZE / 2,
                            cy + THUMB_SIZE + 4, id_buf, COL_MUTED);
        }
    }

    /* Scroll bar */
    if (rows_total > visible_rows) {
        int track_h = content_h;
        int thumb_h = track_h * visible_rows / rows_total;
        if (thumb_h < 8) thumb_h = 8;
        int thumb_y = content_y + track_h * first_row / rows_total;
        gfx_fill(content_w - 6, content_y, 6, track_h, COL_PANEL);
        gfx_fill(content_w - 5, thumb_y, 4, thumb_h, COL_BORDER);
    }

    draw_sidebar(app.thumb_sel);
}

/* ============================================================
   Drawing: Image View
   ============================================================ */
static void draw_image_view(void) {
    int content_w = (int)app.screen_w - SIDEBAR_W;
    int content_h = (int)app.screen_h - HEADER_H - FOOTER_H;

    gfx_fill(0, HEADER_H, content_w, content_h, 0xFF111111);

    if (!app.view_img_loaded) {
        gfx_text_center(content_w / 2, HEADER_H + content_h / 2, "No image loaded", COL_MUTED);
        draw_sidebar(app.view_id);
        return;
    }

    /* Draw checker */
    gfx_checker(0, HEADER_H, content_w, content_h, 12);

    int disp_w = (int)(app.view_img.width  * app.zoom);
    int disp_h = (int)(app.view_img.height * app.zoom);
    int img_x  = content_w / 2 - disp_w / 2 + app.pan_x;
    int img_y  = HEADER_H + content_h / 2 - disp_h / 2 + app.pan_y;

    /* Shadow */
    gfx_fill(img_x + 4, img_y + 4, disp_w, disp_h, 0x55000000);

    /* Image */
    gfx_blit_rgba_zoom(img_x, img_y, disp_w, disp_h,
                        app.view_img.pixels,
                        app.view_img.width, app.view_img.height,
                        0, HEADER_H, content_w, content_h);

    /* Border around image */
    gfx_rect(img_x - 1, img_y - 1, disp_w + 2, disp_h + 2, COL_BORDER);

    /* Zoom label */
    char zbuf[32];
    char znum[16];
    int zpct = (int)(app.zoom * 100.0f);
    itoa(zpct, znum, 10);
    memcpy(zbuf, "Zoom: ", 6);
    memcpy(zbuf + 6, znum, strlen(znum) + 1);
    strcat(zbuf, "%");
    gfx_fill(8, HEADER_H + 8, fnt_string_width(app.font, zbuf) + 10, 20, 0xAA000000);
    gfx_text(12, HEADER_H + 13, zbuf, COL_TEXT);

    /* Pixel coord under mouse */
    int rel_mx = app.mouse_x - img_x;
    int rel_my = app.mouse_y - img_y;
    if (disp_w > 0 && disp_h > 0 &&
        rel_mx >= 0 && rel_my >= 0 &&
        rel_mx < disp_w && rel_my < disp_h) {
        int px = rel_mx * (int)app.view_img.width  / disp_w;
        int py = rel_my * (int)app.view_img.height / disp_h;
        const uint8_t *pix = app.view_img.pixels + (py * app.view_img.width + px) * 4;

        char pbuf[64];
        char tmp[16];
        memcpy(pbuf, "Pixel (", 7);
        itoa(px, tmp, 10); strcat(pbuf, tmp); strcat(pbuf, ",");
        itoa(py, tmp, 10); strcat(pbuf, tmp); strcat(pbuf, ") R:");
        itoa(pix[0], tmp, 10); strcat(pbuf, tmp); strcat(pbuf, " G:");
        itoa(pix[1], tmp, 10); strcat(pbuf, tmp); strcat(pbuf, " B:");
        itoa(pix[2], tmp, 10); strcat(pbuf, tmp); strcat(pbuf, " A:");
        itoa(pix[3], tmp, 10); strcat(pbuf, tmp);

        int pw = fnt_string_width(app.font, pbuf) + 10;
        gfx_fill(8, HEADER_H + 32, pw, 20, 0xAA000000);
        gfx_text(12, HEADER_H + 37, pbuf, COL_TEXT);
    }

    draw_sidebar(app.view_id);
}

/* ============================================================
   Main draw dispatch
   ============================================================ */
static void draw_frame(void) {
    gfx_fill(0, 0, (int)app.screen_w, (int)app.screen_h, COL_BG);

    switch (app.view) {
        case VIEW_FILE_BROWSER: draw_browser();    break;
        case VIEW_THUMB_GRID:   draw_thumb_grid(); break;
        case VIEW_IMAGE:        draw_image_view(); break;
    }

    draw_header();
    draw_footer();

    /* Software cursor — drawn last so it's always on top.
     * Use hand cursor when hovering over clickable UI elements,
     * arrow otherwise. Mirrors the paintgfx approach. */
    {
        /* Final clamp — defensive against any accumulated drift */
        if (app.mouse_x < 0) app.mouse_x = 0;
        if (app.mouse_y < 0) app.mouse_y = 0;
        if ((uint32_t)app.mouse_x >= app.screen_w) app.mouse_x = (int32_t)app.screen_w - 1;
        if ((uint32_t)app.mouse_y >= app.screen_h) app.mouse_y = (int32_t)app.screen_h - 1;

        int over_header  = (app.mouse_y < HEADER_H);
        int over_footer  = (app.mouse_y >= (int)app.screen_h - FOOTER_H);
        int over_sidebar = (app.mouse_x >= (int)app.screen_w - SIDEBAR_W &&
                            app.view != VIEW_FILE_BROWSER);

        CursorImg *cur;
        if ((over_header || over_footer || over_sidebar) &&
             app.cur_hand.loaded) {
            cur = &app.cur_hand;
        } else {
            cur = app.cur_arrow.loaded ? &app.cur_arrow : NULL;
        }

        if (cur) cursor_blit(cur, app.mouse_x, app.mouse_y);
    }

    /* Commit to screen */
    NodGL_DrawTexture(app.ctx, app.backbuf_tex, 0, 0, 0, 0,
                      app.screen_w, app.screen_h);
    NodGL_PresentContext(app.ctx, 0);
}

/* ============================================================
   Input handling
   ============================================================ */
static void handle_key(KeyCode kc, char ch) {
    if (app.view == VIEW_FILE_BROWSER) {
        if (kc == KEY_ARROW_UP) {
            if (app.browser_sel > 0) {
                app.browser_sel--;
                if (app.browser_sel < app.browser_scroll)
                    app.browser_scroll = app.browser_sel;
            }
        } else if (kc == KEY_ARROW_DOWN) {
            if (app.browser_sel < app.dir_count - 1) {
                app.browser_sel++;
                int content_h = (int)app.screen_h - HEADER_H - FOOTER_H - 24;
                int visible = content_h / BROWSER_ITEM_H;
                if (app.browser_sel >= app.browser_scroll + visible)
                    app.browser_scroll = app.browser_sel - visible + 1;
            }
        } else if (kc == KEY_ENTER || ch == '\r' || ch == '\n') {
            if (app.browser_sel >= 0 && app.browser_sel < app.dir_count) {
                DirEntry *de = &app.dir_entries[app.browser_sel];
                if (de->is_dir) {
                    char newpath[MAX_PATH];
                    if (strcmp(de->name, "..") == 0) {
                        memcpy(newpath, app.cwd, strlen(app.cwd) + 1);
                        path_parent(newpath);
                    } else {
                        path_join(newpath, sizeof(newpath), app.cwd, de->name);
                    }
                    browser_load_dir(newpath);
                } else {
                    char fullpath[MAX_PATH];
                    path_join(fullpath, sizeof(fullpath), app.cwd, de->name);
                    lib_open_file(fullpath);
                }
            }
        } else if (kc == KEY_ESCAPE) {
            /* already in browser */
        }
    } else if (app.view == VIEW_THUMB_GRID && app.lib) {
        if (kc == KEY_ARROW_UP) {
            /* compute cols */
            int content_w = (int)app.screen_w - SIDEBAR_W;
            int cols = content_w / THUMB_CELL; if (cols < 1) cols = 1;
            if (app.thumb_sel >= cols) app.thumb_sel -= cols;
            else app.thumb_sel = 0;
        } else if (kc == KEY_ARROW_DOWN) {
            int content_w = (int)app.screen_w - SIDEBAR_W;
            int cols = content_w / THUMB_CELL; if (cols < 1) cols = 1;
            if (app.thumb_sel + cols < app.lib->count) app.thumb_sel += cols;
        } else if (kc == KEY_ARROW_LEFT) {
            if (app.thumb_sel > 0) app.thumb_sel--;
        } else if (kc == KEY_ARROW_RIGHT) {
            if (app.thumb_sel < app.lib->count - 1) app.thumb_sel++;
        } else if (kc == KEY_ENTER || ch == '\r' || ch == '\n') {
            if (app.thumb_sel >= 0) view_open_image(app.thumb_sel);
        } else if (kc == KEY_ESCAPE || ch == 0x1b) {
            app.view = VIEW_FILE_BROWSER;
        } else if (ch == '+' || ch == '=') {
            /* preload thumbnail — just advance selection visually */
        } else if (kc == KEY_BACKSPACE || ch == 8 || ch == 127) {
            app.view = VIEW_FILE_BROWSER;
        }
    } else if (app.view == VIEW_IMAGE) {
        int pan_step = 32;
        if (kc == KEY_ARROW_LEFT)  app.pan_x -= pan_step;
        else if (kc == KEY_ARROW_RIGHT) app.pan_x += pan_step;
        else if (kc == KEY_ARROW_UP)    app.pan_y -= pan_step;
        else if (kc == KEY_ARROW_DOWN)  app.pan_y += pan_step;
        else if (ch == '+' || ch == '=') {
            app.zoom += ZOOM_STEP;
            if (app.zoom > ZOOM_MAX) app.zoom = ZOOM_MAX;
        } else if (ch == '-') {
            app.zoom -= ZOOM_STEP;
            if (app.zoom < ZOOM_MIN) app.zoom = ZOOM_MIN;
        } else if (ch == '0') {
            /* Reset to fit */
            int avail_w = (int)app.screen_w - SIDEBAR_W;
            int avail_h = (int)app.screen_h - HEADER_H - FOOTER_H;
            float zx = (float)avail_w / (float)(app.view_img.width  > 0 ? app.view_img.width  : 1);
            float zy = (float)avail_h / (float)(app.view_img.height > 0 ? app.view_img.height : 1);
            app.zoom = zx < zy ? zx : zy;
            if (app.zoom < ZOOM_MIN) app.zoom = ZOOM_MIN;
            if (app.zoom > ZOOM_MAX) app.zoom = ZOOM_MAX;
            app.pan_x = app.pan_y = 0;
        } else if (ch == '1') {
            /* 100% */
            app.zoom = 1.0f; app.pan_x = app.pan_y = 0;
        } else if (ch == 'n' || ch == 'N' || kc == KEY_ARROW_RIGHT) {
            /* next image */
            if (app.lib && app.view_id + 1 < app.lib->count)
                view_open_image(app.view_id + 1);
        } else if ((ch == 'p' || ch == 'P') && app.view_id > 0) {
            view_open_image(app.view_id - 1);
        } else if (kc == KEY_BACKSPACE || ch == 8 || ch == 127) {
            app.view = VIEW_THUMB_GRID;
            set_status("Thumbnail grid — Enter to open, Arrow keys to navigate", COL_MUTED);
        } else if (kc == KEY_ESCAPE || ch == 0x1b) {
            app.view = VIEW_THUMB_GRID;
        }
    }
}

static void handle_mouse_click(int x, int y) {
    /* Header tab clicks */
    if (y < HEADER_H && app.lib) {
        int bx = (int)app.screen_w - 300;
        ViewMode modes[] = { VIEW_FILE_BROWSER, VIEW_THUMB_GRID, VIEW_IMAGE };
        for (int t = 0; t < 3; t++) {
            if (x >= bx && x < bx + 80) {
                if (modes[t] == VIEW_IMAGE && !app.view_img_loaded) {
                    set_status("No image selected — double-click a thumbnail first", COL_WARN);
                } else {
                    app.view = modes[t];
                }
                return;
            }
            bx += 84;
        }
    }

    if (app.view == VIEW_FILE_BROWSER) {
        int content_y = HEADER_H + 24;
        int row = (y - content_y) / BROWSER_ITEM_H;
        int idx = row + app.browser_scroll;
        if (idx >= 0 && idx < app.dir_count) {
            if (idx == app.browser_sel) {
                /* Double-click effect: open */
                DirEntry *de = &app.dir_entries[idx];
                if (de->is_dir) {
                    char newpath[MAX_PATH];
                    if (strcmp(de->name, "..") == 0) {
                        memcpy(newpath, app.cwd, strlen(app.cwd) + 1);
                        path_parent(newpath);
                    } else {
                        path_join(newpath, sizeof(newpath), app.cwd, de->name);
                    }
                    browser_load_dir(newpath);
                } else {
                    char fullpath[MAX_PATH];
                    path_join(fullpath, sizeof(fullpath), app.cwd, de->name);
                    lib_open_file(fullpath);
                }
            } else {
                app.browser_sel = idx;
            }
        }
    } else if (app.view == VIEW_THUMB_GRID && app.lib) {
        int content_w = (int)app.screen_w - SIDEBAR_W;
        int content_y = HEADER_H;
        int cols = content_w / THUMB_CELL; if (cols < 1) cols = 1;

        int col = (x) / THUMB_CELL;
        int row = (y - content_y) / THUMB_CELL + app.thumb_scroll;
        int idx = row * cols + col;

        if (idx >= 0 && idx < app.lib->count) {
            if (idx == app.thumb_sel) {
                /* Second click = open */
                view_open_image(idx);
            } else {
                app.thumb_sel = idx;
            }
        }
    }
}

/* ============================================================
   Entry point
   ============================================================ */
int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    printf("ilib_viewer starting...\n");

    /* Zero app state FIRST before touching any fields */
    memset(&app, 0, sizeof(app));
    app.view         = VIEW_FILE_BROWSER;
    app.thumb_sel    = -1;
    app.thumb_hover  = -1;
    app.browser_sel  = 0;
    app.zoom         = 1.0f;
    app.status_color = COL_MUTED;

    /* Open event device */
    int efd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    if (efd < 0) {
        printf("ilib_viewer: cannot open event device\n");
        sleep(2);
        return 2;
    }

    /* Init graphics — once */
    if (NodGL_CreateDevice(NodGL_FEATURE_LEVEL_1_0,
                           &app.device, &app.ctx, NULL) != NodGL_OK) {
        printf("ilib_viewer: NodGL_CreateDevice failed\n");
        close(efd);
        sleep(2);
        return 1;
    }
    NodGL_GetScreenResolution(app.device, &app.screen_w, &app.screen_h);
    printf("Screen: %ux%u\n", app.screen_w, app.screen_h);

    NodGL_TextureDesc td;
    memset(&td, 0, sizeof(td));
    td.width      = app.screen_w;
    td.height     = app.screen_h;
    td.format     = NodGL_FORMAT_R8G8B8A8_UNORM;
    td.mip_levels = 1;

    if (NodGL_CreateTexture(app.device, &td, &app.backbuf_tex) != NodGL_OK) {
        printf("ilib_viewer: CreateTexture failed\n");
        NodGL_ReleaseDevice(app.device);
        close(efd);
        return 1;
    }
    if (NodGL_MapResource(app.ctx, app.backbuf_tex,
                          (void **)&app.bb, &app.bb_pitch) != NodGL_OK) {
        printf("ilib_viewer: MapResource failed\n");
        NodGL_ReleaseResource(app.device, app.backbuf_tex);
        NodGL_ReleaseDevice(app.device);
        close(efd);
        return 1;
    }

    /* Load font — once */
    int font_fd = open(FONT_PATH, O_RDONLY, 0);
    if (font_fd < 0) {
        printf("ilib_viewer: cannot open font %s\n", FONT_PATH);
        NodGL_ReleaseResource(app.device, app.backbuf_tex);
        NodGL_ReleaseDevice(app.device);
        close(efd);
        sleep(2);
        return 1;
    }
    long font_size = lseek(font_fd, 0, SEEK_END);
    lseek(font_fd, 0, SEEK_SET);
    if (font_size <= 0 || font_size > 8 * 1024 * 1024) {
        printf("ilib_viewer: bad font size %ld\n", font_size);
        close(font_fd);
        NodGL_ReleaseResource(app.device, app.backbuf_tex);
        NodGL_ReleaseDevice(app.device);
        close(efd);
        sleep(2);
        return 1;
    }
    void *font_data = malloc((size_t)font_size);
    if (!font_data) {
        close(font_fd);
        NodGL_ReleaseResource(app.device, app.backbuf_tex);
        NodGL_ReleaseDevice(app.device);
        close(efd);
        return 1;
    }
    size_t fread_total = 0;
    while (fread_total < (size_t)font_size) {
        ssize_t r = read(font_fd, (uint8_t *)font_data + fread_total,
                         (size_t)font_size - fread_total);
        if (r <= 0) break;
        fread_total += (size_t)r;
    }
    close(font_fd);
    app.font = fnt_load_font(font_data, fread_total);
    free(font_data);
    if (!app.font) {
        printf("ilib_viewer: font parse failed\n");
        NodGL_ReleaseResource(app.device, app.backbuf_tex);
        NodGL_ReleaseDevice(app.device);
        close(efd);
        sleep(2);
        return 1;
    }
    printf("Font loaded: %u glyphs\n", app.font->header.glyph_count);

    /* Load software cursors — same paths as paintgfx */
    cursor_load_bmp("/ModuOS/shared/assets/mouse/arrow.bmp", &app.cur_arrow);
    cursor_load_bmp("/ModuOS/shared/assets/mouse/hand.bmp",  &app.cur_hand);
    if (!app.cur_arrow.loaded)
        printf("ilib_viewer: arrow cursor not found (will be invisible)\n");

    /* Load icon library — icons loaded lazily on first use */
    app.icons_lib = ilib_open("/ModuOS/shared/assets/icons.ilib");
    if (app.icons_lib)
        printf("Icons loaded: %u entries\n", app.icons_lib->count);
    else
        printf("ilib_viewer: icons.ilib not found, using text fallbacks\n");

    /* Start mouse in screen centre */
    app.mouse_x = (int32_t)(app.screen_w / 2);
    app.mouse_y = (int32_t)(app.screen_h / 2);

    /* Start browsing from a sensible root */
    browser_load_dir("/ModuOS");

    set_status("Navigate with Arrow keys  Enter to open  ESC to go back", COL_ACCENT);

    printf("Entering main loop...\n");
    int quit = 0;

    while (!quit) {
        Event ev;
        while (read(efd, &ev, sizeof(ev)) > 0) {
            if (ev.type == EVENT_KEY_PRESSED) {
                KeyCode kc = ev.data.keyboard.keycode;
                char ch    = ev.data.keyboard.ascii;

                /* Global ESC from browser exits */
                if (app.view == VIEW_FILE_BROWSER &&
                    (kc == KEY_ESCAPE || ch == 0x1b)) {
                    quit = 1;
                    break;
                }
                handle_key(kc, ch);

            } else if (ev.type == EVENT_MOUSE_MOVE) {
                /* delta_x/delta_y are int16_t — cast explicitly before adding */
                app.mouse_x += (int32_t)ev.data.mouse.delta_x;
                app.mouse_y += (int32_t)ev.data.mouse.delta_y;
                if (app.mouse_x < 0) app.mouse_x = 0;
                if (app.mouse_y < 0) app.mouse_y = 0;
                if ((uint32_t)app.mouse_x >= app.screen_w)
                    app.mouse_x = (int32_t)app.screen_w - 1;
                if ((uint32_t)app.mouse_y >= app.screen_h)
                    app.mouse_y = (int32_t)app.screen_h - 1;
                app.mouse_btn = ev.data.mouse.buttons;

            } else if (ev.type == EVENT_MOUSE_BUTTON) {
                app.mouse_btn_prev = app.mouse_btn;
                app.mouse_btn      = ev.data.mouse.buttons;
                /* Fire click on left-button press (bit 0 newly set) */
                if ((app.mouse_btn & 1) && !(app.mouse_btn_prev & 1)) {
                    handle_mouse_click(app.mouse_x, app.mouse_y);
                }
            }
        }

        if (quit) break;

        draw_frame();
        yield();
    }

    /* Cleanup */
    printf("Cleaning up...\n");
    if (app.icons_lib) {
        for (int i = 0; i < ICON_COUNT; i++)
            if (app.icons_loaded[i] == 1) ilib_free_image(&app.icons[i]);
        ilib_close(app.icons_lib);
    }
    if (app.lib) {
        if (app.thumbs) {
            for (int i = 0; i < app.lib->count; i++) {
                if (app.thumbs[i].pixels) free(app.thumbs[i].pixels);
            }
            free(app.thumbs);
        }
        ilib_close(app.lib);
    }
    if (app.view_img_loaded) ilib_free_image(&app.view_img);
    if (app.font)            fnt_free_font(app.font);
    if (app.bb)              NodGL_UnmapResource(app.ctx, app.backbuf_tex);
    NodGL_ReleaseResource(app.device, app.backbuf_tex);
    NodGL_ReleaseDevice(app.device);
    close(efd);
    input_flush();

    return 0;
}