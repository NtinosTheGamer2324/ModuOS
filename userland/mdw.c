/*
 * mdw.c — ModuOS Windows (MDW): the window manager / display manager
 *
 * A software compositor: the WM owns the screen framebuffer (via NodGL)
 * and blits every window's pixel buffer into it every frame, back-to-front
 * by z-order, on top of which it draws chrome (title bars, close buttons,
 * focus highlight), a taskbar, and the mouse cursor.
 *
 * Windows are backed by real shared memory. A client talks to the control
 * service at $/user/wm (same invoke() shape as calc_service.c):
 *   WM_CMD_CREATE  -> WM shm_open()s+mmap()s a fresh segment, hands the
 *                      client its name+size to shm_open()+mmap() themselves
 *   WM_CMD_DAMAGE  -> client tells us exactly which sub-rect it redrew;
 *                      we translate that straight into the dirty-rect
 *                      system instead of assuming "the whole window changed"
 *   WM_CMD_MOVE    -> rarely needed (users normally drag), but available
 *   WM_CMD_CLOSE   -> munmap + shm_unlink on our side
 * See wm_protocol.h for the wire format.
 *
 * No GPU: every pixel is a CPU store through the framebuffer, so the
 * compositor is built around a single per-frame damage rect — see the
 * "Damage tracking" section below for how that works and why.
 *
 * Controls:
 *   Left click + drag on a title bar  — move window
 *   Left click on close button (X)    — close window
 *   Left click anywhere on a window   — focus + raise it
 *   Left click on a taskbar entry     — focus + raise that window
 *   Tab                               — cycle focus
 *   Q                                 — quit the compositor (dev/testing)
 *
 * Copyright © 2026 New Technologies Software — GPL v2.0
 */

#include "libc.h"
#include "NodGL.h"
#include "ilib.h"
#include "lib_fnt.h"
#include "wm_protocol.h"
#include "../include/moduos/kernel/events/events.h"

/* ============================================================
   Assets
   ============================================================ */
#define FONT_PRIMARY  "/ModuOS/shared/assets/fonts/Terminus.fnt"
#define FONT_FALLBACK "/ModuOS/shared/assets/fonts/Unicode.fnt"
#define MOUSE_ILIB    "/ModuOS/shared/assets/mouse.ilib"
#define ICONS_ILIB    "/ModuOS/shared/assets/icons.ilib"

/* Cursor IDs (mouse.ilib) */
#define CURSOR_POINTER    0
#define CURSOR_CROSS      1
#define CURSOR_ERASER     2
#define CURSOR_HAND       3
#define CURSOR_MOVE_ALL   4  /* cross with arrows on all sides */
#define CURSOR_NO         5
#define CURSOR_PENCIL     6
#define CURSOR_TEXT       7
#define CURSOR_WAIT       8
#define CURSOR_COUNT      9

/* Icon IDs (icons.ilib) */
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
#define ICON_SIZE             16

/* ============================================================
   Layout constants
   ============================================================ */
#define WM_MAX_WINDOWS     16
#define WM_TITLEBAR_H      24
#define WM_CLOSE_BTN_SIZE  16
#define WM_CLOSE_BTN_MARGIN 4
#define WM_TASKBAR_H       28
#define WM_TASKBAR_SLOT_W  140

/* Colors (0xAARRGGBB, matches fill()/blend() convention) */
#define COL_DESKTOP        0xFF14181F
#define COL_TITLEBAR_FOCUS 0xFF2F6FE0
#define COL_TITLEBAR_BLUR  0xFF33394A
#define COL_TITLE_TEXT     0xFFF2F5FA
#define COL_CLOSE_BG       0xFFE0483F
#define COL_CLOSE_BG_BLUR  0xFF4A3234
#define COL_CLOSE_X        0xFFF2F5FA
#define COL_BORDER_FOCUS   0xFF2F6FE0
#define COL_BORDER_BLUR    0xFF20242E
#define COL_TASKBAR_BG     0xFF0B0E13
#define COL_TASKBAR_SLOT   0xFF1B2130
#define COL_TASKBAR_SLOT_F 0xFF2F6FE0
#define COL_TASKBAR_TEXT   0xFFDCE2EE

/* ============================================================
   Window
   ============================================================ */
typedef enum { HIT_NONE, HIT_TITLEBAR, HIT_CLOSE, HIT_CONTENT } wm_hit_region_t;

typedef struct {
    int       active;
    int       x, y;          /* top-left of the WHOLE window, incl. titlebar */
    int       w, h;          /* content width/height (excludes titlebar)     */
    char      title[64];
    uint32_t *buf;           /* WM's own mapping of the client's SHM segment */
    char      shm_name[64];  /* name to shm_unlink() on close                */
    uint64_t  shm_size;      /* size to munmap() on close                    */
    int       content_cursor;/* cursor to show while hovering this window's content */
    int       opaque;        /* 1 = every pixel has a=255, lets the blitter memcpy whole
                                 rows instead of alpha-testing pixel by pixel. Driven by
                                 the client's WM_FLAG_HAS_ALPHA at creation time. */
    uint32_t  owner_pid;      /* self-reported by the client at CREATE; 0 = not tracked.
                                  Used only to reap windows whose process has actually
                                  died — see wm_reap_dead_clients(). */
} wm_window_t;

/* No GPU here — every pixel we touch is a CPU store through the LFB, so the
 * whole compositor is built around never touching a pixel that didn't
 * change. We track ONE bounding "damage rect" per frame (cheaper than a
 * real rect-list for this window count) and:
 *   - clear/redraw only that rect, not the whole screen
 *   - skip any window that doesn't overlap it
 *   - upload only that rect to the screen texture
 *   - if nothing was marked dirty, skip drawing AND presenting entirely
 * That last point is the big one: most loop iterations nothing moved, and
 * on a machine with no blitter/GPU, a redundant full-screen redraw is pure
 * wasted wall-clock time that starves everything else in the system.
 */
#define WM_CURSOR_DIRTY_DIM 48 /* conservative upper bound on any cursor sprite's footprint */

/* ============================================================
   Global state
   ============================================================ */
static struct {
    NodGL_Device  device;
    NodGL_Context ctx;
    NodGL_Texture screen_tex;
    uint8_t      *bb;
    uint32_t      bb_pitch;
    uint32_t      sw, sh;

    fnt_font_t   *font;

    ilib_t       *cursor_lib;
    ilib_image_t  cursors[CURSOR_COUNT];
    int           cursor_loaded[CURSOR_COUNT];

    ilib_t       *icon_lib;
    ilib_image_t  icons[ICON_COUNT];
    int           icon_loaded[ICON_COUNT];

    wm_window_t   windows[WM_MAX_WINDOWS];
    int           zorder[WM_MAX_WINDOWS]; /* window slot indices, back -> front */
    int           zcount;
    int           focused;                /* window slot index, -1 = none */
    uint32_t      next_shm_id;            /* monotonic, for unique shm segment names */
    uint64_t      last_reap_check_ms;      /* throttle for wm_reap_dead_clients() */

    int      mouse_x, mouse_y;
    uint8_t  prev_buttons;
    int      cursor_id;

    int      dragging;
    int      drag_win;
    int      drag_dx, drag_dy;

    /* Damage tracking: single bounding dirty rect, [x0,y0)-[x1,y1). */
    int      dirty_valid;
    int      dirty_x0, dirty_y0, dirty_x1, dirty_y1;

    int      quit;
} g;

/* ============================================================
   Damage tracking
   ============================================================ */

/* Clip [ax0,ay0)-[ax1,ay1) against [bx0,by0)-[bx1,by1); returns 0 if empty. */
static inline int rect_intersect(int ax0, int ay0, int ax1, int ay1,
                                  int bx0, int by0, int bx1, int by1,
                                  int *ox0, int *oy0, int *ox1, int *oy1) {
    int x0 = ax0 > bx0 ? ax0 : bx0, y0 = ay0 > by0 ? ay0 : by0;
    int x1 = ax1 < bx1 ? ax1 : bx1, y1 = ay1 < by1 ? ay1 : by1;
    if (x0 >= x1 || y0 >= y1) return 0;
    *ox0 = x0; *oy0 = y0; *ox1 = x1; *oy1 = y1;
    return 1;
}

/* Union a screen-space rect into this frame's damage. Clamped to the screen. */
static void mark_dirty(int x, int y, int w, int h) {
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)g.sw) x1 = (int)g.sw;
    if (y1 > (int)g.sh) y1 = (int)g.sh;
    if (x0 >= x1 || y0 >= y1) return;

    if (!g.dirty_valid) {
        g.dirty_x0 = x0; g.dirty_y0 = y0; g.dirty_x1 = x1; g.dirty_y1 = y1;
        g.dirty_valid = 1;
        return;
    }
    if (x0 < g.dirty_x0) g.dirty_x0 = x0;
    if (y0 < g.dirty_y0) g.dirty_y0 = y0;
    if (x1 > g.dirty_x1) g.dirty_x1 = x1;
    if (y1 > g.dirty_y1) g.dirty_y1 = y1;
}

/* A window's outer rect (titlebar+content+border) changed -> mark it. Since
 * the compositor always redraws every window intersecting the dirty rect,
 * in correct z-order, marking just the changed window's own rect is enough
 * to also repaint whatever it now covers or uncovers underneath it. */
static void mark_window_dirty(int slot) {
    if (slot < 0 || slot >= WM_MAX_WINDOWS || !g.windows[slot].active) return;
    wm_window_t *win = &g.windows[slot];
    mark_dirty(win->x, win->y, win->w, WM_TITLEBAR_H + win->h);
}

static void mark_cursor_dirty(int cx, int cy) {
    mark_dirty(cx, cy, WM_CURSOR_DIRTY_DIM, WM_CURSOR_DIRTY_DIM);
}

/* ============================================================
   Pixel helpers (backbuffer space)
   ============================================================ */
static void fill(int x, int y, int w, int h, uint32_t col) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > (int)g.sw) x1 = (int)g.sw;
    int y1 = y + h; if (y1 > (int)g.sh) y1 = (int)g.sh;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = (uint32_t *)(g.bb + (uint64_t)yy * g.bb_pitch);
        for (int xx = x0; xx < x1; xx++) row[xx] = col;
    }
}

static inline uint32_t div255(uint32_t x) { return (x + 1 + (x >> 8)) >> 8; }

static inline void blend(int x, int y, uint8_t r, uint8_t gv, uint8_t b, uint8_t a) {
    if ((uint32_t)x >= g.sw || (uint32_t)y >= g.sh || !g.bb || a == 0) return;
    uint32_t *d = (uint32_t *)(g.bb + (uint64_t)y * g.bb_pitch) + x;
    if (a == 255) { *d = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)gv << 8) | b; return; }
    uint32_t bg = *d; uint32_t ia = 255 - a;
    *d = 0xFF000000u
       | (div255(r * a + ((bg >> 16) & 0xFF) * ia) << 16)
       | (div255(gv * a + ((bg >> 8) & 0xFF) * ia) << 8)
       | (div255(b * a + (bg & 0xFF) * ia));
}

static void draw_rect_outline(int x, int y, int w, int h, uint32_t col) {
    uint8_t a = (uint8_t)(col >> 24), r = (uint8_t)(col >> 16), gv = (uint8_t)(col >> 8), b = (uint8_t)col;
    for (int xx = x; xx < x + w; xx++) { blend(xx, y, r, gv, b, a); blend(xx, y + h - 1, r, gv, b, a); }
    for (int yy = y; yy < y + h; yy++) { blend(x, yy, r, gv, b, a); blend(x + w - 1, yy, r, gv, b, a); }
}

/* ============================================================
   Text
   ============================================================ */
static void draw_text_clip(int x, int y, int clip_x0, int clip_x1, const char *s, uint32_t col) {
    if (!g.font || !s) return;
    int cx = x;
    uint8_t r = (col >> 16) & 0xFF, gv = (col >> 8) & 0xFF, b = col & 0xFF;
    while (*s) {
        fnt_glyph_t *gl = fnt_get_glyph(g.font, (uint32_t)(unsigned char)*s);
        if (gl) {
            if (cx >= clip_x0 && cx + gl->bitmap_width <= clip_x1) {
                for (int dy = 0; dy < gl->bitmap_height; dy++)
                    for (int dx = 0; dx < gl->bitmap_width; dx++)
                        if (fnt_get_pixel(gl, dx, dy))
                            blend(cx + dx, y + dy, r, gv, b, 255);
            } else if (cx < clip_x1) {
                for (int dy = 0; dy < gl->bitmap_height; dy++)
                    for (int dx = 0; dx < gl->bitmap_width; dx++)
                        if (cx + dx >= clip_x0 && cx + dx < clip_x1 && fnt_get_pixel(gl, dx, dy))
                            blend(cx + dx, y + dy, r, gv, b, 255);
            }
            cx += gl->width;
        }
        s++;
    }
}

/* ============================================================
   Image blit (ilib RGBA source -> backbuffer, alpha blended)
   ============================================================ */
static void draw_image_alpha(int dst_x, int dst_y, const ilib_image_t *img) {
    if (!img || !img->pixels) return;
    for (int y = 0; y < img->height; y++) {
        const uint8_t *row = img->pixels + (size_t)y * img->width * 4;
        for (int x = 0; x < img->width; x++) {
            const uint8_t *p = row + x * 4;
            uint8_t a = p[3];
            if (a) blend(dst_x + x, dst_y + y, p[0], p[1], p[2], a);
        }
    }
}

/* Window content buffer -> backbuffer, clipped to [rx0,ry0)-[rx1,ry1).
 * Opaque windows (the common case) take a straight memcpy-per-row path —
 * no per-pixel branch, no alpha math, just moving bytes. Non-opaque windows
 * fall back to per-pixel blend, still bounded to the clip rect. */
static void blit_window_content_region(const wm_window_t *win, int rx0, int ry0, int rx1, int ry1) {
    int content_y0 = win->y + WM_TITLEBAR_H;
    int content_y1 = content_y0 + win->h;
    int cx0, cy0, cx1, cy1;
    if (!rect_intersect(win->x, content_y0, win->x + win->w, content_y1,
                         rx0, ry0, rx1, ry1, &cx0, &cy0, &cx1, &cy1))
        return;

    int row_w = cx1 - cx0;
    int src_x0 = cx0 - win->x;

    if (win->opaque) {
        for (int dy = cy0; dy < cy1; dy++) {
            int sy = dy - content_y0;
            const uint32_t *src = win->buf + (size_t)sy * win->w + src_x0;
            uint32_t *dst = (uint32_t *)(g.bb + (uint64_t)dy * g.bb_pitch) + cx0;
            memcpy(dst, src, (size_t)row_w * 4);
        }
        return;
    }

    for (int dy = cy0; dy < cy1; dy++) {
        int sy = dy - content_y0;
        const uint32_t *src = win->buf + (size_t)sy * win->w + src_x0;
        for (int i = 0; i < row_w; i++) {
            uint32_t px = src[i];
            uint8_t a = (uint8_t)(px >> 24);
            if (!a) continue;
            blend(cx0 + i, dy, (uint8_t)(px >> 16), (uint8_t)(px >> 8), (uint8_t)px, a);
        }
    }
}

/* ============================================================
   Lazy asset loading
   ============================================================ */
static const ilib_image_t *wm_get_cursor(int id) {
    if (id < 0 || id >= CURSOR_COUNT || !g.cursor_lib) return NULL;
    if (!g.cursor_loaded[id]) {
        if (ilib_load_image(g.cursor_lib, (uint16_t)id, &g.cursors[id]) == ILIB_OK)
            g.cursor_loaded[id] = 1;
        else
            return NULL;
    }
    return &g.cursors[id];
}

static const ilib_image_t *wm_get_icon(int id) {
    if (id < 0 || id >= ICON_COUNT || !g.icon_lib) return NULL;
    if (!g.icon_loaded[id]) {
        if (ilib_load_image(g.icon_lib, (uint16_t)id, &g.icons[id]) == ILIB_OK)
            g.icon_loaded[id] = 1;
        else
            return NULL;
    }
    return &g.icons[id];
}

static fnt_font_t *try_load_font(const char *path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return NULL;
    long fsz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    fnt_font_t *out = NULL;
    if (fsz > 0 && fsz < 4 * 1024 * 1024) {
        void *data = malloc((size_t)fsz);
        if (data) {
            size_t got = 0;
            while (got < (size_t)fsz) {
                ssize_t r = read(fd, (uint8_t *)data + got, (size_t)fsz - got);
                if (r <= 0) break;
                got += (size_t)r;
            }
            if (got == (size_t)fsz) out = fnt_load_font(data, got);
            free(data);
        }
    }
    close(fd);
    return out;
}

/* ============================================================
   Window management (SHM-backed)
   ============================================================ */

/* Builds a unique segment name without touching sprintf's format engine —
 * keeps this hot-ish path (window creation) free of any dependency on
 * which conversions the libc snprintf actually implements. */
static void build_shm_name(char *out, size_t out_size, uint32_t id) {
    char digits[16];
    int nd = 0;
    if (id == 0) { digits[nd++] = '0'; }
    while (id > 0 && nd < (int)sizeof(digits)) { digits[nd++] = (char)('0' + (id % 10)); id /= 10; }

    const char *prefix = "wm_win_";
    size_t p = 0;
    while (prefix[p] && p + 1 < out_size) { out[p] = prefix[p]; p++; }
    for (int i = nd - 1; i >= 0 && p + 1 < out_size; i--) out[p++] = digits[i];
    out[p] = 0;
}

static void copy_str(char *dst, size_t dst_size, const char *src) {
    if (!dst_size) return;
    size_t i = 0;
    if (src) while (src[i] && i + 1 < dst_size) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* Allocates a window slot, creates its backing SHM segment, and maps it
 * into the WM's own address space so the compositor can read it. On
 * success fills resp->shm_name/shm_size for the client to shm_open()
 * (no SHM_O_CREAT on their end — the segment already exists) and mmap(). */
static int wm_service_create_window(const char *title, uint32_t w, uint32_t h,
                                     uint32_t flags, uint32_t owner_pid, wm_response_t *resp) {
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return -1;

    int slot = -1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) if (!g.windows[i].active) { slot = i; break; }
    if (slot < 0) return -1;

    wm_window_t *win = &g.windows[slot];
    memset(win, 0, sizeof(*win));

    build_shm_name(win->shm_name, sizeof(win->shm_name), g.next_shm_id++);
    uint64_t size = (uint64_t)w * (uint64_t)h * 4;

    int shm_h = shm_open(win->shm_name, O_RDWR | SHM_O_CREAT | SHM_O_EXCL, 0600, size);
    if (shm_h < 0) return -1;

    void *ptr = mmap(NULL, (size_t)size, PROT_R | PROT_W, MAP_SHARED, shm_h);
    if (ptr == MAP_FAILED) { shm_unlink(win->shm_name); return -1; }

    win->active = 1;
    /* simple cascade so new windows don't all land exactly on top of each other */
    win->x = 60 + (slot % 6) * 24;
    win->y = 40 + (slot % 6) * 24;
    win->w = (int)w; win->h = (int)h;
    win->buf = (uint32_t *)ptr;
    win->shm_size = size;
    win->content_cursor = CURSOR_POINTER;
    win->opaque = (flags & WM_FLAG_HAS_ALPHA) ? 0 : 1;
    win->owner_pid = owner_pid;
    copy_str(win->title, sizeof(win->title), title);

    g.zorder[g.zcount++] = slot;
    g.focused = slot;
    mark_window_dirty(slot);

    if (resp) {
        copy_str(resp->shm_name, sizeof(resp->shm_name), win->shm_name);
        resp->shm_size = size;
    }
    return slot;
}

static void wm_close_window(int slot) {
    if (slot < 0 || slot >= WM_MAX_WINDOWS || !g.windows[slot].active) return;
    mark_window_dirty(slot); /* capture the rect before it's torn down */
    wm_window_t *win = &g.windows[slot];
    if (win->buf) munmap(win->buf, (size_t)win->shm_size);
    if (win->shm_name[0]) shm_unlink(win->shm_name); /* client's own mapping (if any) stays
                                                          valid until it munmaps too */
    win->active = 0;

    int w = 0;
    for (int i = 0; i < g.zcount; i++) if (g.zorder[i] != slot) g.zorder[w++] = g.zorder[i];
    g.zcount = w;

    if (g.focused == slot)
        g.focused = (g.zcount > 0) ? g.zorder[g.zcount - 1] : -1;
}

/* Periodic liveness sweep: close windows whose owning process is gone.
 * Deliberately NOT "hasn't sent damage recently" — a legitimately idle
 * window (static content, nothing to redraw) looks identical to a dead
 * one under that signal, and we'd auto-close things that are working
 * fine. Process existence is the actual thing we care about, so that's
 * what we check, via the same md64api_get_pid_info the process list
 * uses. Throttled to once a second — this is a cheap syscall per window,
 * but there's no reason to pay it every loop iteration. */
#define WM_REAP_INTERVAL_MS 1000

static void wm_reap_dead_clients(void) {
    uint64_t now = time_ms();
    if (now - g.last_reap_check_ms < WM_REAP_INTERVAL_MS) return;
    g.last_reap_check_ms = now;

    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!g.windows[i].active || g.windows[i].owner_pid == 0) continue;
        md64api_pid_info_u info;
        if (md64api_get_pid_info(g.windows[i].owner_pid, &info) != 0) {
            /* pid lookup failed -> process is gone -> reclaim the window */
            wm_close_window(i);
        }
    }
}

static void wm_bring_to_front(int slot) {
    int idx = -1;
    for (int i = 0; i < g.zcount; i++) if (g.zorder[i] == slot) { idx = i; break; }
    if (idx < 0) return;
    int old_focused = g.focused;
    for (int i = idx; i < g.zcount - 1; i++) g.zorder[i] = g.zorder[i + 1];
    g.zorder[g.zcount - 1] = slot;
    g.focused = slot;
    mark_window_dirty(slot);                      /* stacking order changed under it */
    if (old_focused != slot) mark_window_dirty(old_focused); /* its border/titlebar dims */
}

static void wm_cycle_focus(void) {
    if (g.zcount == 0) return;
    int old_focused = g.focused;
    int slot = g.zorder[0];
    for (int i = 1; i < g.zcount; i++) g.zorder[i - 1] = g.zorder[i];
    g.zorder[g.zcount - 1] = slot;
    g.focused = g.zorder[g.zcount - 1];
    mark_window_dirty(g.focused);
    if (old_focused != g.focused) mark_window_dirty(old_focused);
}

/* Topmost window under (px,py), plus which region was hit. -1 if none. */
static int wm_window_at(int px, int py, wm_hit_region_t *out_region) {
    for (int i = g.zcount - 1; i >= 0; i--) {
        int slot = g.zorder[i];
        wm_window_t *win = &g.windows[slot];
        int x0 = win->x, x1 = win->x + win->w;
        int y0 = win->y, y1 = win->y + WM_TITLEBAR_H + win->h;
        if (px < x0 || px >= x1 || py < y0 || py >= y1) continue;

        if (py < win->y + WM_TITLEBAR_H) {
            int cbx0 = win->x + win->w - WM_CLOSE_BTN_SIZE - WM_CLOSE_BTN_MARGIN;
            int cby0 = win->y + (WM_TITLEBAR_H - WM_CLOSE_BTN_SIZE) / 2;
            if (px >= cbx0 && px < cbx0 + WM_CLOSE_BTN_SIZE &&
                py >= cby0 && py < cby0 + WM_CLOSE_BTN_SIZE) {
                if (out_region) *out_region = HIT_CLOSE;
            } else {
                if (out_region) *out_region = HIT_TITLEBAR;
            }
        } else {
            if (out_region) *out_region = HIT_CONTENT;
        }
        return slot;
    }
    if (out_region) *out_region = HIT_NONE;
    return -1;
}

/* ============================================================
   Control service ($/user/wm) — same invoke() shape as calc_service.c.
   This is what actually drives window creation now: clients call in with
   WM_CMD_CREATE/DAMAGE/CLOSE, we never create windows ourselves.
   ============================================================ */
static ssize_t wm_service_invoke(void *ctx, const void *in_buf, size_t in_size,
                                  void *out_buf, size_t out_size) {
    (void)ctx;
    if (in_size != sizeof(wm_request_t) || out_size != sizeof(wm_response_t)) return -1;
    const wm_request_t *req = (const wm_request_t *)in_buf;
    wm_response_t *resp = (wm_response_t *)out_buf;
    memset(resp, 0, sizeof(*resp));

    switch (req->cmd) {
        case WM_CMD_CREATE: {
            int slot = wm_service_create_window(req->title, req->w, req->h, req->flags, req->pid, resp);
            if (slot < 0) {
                copy_str(resp->message, sizeof(resp->message), "create failed (bad size or no free slot/shm)");
                return -1;
            }
            resp->success = 1;
            resp->win_id = slot;
            return (ssize_t)sizeof(*resp);
        }
        case WM_CMD_DAMAGE: {
            if (req->win_id < 0 || req->win_id >= WM_MAX_WINDOWS || !g.windows[req->win_id].active) {
                copy_str(resp->message, sizeof(resp->message), "bad win_id");
                return -1;
            }
            wm_window_t *win = &g.windows[req->win_id];
            int cx0 = req->x, cy0 = req->y;
            int cx1 = req->x + (int)req->w, cy1 = req->y + (int)req->h;
            if (cx0 < 0) cx0 = 0;
            if (cy0 < 0) cy0 = 0;
            if (cx1 > win->w) cx1 = win->w;
            if (cy1 > win->h) cy1 = win->h;
            if (cx1 > cx0 && cy1 > cy0)
                mark_dirty(win->x + cx0, win->y + WM_TITLEBAR_H + cy0, cx1 - cx0, cy1 - cy0);
            resp->success = 1;
            resp->win_id = req->win_id;
            return (ssize_t)sizeof(*resp);
        }
        case WM_CMD_MOVE: {
            if (req->win_id < 0 || req->win_id >= WM_MAX_WINDOWS || !g.windows[req->win_id].active) {
                copy_str(resp->message, sizeof(resp->message), "bad win_id");
                return -1;
            }
            mark_window_dirty(req->win_id); /* old position */
            g.windows[req->win_id].x = req->x;
            g.windows[req->win_id].y = req->y;
            mark_window_dirty(req->win_id); /* new position */
            resp->success = 1;
            resp->win_id = req->win_id;
            return (ssize_t)sizeof(*resp);
        }
        case WM_CMD_CLOSE: {
            if (req->win_id < 0 || req->win_id >= WM_MAX_WINDOWS || !g.windows[req->win_id].active) {
                copy_str(resp->message, sizeof(resp->message), "bad win_id");
                return -1;
            }
            wm_close_window(req->win_id);
            resp->success = 1;
            return (ssize_t)sizeof(*resp);
        }
        default:
            copy_str(resp->message, sizeof(resp->message), "unknown cmd");
            return -1;
    }
}

/* ============================================================
   Chrome (title bar, close button, border) + taskbar
   ============================================================ */
static void draw_window_chrome(int slot) {
    wm_window_t *win = &g.windows[slot];
    int focused = (slot == g.focused);
    uint32_t tb_col = focused ? COL_TITLEBAR_FOCUS : COL_TITLEBAR_BLUR;
    uint32_t border_col = focused ? COL_BORDER_FOCUS : COL_BORDER_BLUR;

    fill(win->x, win->y, win->w, WM_TITLEBAR_H, tb_col);

    int cbx0 = win->x + win->w - WM_CLOSE_BTN_SIZE - WM_CLOSE_BTN_MARGIN;
    int cby0 = win->y + (WM_TITLEBAR_H - WM_CLOSE_BTN_SIZE) / 2;
    fill(cbx0, cby0, WM_CLOSE_BTN_SIZE, WM_CLOSE_BTN_SIZE, focused ? COL_CLOSE_BG : COL_CLOSE_BG_BLUR);
    for (int i = 2; i < WM_CLOSE_BTN_SIZE - 2; i++) {
        blend(cbx0 + i, cby0 + i, 0xF2, 0xF5, 0xFA, 255);
        blend(cbx0 + i, cby0 + WM_CLOSE_BTN_SIZE - 1 - i, 0xF2, 0xF5, 0xFA, 255);
    }

    int title_clip_x1 = cbx0 - 6;
    draw_text_clip(win->x + 8, win->y + (WM_TITLEBAR_H - 8) / 2, win->x + 8, title_clip_x1, win->title, COL_TITLE_TEXT);

    draw_rect_outline(win->x, win->y, win->w, WM_TITLEBAR_H + win->h, border_col);
}

static void draw_taskbar(void) {
    int ty = (int)g.sh - WM_TASKBAR_H;
    fill(0, ty, (int)g.sw, WM_TASKBAR_H, COL_TASKBAR_BG);

    const ilib_image_t *start_icon = wm_get_icon(ICON_THIS_PC);
    if (start_icon) draw_image_alpha(6, ty + (WM_TASKBAR_H - ICON_SIZE) / 2, start_icon);

    int slot_x = 32;
    for (int i = 0; i < g.zcount; i++) {
        int slot = g.zorder[i];
        wm_window_t *win = &g.windows[slot];
        int focused = (slot == g.focused);
        fill(slot_x, ty + 2, WM_TASKBAR_SLOT_W - 4, WM_TASKBAR_H - 4, focused ? COL_TASKBAR_SLOT_F : COL_TASKBAR_SLOT);
        const ilib_image_t *icon = wm_get_icon(ICON_UNKNOWN_FILE);
        int tx = slot_x + 4;
        if (icon) { draw_image_alpha(tx, ty + (WM_TASKBAR_H - ICON_SIZE) / 2, icon); tx += ICON_SIZE + 6; }
        draw_text_clip(tx, ty + (WM_TASKBAR_H - 8) / 2, tx, slot_x + WM_TASKBAR_SLOT_W - 6, win->title, COL_TASKBAR_TEXT);
        slot_x += WM_TASKBAR_SLOT_W;
    }
}

/* Taskbar hit test: returns window slot for the entry under (px,py), or -1 */
static int wm_taskbar_hit(int px, int py) {
    int ty = (int)g.sh - WM_TASKBAR_H;
    if (py < ty) return -1;
    if (px < 32) return -1; /* start button area: no action wired up yet */
    int idx = (px - 32) / WM_TASKBAR_SLOT_W;
    if (idx < 0 || idx >= g.zcount) return -1;
    return g.zorder[idx];
}

/* ============================================================
   Frame composition — redraws ONLY [rx0,ry0)-[rx1,ry1). This is the whole
   performance story: no GPU means every pixel is a CPU-bound LFB store, so
   the compositor never walks a pixel outside the rect that actually changed.
   ============================================================ */
static void compose_dirty_region(int rx0, int ry0, int rx1, int ry1) {
    fill(rx0, ry0, rx1 - rx0, ry1 - ry0, COL_DESKTOP);

    for (int i = 0; i < g.zcount; i++) {
        int slot = g.zorder[i];
        wm_window_t *win = &g.windows[slot];
        int ix0, iy0, ix1, iy1;
        int outer_y1 = win->y + WM_TITLEBAR_H + win->h;
        if (!rect_intersect(win->x, win->y, win->x + win->w, outer_y1,
                             rx0, ry0, rx1, ry1, &ix0, &iy0, &ix1, &iy1))
            continue; /* window doesn't touch the damaged area at all — skip it entirely */

        blit_window_content_region(win, rx0, ry0, rx1, ry1);
        draw_window_chrome(slot); /* cheap (titlebar-height strip + border); no need to sub-clip */
    }

    int tx0, ty0, tx1, ty1;
    if (rect_intersect(0, (int)g.sh - WM_TASKBAR_H, (int)g.sw, (int)g.sh, rx0, ry0, rx1, ry1, &tx0, &ty0, &tx1, &ty1))
        draw_taskbar();

    const ilib_image_t *cur = wm_get_cursor(g.cursor_id);
    if (cur) draw_image_alpha(g.mouse_x, g.mouse_y, cur);
    else     fill(g.mouse_x, g.mouse_y, 4, 4, 0xFFFFFFFFu); /* fallback if cursor art missing */
}

/* ============================================================
   Input handling
   ============================================================ */
static void wm_clamp_mouse(void) {
    if (g.mouse_x < 0) g.mouse_x = 0;
    if (g.mouse_y < 0) g.mouse_y = 0;
    if (g.mouse_x >= (int)g.sw) g.mouse_x = (int)g.sw - 1;
    if (g.mouse_y >= (int)g.sh) g.mouse_y = (int)g.sh - 1;
}

static void wm_update_hover_cursor(void) {
    if (g.dragging) { g.cursor_id = CURSOR_MOVE_ALL; return; }

    wm_hit_region_t region;
    int slot = wm_window_at(g.mouse_x, g.mouse_y, &region);
    if (slot >= 0) {
        if (region == HIT_TITLEBAR || region == HIT_CLOSE) { g.cursor_id = CURSOR_HAND; return; }
        if (region == HIT_CONTENT) { g.cursor_id = g.windows[slot].content_cursor; return; }
    }
    if (wm_taskbar_hit(g.mouse_x, g.mouse_y) >= 0) { g.cursor_id = CURSOR_HAND; return; }
    g.cursor_id = CURSOR_POINTER;
}

static void wm_handle_mouse_move(const MouseEventData *m) {
    mark_cursor_dirty(g.mouse_x, g.mouse_y); /* erase the old cursor footprint */

    g.mouse_x = m->x;
    g.mouse_y = m->y;
    wm_clamp_mouse();

    if (g.dragging) {
        wm_window_t *win = &g.windows[g.drag_win];
        mark_window_dirty(g.drag_win); /* old position */

        int nx = g.mouse_x - g.drag_dx;
        int ny = g.mouse_y - g.drag_dy;
        int max_x = (int)g.sw - win->w;
        int max_y = (int)g.sh - WM_TASKBAR_H - WM_TITLEBAR_H - win->h;
        if (max_x < 0) max_x = 0;
        if (max_y < 0) max_y = 0;
        if (nx < 0) nx = 0;
        if (nx > max_x) nx = max_x;
        if (ny < 0) ny = 0;
        if (ny > max_y) ny = max_y;
        win->x = nx; win->y = ny;

        mark_window_dirty(g.drag_win); /* new position */
    }
    wm_update_hover_cursor();
    mark_cursor_dirty(g.mouse_x, g.mouse_y); /* draw the new cursor footprint */
}

static void wm_handle_mouse_button(const MouseEventData *m) {
    uint8_t pressed  = (uint8_t)(~g.prev_buttons & m->buttons);
    uint8_t released = (uint8_t)(g.prev_buttons & ~m->buttons);
    g.prev_buttons = m->buttons;

    int left_down = pressed & 0x1;
    int left_up   = released & 0x1;

    if (left_down) {
        wm_hit_region_t region;
        int slot = wm_window_at(g.mouse_x, g.mouse_y, &region);
        if (slot >= 0) {
            wm_bring_to_front(slot);
            if (region == HIT_CLOSE) {
                wm_close_window(slot);
            } else if (region == HIT_TITLEBAR) {
                g.dragging = 1;
                g.drag_win = slot;
                g.drag_dx = g.mouse_x - g.windows[slot].x;
                g.drag_dy = g.mouse_y - g.windows[slot].y;
            }
        } else {
            int tb_slot = wm_taskbar_hit(g.mouse_x, g.mouse_y);
            if (tb_slot >= 0) wm_bring_to_front(tb_slot);
        }
    }
    if (left_up && g.dragging) g.dragging = 0;

    wm_update_hover_cursor();
}

static void wm_handle_key(const KeyboardEventData *k) {
    if (k->ascii == 'q' || k->ascii == 'Q') {
        g.quit = 1; /* compositor-level quit, for development only */
    } else if (k->keycode == KEY_TAB) {
        wm_cycle_focus();
    }
}

/* ============================================================
   Entry point
   ============================================================ */
int md_main(long argc, char **argv) {
    (void)argc; (void)argv;
    memset(&g, 0, sizeof(g));
    g.focused = -1;

    int efd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    if (efd < 0) { printf("mdw: no event device\n"); return 2; }

    if (NodGL_CreateDevice(NodGL_FEATURE_LEVEL_1_0, &g.device, &g.ctx, NULL) != NodGL_OK) {
        printf("mdw: NodGL_CreateDevice failed\n");
        close(efd);
        return 1;
    }
    NodGL_GetScreenResolution(g.device, &g.sw, &g.sh);

    NodGL_TextureDesc td; memset(&td, 0, sizeof(td));
    td.width = g.sw; td.height = g.sh;
    td.format = NodGL_FORMAT_R8G8B8A8_UNORM; td.mip_levels = 1;
    if (NodGL_CreateTexture(g.device, &td, &g.screen_tex) != NodGL_OK) {
        printf("mdw: NodGL_CreateTexture failed\n");
        NodGL_ReleaseDevice(g.device); close(efd);
        return 1;
    }
    if (NodGL_MapResource(g.ctx, g.screen_tex, (void **)&g.bb, &g.bb_pitch) != NodGL_OK) {
        printf("mdw: NodGL_MapResource failed\n");
        NodGL_ReleaseResource(g.device, g.screen_tex);
        NodGL_ReleaseDevice(g.device); close(efd);
        return 1;
    }

    g.font = try_load_font(FONT_PRIMARY);
    if (!g.font) g.font = try_load_font(FONT_FALLBACK);

    g.cursor_lib = ilib_open(MOUSE_ILIB);
    g.icon_lib   = ilib_open(ICONS_ILIB);
    if (!g.cursor_lib) printf("mdw: warning: couldn't open %s (cursor art disabled)\n", MOUSE_ILIB);
    if (!g.icon_lib)   printf("mdw: warning: couldn't open %s (icons disabled)\n", ICONS_ILIB);

    g.mouse_x = (int)g.sw / 2;
    g.mouse_y = (int)g.sh / 2;
    g.cursor_id = CURSOR_POINTER;

    /* Register the control channel — from here on, windows come from
     * clients calling invoke() on $/user/wm, not from us. */
    userfs_user_ops_t ops; memset(&ops, 0, sizeof(ops));
    ops.invoke = wm_service_invoke;
    userfs_user_node_t node; memset(&node, 0, sizeof(node));
    node.path     = "wm";
    node.owner_id = "wm";
    node.perms    = USERFS_PERM_READ_WRITE | USERFS_PERM_INVOKE;
    node.ops      = ops;
    node.ctx      = NULL;
    if (userfs_register(&node) != 0) {
        printf("mdw: failed to register %s\n", WM_SERVICE_PATH);
        if (g.bb) NodGL_UnmapResource(g.ctx, g.screen_tex);
        NodGL_ReleaseResource(g.device, g.screen_tex);
        NodGL_ReleaseDevice(g.device);
        close(efd);
        return 1;
    }
    printf("mdw: %s registered, waiting for clients\n", WM_SERVICE_PATH);

    /* Force one full-screen paint to prime the display; after this every
     * frame only touches what actually changed. */
    mark_dirty(0, 0, (int)g.sw, (int)g.sh);
    mark_cursor_dirty(g.mouse_x, g.mouse_y);

    g.quit = 0;
    while (!g.quit) {
        Event ev;
        while (read(efd, &ev, sizeof(ev)) > 0) {
            switch (ev.type) {
                case EVENT_MOUSE_MOVE:   wm_handle_mouse_move(&ev.data.mouse); break;
                case EVENT_MOUSE_BUTTON: wm_handle_mouse_button(&ev.data.mouse); break;
                case EVENT_KEY_PRESSED:  wm_handle_key(&ev.data.keyboard); break;
                default: break;
            }
        }
        if (g.quit) break;

        if (g.dirty_valid) {
            /* Clip in case damage marking ever runs before sw/sh are set (it doesn't
             * here, but this keeps the invariant airtight if that ever changes). */
            int rx0 = g.dirty_x0, ry0 = g.dirty_y0, rx1 = g.dirty_x1, ry1 = g.dirty_y1;
            if (rx0 < 0) rx0 = 0;
            if (ry0 < 0) ry0 = 0;
            if (rx1 > (int)g.sw) rx1 = (int)g.sw;
            if (ry1 > (int)g.sh) ry1 = (int)g.sh;

            if (rx1 > rx0 && ry1 > ry0) {
                compose_dirty_region(rx0, ry0, rx1, ry1);
                /* Partial upload: only the changed rect crosses into the presented
                 * texture, not the whole screen — this is the other half of the
                 * no-GPU budget, since NodGL_DrawTexture's cost scales with area. */
                NodGL_DrawTexture(g.ctx, g.screen_tex, rx0, ry0, rx0, ry0,
                                   (uint32_t)(rx1 - rx0), (uint32_t)(ry1 - ry0));
                /* Rect-scoped present — NodGL_PresentContext would flush the full
                 * viewport regardless of what we drew, undoing all the dirty-rect
                 * work above it. */
                NodGL_PresentContextRect(g.ctx, (uint32_t)rx0, (uint32_t)ry0,
                                          (uint32_t)(rx1 - rx0), (uint32_t)(ry1 - ry0), 1);
            }
            g.dirty_valid = 0;
        }
        /* No damage this iteration -> no clear, no blit, no present. Just poll
         * input again. This is what keeps an idle WM from burning a full core
         * doing nothing, which is what was starving the rest of the system. */
        yield();
    }

    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (g.windows[i].active) wm_close_window(i); /* munmaps + shm_unlinks properly */

    for (int i = 0; i < CURSOR_COUNT; i++) if (g.cursor_loaded[i]) ilib_free_image(&g.cursors[i]);
    for (int i = 0; i < ICON_COUNT; i++)   if (g.icon_loaded[i])   ilib_free_image(&g.icons[i]);
    if (g.cursor_lib) ilib_close(g.cursor_lib);
    if (g.icon_lib)   ilib_close(g.icon_lib);
    if (g.font) fnt_free_font(g.font);

    if (g.bb) NodGL_UnmapResource(g.ctx, g.screen_tex);
    NodGL_ReleaseResource(g.device, g.screen_tex);
    NodGL_ReleaseDevice(g.device);
    close(efd);
    input_flush();
    return 0;
}