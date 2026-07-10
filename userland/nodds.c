// nodds.c - NodGL Display Server for ModuOS
// Analogous to Wayland/X11 — manages windows, compositing, and input routing.
//
// Architecture:
//   $/user/nodds/control  - invoke: create/destroy/move/resize windows
//   $/user/nodds/events   - read:   clients poll for window events (input, expose, etc.)
//   $/user/nodds/<wid>    - write:  client blits a rendered frame (pixel buffer)
//   $/user/nodds/<wid>/ctl- invoke: per-window ops (set title, raise, etc.)
//
// The server composites all windows onto the NodGL framebuffer.
// All IPC is via UserFS (no mmap on nodes).

#include "libc.h"
#include "NodGL.h"
#include "ilib.h"
#include "lib_fnt.h"
#include "../include/moduos/kernel/events/events.h"

// ============================================================
// Protocol Definitions (shared with clients via nodds_client.h)
// ============================================================

#define NODDS_MAX_WINDOWS     32
#define NODDS_MAX_TITLE       64
#define NODDS_MAX_CLIENTS     32

// --- Control invoke: request commands ---
#define NODDS_CMD_CREATE_WINDOW   1
#define NODDS_CMD_DESTROY_WINDOW  2
#define NODDS_CMD_MOVE_WINDOW     3
#define NODDS_CMD_RESIZE_WINDOW   4
#define NODDS_CMD_RAISE_WINDOW    5
#define NODDS_CMD_LOWER_WINDOW    6
#define NODDS_CMD_SET_TITLE       7
#define NODDS_CMD_QUERY_SCREEN    8
#define NODDS_CMD_SHOW_WINDOW     9
#define NODDS_CMD_HIDE_WINDOW     10
#define NODDS_CMD_FOCUS_WINDOW    11

// --- Event types (clients read from $/user/nodds/events/<wid>) ---
#define NODDS_EVT_EXPOSE          1   // Window needs redraw
#define NODDS_EVT_KEY_PRESS       2
#define NODDS_EVT_KEY_RELEASE     3
#define NODDS_EVT_MOUSE_MOVE      4
#define NODDS_EVT_MOUSE_BUTTON    5
#define NODDS_EVT_RESIZE          6
#define NODDS_EVT_CLOSE           7   // User clicked X / WM close
#define NODDS_EVT_FOCUS_IN        8
#define NODDS_EVT_FOCUS_OUT       9

// ============================================================
// Cursor shape IDs (matches mouse.ilib)
// ============================================================
#define CURSOR_ARROW        0
#define CURSOR_CROSSHAIR    1
#define CURSOR_ERASER       2
#define CURSOR_HAND         3
#define CURSOR_MOVE         4
// ID 5 is not defined in the spec — skip
#define CURSOR_NO           6
#define CURSOR_TEXT         7
#define CURSOR_BUSY         8
#define CURSOR_COUNT        9

#define CURSOR_W 16
#define CURSOR_H 16

// ============================================================
// Wire Structs (packed for UserFS read/write/invoke)
// ============================================================

typedef struct __attribute__((packed)) {
    int cmd;
    uint32_t wid;
    int x, y;
    uint32_t width, height;
    uint32_t flags;
    char title[NODDS_MAX_TITLE];
} nodds_control_req_t;

typedef struct __attribute__((packed)) {
    int status;
    uint32_t wid;
    uint32_t screen_width;
    uint32_t screen_height;
    char msg[64];
} nodds_control_resp_t;

typedef struct __attribute__((packed)) {
    uint32_t wid;
    uint32_t type;
    uint32_t timestamp;
    union {
        struct { uint32_t keycode; uint32_t modifiers; } key;
        struct { int32_t x; int32_t y; uint32_t buttons; } mouse;
        struct { uint32_t width; uint32_t height; } resize;
    } data;
} nodds_event_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t wid;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t data_size;
} nodds_frame_header_t;

#define NODDS_FRAME_MAGIC 0x4E445346u

// ============================================================
// Server-side Window Table
// ============================================================

typedef struct {
    uint32_t wid;
    int32_t  x, y;
    uint32_t width, height;
    int      visible;
    int      focused;
    int      z_order;
    char     title[NODDS_MAX_TITLE];
    int      owner_pid;

    uint32_t *pixels;
    uint32_t  pixel_count;
    int       dirty;

    nodds_event_t evq[64];
    int evq_head, evq_tail;

    char frame_node[64];
    char event_node[64];
    char wctl_node[64];
    int  active;
    NodGL_Texture tex;
} nodds_window_t;

// ============================================================
// Server State
// ============================================================

static nodds_window_t g_windows[NODDS_MAX_WINDOWS];
static int g_win_count = 0;
static uint32_t g_next_wid = 1;
static uint32_t g_screen_w = 0, g_screen_h = 0;

static NodGL_Device  g_device  = NULL;
static NodGL_Context g_context = NULL;

// ============================================================
// Decoration constants
// ============================================================
#define TITLEBAR_H      24
#define TITLEBAR_COL    0xFF2D2D2D
#define BORDER_COL      0xFF444444
#define FOCUS_COL       0xFF0078D4
#define TEXT_COL        0xFFFFFFFF
#define DESKTOP_COL     0xFF1E3A5F

// Close button: fixed 16×16 region, right-aligned with 4px margin
#define CLOSEBTN_W      16
#define CLOSEBTN_H      16
#define CLOSEBTN_MARGIN 4
#define CLOSEBTN_COL    0xFFE81123   // Windows-style red
#define CLOSEBTN_HOV    0xFFFF4444   // lighter on hover (future use)
#define CLOSEBTN_X_COL  0xFFFFFFFF

// ============================================================
// Font state
// ============================================================
static fnt_font_t *g_font = NULL;

static void font_load(void) {
    int ffd = open("/ModuOS/shared/assets/fonts/Terminus.fnt", O_RDONLY, 0);
    if (ffd < 0) {
        printf("[nodds] Warning: cannot open Terminus.fnt — titlebar text disabled\n");
        return;
    }
    off_t fsz = lseek(ffd, 0, SEEK_END);
    lseek(ffd, 0, SEEK_SET);
    if (fsz <= 0) { close(ffd); return; }

    void *fdata = malloc((size_t)fsz);
    if (!fdata) { close(ffd); return; }
    read(ffd, fdata, (size_t)fsz);
    close(ffd);

    g_font = fnt_load_font(fdata, (size_t)fsz);
    free(fdata);
    if (!g_font)
        printf("[nodds] Warning: fnt_load_font failed\n");
    else
        printf("[nodds] System font loaded\n");
}

// ============================================================
// Dirty flags
// ============================================================
static int g_scene_dirty  = 1;
// FIX: initialise to 1 so the cursor is drawn on the very first
// composite even if no mouse movement has occurred yet.
static int g_cursor_dirty = 1;

// Cursor state
static int32_t  g_mouse_x = 0, g_mouse_y = 0;
static uint32_t g_mouse_buttons = 0;
static int32_t  g_rendered_cursor_x = -1;
static int32_t  g_rendered_cursor_y = -1;
static int      g_cursor_shape = CURSOR_ARROW;

// ============================================================
// Titlebar drag state
// ============================================================
static uint32_t g_drag_wid      = 0;
static int32_t  g_drag_offset_x = 0;
static int32_t  g_drag_offset_y = 0;

// ============================================================
// Cursor sprite cache
// ============================================================

static NodGL_Texture g_cursor_tex[CURSOR_COUNT];
static int           g_cursor_valid[CURSOR_COUNT];
static uint8_t      *g_cursor_pixels[CURSOR_COUNT];

static void cursor_textures_upload(void) {
    for (int i = 0; i < CURSOR_COUNT; i++) {
        if (!g_cursor_valid[i] || !g_cursor_pixels[i]) continue;
        NodGL_TextureDesc tdesc = {0};
        tdesc.width             = CURSOR_W;
        tdesc.height            = CURSOR_H;
        tdesc.format            = NodGL_FORMAT_R8G8B8A8_UNORM;
        tdesc.mip_levels        = 1;
        tdesc.initial_data      = g_cursor_pixels[i];
        tdesc.initial_data_size = CURSOR_W * CURSOR_H * 4;
        NodGL_Texture tex = 0;
        if (NodGL_CreateTexture(g_device, &tdesc, &tex) == NodGL_OK)
            g_cursor_tex[i] = tex;
        else {
            printf("[nodds] Warning: failed to upload cursor texture %d\n", i);
            g_cursor_tex[i] = 0;
        }
    }
}

static void cursor_sprites_load(void) {
    for (int i = 0; i < CURSOR_COUNT; i++) {
        g_cursor_tex[i]    = 0;
        g_cursor_valid[i]  = 0;
        g_cursor_pixels[i] = NULL;
    }

    ilib_t *lib = ilib_open("/ModuOS/shared/assets/mouse.ilib");
    if (!lib) {
        printf("[nodds] Warning: cannot open mouse.ilib — using fallback cursor\n");
        return;
    }

    int ids[] = { 0, 1, 2, 3, 4, 6, 7, 8 };
    for (int k = 0; k < (int)(sizeof(ids)/sizeof(ids[0])); k++) {
        int id = ids[k];
        ilib_image_t img;
        if (ilib_load_image(lib, (uint16_t)id, &img) != ILIB_OK) {
            printf("[nodds] Warning: failed to load cursor sprite %d\n", id);
            continue;
        }
        g_cursor_pixels[id] = img.pixels;
        g_cursor_valid[id]  = 1;
        printf("[nodds] Cursor sprite %d loaded (%ux%u)\n", id, img.width, img.height);
    }

    ilib_close(lib);
    printf("[nodds] Cursor sprites load complete\n");
    cursor_textures_upload();
}

static void cursor_sprites_release(void) {
    for (int i = 0; i < CURSOR_COUNT; i++) {
        if (g_cursor_tex[i] && g_device) {
            NodGL_ReleaseResource(g_device, (NodGL_Resource)g_cursor_tex[i]);
            g_cursor_tex[i] = 0;
        }
        if (g_cursor_pixels[i]) {
            free(g_cursor_pixels[i]);
            g_cursor_pixels[i] = NULL;
        }
        g_cursor_valid[i] = 0;
    }
}

// ============================================================
// Window Table Helpers
// ============================================================

static nodds_window_t* find_window(uint32_t wid) {
    for (int i = 0; i < NODDS_MAX_WINDOWS; i++) {
        if (g_windows[i].active && g_windows[i].wid == wid)
            return &g_windows[i];
    }
    return NULL;
}

static nodds_window_t* alloc_window(void) {
    for (int i = 0; i < NODDS_MAX_WINDOWS; i++) {
        if (!g_windows[i].active)
            return &g_windows[i];
    }
    return NULL;
}

static void evq_push(nodds_window_t *w, const nodds_event_t *evt) {
    int next = (w->evq_head + 1) % 64;
    if (next == w->evq_tail) return;  // drop on full
    w->evq[w->evq_head] = *evt;
    w->evq_head = next;
}

static int evq_pop(nodds_window_t *w, nodds_event_t *out) {
    if (w->evq_head == w->evq_tail) return 0;
    *out = w->evq[w->evq_tail];
    w->evq_tail = (w->evq_tail + 1) % 64;
    return 1;
}

// ============================================================
// Decoration helpers
// ============================================================

static void closebtn_rect(const nodds_window_t *w,
                           int *out_x, int *out_y,
                           int *out_w, int *out_h)
{
    *out_w = CLOSEBTN_W;
    *out_h = CLOSEBTN_H;
    *out_x = w->x + (int)w->width - CLOSEBTN_W - CLOSEBTN_MARGIN;
    *out_y = w->y + (TITLEBAR_H - CLOSEBTN_H) / 2;
}

static int closebtn_hit(const nodds_window_t *w, int32_t px, int32_t py) {
    int bx, by, bw, bh;
    closebtn_rect(w, &bx, &by, &bw, &bh);
    return (px >= bx && px < bx + bw && py >= by && py < by + bh);
}

static int titlebar_drag_hit(const nodds_window_t *w, int32_t px, int32_t py) {
    if (px < w->x || px >= w->x + (int)w->width) return 0;
    if (py < w->y || py >= w->y + TITLEBAR_H)     return 0;
    return !closebtn_hit(w, px, py);
}

// ============================================================
// Font rendering helpers
// ============================================================

static void draw_glyph_ctx(fnt_font_t *font, uint32_t cp,
                            int *cx, int cy, uint32_t col)
{
    fnt_glyph_t *g = fnt_get_glyph(font, cp);
    if (!g) {
        *cx += font->header.glyph_width;
        return;
    }
    for (int gy = 0; gy < g->bitmap_height; gy++) {
        for (int gx = 0; gx < g->bitmap_width; gx++) {
            if (fnt_get_pixel(g, gx, gy))
                NodGL_FillRectContext(g_context,
                                     *cx + gx, cy + gy,
                                     1, 1, col);
        }
    }
    *cx += g->width;
}

static void ctx_draw_string(fnt_font_t *font, const char *text,
                             int x, int y, uint32_t col)
{
    int cx = x;
    for (const char *p = text; *p; p++)
        draw_glyph_ctx(font, (uint32_t)(unsigned char)*p, &cx, y, col);
}

static void ctx_draw_string_centered(fnt_font_t *font, const char *text,
                                      int x, int y, int w, uint32_t col)
{
    int tw = fnt_string_width(font, text);
    int cx = x + (w - tw) / 2;
    if (cx < x) cx = x;
    ctx_draw_string(font, text, cx, y, col);
}

// ============================================================
// Compositor
// ============================================================

static void composite_frame(void) {
    NodGL_ClearContext(g_context, NodGL_CLEAR_COLOR, DESKTOP_COL, 1.0f, 0);

    int order[NODDS_MAX_WINDOWS];
    int n = 0;
    for (int i = 0; i < NODDS_MAX_WINDOWS; i++) {
        if (g_windows[i].active && g_windows[i].visible)
            order[n++] = i;
    }
    for (int a = 0; a < n - 1; a++) {
        for (int b = a + 1; b < n; b++) {
            if (g_windows[order[a]].z_order > g_windows[order[b]].z_order) {
                int tmp = order[a]; order[a] = order[b]; order[b] = tmp;
            }
        }
    }

    for (int idx = 0; idx < n; idx++) {
        nodds_window_t *w = &g_windows[order[idx]];

        // ---- Titlebar background ----
        uint32_t tb_col = w->focused ? FOCUS_COL : TITLEBAR_COL;
        NodGL_FillRectContext(g_context, w->x, w->y, w->width, TITLEBAR_H, tb_col);

        // ---- Title text (centred) ----
        if (g_font && w->title[0]) {
            int text_y = w->y + (TITLEBAR_H - (int)g_font->header.glyph_height) / 2;
            int text_area_w = (int)w->width - CLOSEBTN_W - CLOSEBTN_MARGIN * 2;
            ctx_draw_string_centered(g_font, w->title,
                                     w->x, text_y,
                                     text_area_w, TEXT_COL);
        }

        // ---- Close button ----
        {
            int bx, by, bw, bh;
            closebtn_rect(w, &bx, &by, &bw, &bh);
            NodGL_FillRectContext(g_context, bx, by, bw, bh, CLOSEBTN_COL);
            NodGL_DrawLineContext(g_context,
                bx + 4,      by + 4,
                bx + bw - 5, by + bh - 5,
                CLOSEBTN_X_COL, 1);
            NodGL_DrawLineContext(g_context,
                bx + bw - 5, by + 4,
                bx + 4,      by + bh - 5,
                CLOSEBTN_X_COL, 1);
        }

        // ---- Border ----
        NodGL_FillRect(w->x, w->y,
                         w->width, TITLEBAR_H + w->height,
                         BORDER_COL);
        
        NodGL_DrawLineContext(g_context, w->x, w->y,
            w->x + (int)w->width, w->y, BORDER_COL, 1);
        NodGL_DrawLineContext(g_context,
            w->x, w->y + TITLEBAR_H + (int)w->height,
            w->x + (int)w->width, w->y + TITLEBAR_H + (int)w->height,
            BORDER_COL, 1);
        NodGL_DrawLineContext(g_context, w->x, w->y,
            w->x, w->y + TITLEBAR_H + (int)w->height, BORDER_COL, 1);
        NodGL_DrawLineContext(g_context,
            w->x + (int)w->width, w->y,
            w->x + (int)w->width, w->y + TITLEBAR_H + (int)w->height,
            BORDER_COL, 1);
        

        // ---- Client pixel buffer ----
        if (w->pixels && w->pixel_count > 0) {
            if (w->tex == 0) {
                NodGL_TextureDesc tdesc = {0};
                tdesc.width             = w->width;
                tdesc.height            = w->height;
                tdesc.format            = NodGL_FORMAT_R8G8B8A8_UNORM;
                tdesc.mip_levels        = 1;
                tdesc.initial_data      = w->pixels;
                tdesc.initial_data_size = w->pixel_count * 4;
                NodGL_CreateTexture(g_device, &tdesc, &w->tex);
            } else if (w->dirty) {
                void *mapped = NULL; uint32_t pitch = 0;
                if (NodGL_MapResource(g_context, w->tex, &mapped, &pitch) == NodGL_OK) {
                    memcpy(mapped, w->pixels, w->pixel_count * 4);
                    NodGL_UnmapResource(g_context, w->tex);
                }
            }
            if (w->tex)
                NodGL_DrawTexture(g_context, w->tex, 0, 0,
                                  w->x, w->y + TITLEBAR_H,
                                  w->width, w->height);
        }

        w->dirty = 0;
    }
}

// ============================================================
// Cursor rendering
// ============================================================

static void draw_cursor(void) {
    int shape = g_cursor_shape;
    if (shape < 0 || shape >= CURSOR_COUNT || shape == 5)
        shape = CURSOR_ARROW;

    if (g_cursor_valid[shape] && g_cursor_tex[shape] != 0) {
        // FIX: enable alpha blending so the cursor sprite's transparent
        // pixels don't paint a solid black rectangle over the desktop.
        NodGL_SetBlendMode(g_context, NodGL_BLEND_ALPHA);
        NodGL_DrawTexture(g_context,
                          g_cursor_tex[shape],
                          0, 0,
                          g_mouse_x, g_mouse_y,
                          CURSOR_W, CURSOR_H);
        // Restore opaque blending for subsequent frames' filled rects /
        // line draws so they aren't affected by leftover alpha state.
        NodGL_SetBlendMode(g_context, NodGL_BLEND_NONE);
        return;
    }


    // Fallback: software crosshair (no texture — always visible)
    {
        uint32_t col    = 0xFFFFFFFF;
        uint32_t shadow = 0xFF000000;
        NodGL_DrawLineContext(g_context,
            g_mouse_x - 7 + 1, g_mouse_y + 1,
            g_mouse_x + 7 + 1, g_mouse_y + 1, shadow, 1);
        NodGL_DrawLineContext(g_context,
            g_mouse_x + 1, g_mouse_y - 7 + 1,
            g_mouse_x + 1, g_mouse_y + 7 + 1, shadow, 1);
        NodGL_DrawLineContext(g_context,
            g_mouse_x - 7, g_mouse_y, g_mouse_x + 7, g_mouse_y, col, 1);
        NodGL_DrawLineContext(g_context,
            g_mouse_x, g_mouse_y - 7, g_mouse_x, g_mouse_y + 7, col, 1);
        NodGL_FillRectContext(g_context, g_mouse_x - 1, g_mouse_y - 1, 3, 3, col);
    }
}

// ============================================================
// UserFS Callbacks
// ============================================================

static ssize_t ctl_invoke(void *ctx,
                          const void *in_buf, size_t in_size,
                          void *out_buf, size_t out_size)
{
    (void)ctx;
    if (in_size < sizeof(nodds_control_req_t) || out_size < sizeof(nodds_control_resp_t))
        return -1;

    const nodds_control_req_t  *req  = (const nodds_control_req_t *)in_buf;
    nodds_control_resp_t       *resp = (nodds_control_resp_t *)out_buf;

    resp->status        = 0;
    resp->wid           = 0;
    resp->screen_width  = g_screen_w;
    resp->screen_height = g_screen_h;
    resp->msg[0]        = '\0';

    switch (req->cmd) {

    case NODDS_CMD_QUERY_SCREEN:
        break;

    case NODDS_CMD_CREATE_WINDOW: {
        nodds_window_t *w = alloc_window();
        if (!w) { resp->status = -1; snprintf(resp->msg, 64, "Max windows"); break; }

        memset(w, 0, sizeof(*w));
        w->wid     = g_next_wid++;
        w->x       = req->x;
        w->y       = req->y;
        w->width   = req->width  ? req->width  : 320;
        w->height  = req->height ? req->height : 240;
        w->visible = 1;
        w->active  = 1;
        w->z_order = g_win_count++;
        w->focused = 0;
        w->dirty   = 1;
        strncpy(w->title, req->title[0] ? req->title : "Untitled", NODDS_MAX_TITLE - 1);

        w->pixel_count = w->width * w->height;
        w->pixels = (uint32_t *)calloc(w->pixel_count, sizeof(uint32_t));
        for (uint32_t i = 0; i < w->pixel_count; i++)
            w->pixels[i] = 0xFF1A1A2E;

        snprintf(w->frame_node, 64, "nodds/%u", w->wid);
        snprintf(w->event_node, 64, "nodds/events/%u", w->wid);
        snprintf(w->wctl_node,  64, "nodds/%u/ctl", w->wid);

        {
            userfs_user_node_t fnode = {0};
            fnode.path     = w->frame_node;
            fnode.owner_id = "nodds";
            fnode.perms    = USERFS_PERM_WRITE_ONLY;
            fnode.ctx      = (void*)(uintptr_t)w->wid;
            userfs_register(&fnode);
        }
        {
            userfs_user_node_t enode = {0};
            enode.path     = w->event_node;
            enode.owner_id = "nodds";
            enode.perms    = USERFS_PERM_READ_ONLY;
            enode.ctx      = (void*)(uintptr_t)w->wid;
            userfs_register(&enode);
        }
        {
            userfs_user_node_t wnode = {0};
            wnode.path     = w->wctl_node;
            wnode.owner_id = "nodds";
            wnode.perms    = USERFS_PERM_READ_WRITE | USERFS_PERM_INVOKE;
            wnode.ctx      = (void*)(uintptr_t)w->wid;
            userfs_register(&wnode);
        }

        nodds_event_t expose = {0};
        expose.wid                = w->wid;
        expose.type               = NODDS_EVT_EXPOSE;
        expose.timestamp          = (uint32_t)time_ms();
        expose.data.resize.width  = w->width;
        expose.data.resize.height = w->height;
        evq_push(w, &expose);

        resp->wid    = w->wid;
        resp->status = 0;
        snprintf(resp->msg, 64, "Window %u created", w->wid);
        printf("[nodds] Created window %u (%ux%u) at (%d,%d) title='%s'\n",
               w->wid, w->width, w->height, w->x, w->y, w->title);

        g_scene_dirty = 1;
        break;
    }

    case NODDS_CMD_DESTROY_WINDOW: {
        nodds_window_t *w = find_window(req->wid);
        if (!w) { resp->status = -1; snprintf(resp->msg, 64, "Bad wid"); break; }
        if (g_drag_wid == w->wid) g_drag_wid = 0;
        if (w->tex && g_device) {
            NodGL_ReleaseResource(g_device, (NodGL_Resource)w->tex);
            w->tex = 0;
        }
        free(w->pixels);
        w->pixels = NULL;
        w->active = 0;
        g_win_count--;
        resp->wid = req->wid;
        g_scene_dirty = 1;
        printf("[nodds] Destroyed window %u\n", req->wid);
        break;
    }

    case NODDS_CMD_MOVE_WINDOW: {
        nodds_window_t *w = find_window(req->wid);
        if (!w) { resp->status = -1; break; }
        w->x = req->x; w->y = req->y;
        w->dirty = 1;
        resp->wid = w->wid;
        g_scene_dirty = 1;
        break;
    }

    case NODDS_CMD_RESIZE_WINDOW: {
        nodds_window_t *w = find_window(req->wid);
        if (!w) { resp->status = -1; break; }
        if (w->tex && g_device) {
            NodGL_ReleaseResource(g_device, (NodGL_Resource)w->tex);
            w->tex = 0;
        }
        free(w->pixels);
        w->width  = req->width  ? req->width  : w->width;
        w->height = req->height ? req->height : w->height;
        w->pixel_count = w->width * w->height;
        w->pixels = (uint32_t *)calloc(w->pixel_count, sizeof(uint32_t));
        for (uint32_t i = 0; i < w->pixel_count; i++)
            w->pixels[i] = 0xFF1A1A2E;
        w->dirty = 1;
        resp->wid = w->wid;
        g_scene_dirty = 1;

        nodds_event_t evt = {0};
        evt.wid = w->wid; evt.type = NODDS_EVT_RESIZE;
        evt.timestamp = (uint32_t)time_ms();
        evt.data.resize.width  = w->width;
        evt.data.resize.height = w->height;
        evq_push(w, &evt);
        break;
    }

    case NODDS_CMD_RAISE_WINDOW: {
        nodds_window_t *w = find_window(req->wid);
        if (!w) { resp->status = -1; break; }
        int max_z = 0;
        for (int i = 0; i < NODDS_MAX_WINDOWS; i++)
            if (g_windows[i].active && g_windows[i].z_order > max_z)
                max_z = g_windows[i].z_order;
        w->z_order = max_z + 1;
        resp->wid = w->wid;
        g_scene_dirty = 1;
        break;
    }

    case NODDS_CMD_LOWER_WINDOW: {
        nodds_window_t *w = find_window(req->wid);
        if (!w) { resp->status = -1; break; }
        int min_z = 0;
        for (int i = 0; i < NODDS_MAX_WINDOWS; i++)
            if (g_windows[i].active && g_windows[i].z_order < min_z)
                min_z = g_windows[i].z_order;
        w->z_order = min_z - 1;
        resp->wid = w->wid;
        g_scene_dirty = 1;
        break;
    }

    case NODDS_CMD_SET_TITLE: {
        nodds_window_t *w = find_window(req->wid);
        if (!w) { resp->status = -1; break; }
        strncpy(w->title, req->title, NODDS_MAX_TITLE - 1);
        w->dirty = 1;
        resp->wid = w->wid;
        g_scene_dirty = 1;
        break;
    }

    case NODDS_CMD_SHOW_WINDOW: {
        nodds_window_t *w = find_window(req->wid);
        if (!w) { resp->status = -1; break; }
        w->visible = 1; w->dirty = 1; resp->wid = w->wid;
        g_scene_dirty = 1;
        break;
    }

    case NODDS_CMD_HIDE_WINDOW: {
        nodds_window_t *w = find_window(req->wid);
        if (!w) { resp->status = -1; break; }
        w->visible = 0; w->dirty = 1; resp->wid = w->wid;
        g_scene_dirty = 1;
        break;
    }

    case NODDS_CMD_FOCUS_WINDOW: {
        nodds_window_t *target = find_window(req->wid);
        if (!target) { resp->status = -1; break; }
        for (int i = 0; i < NODDS_MAX_WINDOWS; i++) {
            if (!g_windows[i].active) continue;
            if (g_windows[i].wid == req->wid) {
                if (!g_windows[i].focused) {
                    g_windows[i].focused = 1;
                    nodds_event_t evt = {0};
                    evt.wid = g_windows[i].wid; evt.type = NODDS_EVT_FOCUS_IN;
                    evt.timestamp = (uint32_t)time_ms();
                    evq_push(&g_windows[i], &evt);
                }
            } else if (g_windows[i].focused) {
                g_windows[i].focused = 0;
                nodds_event_t evt = {0};
                evt.wid = g_windows[i].wid; evt.type = NODDS_EVT_FOCUS_OUT;
                evt.timestamp = (uint32_t)time_ms();
                evq_push(&g_windows[i], &evt);
            }
        }
        resp->wid = req->wid;
        g_scene_dirty = 1;
        break;
    }

    default:
        resp->status = -1;
        snprintf(resp->msg, 64, "Unknown cmd %d", req->cmd);
        return -1;
    }

    return sizeof(nodds_control_resp_t);
}

// ---- $/user/nodds/events/<wid>  (read) ----
static ssize_t evt_read(void *ctx, void *buf, size_t size)
{
    uint32_t wid = (uint32_t)(uintptr_t)ctx;
    nodds_window_t *w = find_window(wid);
    if (!w) return -1;
    if (size < sizeof(nodds_event_t)) return -1;
    nodds_event_t evt;
    if (!evq_pop(w, &evt)) return 0;
    memcpy(buf, &evt, sizeof(nodds_event_t));
    return (ssize_t)sizeof(nodds_event_t);
}

// ---- $/user/nodds/<wid>  (write: client frame blit) ----
static ssize_t frame_write(void *ctx, const void *buf, size_t size)
{
    uint32_t wid = (uint32_t)(uintptr_t)ctx;
    nodds_window_t *w = find_window(wid);
    if (!w || !w->pixels) return -1;
    if (size < sizeof(nodds_frame_header_t)) return -1;

    const nodds_frame_header_t *hdr = (const nodds_frame_header_t *)buf;
    if (hdr->magic != NODDS_FRAME_MAGIC) return -1;
    if (hdr->wid   != wid)               return -1;

    size_t expected = sizeof(nodds_frame_header_t) + hdr->data_size;
    if (size < expected)                              return -1;
    if (hdr->width != w->width || hdr->height != w->height) return -1;

    const uint8_t *pixels = (const uint8_t *)buf + sizeof(nodds_frame_header_t);
    size_t copy_bytes = hdr->data_size < (w->pixel_count * 4)
                        ? hdr->data_size : (w->pixel_count * 4);
    memcpy(w->pixels, pixels, copy_bytes);
    w->dirty      = 1;
    g_scene_dirty = 1;
    return (ssize_t)size;
}

// ============================================================
// Hit testing
// ============================================================

static nodds_window_t* hit_test(int32_t x, int32_t y) {
    nodds_window_t *hit = NULL;
    int best_z = INT_MIN;
    for (int i = 0; i < NODDS_MAX_WINDOWS; i++) {
        nodds_window_t *w = &g_windows[i];
        if (!w->active || !w->visible) continue;
        if (x >= w->x && x < w->x + (int)w->width &&
            y >= w->y && y < w->y + (int)w->height + TITLEBAR_H) {
            if (w->z_order > best_z) {
                best_z = w->z_order;
                hit = w;
            }
        }
    }
    return hit;
}

// ============================================================
// Input Dispatcher
// ============================================================

static void dispatch_input(int efd) {
    Event ev;
    while (read(efd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        nodds_event_t evt = {0};
        evt.timestamp = (uint32_t)time_ms();

        if (ev.type == EVENT_KEY_PRESSED || ev.type == EVENT_KEY_RELEASED) {
            nodds_window_t *focused = NULL;
            for (int i = 0; i < NODDS_MAX_WINDOWS; i++)
                if (g_windows[i].active && g_windows[i].focused)
                    { focused = &g_windows[i]; break; }

            if (focused) {
                evt.wid               = focused->wid;
                evt.type              = (ev.type == EVENT_KEY_PRESSED)
                                        ? NODDS_EVT_KEY_PRESS
                                        : NODDS_EVT_KEY_RELEASE;
                evt.data.key.keycode  = (uint32_t)ev.data.keyboard.keycode;
                evt.data.key.modifiers = (uint32_t)ev.data.keyboard.modifiers;
                evq_push(focused, &evt);
            }

        } else if (ev.type == EVENT_MOUSE_MOVE) {
            int32_t prev_x = g_mouse_x;
            int32_t prev_y = g_mouse_y;

            g_mouse_x += ev.data.mouse.delta_x;
            g_mouse_y += ev.data.mouse.delta_y;

            if (g_mouse_x < 0) g_mouse_x = 0;
            if (g_mouse_y < 0) g_mouse_y = 0;
            if (g_mouse_x >= (int32_t)g_screen_w) g_mouse_x = (int32_t)g_screen_w - 1;
            if (g_mouse_y >= (int32_t)g_screen_h) g_mouse_y = (int32_t)g_screen_h - 1;

            if (g_mouse_x != prev_x || g_mouse_y != prev_y)
                g_cursor_dirty = 1;

            // ---- Titlebar drag: move window ----
            if (g_drag_wid != 0) {
                nodds_window_t *dw = find_window(g_drag_wid);
                if (dw) {
                    dw->x     = g_mouse_x - g_drag_offset_x;
                    dw->y     = g_mouse_y - g_drag_offset_y;
                    dw->dirty = 1;
                    g_scene_dirty = 1;
                    continue;
                } else {
                    g_drag_wid = 0;
                }
            }

            nodds_window_t *target = hit_test(g_mouse_x, g_mouse_y);
            if (target) {
                evt.wid                = target->wid;
                evt.type               = NODDS_EVT_MOUSE_MOVE;
                evt.data.mouse.x       = g_mouse_x - target->x;
                evt.data.mouse.y       = g_mouse_y - target->y - TITLEBAR_H;
                evt.data.mouse.buttons = g_mouse_buttons;
                evq_push(target, &evt);
            }

        } else if (ev.type == EVENT_MOUSE_BUTTON) {
            uint32_t mask   = (uint32_t)ev.data.mouse.buttons;
            int      pressed = (mask != 0);

            if (pressed) {
                g_mouse_buttons = mask;
                nodds_window_t *hit = hit_test(g_mouse_x, g_mouse_y);

                if (hit) {
                    if (!hit->focused) {
                        nodds_control_req_t  freq  = {0};
                        nodds_control_resp_t fresp = {0};
                        freq.cmd = NODDS_CMD_FOCUS_WINDOW;
                        freq.wid = hit->wid;
                        ctl_invoke(NULL, &freq, sizeof(freq), &fresp, sizeof(fresp));
                        freq.cmd = NODDS_CMD_RAISE_WINDOW;
                        ctl_invoke(NULL, &freq, sizeof(freq), &fresp, sizeof(fresp));
                    }

                    if (closebtn_hit(hit, g_mouse_x, g_mouse_y)) {
                        printf("[nodds] Close button clicked on wid %u\n", hit->wid);
                        nodds_event_t close_evt = {0};
                        close_evt.wid       = hit->wid;
                        close_evt.type      = NODDS_EVT_CLOSE;
                        close_evt.timestamp = (uint32_t)time_ms();
                        evq_push(hit, &close_evt);
                        g_scene_dirty = 1;

                    } else if (titlebar_drag_hit(hit, g_mouse_x, g_mouse_y)) {
                        g_drag_wid      = hit->wid;
                        g_drag_offset_x = g_mouse_x - hit->x;
                        g_drag_offset_y = g_mouse_y - hit->y;

                    } else {
                        evt.wid                = hit->wid;
                        evt.type               = NODDS_EVT_MOUSE_BUTTON;
                        evt.data.mouse.x       = g_mouse_x - hit->x;
                        evt.data.mouse.y       = g_mouse_y - hit->y - TITLEBAR_H;
                        evt.data.mouse.buttons = g_mouse_buttons;
                        evq_push(hit, &evt);
                    }
                }

            } else {
                if (g_drag_wid != 0) {
                    g_drag_wid = 0;
                }
                g_mouse_buttons = 0;

                nodds_window_t *target = hit_test(g_mouse_x, g_mouse_y);
                if (target) {
                    int32_t rel_y = g_mouse_y - target->y - TITLEBAR_H;
                    if (rel_y >= 0) {
                        evt.wid                = target->wid;
                        evt.type               = NODDS_EVT_MOUSE_BUTTON;
                        evt.data.mouse.x       = g_mouse_x - target->x;
                        evt.data.mouse.y       = rel_y;
                        evt.data.mouse.buttons = 0;
                        evq_push(target, &evt);
                    }
                }
            }

            g_scene_dirty = 1;
        }
    }
}

// ============================================================
// Main
// ============================================================

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    printf("NodDS Display Server starting...\n");

    NodGL_FeatureLevel actual_level;
    int r = NodGL_CreateDevice(NodGL_FEATURE_LEVEL_1_0, &g_device, &g_context, &actual_level);
    if (r != NodGL_OK) {
        printf("[nodds] Failed to create NodGL device: %s\n", NodGL_GetErrorString(r));
        return 1;
    }

    NodGL_GetScreenResolution(g_device, &g_screen_w, &g_screen_h);
    g_mouse_x = (int32_t)(g_screen_w / 2);
    g_mouse_y = (int32_t)(g_screen_h / 2);

    printf("[nodds] Screen: %ux%u, NodGL feature level: 0x%x\n",
           g_screen_w, g_screen_h, (unsigned)actual_level);

    // FIX: enable alpha blending globally so cursor sprites (and any
    // semi-transparent window content) render correctly.  Without this
    // the cursor texture's transparent pixels were written as opaque
    // black, making the sprite invisible against the dark desktop.
    NodGL_SetBlendMode(g_context, NodGL_BLEND_ALPHA);

    cursor_sprites_load();
    font_load();

    // Register control node
    {
        static userfs_user_ops_t  cops;  memset(&cops,  0, sizeof(cops));
        static userfs_user_node_t cnode; memset(&cnode, 0, sizeof(cnode));
        cops.invoke    = ctl_invoke;
        cnode.path     = "nodds/control";
        cnode.owner_id = "nodds";
        cnode.perms    = USERFS_PERM_READ_WRITE | USERFS_PERM_INVOKE;
        cnode.ops      = cops;
        cnode.ctx      = NULL;

        r = userfs_register(&cnode);
        if (r != 0) {
            printf("[nodds] Failed to register control node: %d\n", r);
            if (g_font) fnt_free_font(g_font);
            cursor_sprites_release();
            NodGL_ReleaseDevice(g_device);
            return 1;
        }
    }

    int efd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    if (efd < 0)
        printf("[nodds] Warning: cannot open event0 — input disabled\n");

    printf("[nodds] Registered $/user/nodds/control\n");
    printf("[nodds] Running compositor loop...\n");

    while (1) {
        if (efd >= 0)
            dispatch_input(efd);

        if (g_scene_dirty) {
            composite_frame();
            draw_cursor();
            NodGL_PresentContext(g_context, 1);
            g_scene_dirty       = 0;
            g_cursor_dirty      = 0;
            g_rendered_cursor_x = g_mouse_x;
            g_rendered_cursor_y = g_mouse_y;

        } else if (g_cursor_dirty) {
            composite_frame();
            draw_cursor();
            NodGL_PresentContext(g_context, 1);
            g_cursor_dirty      = 0;
            g_rendered_cursor_x = g_mouse_x;
            g_rendered_cursor_y = g_mouse_y;
        }

        yield();
    }

    if (efd >= 0) close(efd);
    if (g_font) fnt_free_font(g_font);
    cursor_sprites_release();
    NodGL_ReleaseDevice(g_device);
    return 0;
}