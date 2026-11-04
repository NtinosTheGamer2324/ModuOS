/*
 * mdtaskbar.c — bottom-of-screen taskbar for ModuOS.
 *
 * A chrome-less WM_FLAG_DOCK window pinned to WM_DOCK_EDGE_BOTTOM. Polls
 * wm_list_windows() every tick and draws one button per live, non-dock
 * window. Click a button:
 *   - if that window isn't focused -> wm_focus() (also un-minimizes it)
 *   - if it IS already the focused window -> wm_minimize() it (click-to-hide,
 *     same convention as most taskbars)
 * The focused window's button is drawn highlighted; a minimized window's
 * button is drawn dimmed so you can tell at a glance what's hidden.
 *
 * No special-cased mdw support — same wm_protocol.h any other client uses.
 */

#include "libc.h"
#include "string.h"
#include "lib_fnt.h"
#include "wm_protocol.h"
#include <stdint.h>

#define FONT_PRIMARY  "/ModuOS/shared/assets/fonts/Terminus.fnt"
#define FONT_FALLBACK "/ModuOS/shared/assets/fonts/Unicode.fnt"

#define BAR_H       32
#define PAD         6
#define BTN_GAP     4
#define BTN_MIN_W   90
#define BTN_MAX_W   180
#define POLL_MS     150   /* window list doesn't need per-frame freshness */
#define TICK_MS     16

#define C_BG          0xFF14171Du
#define C_BTN         0xFF262C38u
#define C_BTN_HOVER   0xFF323A4Au
#define C_BTN_FOCUSED 0xFF3B5B8Cu
#define C_BTN_MIN     0xFF1B1F27u
#define C_TEXT        0xFFDCE2EEu
#define C_TEXT_DIM    0xFF7A8194u
#define C_EDGE        0xFF3A4152u

static uint32_t   *g_buf;
static int          g_W = 0; /* full screen width, thickness is BAR_H */
static fnt_font_t  *g_font;

static wm_win_info_t g_wins[WM_MAX_WINDOWS];
static int            g_win_count = 0;

typedef struct { int x, y, w, h; int win_id; } tbtn_t;
static tbtn_t g_btns[WM_MAX_WINDOWS];
static int    g_btn_count = 0;

static int hover_btn = -1;
static int dirty = 1;
static int quit = 0;

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

static void fill_rect(int x, int y, int w, int h, uint32_t col) {
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > g_W) x1 = g_W;
    if (y1 > BAR_H) y1 = BAR_H;
    for (int yy = y; yy < y1; yy++)
        for (int xx = x; xx < x1; xx++)
            g_buf[(size_t)yy * g_W + xx] = col;
}

static void frame_rect(int x, int y, int w, int h, uint32_t col) {
    fill_rect(x, y, w, 1, col);
    fill_rect(x, y + h - 1, w, 1, col);
    fill_rect(x, y, 1, h, col);
    fill_rect(x + w - 1, y, 1, h, col);
}

static int text_w(const char *s) {
    if (!g_font || !s) return 0;
    int w = 0;
    while (*s) { fnt_glyph_t *gl = fnt_get_glyph(g_font, (uint32_t)(unsigned char)*s); if (gl) w += gl->width; s++; }
    return w;
}

static void draw_text(int x, int y, const char *s, uint32_t col) {
    if (!g_font || !s) return;
    int cx = x;
    while (*s) {
        fnt_glyph_t *gl = fnt_get_glyph(g_font, (uint32_t)(unsigned char)*s);
        if (gl) {
            for (int dy = 0; dy < gl->bitmap_height; dy++)
                for (int dx = 0; dx < gl->bitmap_width; dx++)
                    if (fnt_get_pixel(gl, dx, dy)) {
                        int px = cx + dx, py = y + dy;
                        if (px >= 0 && px < g_W && py >= 0 && py < BAR_H)
                            g_buf[(size_t)py * g_W + px] = col;
                    }
            cx += gl->width;
        }
        s++;
    }
}

/* Truncate s in-place-ish into out (cap incl NUL) so it fits max_w px,
 * appending ".." if it had to cut. */
static void fit_label(const char *s, char *out, int cap, int max_w) {
    int n = 0;
    while (s[n] && n < cap - 1) n++;
    int i;
    for (i = 0; i <= n; i++) { out[i] = i < n ? s[i] : '\0'; }
    out[n] = '\0';
    if (text_w(out) <= max_w) return;

    /* binary-ish shrink: chop from the end until "label.." fits */
    int len = n;
    while (len > 0) {
        out[len] = '\0';
        char tmp[64];
        int tn = 0;
        while (out[tn] && tn < (int)sizeof(tmp) - 3) { tmp[tn] = out[tn]; tn++; }
        tmp[tn] = '.'; tmp[tn+1] = '.'; tmp[tn+2] = '\0';
        if (text_w(tmp) <= max_w || len == 1) {
            int i2 = 0;
            while (tmp[i2] && i2 < cap - 1) { out[i2] = tmp[i2]; i2++; }
            out[i2] = '\0';
            return;
        }
        len--;
    }
}

static void layout_buttons(void) {
    g_btn_count = 0;
    int x = PAD;
    for (int i = 0; i < g_win_count; i++) {
        if (g_wins[i].is_dock) continue; /* never show dock/taskbar windows themselves */
        if (g_wins[i].is_desktop) continue; /* the desktop background isn't a taskbar entry either */
        if (g_btn_count >= WM_MAX_WINDOWS) break;

        int w = text_w(g_wins[i].title) + 20;
        if (w < BTN_MIN_W) w = BTN_MIN_W;
        if (w > BTN_MAX_W) w = BTN_MAX_W;
        if (x + w > g_W - PAD) w = g_W - PAD - x;
        if (w <= 0) break;

        tbtn_t *b = &g_btns[g_btn_count++];
        b->x = x; b->y = PAD / 2; b->w = w; b->h = BAR_H - PAD;
        b->win_id = g_wins[i].win_id;

        x += w + BTN_GAP;
    }
}

static wm_win_info_t *find_win(int win_id) {
    for (int i = 0; i < g_win_count; i++)
        if (g_wins[i].win_id == win_id) return &g_wins[i];
    return NULL;
}

static void render(void) {
    fill_rect(0, 0, g_W, BAR_H, C_BG);
    fill_rect(0, 0, g_W, 1, C_EDGE);

    for (int i = 0; i < g_btn_count; i++) {
        tbtn_t *b = &g_btns[i];
        wm_win_info_t *w = find_win(b->win_id);
        if (!w) continue;

        int is_min = (w->state == WM_STATE_MINIMIZED);
        int is_hot = (hover_btn == i);

        uint32_t bg = is_min ? C_BTN_MIN : (w->focused ? C_BTN_FOCUSED : (is_hot ? C_BTN_HOVER : C_BTN));
        fill_rect(b->x, b->y, b->w, b->h, bg);
        frame_rect(b->x, b->y, b->w, b->h, is_hot ? 0xFF6FB2FFu : C_EDGE);

        char label[64];
        fit_label(w->title[0] ? w->title : "(untitled)", label, sizeof(label), b->w - 12);
        uint32_t tcol = is_min ? C_TEXT_DIM : C_TEXT;
        draw_text(b->x + 6, b->y + (b->h - 8) / 2, label, tcol);
    }
}

static int hit_button(int x, int y) {
    for (int i = 0; i < g_btn_count; i++) {
        tbtn_t *b = &g_btns[i];
        if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) return i;
    }
    return -1;
}

static void handle_mouse(wm_shm_t *mb, const wm_mouse_event_t *m) {
    if (m->type == WM_MOUSE_LEAVE) {
        if (hover_btn != -1) dirty = 1;
        hover_btn = -1;
        return;
    }

    int hb = hit_button(m->x, m->y);
    if (hb != hover_btn) { hover_btn = hb; dirty = 1; }

    if (m->type == WM_MOUSE_UP && (m->changed & WM_MBTN_LEFT)) {
        if (hb < 0) return;
        wm_win_info_t *w = find_win(g_btns[hb].win_id);
        if (!w) return;

        if (w->focused && w->state != WM_STATE_MINIMIZED) {
            wm_minimize(mb, w->win_id);
        } else {
            wm_focus(mb, w->win_id);
        }
        dirty = 1;
    }
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    wm_shm_t *mb = wm_ipc_connect(3000);
    if (!mb) { puts("mdtaskbar: mdw never came up (waited 3s)\n"); return 1; }

    /* Thickness only; width/span is computed by mdw for a dock window. */
    wm_response_t resp;
    if (wm_dock_create(mb, "Taskbar", WM_DOCK_EDGE_BOTTOM, BAR_H, &resp) != 0 || !resp.success) {
        printf("mdtaskbar: dock create failed: %s\n", resp.message);
        return 1;
    }

    int shmfd = shm_open(resp.shm_name, O_RDWR, 0, 0);
    if (shmfd < 0) { puts("mdtaskbar: shm_open failed\n"); return 1; }
    g_buf = (uint32_t *)mmap(NULL, (size_t)resp.shm_size, PROT_R | PROT_W, MAP_SHARED, shmfd);
    if (g_buf == MAP_FAILED) { puts("mdtaskbar: mmap failed\n"); return 1; }

    /* Full window content width = shm_size / (BAR_H * 4 bytes/px). */
    g_W = (int)(resp.shm_size / (uint64_t)(BAR_H * 4));
    if (g_W <= 0) { puts("mdtaskbar: bad dock geometry\n"); return 1; }

    wm_mouse_ring_t *mice = NULL;
    int mousefd = shm_open(resp.mouse_shm_name, O_RDWR, 0, 0);
    if (mousefd >= 0) {
        void *p = mmap(NULL, (size_t)resp.mouse_shm_size, PROT_R | PROT_W, MAP_SHARED, mousefd);
        if (p != MAP_FAILED) mice = (wm_mouse_ring_t *)p;
    }
    if (!mice) puts("mdtaskbar: no mouse ring — clicking disabled\n");

    g_font = try_load_font(FONT_PRIMARY);
    if (!g_font) g_font = try_load_font(FONT_FALLBACK);

    g_win_count = wm_list_windows(mb, g_wins);
    if (g_win_count < 0) g_win_count = 0;
    layout_buttons();

    render();
    wm_damage(mb, resp.win_id, 0, 0, g_W, BAR_H);

    uint64_t next_tick = time_ms();
    uint64_t next_poll = time_ms();

    while (!quit) {
        if (mice) {
            wm_mouse_event_t m;
            while (wm_mouse_ring_pop(mice, &m)) handle_mouse(mb, &m);
        }

        if (time_ms() >= next_poll) {
            int n = wm_list_windows(mb, g_wins);
            if (n >= 0) {
                /* Cheap change check: redraw whenever count or any
                 * focus/state/title differs from last snapshot. Simplest
                 * correct approach is just "always mark dirty" here since
                 * polling is already throttled to POLL_MS. */
                g_win_count = n;
                layout_buttons();
                dirty = 1;
            }
            next_poll = time_ms() + POLL_MS;
        }

        if (dirty) {
            render();
            wm_damage(mb, resp.win_id, 0, 0, g_W, BAR_H);
            dirty = 0;
        }

        next_tick += TICK_MS;
        while (time_ms() < next_tick) yield();
        if (time_ms() > next_tick + 200) next_tick = time_ms();
    }

    if (g_font) fnt_free_font(g_font);
    return 0;
}