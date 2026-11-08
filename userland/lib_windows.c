/* lib_windows.c -> windows.sqrl  (depends on libc.sqrl, fnt.sqrl) */
#include "libc/libc.h"
#include "libc/string.h"
#include "lib_fnt.h"
#include "wm_protocol.h"
#include "windows.h"

#define FONT_PRIMARY  "/ModuOS/shared/assets/fonts/Terminus.fnt"
#define FONT_FALLBACK "/ModuOS/shared/assets/fonts/Unicode.fnt"
#define RESIZE_CHECK_MS 150
#define EVQ 32

struct win {
    wm_shm_t       *mb;
    int             id;
    int             w, h;
    uint32_t       *px;
    size_t          px_size;
    char            shm_name[64];
    wm_key_ring_t  *keys;
    wm_mouse_ring_t*mice;
    fnt_font_t     *font;
    int             alive;
    uint64_t        last_check;
    win_event_t     q[EVQ];   /* small overflow queue (resize events) */
    int             qh, qt;
};

/* one shared mailbox connection + font per process */
static wm_shm_t   *g_mb;
static fnt_font_t *g_font;
static int         g_font_tried;

static fnt_font_t *load_font_file(const char *path) {
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

static fnt_font_t *get_font(void) {
    if (!g_font_tried) {
        g_font_tried = 1;
        g_font = load_font_file(FONT_PRIMARY);
        if (!g_font) g_font = load_font_file(FONT_FALLBACK);
    }
    return g_font;
}

static void *map_shm(const char *name, size_t size) {
    int fd = shm_open(name, O_RDWR, 0, 0);
    if (fd < 0) return NULL;
    void *p = mmap(NULL, size, PROT_R | PROT_W, MAP_SHARED, fd);
    return p == MAP_FAILED ? NULL : p;
}

static void copy_str(char *dst, const char *src, size_t n) {
    size_t i = 0;
    if (src) while (src[i] && i + 1 < n) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

win_t *win_open_flags(const char *title, int w, int h, unsigned flags) {
    if (w <= 0 || h <= 0) return NULL;
    if (!g_mb) g_mb = wm_ipc_connect(3000);
    if (!g_mb) return NULL;

    wm_request_t req; memset(&req, 0, sizeof(req));
    req.cmd = WM_CMD_CREATE;
    req.w = (uint32_t)w; req.h = (uint32_t)h;
    req.flags = flags;
    req.pid = (uint32_t)getpid();
    copy_str(req.title, title ? title : "Window", sizeof(req.title));

    wm_response_t resp;
    if (wm_ipc_call(g_mb, &req, &resp, 3000) != 0 || !resp.success) return NULL;

    win_t *win = (win_t *)malloc(sizeof(win_t));
    if (!win) return NULL;
    memset(win, 0, sizeof(*win));
    win->mb = g_mb;
    win->id = resp.win_id;
    win->w = w; win->h = h;
    win->px_size = (size_t)resp.shm_size;
    copy_str(win->shm_name, resp.shm_name, sizeof(win->shm_name));
    win->px   = (uint32_t *)map_shm(resp.shm_name, (size_t)resp.shm_size);
    win->keys = (wm_key_ring_t *)map_shm(resp.key_shm_name, (size_t)resp.key_shm_size);
    win->mice = (wm_mouse_ring_t *)map_shm(resp.mouse_shm_name, (size_t)resp.mouse_shm_size);
    if (!win->px) { free(win); return NULL; }
    win->alive = 1;
    win->last_check = time_ms();
    for (size_t i = 0; i < (size_t)w * h; i++) win->px[i] = 0xFF000000u;
    win_present(win);
    return win;
}

win_t *win_open(const char *title, int w, int h) { return win_open_flags(title, w, h, 0); }

void win_close(win_t *w) {
    if (!w || !w->alive) return;
    wm_request_t req; memset(&req, 0, sizeof(req));
    req.cmd = WM_CMD_CLOSE; req.win_id = w->id;
    wm_response_t resp;
    wm_ipc_call(w->mb, &req, &resp, 1000);
    w->alive = 0;
}

int win_alive(win_t *w) { return w && w->alive; }
int win_width(win_t *w)  { return w ? w->w : 0; }
int win_height(win_t *w) { return w ? w->h : 0; }
uint32_t *win_pixels(win_t *w) { return w ? w->px : NULL; }
uint64_t win_time_ms(void) { return time_ms(); }
void win_sleep_frame(void) { yield(); }

/* ---- events ---- */

static void q_push(win_t *w, const win_event_t *e) {
    int nt = (w->qt + 1) % EVQ;
    if (nt == w->qh) return; /* full: drop */
    w->q[w->qt] = *e;
    w->qt = nt;
}

/* detect maximize/resize done by someone else (taskbar etc.) and remap */
static void check_remap(win_t *w) {
    uint64_t now = time_ms();
    if (now - w->last_check < RESIZE_CHECK_MS) return;
    w->last_check = now;

    wm_win_info_t info[WM_MAX_WINDOWS];
    int n = wm_list_windows(w->mb, info);
    if (n < 0) return;
    int found = 0;
    for (int i = 0; i < n; i++) {
        if (info[i].win_id != w->id) continue;
        found = 1;
        if (strcmp(info[i].shm_name, w->shm_name) != 0 || info[i].w != w->w || info[i].h != w->h) {
            uint32_t *np = (uint32_t *)map_shm(info[i].shm_name, (size_t)info[i].shm_size);
            if (np) {
                w->px = np;
                w->px_size = (size_t)info[i].shm_size;
                w->w = info[i].w; w->h = info[i].h;
                copy_str(w->shm_name, info[i].shm_name, sizeof(w->shm_name));
                win_event_t e; memset(&e, 0, sizeof(e));
                e.type = WIN_EV_RESIZE; e.w = w->w; e.h = w->h;
                q_push(w, &e);
            }
        }
        break;
    }
    if (!found) w->alive = 0; /* mdw closed us (titlebar X, crash reclaim) */
}

int win_poll(win_t *w, win_event_t *ev) {
    if (!w || !w->alive || !ev) return 0;
    memset(ev, 0, sizeof(*ev));

    if (w->qh != w->qt) { *ev = w->q[w->qh]; w->qh = (w->qh + 1) % EVQ; return 1; }

    if (w->keys) {
        KeyboardEventData k;
        if (wm_key_ring_pop(w->keys, &k)) {
            ev->type = WIN_EV_KEY;
            ev->ascii = (int)k.ascii;
            ev->keycode = (int)k.keycode;
            return 1;
        }
    }
    if (w->mice) {
        wm_mouse_event_t m;
        if (wm_mouse_ring_pop(w->mice, &m)) {
            switch (m.type) {
                case WM_MOUSE_MOVE:  ev->type = WIN_EV_MOUSE_MOVE;  break;
                case WM_MOUSE_DOWN:  ev->type = WIN_EV_MOUSE_DOWN;  break;
                case WM_MOUSE_UP:    ev->type = WIN_EV_MOUSE_UP;    break;
                case WM_MOUSE_ENTER: ev->type = WIN_EV_MOUSE_ENTER; break;
                default:             ev->type = WIN_EV_MOUSE_LEAVE; break;
            }
            ev->x = m.x; ev->y = m.y; ev->buttons = m.buttons; ev->button = m.changed;
            return 1;
        }
    }
    check_remap(w); /* rate-limited; may queue a RESIZE or kill alive */
    if (w->qh != w->qt) { *ev = w->q[w->qh]; w->qh = (w->qh + 1) % EVQ; return 1; }
    return 0;
}

/* ---- drawing ---- */

void win_clear(win_t *w, uint32_t c) {
    if (!w || !w->px) return;
    size_t n = (size_t)w->w * w->h;
    for (size_t i = 0; i < n; i++) w->px[i] = c;
}

void win_pixel(win_t *w, int x, int y, uint32_t c) {
    if (!w || !w->px || x < 0 || y < 0 || x >= w->w || y >= w->h) return;
    w->px[(size_t)y * w->w + x] = c;
}

void win_fill_rect(win_t *w, int x, int y, int rw, int rh, uint32_t c) {
    if (!w || !w->px) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + rw > w->w ? w->w : x + rw;
    int y1 = y + rh > w->h ? w->h : y + rh;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = w->px + (size_t)yy * w->w;
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
}

void win_rect(win_t *w, int x, int y, int rw, int rh, uint32_t c) {
    if (rw <= 0 || rh <= 0) return;
    win_fill_rect(w, x, y, rw, 1, c);
    win_fill_rect(w, x, y + rh - 1, rw, 1, c);
    win_fill_rect(w, x, y, 1, rh, c);
    win_fill_rect(w, x + rw - 1, y, 1, rh, c);
}

void win_line(win_t *w, int x0, int y0, int x1, int y1, uint32_t c) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = -(y1 > y0 ? y1 - y0 : y0 - y1), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        win_pixel(w, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void win_circle(win_t *w, int cx, int cy, int r, uint32_t c) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        win_pixel(w, cx + x, cy + y, c); win_pixel(w, cx - x, cy + y, c);
        win_pixel(w, cx + x, cy - y, c); win_pixel(w, cx - x, cy - y, c);
        win_pixel(w, cx + y, cy + x, c); win_pixel(w, cx - y, cy + x, c);
        win_pixel(w, cx + y, cy - x, c); win_pixel(w, cx - y, cy - x, c);
        y++;
        if (err < 0) err += 2 * y + 1; else { x--; err += 2 * (y - x) + 1; }
    }
}

void win_fill_circle(win_t *w, int cx, int cy, int r, uint32_t c) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r) win_pixel(w, cx + dx, cy + dy, c);
}

void win_text_scaled(win_t *w, int x, int y, const char *s, uint32_t c, int scale) {
    fnt_font_t *f = get_font();
    if (!w || !f || !s) return;
    if (scale < 1) scale = 1;
    int cx = x;
    for (; *s; s++) {
        if (*s == '\n') { cx = x; y += f->header.glyph_height * scale; continue; }
        fnt_glyph_t *g = fnt_get_glyph(f, (uint32_t)(unsigned char)*s);
        if (!g) continue;
        for (int dy = 0; dy < g->bitmap_height; dy++)
            for (int dx = 0; dx < g->bitmap_width; dx++)
                if (fnt_get_pixel(g, dx, dy))
                    win_fill_rect(w, cx + dx * scale, y + dy * scale, scale, scale, c);
        cx += g->width * scale;
    }
}

void win_text(win_t *w, int x, int y, const char *s, uint32_t c) { win_text_scaled(w, x, y, s, c, 1); }

int win_text_width(win_t *w, const char *s) {
    (void)w;
    fnt_font_t *f = get_font();
    return (f && s) ? fnt_string_width(f, s) : 0;
}

int win_text_height(win_t *w) {
    (void)w;
    fnt_font_t *f = get_font();
    return f ? f->header.glyph_height : 0;
}

/* ---- present + window ops ---- */

void win_present_rect(win_t *w, int x, int y, int rw, int rh) {
    if (!w || !w->alive) return;
    if (wm_damage(w->mb, w->id, x, y, rw, rh) != 0) { /* mdw gone/stuck */ }
    yield();
}

void win_present(win_t *w) { if (w) win_present_rect(w, 0, 0, w->w, w->h); }

void win_set_title_cursor(win_t *w, int id) { if (w && w->alive) wm_set_cursor(w->mb, w->id, (uint32_t)id); }
void win_minimize(win_t *w) { if (w && w->alive) wm_minimize(w->mb, w->id); }

static void apply_resp(win_t *w, wm_response_t *r, int ok) {
    if (ok != 0 || !r->success) return;
    uint32_t *np = (uint32_t *)map_shm(r->shm_name, (size_t)r->shm_size);
    if (!np) return;
    w->px = np; w->px_size = (size_t)r->shm_size;
    copy_str(w->shm_name, r->shm_name, sizeof(w->shm_name));
    /* dimensions: ask mdw on next remap check */
    w->last_check = 0;
}

void win_maximize(win_t *w)   { if (w && w->alive) { wm_response_t r; int ok = wm_maximize(w->mb, w->id, &r);   apply_resp(w, &r, ok); } }
void win_restore(win_t *w)    { if (w && w->alive) { wm_response_t r; int ok = wm_restore(w->mb, w->id, &r);    apply_resp(w, &r, ok); } }
void win_fullscreen(win_t *w) { if (w && w->alive) { wm_response_t r; int ok = wm_fullscreen(w->mb, w->id, &r); apply_resp(w, &r, ok); } }
void win_resize(win_t *w, int nw, int nh) { if (w && w->alive) { wm_response_t r; int ok = wm_resize(w->mb, w->id, nw, nh, &r); apply_resp(w, &r, ok); } }