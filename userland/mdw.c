/*
 * mdw.c — ModuOS Windows (MDW): the window manager / display manager
 *
 * A software compositor: the WM owns the screen framebuffer (via NodGL)
 * and blits every window's pixel buffer into it every frame, back-to-front
 * by z-order, on top of which it draws chrome (title bars, close buttons,
 * focus highlight), a taskbar, and the mouse cursor.
 *
 * Windows are backed by real shared memory. A client talks to mdw over a
 * single SHM mailbox (see wm_protocol.h, ported from netd's SHM IPC --
 * NOT UserFS's $/user/* open()/invoke() anymore):
 *   WM_CMD_CREATE     -> WM shm_open()s+mmap()s a fresh segment, hands the
 *                         client its name+size to shm_open()+mmap() themselves
 *   WM_CMD_DAMAGE     -> client tells us exactly which sub-rect it redrew;
 *                         we translate that straight into the dirty-rect
 *                         system instead of assuming "the whole window changed"
 *   WM_CMD_MOVE       -> rarely needed (users normally drag), but available
 *   WM_CMD_CLOSE      -> munmap + shm_unlink on our side
 *   WM_CMD_SET_CURSOR -> which cursor to show over that window's content
 *   WM_CMD_KEYFEED_OPEN/CLOSE -> windowless global key ring, for clients
 *                         like mdbindkeys that need every keypress without
 *                         ever creating a window
 * See wm_protocol.h for the wire format and mailbox protocol.
 *
 * Input: keyboard goes to the focused window through a per-window key
 * ring; mouse goes to the window under the pointer (or the window that
 * grabbed it by being pressed in) through a per-window mouse ring. Both
 * are plain SPSC SHM rings — see wm_post_mouse() and wm_handle_mouse_*
 * below for the delivery policy, and wm_protocol.h for the ring itself.
 *
 * No GPU: every pixel is a CPU store through the framebuffer, so the
 * compositor is built around a single per-frame damage rect — see the
 * "Damage tracking" section below for how that works and why.
 *
 * Controls:
 *   Left click + drag on a title bar        — move window
 *   Left click on minimize / maximize / close — as labeled, on any normal window
 *   Left click anywhere on a window          — focus + raise it
 *   Tab                                      — cycle focus (skips minimized windows)
 *   Ctrl+Alt+Q                               — quit the compositor (dev/testing)
 *
 * There is no built-in taskbar. Panel/dock/taskbar-style UI is just a
 * normal client that creates a WM_FLAG_DOCK window (chrome-less, pinned to
 * a screen edge) and drives other windows through the Window API
 * (WM_CMD_LIST_WINDOWS/FOCUS/MINIMIZE/MAXIMIZE/RESTORE/FULLSCREEN/RESIZE/
 * CLOSE/MOVE, all taking an arbitrary win_id) -- see wm_protocol.h and
 * taskbar.c for the reference implementation.
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

/* Cursor IDs (mouse.ilib). Mirrored as WM_CURSOR_* in wm_protocol.h so
 * clients can name them in WM_CMD_SET_CURSOR — keep the two in sync. */
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
#define WM_TITLEBAR_H      24
#define WM_BTN_SIZE        16   /* close/maximize/minimize are all this size */
#define WM_BTN_MARGIN      4    /* gap from the close button to the window edge */
#define WM_BTN_GAP         4    /* gap between adjacent titlebar buttons */

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
#define COL_BTN_BG         0xFF3A4152 /* minimize/maximize button background, focused */
#define COL_BTN_BG_BLUR    0xFF2A2F3B /* ditto, unfocused */
#define COL_BTN_ICON       0xFFF2F5FA

/* ============================================================
   Window
   ============================================================ */
typedef enum { HIT_NONE, HIT_TITLEBAR, HIT_CLOSE, HIT_MAXIMIZE, HIT_MINIMIZE, HIT_CONTENT } wm_hit_region_t;

typedef struct {
    int       active;
    int       x, y;          /* top-left of the WHOLE window, incl. titlebar */
    int       w, h;          /* content width/height (excludes titlebar)     */
    char      title[64];
    uint32_t *buf;           /* WM's own mapping of the client's SHM segment */
    char      shm_name[64];  /* name to shm_unlink() on close                */
    uint64_t  shm_size;      /* size to munmap() on close                    */
    wm_key_ring_t *key_ring; /* WM's own mapping of this window's key-input segment */
    char      key_shm_name[64]; /* name to shm_unlink() on close             */
    wm_mouse_ring_t *mouse_ring;   /* ditto for this window's mouse-input segment */
    char      mouse_shm_name[64];  /* name to shm_unlink() on close          */
    int       content_cursor;/* cursor to show while hovering this window's content */
    int       opaque;        /* 1 = every pixel has a=255, lets the blitter memcpy whole
                                 rows instead of alpha-testing pixel by pixel. Driven by
                                 the client's WM_FLAG_HAS_ALPHA at creation time. */
    uint32_t  owner_pid;      /* self-reported by the client at CREATE; 0 = not tracked.
                                  Used only to reap windows whose process has actually
                                  died — see wm_reap_dead_clients(). */

    /* -- state machine (minimize/maximize/fullscreen) -------------------- */
    int       state;              /* WM_STATE_* */
    int       state_before_minimize; /* WM_STATE_* to return to on un-minimize;
                                         only meaningful while state==MINIMIZED */
    int       saved_x, saved_y, saved_w, saved_h; /* pre-maximize/fullscreen geometry,
                                                      restored by WM_CMD_RESTORE */

    /* -- dock windows (chrome-less, pinned to a screen edge) -------------- */
    int       is_dock;        /* 1 if created with WM_FLAG_DOCK */
    uint32_t  dock_edge;      /* WM_DOCK_EDGE_*, valid only if is_dock */

    /* -- desktop window (chrome-less, full-screen, always at the back) ---- */
    int       is_desktop;     /* 1 if created with WM_FLAG_DESKTOP */
} wm_window_t;

/* A "chromeless" window has no titlebar/border and its content fills its
 * entire outer rect starting at (x,y) -- true for dock windows always, and
 * for any normal window currently WM_STATE_FULLSCREEN. Centralizing this
 * here means every place that used to hardcode "+WM_TITLEBAR_H" (hit
 * testing, damage rects, blitting, mouse-event translation, chrome
 * drawing) instead asks these three, so fullscreen and dock windows are
 * automatically consistent everywhere rather than needing to be special-
 * cased at each call site individually. */
static inline int wm_chromeless(const wm_window_t *win) {
    return win->is_dock || win->is_desktop || win->state == WM_STATE_FULLSCREEN;
}
static inline int wm_outer_h(const wm_window_t *win) {
    return wm_chromeless(win) ? win->h : (WM_TITLEBAR_H + win->h);
}
static inline int wm_content_y0(const wm_window_t *win) {
    return wm_chromeless(win) ? win->y : (win->y + WM_TITLEBAR_H);
}

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
    int           dockorder[WM_MAX_WINDOWS]; /* dock window slot indices, insertion order.
                                                 Drawn after zorder (always on top), never
                                                 raised/focus-cycled, own tiny "z-order". */
    int           dockcount;
    int           focused;                /* window slot index, -1 = none */
    uint32_t      next_shm_id;            /* monotonic, for unique shm segment names */
    uint64_t      last_reap_check_ms;      /* throttle for wm_reap_dead_clients() */

    struct {
        int             active;
        wm_key_ring_t  *ring;
        char            shm_name[64];
    } keyfeeds[WM_MAX_KEYFEEDS];           /* windowless global key subscribers */

    int      mouse_x, mouse_y;
    uint8_t  prev_buttons;
    int      cursor_id;

    /* Mouse routing. hover_win is the window whose CONTENT the pointer is
     * over (-1 for chrome/desktop/taskbar); mouse_grab is the window that
     * owns the pointer until every button comes back up, so a drag that
     * starts inside a client keeps being delivered to that client even
     * once the cursor leaves it. */
    int      hover_win;
    int      mouse_grab;

    int      dragging;
    int      drag_win;
    int      drag_dx, drag_dy;

    /* Damage tracking: single bounding dirty rect, [x0,y0)-[x1,y1). */
    int      dirty_valid;
    int      dirty_x0, dirty_y0, dirty_x1, dirty_y1;

    int      quit;
} g;

/* SHM control-channel mailbox mdw owns. Replaces the old userfs_register()
 * node entirely -- there is no $/user/wm anymore. */
static wm_shm_t *g_mailbox;

static void wm_update_hover_cursor(void); /* used by the SET_CURSOR handler below */
static void wm_reflow_maximized(void);    /* used by dock create/close, defined near the
                                              rest of the minimize/maximize state machine */

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
    mark_dirty(win->x, win->y, win->w, wm_outer_h(win));
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
    int content_y0 = wm_content_y0(win);
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

/* Sums up how much screen space is currently reserved by dock windows on
 * each edge (thickness only, since a dock always spans its whole edge).
 * Driven purely by whatever dock windows exist right now -- there's no
 * separate "taskbar height" constant anymore. Used by MAXIMIZE to pick the
 * available area, and by dock CREATE itself to stack a second dock on an
 * edge outward from the first rather than on top of it. */
static void wm_compute_reserved_ex(int *top, int *bottom, int *left, int *right, int exclude_slot) {
    int t = 0, b = 0, l = 0, r = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (i == exclude_slot) continue;
        wm_window_t *w = &g.windows[i];
        if (!w->active || !w->is_dock) continue;
        switch (w->dock_edge) {
            case WM_DOCK_EDGE_TOP:    t += w->h; break;
            case WM_DOCK_EDGE_BOTTOM: b += w->h; break;
            case WM_DOCK_EDGE_LEFT:   l += w->w; break;
            case WM_DOCK_EDGE_RIGHT:  r += w->w; break;
        }
    }
    *top = t; *bottom = b; *left = l; *right = r;
}

static void wm_compute_reserved(int *top, int *bottom, int *left, int *right) {
    wm_compute_reserved_ex(top, bottom, left, right, -1);
}

/* Allocates a window slot, creates its backing SHM segments (pixels, key
 * ring, mouse ring), and maps them into the WM's own address space. On
 * success fills resp->shm_name/shm_size (pixels) plus the key/mouse ring
 * names and sizes for the client to shm_open() (no SHM_O_CREAT on their
 * end — the segments already exist) and mmap().
 *
 * WM_FLAG_DOCK windows are pinned to a screen edge: w/h are reinterpreted
 * as just the thickness off that edge (the span along the edge is always
 * the full screen dimension, computed here, not requested by the client --
 * see WM_FLAG_DOCK's comment in wm_protocol.h). They get no titlebar, no
 * close button, and don't participate in the normal window's z-order or
 * MINIMIZE/MAXIMIZE/FULLSCREEN state machine at all.
 *
 * WM_FLAG_DESKTOP windows are similar in spirit (chrome-less, no state
 * machine) but the opposite in placement: always full-screen (both w AND
 * h ignored, not just one), and inserted at the FRONT of g.zorder (index
 * 0 = drawn first = furthest back) rather than appended at the end, so
 * every other normal window ends up on top of it. It also never becomes
 * g.focused on creation -- see wm_bring_to_front()'s is_desktop guard for
 * how that stays true even if something later tries to raise it. */
static int wm_service_create_window(const char *title, uint32_t w, uint32_t h,
                                     uint32_t flags, uint32_t owner_pid, uint32_t dock_edge,
                                     wm_response_t *resp) {
    int is_dock = (flags & WM_FLAG_DOCK) != 0;
    int is_desktop = (flags & WM_FLAG_DESKTOP) != 0;
    if (is_dock && is_desktop) return -1; /* nonsensical combination */
    uint32_t actual_w = w, actual_h = h;

    if (is_dock) {
        if (dock_edge == WM_DOCK_EDGE_TOP || dock_edge == WM_DOCK_EDGE_BOTTOM) {
            actual_w = g.sw;
            actual_h = h ? h : 1;
        } else if (dock_edge == WM_DOCK_EDGE_LEFT || dock_edge == WM_DOCK_EDGE_RIGHT) {
            actual_w = w ? w : 1;
            actual_h = g.sh;
        } else {
            return -1; /* bad edge */
        }
    } else if (is_desktop) {
        actual_w = g.sw;
        actual_h = g.sh;
    }
    if (actual_w == 0 || actual_h == 0 || actual_w > 4096 || actual_h > 4096) return -1;

    int slot = -1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) if (!g.windows[i].active) { slot = i; break; }
    if (slot < 0) return -1;

    wm_window_t *win = &g.windows[slot];
    memset(win, 0, sizeof(*win));

    build_shm_name(win->shm_name, sizeof(win->shm_name), g.next_shm_id++);
    uint64_t size = (uint64_t)actual_w * (uint64_t)actual_h * 4;

    int shm_h = shm_open(win->shm_name, O_RDWR | SHM_O_CREAT | SHM_O_EXCL, 0600, size);
    if (shm_h < 0) return -1;

    void *ptr = mmap(NULL, (size_t)size, PROT_R | PROT_W, MAP_SHARED, shm_h);
    if (ptr == MAP_FAILED) { shm_unlink(win->shm_name); return -1; }

    /* Second segment: fixed-size key ring, independent of w/h. Failure
     * here unwinds the pixel segment too -- a window with no input path
     * is not a degraded-but-usable state, it's a broken window. */
    char key_shm_name[64];
    build_shm_name(key_shm_name, sizeof(key_shm_name), g.next_shm_id++);
    int key_shm_h = shm_open(key_shm_name, O_RDWR | SHM_O_CREAT | SHM_O_EXCL, 0600,
                              sizeof(wm_key_ring_t));
    if (key_shm_h < 0) {
        munmap(ptr, (size_t)size);
        shm_unlink(win->shm_name);
        return -1;
    }
    void *key_ptr = mmap(NULL, sizeof(wm_key_ring_t), PROT_R | PROT_W, MAP_SHARED, key_shm_h);
    if (key_ptr == MAP_FAILED) {
        shm_unlink(key_shm_name);
        munmap(ptr, (size_t)size);
        shm_unlink(win->shm_name);
        return -1;
    }
    memset(key_ptr, 0, sizeof(wm_key_ring_t));

    /* Third segment: the mouse ring. Same all-or-nothing unwind reasoning
     * as the key ring -- a window that can be clicked but whose clicks go
     * nowhere is worse than one that never opened. */
    char mouse_shm_name[64];
    build_shm_name(mouse_shm_name, sizeof(mouse_shm_name), g.next_shm_id++);
    int mouse_shm_h = shm_open(mouse_shm_name, O_RDWR | SHM_O_CREAT | SHM_O_EXCL, 0600,
                                sizeof(wm_mouse_ring_t));
    if (mouse_shm_h < 0) {
        munmap(key_ptr, sizeof(wm_key_ring_t));
        shm_unlink(key_shm_name);
        munmap(ptr, (size_t)size);
        shm_unlink(win->shm_name);
        return -1;
    }
    void *mouse_ptr = mmap(NULL, sizeof(wm_mouse_ring_t), PROT_R | PROT_W, MAP_SHARED, mouse_shm_h);
    if (mouse_ptr == MAP_FAILED) {
        shm_unlink(mouse_shm_name);
        munmap(key_ptr, sizeof(wm_key_ring_t));
        shm_unlink(key_shm_name);
        munmap(ptr, (size_t)size);
        shm_unlink(win->shm_name);
        return -1;
    }
    memset(mouse_ptr, 0, sizeof(wm_mouse_ring_t));

    copy_str(win->key_shm_name, sizeof(win->key_shm_name), key_shm_name);
    win->key_ring = (wm_key_ring_t *)key_ptr;
    copy_str(win->mouse_shm_name, sizeof(win->mouse_shm_name), mouse_shm_name);
    win->mouse_ring = (wm_mouse_ring_t *)mouse_ptr;

    win->active = 1;
    win->w = (int)actual_w; win->h = (int)actual_h;
    win->buf = (uint32_t *)ptr;
    win->shm_size = size;
    win->content_cursor = CURSOR_POINTER;
    win->opaque = (flags & WM_FLAG_HAS_ALPHA) ? 0 : 1;
    win->owner_pid = owner_pid;
    win->state = WM_STATE_NORMAL;
    win->is_dock = is_dock;
    win->dock_edge = dock_edge;
    win->is_desktop = is_desktop;
    copy_str(win->title, sizeof(win->title), title);

    if (is_dock) {
        /* win->active was already set to 1 above (needed regardless of
         * dock-ness), so a naive wm_compute_reserved() here would sum in
         * THIS window's own thickness on its own edge and push it back
         * off the screen by exactly that much. Exclude our own slot so
         * only OTHER docks on the same edge count toward the offset. */
        int top, bottom, left, right;
        wm_compute_reserved_ex(&top, &bottom, &left, &right, slot);
        switch (dock_edge) {
            case WM_DOCK_EDGE_TOP:    win->x = 0; win->y = top; break;
            case WM_DOCK_EDGE_BOTTOM: win->x = 0; win->y = (int)g.sh - bottom - win->h; break;
            case WM_DOCK_EDGE_LEFT:   win->x = left; win->y = 0; break;
            case WM_DOCK_EDGE_RIGHT:  win->x = (int)g.sw - right - win->w; win->y = 0; break;
        }
        g.dockorder[g.dockcount++] = slot;
        /* A new dock claiming screen space can shrink what's available to
         * any already-maximized window -- reflow them now rather than
         * leaving them overlapping the new dock until something else
         * nudges them. */
        wm_reflow_maximized();
    } else if (is_desktop) {
        win->x = 0; win->y = 0;
        /* Insert at the FRONT of zorder (index 0 = drawn first = furthest
         * back), not appended at the end like a normal window -- every
         * window that exists now, and every one created after this, ends
         * up on top of it. Deliberately does NOT set g.focused: a
         * freshly-created desktop should never steal focus from whatever
         * else is already up. */
        for (int i = g.zcount; i > 0; i--) g.zorder[i] = g.zorder[i - 1];
        g.zorder[0] = slot;
        g.zcount++;
    } else {
        /* simple cascade so new windows don't all land exactly on top of each other */
        win->x = 60 + (slot % 6) * 24;
        win->y = 40 + (slot % 6) * 24;
        g.zorder[g.zcount++] = slot;
        g.focused = slot;
    }
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
    int was_dock = win->is_dock;
    if (win->buf) munmap(win->buf, (size_t)win->shm_size);
    if (win->shm_name[0]) shm_unlink(win->shm_name); /* client's own mapping (if any) stays
                                                          valid until it munmaps too */
    if (win->key_ring) munmap(win->key_ring, sizeof(wm_key_ring_t));
    if (win->key_shm_name[0]) shm_unlink(win->key_shm_name);
    if (win->mouse_ring) munmap(win->mouse_ring, sizeof(wm_mouse_ring_t));
    if (win->mouse_shm_name[0]) shm_unlink(win->mouse_shm_name);
    win->active = 0;

    if (was_dock) {
        int w = 0;
        for (int i = 0; i < g.dockcount; i++) if (g.dockorder[i] != slot) g.dockorder[w++] = g.dockorder[i];
        g.dockcount = w;
    } else {
        int w = 0;
        for (int i = 0; i < g.zcount; i++) if (g.zorder[i] != slot) g.zorder[w++] = g.zorder[i];
        g.zcount = w;
    }

    if (g.focused == slot)
        g.focused = (g.zcount > 0) ? g.zorder[g.zcount - 1] : -1;

    /* Drop any input routing that pointed at this window, or the next
     * mouse event would be posted into a freed ring. */
    if (g.hover_win == slot)  g.hover_win = -1;
    if (g.mouse_grab == slot) g.mouse_grab = -1;
    if (g.dragging && g.drag_win == slot) g.dragging = 0;

    /* A dock going away frees up reserved screen space -- let anything
     * currently maximized expand back into it. */
    if (was_dock) wm_reflow_maximized();
}

/* Tears down a window's current content SHM segment and replaces it with a
 * freshly allocated one sized for new_w x new_h, updating win->buf/w/h/
 * shm_name/shm_size in place. Used by MAXIMIZE/FULLSCREEN/RESTORE/RESIZE --
 * none of those can just reinterpret the existing buffer at a new size,
 * since the compositor would then blit past whatever was actually
 * allocated. If resp is non-NULL its shm_name/shm_size are filled with the
 * new segment so the CALLER can remap -- see the remap-detection note on
 * wm_win_info_t in wm_protocol.h for what happens when the caller isn't
 * the window's own client. */
static int wm_realloc_window_buffer(int slot, int new_w, int new_h, wm_response_t *resp) {
    if (slot < 0 || slot >= WM_MAX_WINDOWS || !g.windows[slot].active) return -1;
    if (new_w <= 0 || new_h <= 0 || new_w > 4096 || new_h > 4096) return -1;
    wm_window_t *win = &g.windows[slot];

    char new_name[64];
    build_shm_name(new_name, sizeof(new_name), g.next_shm_id++);
    uint64_t new_size = (uint64_t)new_w * (uint64_t)new_h * 4;

    int shm_h = shm_open(new_name, O_RDWR | SHM_O_CREAT | SHM_O_EXCL, 0600, new_size);
    if (shm_h < 0) return -1;
    void *ptr = mmap(NULL, (size_t)new_size, PROT_R | PROT_W, MAP_SHARED, shm_h);
    if (ptr == MAP_FAILED) { shm_unlink(new_name); return -1; }

    if (win->buf) munmap(win->buf, (size_t)win->shm_size);
    if (win->shm_name[0]) shm_unlink(win->shm_name);

    win->buf = (uint32_t *)ptr;
    win->shm_size = new_size;
    copy_str(win->shm_name, sizeof(win->shm_name), new_name);
    win->w = new_w;
    win->h = new_h;

    if (resp) {
        copy_str(resp->shm_name, sizeof(resp->shm_name), win->shm_name);
        resp->shm_size = new_size;
    }
    return 0;
}

/*
 * own ring, just not attached to any window slot. Every active feed gets
 * every key press mdw sees, independent of focus -- see wm_handle_key(). 
 */
static int wm_service_open_keyfeed(wm_response_t *resp) {
    int slot = -1;
    for (int i = 0; i < WM_MAX_KEYFEEDS; i++) if (!g.keyfeeds[i].active) { slot = i; break; }
    if (slot < 0) return -1;

    char name[64];
    build_shm_name(name, sizeof(name), g.next_shm_id++);
    int h = shm_open(name, O_RDWR | SHM_O_CREAT | SHM_O_EXCL, 0600, sizeof(wm_key_ring_t));
    if (h < 0) return -1;
    void *ptr = mmap(NULL, sizeof(wm_key_ring_t), PROT_R | PROT_W, MAP_SHARED, h);
    if (ptr == MAP_FAILED) { shm_unlink(name); return -1; }
    memset(ptr, 0, sizeof(wm_key_ring_t));

    g.keyfeeds[slot].active = 1;
    g.keyfeeds[slot].ring = (wm_key_ring_t *)ptr;
    copy_str(g.keyfeeds[slot].shm_name, sizeof(g.keyfeeds[slot].shm_name), name);

    if (resp) {
        copy_str(resp->key_shm_name, sizeof(resp->key_shm_name), name);
        resp->key_shm_size = sizeof(wm_key_ring_t);
    }
    return slot;
}

static void wm_service_close_keyfeed(int slot) {
    if (slot < 0 || slot >= WM_MAX_KEYFEEDS || !g.keyfeeds[slot].active) return;
    munmap(g.keyfeeds[slot].ring, sizeof(wm_key_ring_t));
    shm_unlink(g.keyfeeds[slot].shm_name);
    g.keyfeeds[slot].active = 0;
    g.keyfeeds[slot].ring = NULL;
    g.keyfeeds[slot].shm_name[0] = 0;
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
    if (slot < 0 || slot >= WM_MAX_WINDOWS || g.windows[slot].is_desktop) return;
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
    /* Rotate the ring at most once per window; skip past any that are
     * minimized (there's nothing on screen to focus) rather than landing
     * keyboard focus on a window the user can't currently see. If every
     * window is minimized, nothing to do -- leave focus as-is. */
    for (int tries = 0; tries < g.zcount; tries++) {
        int slot = g.zorder[0];
        for (int i = 1; i < g.zcount; i++) g.zorder[i - 1] = g.zorder[i];
        g.zorder[g.zcount - 1] = slot;
        if (g.windows[slot].state != WM_STATE_MINIMIZED) {
            g.focused = slot;
            mark_window_dirty(g.focused);
            if (old_focused != g.focused) mark_window_dirty(old_focused);
            return;
        }
    }
}

/* -- Minimize/maximize/fullscreen/restore/focus state machine -----------
 * Shared by both the titlebar button clicks (wm_handle_mouse_button) and
 * the WM_CMD_MINIMIZE/MAXIMIZE/FULLSCREEN/RESTORE/FOCUS mailbox commands
 * (wm_service_handle_request) -- same pattern as wm_close_window already
 * being shared between the close button and WM_CMD_CLOSE. None of these
 * apply to dock windows; callers should check win->is_dock themselves for
 * a proper error message, but each function also no-ops safely on a dock
 * slot as defense in depth. */

static void wm_minimize_window(int slot) {
    if (slot < 0 || slot >= WM_MAX_WINDOWS || !g.windows[slot].active) return;
    wm_window_t *win = &g.windows[slot];
    if (win->is_dock || win->is_desktop || win->state == WM_STATE_MINIMIZED) return;

    mark_window_dirty(slot); /* erase it from the screen */
    win->state_before_minimize = win->state;
    win->state = WM_STATE_MINIMIZED;

    if (g.focused == slot) {
        int next = -1;
        for (int i = g.zcount - 1; i >= 0; i--) {
            int s = g.zorder[i];
            if (s != slot && g.windows[s].active && g.windows[s].state != WM_STATE_MINIMIZED) { next = s; break; }
        }
        g.focused = next;
        if (next >= 0) mark_window_dirty(next); /* its focus highlight just changed */
    }
}

/* Fills available_area from the current screen size minus whatever's
 * reserved by dock windows. Shared by MAXIMIZE and its reflow path. */
static int wm_maximize_window(int slot, wm_response_t *resp) {
    if (slot < 0 || slot >= WM_MAX_WINDOWS || !g.windows[slot].active) return -1;
    wm_window_t *win = &g.windows[slot];
    if (win->is_dock || win->is_desktop) return -1;

    mark_window_dirty(slot); /* old rect */
    if (win->state == WM_STATE_NORMAL) {
        win->saved_x = win->x; win->saved_y = win->y;
        win->saved_w = win->w; win->saved_h = win->h;
    }

    int top, bottom, left, right;
    wm_compute_reserved(&top, &bottom, &left, &right);
    int ax0 = left, ay0 = top, ax1 = (int)g.sw - right, ay1 = (int)g.sh - bottom;
    int new_w = ax1 - ax0;
    int new_h = (ay1 - ay0) - WM_TITLEBAR_H; /* maximize keeps the titlebar; only
                                                  FULLSCREEN goes edge-to-edge */
    if (new_w <= 0 || new_h <= 0) return -1; /* docks have eaten the whole screen */

    if (wm_realloc_window_buffer(slot, new_w, new_h, resp) != 0) return -1;
    win->x = ax0; win->y = ay0;
    win->state = WM_STATE_MAXIMIZED;
    mark_window_dirty(slot); /* new rect */
    return 0;
}

/* Fullscreen ignores dock-reserved margins entirely and covers the whole
 * screen, chrome hidden -- standard "fullscreen goes over the taskbar too"
 * behavior. */
static int wm_fullscreen_window(int slot, wm_response_t *resp) {
    if (slot < 0 || slot >= WM_MAX_WINDOWS || !g.windows[slot].active) return -1;
    wm_window_t *win = &g.windows[slot];
    if (win->is_dock || win->is_desktop) return -1;

    mark_window_dirty(slot);
    if (win->state == WM_STATE_NORMAL) {
        win->saved_x = win->x; win->saved_y = win->y;
        win->saved_w = win->w; win->saved_h = win->h;
    }
    if (wm_realloc_window_buffer(slot, (int)g.sw, (int)g.sh, resp) != 0) return -1;
    win->x = 0; win->y = 0;
    win->state = WM_STATE_FULLSCREEN;
    mark_window_dirty(slot);
    return 0;
}

static int wm_restore_window(int slot, wm_response_t *resp) {
    if (slot < 0 || slot >= WM_MAX_WINDOWS || !g.windows[slot].active) return -1;
    wm_window_t *win = &g.windows[slot];
    if (win->is_dock || win->is_desktop) return -1;

    if (win->state == WM_STATE_MINIMIZED) {
        mark_window_dirty(slot); /* no-op visually (it was hidden) but keeps damage
                                     tracking's invariant that every state change marks */
        win->state = win->state_before_minimize;
        mark_window_dirty(slot);
        if (resp) { copy_str(resp->shm_name, sizeof(resp->shm_name), win->shm_name); resp->shm_size = win->shm_size; }
        return 0;
    }
    if (win->state == WM_STATE_MAXIMIZED || win->state == WM_STATE_FULLSCREEN) {
        mark_window_dirty(slot);
        int nw = win->saved_w, nh = win->saved_h;
        if (nw <= 0 || nh <= 0) { nw = (int)g.sw / 2; nh = (int)g.sh / 2; } /* shouldn't
            happen (saved_* is always set on the NORMAL->MAXIMIZED/FULLSCREEN edge),
            but a sane fallback beats a 0x0 window if it ever does */
        if (wm_realloc_window_buffer(slot, nw, nh, resp) != 0) return -1;
        win->x = win->saved_x; win->y = win->saved_y;
        win->state = WM_STATE_NORMAL;
        mark_window_dirty(slot);
        return 0;
    }
    /* already normal: still a success, just hand back the current buffer info */
    if (resp) { copy_str(resp->shm_name, sizeof(resp->shm_name), win->shm_name); resp->shm_size = win->shm_size; }
    return 0;
}

/* Raise + focus any window, restoring it first if it was minimized. This
 * is what a taskbar entry click should call -- the one-shot "bring this to
 * the front, whatever state it's in" operation. For a dock window this
 * just moves keyboard focus there (docks aren't in the normal z-order to
 * "raise" — wm_bring_to_front() is a safe no-op on a slot it can't find). */
static int wm_focus_window(int slot) {
    if (slot < 0 || slot >= WM_MAX_WINDOWS || !g.windows[slot].active) return -1;
    wm_window_t *win = &g.windows[slot];
    if (win->state == WM_STATE_MINIMIZED) {
        mark_window_dirty(slot);
        win->state = win->state_before_minimize;
        mark_window_dirty(slot);
    }
    if (win->is_dock || win->is_desktop) {
        int old = g.focused;
        g.focused = slot;
        if (old != slot && old >= 0) mark_window_dirty(old);
        return 0;
    }
    wm_bring_to_front(slot); /* also sets g.focused and marks dirty */
    return 0;
}

/* Any window currently WM_STATE_MAXIMIZED gets re-fit to the current
 * available area. Called after a dock window is created or closed, since
 * either changes how much screen space is actually available -- see the
 * call sites in wm_service_create_window/wm_close_window. This reallocates
 * those windows' content buffers exactly like a fresh MAXIMIZE would; see
 * the remap-detection note on wm_win_info_t in wm_protocol.h for how their
 * owning clients notice. */
static void wm_reflow_maximized(void) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (g.windows[i].active && !g.windows[i].is_dock && !g.windows[i].is_desktop && g.windows[i].state == WM_STATE_MAXIMIZED)
            wm_maximize_window(i, NULL);
}

/* Position of the three titlebar buttons for a window, shared by drawing
 * and hit-testing so they can never disagree about where a click landed. */
static void wm_titlebar_buttons(const wm_window_t *win, int *min_x, int *max_x, int *close_x, int *by) {
    *by = win->y + (WM_TITLEBAR_H - WM_BTN_SIZE) / 2;
    *close_x = win->x + win->w - WM_BTN_SIZE - WM_BTN_MARGIN;
    *max_x   = *close_x - WM_BTN_SIZE - WM_BTN_GAP;
    *min_x   = *max_x   - WM_BTN_SIZE - WM_BTN_GAP;
}

/* Topmost thing under (px,py), plus which region was hit. -1 if none.
 * Dock windows are checked first since they're always drawn on top of the
 * normal z-order (see compose_dirty_region); within the normal z-order,
 * minimized windows are invisible and skipped entirely. */
static int wm_window_at(int px, int py, wm_hit_region_t *out_region) {
    for (int i = g.dockcount - 1; i >= 0; i--) {
        int slot = g.dockorder[i];
        wm_window_t *win = &g.windows[slot];
        if (px >= win->x && px < win->x + win->w && py >= win->y && py < win->y + win->h) {
            if (out_region) *out_region = HIT_CONTENT; /* docks have no chrome at all */
            return slot;
        }
    }

    for (int i = g.zcount - 1; i >= 0; i--) {
        int slot = g.zorder[i];
        wm_window_t *win = &g.windows[slot];
        if (win->state == WM_STATE_MINIMIZED) continue; /* nothing on screen to hit */

        int x0 = win->x, x1 = win->x + win->w;
        int y0 = win->y, y1 = win->y + wm_outer_h(win);
        if (px < x0 || px >= x1 || py < y0 || py >= y1) continue;

        if (!wm_chromeless(win) && py < win->y + WM_TITLEBAR_H) {
            int min_x, max_x, close_x, by;
            wm_titlebar_buttons(win, &min_x, &max_x, &close_x, &by);
            if (px >= close_x && px < close_x + WM_BTN_SIZE && py >= by && py < by + WM_BTN_SIZE) {
                if (out_region) *out_region = HIT_CLOSE;
            } else if (px >= max_x && px < max_x + WM_BTN_SIZE && py >= by && py < by + WM_BTN_SIZE) {
                if (out_region) *out_region = HIT_MAXIMIZE;
            } else if (px >= min_x && px < min_x + WM_BTN_SIZE && py >= by && py < by + WM_BTN_SIZE) {
                if (out_region) *out_region = HIT_MINIMIZE;
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
   Mouse delivery to clients
   ============================================================ */

/* Push one event into a window's mouse ring, translating the compositor's
 * screen-space pointer into that window's content space. The client never
 * learns where its window is on screen and never has to account for the
 * title bar -- (0,0) is always its own first pixel. */
static void wm_post_mouse(int slot, int type, uint32_t buttons, uint32_t changed) {
    if (slot < 0 || slot >= WM_MAX_WINDOWS) return;
    wm_window_t *win = &g.windows[slot];
    if (!win->active || !win->mouse_ring) return;

    wm_mouse_event_t ev;
    ev.type    = type;
    ev.x       = g.mouse_x - win->x;
    ev.y       = g.mouse_y - wm_content_y0(win);
    ev.buttons = buttons;
    ev.changed = changed;
    wm_mouse_ring_push(win->mouse_ring, &ev);
}

/* Which window's content is under the pointer right now, or -1. While a
 * title bar drag is in progress the answer is always -1: that gesture
 * belongs to the WM, and leaking its moves into the client would make a
 * window-drag look like a content-drag to whatever it passes over. */
static int wm_content_window_under_cursor(void) {
    if (g.dragging) return -1;
    wm_hit_region_t region;
    int slot = wm_window_at(g.mouse_x, g.mouse_y, &region);
    return (slot >= 0 && region == HIT_CONTENT) ? slot : -1;
}

/* ============================================================
   Control channel handler — SHM mailbox only, no UserFS.
   Called from md_main's loop whenever g_mailbox's state is
   WM_SLOT_REQUEST_PENDING. Always fully populates *resp, on every path,
   including every error path -- this is what keeps clients from ever
   reading an uninitialized response (see wm_ipc_call()'s own memset too;
   this is belt-and-suspenders on the server side of the same contract).
   ============================================================ */
static void wm_service_handle_request(const wm_request_t *req, wm_response_t *resp) {
    memset(resp, 0, sizeof(*resp));

    switch (req->cmd) {
        case WM_CMD_CREATE: {
            int slot = wm_service_create_window(req->title, req->w, req->h, req->flags,
                                                 req->pid, req->dock_edge, resp);
            if (slot < 0) {
                copy_str(resp->message, sizeof(resp->message), "create failed (bad size/edge or no free slot/shm)");
                resp->success = 0;
                return;
            }
            resp->success = 1;
            resp->win_id = slot;
            copy_str(resp->key_shm_name, sizeof(resp->key_shm_name), g.windows[slot].key_shm_name);
            resp->key_shm_size = sizeof(wm_key_ring_t);
            copy_str(resp->mouse_shm_name, sizeof(resp->mouse_shm_name), g.windows[slot].mouse_shm_name);
            resp->mouse_shm_size = sizeof(wm_mouse_ring_t);
            return;
        }
        case WM_CMD_DAMAGE: {
            if (req->win_id < 0 || req->win_id >= WM_MAX_WINDOWS || !g.windows[req->win_id].active) {
                copy_str(resp->message, sizeof(resp->message), "bad win_id");
                resp->success = 0;
                return;
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
            return;
        }
        case WM_CMD_MOVE: {
            if (req->win_id < 0 || req->win_id >= WM_MAX_WINDOWS || !g.windows[req->win_id].active) {
                copy_str(resp->message, sizeof(resp->message), "bad win_id");
                resp->success = 0;
                return;
            }
            wm_window_t *win = &g.windows[req->win_id];
            if (win->is_dock) {
                copy_str(resp->message, sizeof(resp->message), "dock windows are pinned and can't be moved");
                resp->success = 0;
                return;
            }
            if (win->is_desktop) {
                copy_str(resp->message, sizeof(resp->message), "the desktop window is pinned and can't be moved");
                resp->success = 0;
                return;
            }
            if (win->state == WM_STATE_FULLSCREEN) {
                copy_str(resp->message, sizeof(resp->message), "can't move a fullscreen window");
                resp->success = 0;
                return;
            }
            mark_window_dirty(req->win_id); /* old position */
            win->x = req->x;
            win->y = req->y;
            mark_window_dirty(req->win_id); /* new position */
            resp->success = 1;
            resp->win_id = req->win_id;
            return;
        }
        case WM_CMD_SET_CURSOR: {
            if (req->win_id < 0 || req->win_id >= WM_MAX_WINDOWS || !g.windows[req->win_id].active) {
                copy_str(resp->message, sizeof(resp->message), "bad win_id");
                resp->success = 0;
                return;
            }
            if (req->flags >= (uint32_t)CURSOR_COUNT) {
                copy_str(resp->message, sizeof(resp->message), "bad cursor id");
                resp->success = 0;
                return;
            }
            g.windows[req->win_id].content_cursor = (int)req->flags;
            /* If the pointer is already sitting on that window, swap the
             * sprite now rather than waiting for the next move event. */
            if (g.hover_win == req->win_id) {
                mark_cursor_dirty(g.mouse_x, g.mouse_y);
                wm_update_hover_cursor();
                mark_cursor_dirty(g.mouse_x, g.mouse_y);
            }
            resp->success = 1;
            resp->win_id = req->win_id;
            return;
        }
        case WM_CMD_CLOSE: {
            if (req->win_id < 0 || req->win_id >= WM_MAX_WINDOWS || !g.windows[req->win_id].active) {
                copy_str(resp->message, sizeof(resp->message), "bad win_id");
                resp->success = 0;
                return;
            }
            wm_close_window(req->win_id);
            resp->success = 1;
            return;
        }
        case WM_CMD_KEYFEED_OPEN: {
            int slot = wm_service_open_keyfeed(resp);
            if (slot < 0) {
                copy_str(resp->message, sizeof(resp->message), "keyfeed open failed (no free slot/shm)");
                resp->success = 0;
                return;
            }
            resp->success = 1;
            resp->win_id = slot;
            return;
        }
        case WM_CMD_KEYFEED_CLOSE: {
            if (req->win_id < 0 || req->win_id >= WM_MAX_KEYFEEDS || !g.keyfeeds[req->win_id].active) {
                copy_str(resp->message, sizeof(resp->message), "bad feed_id");
                resp->success = 0;
                return;
            }
            wm_service_close_keyfeed(req->win_id);
            resp->success = 1;
            return;
        }
        case WM_CMD_MINIMIZE: {
            if (req->win_id < 0 || req->win_id >= WM_MAX_WINDOWS || !g.windows[req->win_id].active) {
                copy_str(resp->message, sizeof(resp->message), "bad win_id");
                resp->success = 0;
                return;
            }
            if (g.windows[req->win_id].is_dock) {
                copy_str(resp->message, sizeof(resp->message), "dock windows can't be minimized");
                resp->success = 0;
                return;
            }
            if (g.windows[req->win_id].is_desktop) {
                copy_str(resp->message, sizeof(resp->message), "the desktop window can't be minimized");
                resp->success = 0;
                return;
            }
            wm_minimize_window(req->win_id);
            resp->success = 1;
            resp->win_id = req->win_id;
            return;
        }
        case WM_CMD_MAXIMIZE: {
            if (req->win_id < 0 || req->win_id >= WM_MAX_WINDOWS || !g.windows[req->win_id].active) {
                copy_str(resp->message, sizeof(resp->message), "bad win_id");
                resp->success = 0;
                return;
            }
            if (g.windows[req->win_id].is_dock) {
                copy_str(resp->message, sizeof(resp->message), "dock windows can't be maximized");
                resp->success = 0;
                return;
            }
            if (g.windows[req->win_id].is_desktop) {
                copy_str(resp->message, sizeof(resp->message), "the desktop window can't be maximized");
                resp->success = 0;
                return;
            }
            if (wm_maximize_window(req->win_id, resp) != 0) {
                copy_str(resp->message, sizeof(resp->message), "maximize failed (no room available)");
                resp->success = 0;
                return;
            }
            resp->success = 1;
            resp->win_id = req->win_id;
            return;
        }
        case WM_CMD_FULLSCREEN: {
            if (req->win_id < 0 || req->win_id >= WM_MAX_WINDOWS || !g.windows[req->win_id].active) {
                copy_str(resp->message, sizeof(resp->message), "bad win_id");
                resp->success = 0;
                return;
            }
            if (g.windows[req->win_id].is_dock) {
                copy_str(resp->message, sizeof(resp->message), "dock windows can't go fullscreen");
                resp->success = 0;
                return;
            }
            if (g.windows[req->win_id].is_desktop) {
                copy_str(resp->message, sizeof(resp->message), "the desktop window can't go fullscreen");
                resp->success = 0;
                return;
            }
            if (wm_fullscreen_window(req->win_id, resp) != 0) {
                copy_str(resp->message, sizeof(resp->message), "fullscreen failed");
                resp->success = 0;
                return;
            }
            resp->success = 1;
            resp->win_id = req->win_id;
            return;
        }
        case WM_CMD_RESTORE: {
            if (req->win_id < 0 || req->win_id >= WM_MAX_WINDOWS || !g.windows[req->win_id].active) {
                copy_str(resp->message, sizeof(resp->message), "bad win_id");
                resp->success = 0;
                return;
            }
            if (g.windows[req->win_id].is_dock) {
                copy_str(resp->message, sizeof(resp->message), "dock windows have no state to restore");
                resp->success = 0;
                return;
            }
            if (g.windows[req->win_id].is_desktop) {
                copy_str(resp->message, sizeof(resp->message), "the desktop window has no state to restore");
                resp->success = 0;
                return;
            }
            if (wm_restore_window(req->win_id, resp) != 0) {
                copy_str(resp->message, sizeof(resp->message), "restore failed");
                resp->success = 0;
                return;
            }
            resp->success = 1;
            resp->win_id = req->win_id;
            return;
        }
        case WM_CMD_RESIZE: {
            if (req->win_id < 0 || req->win_id >= WM_MAX_WINDOWS || !g.windows[req->win_id].active) {
                copy_str(resp->message, sizeof(resp->message), "bad win_id");
                resp->success = 0;
                return;
            }
            wm_window_t *win = &g.windows[req->win_id];
            if (win->is_dock) {
                copy_str(resp->message, sizeof(resp->message), "dock windows can't be resized this way");
                resp->success = 0;
                return;
            }
            if (win->is_desktop) {
                copy_str(resp->message, sizeof(resp->message), "the desktop window can't be resized this way");
                resp->success = 0;
                return;
            }
            if (win->state != WM_STATE_NORMAL) {
                copy_str(resp->message, sizeof(resp->message), "can only resize a normal (not maximized/fullscreen/minimized) window");
                resp->success = 0;
                return;
            }
            mark_window_dirty(req->win_id); /* old rect */
            if (wm_realloc_window_buffer(req->win_id, (int)req->w, (int)req->h, resp) != 0) {
                copy_str(resp->message, sizeof(resp->message), "resize failed (bad size or alloc failure)");
                resp->success = 0;
                return;
            }
            mark_window_dirty(req->win_id); /* new rect */
            resp->success = 1;
            resp->win_id = req->win_id;
            return;
        }
        case WM_CMD_FOCUS: {
            if (req->win_id < 0 || req->win_id >= WM_MAX_WINDOWS || !g.windows[req->win_id].active) {
                copy_str(resp->message, sizeof(resp->message), "bad win_id");
                resp->success = 0;
                return;
            }
            wm_focus_window(req->win_id);
            resp->success = 1;
            resp->win_id = req->win_id;
            return;
        }
        case WM_CMD_LIST_WINDOWS: {
            int n = 0;
            for (int i = 0; i < WM_MAX_WINDOWS && n < WM_MAX_WINDOWS; i++) {
                wm_window_t *win = &g.windows[i];
                if (!win->active) continue;
                wm_win_info_t *info = &resp->windows[n];
                info->win_id  = i;
                info->state   = win->state;
                info->focused = (i == g.focused) ? 1 : 0;
                info->is_dock = win->is_dock;
                info->is_desktop = win->is_desktop;
                info->dock_edge = win->dock_edge;
                info->x = win->x; info->y = win->y;
                info->w = win->w; info->h = win->h;
                info->shm_size = win->shm_size;
                copy_str(info->shm_name, sizeof(info->shm_name), win->shm_name);
                copy_str(info->title, sizeof(info->title), win->title);
                n++;
            }
            resp->window_count = n;
            resp->success = 1;
            return;
        }
        default:
            copy_str(resp->message, sizeof(resp->message), "unknown cmd");
            resp->success = 0;
            return;
    }
}

/* Non-blocking: called once per main-loop iteration. If a client has
 * posted a request (state == WM_SLOT_REQUEST_PENDING), service it
 * immediately and post the response; otherwise return right away. This
 * runs in mdw's own single thread, same as the compositor's frame work --
 * there's no concurrent access to g.windows[]/g.zorder[]/etc. from any
 * other context, so no additional locking is needed on mdw's side beyond
 * the mailbox's own spinlock (which only ever guards client-vs-client
 * access, not client-vs-mdw). */
static void wm_service_poll_mailbox(void) {
    if (wm_ipc_get_state(g_mailbox) != WM_SLOT_REQUEST_PENDING) return;

    wm_response_t resp;
    wm_service_handle_request(&g_mailbox->req, &resp);
    memcpy(&g_mailbox->resp, &resp, sizeof(resp));
    wm_ipc_set_state(g_mailbox, WM_SLOT_RESPONSE_READY);
}

/* ============================================================
   Chrome (title bar, close button, border) + taskbar
   ============================================================ */
static void draw_window_chrome(int slot) {
    wm_window_t *win = &g.windows[slot];
    if (wm_chromeless(win)) return; /* dock windows and fullscreen windows draw no chrome at all */

    int focused = (slot == g.focused);
    uint32_t tb_col = focused ? COL_TITLEBAR_FOCUS : COL_TITLEBAR_BLUR;
    uint32_t border_col = focused ? COL_BORDER_FOCUS : COL_BORDER_BLUR;
    uint32_t btn_bg = focused ? COL_BTN_BG : COL_BTN_BG_BLUR;

    fill(win->x, win->y, win->w, WM_TITLEBAR_H, tb_col);

    int min_x, max_x, close_x, by;
    wm_titlebar_buttons(win, &min_x, &max_x, &close_x, &by);

    /* close */
    fill(close_x, by, WM_BTN_SIZE, WM_BTN_SIZE, focused ? COL_CLOSE_BG : COL_CLOSE_BG_BLUR);
    for (int i = 2; i < WM_BTN_SIZE - 2; i++) {
        blend(close_x + i, by + i, 0xF2, 0xF5, 0xFA, 255);
        blend(close_x + i, by + WM_BTN_SIZE - 1 - i, 0xF2, 0xF5, 0xFA, 255);
    }

    /* maximize/restore -- a single square outline normally; once maximized,
     * the classic two-overlapping-squares "restore" glyph instead, so the
     * button visually tells you which way it's about to switch. */
    fill(max_x, by, WM_BTN_SIZE, WM_BTN_SIZE, btn_bg);
    if (win->state == WM_STATE_MAXIMIZED) {
        draw_rect_outline(max_x + 5, by + 3, WM_BTN_SIZE - 9, WM_BTN_SIZE - 9, COL_BTN_ICON);
        draw_rect_outline(max_x + 3, by + 5, WM_BTN_SIZE - 9, WM_BTN_SIZE - 9, COL_BTN_ICON);
    } else {
        draw_rect_outline(max_x + 3, by + 3, WM_BTN_SIZE - 6, WM_BTN_SIZE - 6, COL_BTN_ICON);
    }

    /* minimize -- a single horizontal bar near the bottom of the button */
    fill(min_x, by, WM_BTN_SIZE, WM_BTN_SIZE, btn_bg);
    for (int i = 3; i < WM_BTN_SIZE - 3; i++) blend(min_x + i, by + WM_BTN_SIZE - 5, 0xF2, 0xF5, 0xFA, 255);

    int title_clip_x1 = min_x - 6;
    draw_text_clip(win->x + 8, win->y + (WM_TITLEBAR_H - 8) / 2, win->x + 8, title_clip_x1, win->title, COL_TITLE_TEXT);

    draw_rect_outline(win->x, win->y, win->w, wm_outer_h(win), border_col);
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
        if (win->state == WM_STATE_MINIMIZED) continue; /* hidden, nothing to draw */

        int ix0, iy0, ix1, iy1;
        int outer_y1 = win->y + wm_outer_h(win);
        if (!rect_intersect(win->x, win->y, win->x + win->w, outer_y1,
                             rx0, ry0, rx1, ry1, &ix0, &iy0, &ix1, &iy1))
            continue; /* window doesn't touch the damaged area at all — skip it entirely */

        blit_window_content_region(win, rx0, ry0, rx1, ry1);
        draw_window_chrome(slot); /* cheap (titlebar-height strip + border); no-op if chromeless */
    }

    /* Dock windows always draw on top of the normal z-order, chrome-less --
     * whatever a dock app renders into its own buffer IS its appearance,
     * same blit path as a normal window's content, just no titlebar/border
     * layered over it. There's no built-in taskbar to draw here anymore;
     * that job now belongs entirely to whatever dock app(s) are running. */
    for (int i = 0; i < g.dockcount; i++) {
        int slot = g.dockorder[i];
        wm_window_t *win = &g.windows[slot];
        int ix0, iy0, ix1, iy1;
        if (!rect_intersect(win->x, win->y, win->x + win->w, win->y + win->h,
                             rx0, ry0, rx1, ry1, &ix0, &iy0, &ix1, &iy1))
            continue;
        blit_window_content_region(win, rx0, ry0, rx1, ry1);
    }

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

    /* A grabbing client keeps its cursor for the whole drag, even once the
     * pointer wanders off its window -- the gesture is still theirs. */
    if (g.mouse_grab >= 0 && g.windows[g.mouse_grab].active) {
        g.cursor_id = g.windows[g.mouse_grab].content_cursor;
        return;
    }

    wm_hit_region_t region;
    int slot = wm_window_at(g.mouse_x, g.mouse_y, &region);
    if (slot >= 0) {
        if (region == HIT_TITLEBAR || region == HIT_CLOSE || region == HIT_MAXIMIZE || region == HIT_MINIMIZE) {
            g.cursor_id = CURSOR_HAND;
            return;
        }
        if (region == HIT_CONTENT) { g.cursor_id = g.windows[slot].content_cursor; return; }
    }
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
        int max_y = (int)g.sh - WM_TITLEBAR_H - win->h; /* no more hardcoded taskbar strip
            to keep clear of -- dock windows still visually sit on top if you drag under
            them, same as any two normal windows can overlap */
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

    /* --- client delivery ---------------------------------------------
     * Hover changes fire LEAVE on the old target and ENTER on the new one
     * so a client can drop a hover highlight without polling. The move
     * itself goes to the grabbing window if there is one (mid-drag), else
     * to whatever content is under the pointer. */
    int hover = wm_content_window_under_cursor();
    if (hover != g.hover_win) {
        if (g.hover_win >= 0) wm_post_mouse(g.hover_win, WM_MOUSE_LEAVE, m->buttons, 0);
        g.hover_win = hover;
        if (hover >= 0) wm_post_mouse(hover, WM_MOUSE_ENTER, m->buttons, 0);
    }
    int target = (g.mouse_grab >= 0) ? g.mouse_grab : hover;
    if (target >= 0) wm_post_mouse(target, WM_MOUSE_MOVE, m->buttons, 0);
}

static void wm_handle_mouse_button(const MouseEventData *m) {
    uint8_t pressed  = (uint8_t)(~g.prev_buttons & m->buttons);
    uint8_t released = (uint8_t)(g.prev_buttons & ~m->buttons);
    g.prev_buttons = m->buttons;

    if (pressed) {
        wm_hit_region_t region;
        int slot = wm_window_at(g.mouse_x, g.mouse_y, &region);
        if (slot >= 0) {
            wm_bring_to_front(slot); /* safe no-op for dock windows, which aren't in zorder */
            if (region == HIT_CLOSE && (pressed & 0x1)) {
                wm_close_window(slot);
            } else if (region == HIT_MAXIMIZE && (pressed & 0x1)) {
                if (g.windows[slot].state == WM_STATE_MAXIMIZED) wm_restore_window(slot, NULL);
                else wm_maximize_window(slot, NULL);
            } else if (region == HIT_MINIMIZE && (pressed & 0x1)) {
                wm_minimize_window(slot);
            } else if (region == HIT_TITLEBAR && (pressed & 0x1)) {
                g.dragging = 1;
                g.drag_win = slot;
                g.drag_dx = g.mouse_x - g.windows[slot].x;
                g.drag_dy = g.mouse_y - g.windows[slot].y;
            } else if (region == HIT_CONTENT) {
                /* Grab: every subsequent move/release goes to this window
                 * until all buttons are up, so a drag started inside a
                 * client doesn't get stolen the moment it crosses an edge. */
                if (g.mouse_grab < 0) g.mouse_grab = slot;
                if (g.hover_win != slot) {
                    if (g.hover_win >= 0) wm_post_mouse(g.hover_win, WM_MOUSE_LEAVE, m->buttons, 0);
                    g.hover_win = slot;
                    wm_post_mouse(slot, WM_MOUSE_ENTER, m->buttons, 0);
                }
                wm_post_mouse(slot, WM_MOUSE_DOWN, m->buttons, pressed);
            }
        }
        /* else: nothing under the pointer -- no built-in taskbar/desktop menu to hit anymore */
    }

    if (released) {
        if (g.mouse_grab >= 0) wm_post_mouse(g.mouse_grab, WM_MOUSE_UP, m->buttons, released);
        if (m->buttons == 0) {
            g.mouse_grab = -1;
            /* The pointer may have ended up somewhere else entirely during
             * the drag; resync hover so the next move doesn't think it's
             * still over the window the gesture started in. */
            int hover = wm_content_window_under_cursor();
            if (hover != g.hover_win) {
                if (g.hover_win >= 0) wm_post_mouse(g.hover_win, WM_MOUSE_LEAVE, m->buttons, 0);
                g.hover_win = hover;
                if (hover >= 0) wm_post_mouse(hover, WM_MOUSE_ENTER, m->buttons, 0);
            }
        }
        if ((released & 0x1) && g.dragging) g.dragging = 0;
    }

    wm_update_hover_cursor();
}

static void wm_handle_key(const KeyboardEventData *k) {
    /* Windowless feeds see every key press system-wide, focus or no focus,
     * before anything below can consume it (WM shortcuts, focused client). */
    for (int i = 0; i < WM_MAX_KEYFEEDS; i++)
        if (g.keyfeeds[i].active) wm_key_ring_push(g.keyfeeds[i].ring, k);

    /* Compositor-level quit, dev-only. Gated on Ctrl+Alt so a focused
     * client (e.g. a terminal's shell) can receive plain 'q'/'Q' --
     * previously this ate every lowercase-q keystroke system-wide. */
    if ((k->ascii == 'q' || k->ascii == 'Q') &&
        (k->modifiers & MOD_CTRL) && (k->modifiers & MOD_ALT)) {
        g.quit = 1;
        return;
    }
    if (k->keycode == KEY_TAB) {
        wm_cycle_focus();
        return;
    }
    /* Anything else goes to whichever window currently has focus, if
     * any. This is the only path by which a client window receives
     * keyboard input at all -- previously there was none. */
    if (g.focused >= 0 && g.windows[g.focused].active && g.windows[g.focused].key_ring) {
        wm_key_ring_push(g.windows[g.focused].key_ring, k);
    }
}

/* ============================================================
   Entry point
   ============================================================ */
int md_main(long argc, char **argv) {
    (void)argc; (void)argv;
    memset(&g, 0, sizeof(g));
    g.focused    = -1;
    g.hover_win  = -1;
    g.mouse_grab = -1;

    int efd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    if (efd < 0) { printf("mdw: no event device\n"); return 2; }

    if (NodGL_CreateDevice(NodGL_FEATURE_LEVEL_1_0, &g.device, &g.ctx, NULL) != NodGL_OK) {
        printf("mdw: NodGL_CreateDevice failed\n");
        close(efd);
        return 1;
    }
    NodGL_GetScreenResolution(g.device, &g.sw, &g.sh);

    /* Query vsync support. sync_interval on NodGL_PresentContextRect only
     * actually throttles presentation if the device advertises
     * NodGL_CAP_VSYNC; on feature level 1.0 that isn't guaranteed. If it's
     * absent, sync_interval=1 is likely a no-op and every dirty-rect
     * present goes straight to the display unsynced -- on a fast CPU this
     * means many presents per refresh interval, i.e. tearing. Fall back to
     * a software frame-rate cap in that case. */
    NodGL_DeviceCaps caps;
    int has_vsync = 0;
    if (NodGL_GetDeviceCaps(g.device, &caps) == NodGL_OK)
        has_vsync = (caps.capabilities & NodGL_CAP_VSYNC) != 0;
    if (!has_vsync)
        printf("mdw: warning: device has no NodGL_CAP_VSYNC, using software frame pacing\n");

    uint64_t g_last_present_ms = 0;
    const uint32_t g_target_frame_ms = 16; /* ~60Hz fallback cap when no vsync */

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

    /* Create + own the SHM control-channel mailbox. Replaces the old
     * userfs_register($/user/wm) entirely -- there is no UserFS node
     * for the WM anymore, control traffic is pure SHM from here on. */
    g_mailbox = wm_ipc_create();
    if (!g_mailbox) {
        printf("mdw: failed to create %s mailbox\n", WM_IPC_SHM_NAME);
        if (g.bb) NodGL_UnmapResource(g.ctx, g.screen_tex);
        NodGL_ReleaseResource(g.device, g.screen_tex);
        NodGL_ReleaseDevice(g.device);
        close(efd);
        return 1;
    }
    printf("mdw: %s mailbox created, waiting for clients\n", WM_IPC_SHM_NAME);

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

        /* Service at most one pending client request per loop iteration --
         * non-blocking, same cadence as event polling. Single mailbox
         * slot means only one client's control call is in flight system-
         * wide at a time (identical trade-off to netd's mailbox), which
         * is fine: CREATE/DAMAGE/MOVE/CLOSE are short and this isn't the
         * per-frame compositing path. */
        wm_service_poll_mailbox();

        wm_reap_dead_clients();

        /* Damage keeps accumulating every iteration regardless of whether we
         * present this iteration -- mark_dirty/mark_window_dirty/mark_cursor_dirty
         * calls from events and WM_CMD_DAMAGE above are untouched. What's gated
         * here is only the actual flush to the screen: with real vsync support
         * we present as soon as there's damage (the driver/hardware paces us);
         * without it we self-throttle to g_target_frame_ms so a fast CPU can't
         * issue multiple unsynced presents inside a single refresh interval,
         * which is what produces tearing. */
        uint64_t now_ms = time_ms();
        int should_present = g.dirty_valid &&
            (has_vsync || (now_ms - g_last_present_ms) >= g_target_frame_ms);

        if (should_present) {
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
                 * work above it. sync_interval=1 gives true tear-free vsync when
                 * has_vsync is set; when it isn't, the software pacing above is
                 * what actually prevents tearing. */
                NodGL_PresentContextRect(g.ctx, (uint32_t)rx0, (uint32_t)ry0,
                                          (uint32_t)(rx1 - rx0), (uint32_t)(ry1 - ry0), 1);
                g_last_present_ms = now_ms;
            }
            g.dirty_valid = 0;
        }
        /* No damage this iteration, or damage is being held back by frame pacing
         * -> no clear, no blit, no present. Just poll input/mailbox again. This is
         * what keeps an idle WM from burning a full core doing nothing, which is
         * what was starving the rest of the system. */
        yield();
    }

    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (g.windows[i].active) wm_close_window(i); /* munmaps + shm_unlinks properly */

    for (int i = 0; i < WM_MAX_KEYFEEDS; i++)
        if (g.keyfeeds[i].active) wm_service_close_keyfeed(i);

    for (int i = 0; i < CURSOR_COUNT; i++) if (g.cursor_loaded[i]) ilib_free_image(&g.cursors[i]);
    for (int i = 0; i < ICON_COUNT; i++)   if (g.icon_loaded[i])   ilib_free_image(&g.icons[i]);
    if (g.cursor_lib) ilib_close(g.cursor_lib);
    if (g.icon_lib)   ilib_close(g.icon_lib);
    if (g.font) fnt_free_font(g.font);

    if (g_mailbox) {
        munmap(g_mailbox, sizeof(wm_shm_t));
        shm_unlink(WM_IPC_SHM_NAME);
    }

    if (g.bb) NodGL_UnmapResource(g.ctx, g.screen_tex);
    NodGL_ReleaseResource(g.device, g.screen_tex);
    NodGL_ReleaseDevice(g.device);
    close(efd);
    input_flush();
    return 0;
}