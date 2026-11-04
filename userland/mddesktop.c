/*
 * mddesktop.c — ModuOS desktop shell.
 *
 * A WM_FLAG_DESKTOP client: full-screen, chrome-less, always drawn behind
 * every other window, never focusable/raisable/minimizable/maximizable.
 * See WM_FLAG_DESKTOP in wm_protocol.h for exactly what mdw guarantees.
 *
 * Shows one icon per entry of the root directory. No user-account system
 * exists yet, so the desktop root is hardcoded to "/" -- once accounts
 * exist this should become something like "/home/<user>/Desktop" instead
 * (see ROOT_PATH below).
 *
 * Single-click selects an icon; double-click opens it:
 *   - a directory launches mdfm at that path
 *   - a ".sqr" file is exec'd directly, same restriction mdrun.c enforces
 *   - anything else just reports "no default app" -- there's no file-
 *     association system in ModuOS yet to consult
 *
 * Icon set and the extension->icon mapping mirror mdfm.c's exactly (see
 * ilib.h) so a file looks the same whether you're looking at it on the
 * desktop or in the file manager.
 */

#include "libc.h"
#include "string.h"
#include "lib_fnt.h"
#include "wm_protocol.h"
#include "wm_client_resize.h"
#include "ilib.h"
#include <stdint.h>

/* Hardcoded per the current lack of a user-account system -- see the file
 * header comment above. The only thing that should need to change once
 * accounts exist is this one constant. */
#define ROOT_PATH "/"

/* mdfm.c isn't necessarily installed at this exact path in every build --
 * update this if mdfm.sqr lives somewhere else. Matches the "/Apps/" pin
 * mdfm.c's own sidebar uses, which is the closest thing to a documented
 * install convention in this codebase. */
#define MDFM_PATH "/Apps/mdfm.sqr"

#define FONT_PRIMARY  "/ModuOS/shared/assets/fonts/Terminus.fnt"
#define FONT_FALLBACK "/ModuOS/shared/assets/fonts/Unicode.fnt"
#define ICONS_PATH    "/ModuOS/shared/assets/icons.ilib"

/* Same icon set/order as mdfm.c -- keep these two files' enums in sync. */
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
#define ICON_SIZE             32   /* native ilib resolution -- drawn 1:1, no downsampling */

#define MAX_ENTRIES  256
#define NAME_CAP     260
#define PATH_CAP     512

#define CELL_W      90
#define CELL_H      92
#define GRID_PAD    16   /* margin from the (reserved-adjusted) screen edge */
#define LABEL_H     16
#define DCLICK_MS   450  /* max gap between clicks to count as a double-click */

#define C_BG          0xFF14181Fu  /* matches mdw's own flat desktop fill */
#define C_LABEL       0xFFDCE2EEu
#define C_LABEL_SHADOW 0xFF000000u
#define C_SEL_BG      0xFF2A3A55u
#define C_SEL_EDGE    0xFF6FB2FFu
#define C_STATUS      0xFFB8C2D6u
#define C_STATUS_ERR  0xFFE0483Fu

typedef struct { char name[NAME_CAP]; int is_dir; uint32_t size; } entry_t;

static entry_t g_entries[MAX_ENTRIES];
static int      g_nentries = 0;
static int      g_sel = -1;
static int      g_last_click_idx = -1;
static uint64_t g_last_click_ms = 0;

static char     g_status[160];
static uint64_t g_status_until_ms = 0;
static uint32_t g_status_col = C_STATUS;

static uint32_t   *g_buf;
static fnt_font_t *g_font;
static int          g_W = 0, g_H = 0;

/* Reserved margins, same concept as mdw's own wm_compute_reserved -- sum
 * of dock thickness per edge, read straight off wm_list_windows() rather
 * than inferred from geometry now that wm_win_info_t exposes dock_edge
 * directly. Icons stay inside this inset so a bottom-docked taskbar never
 * covers them. */
static int g_res_top = 0, g_res_bottom = 0, g_res_left = 0, g_res_right = 0;

static ilib_t      *g_icons;
static ilib_image_t g_icon_img[ICON_COUNT];
static int          g_icon_ok[ICON_COUNT];

/* -- small string helpers, same as mdfm.c ------------------------------ */

static int sl(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void scat(char *dst, int cap, const char *s) {
    int n = sl(dst);
    while (*s && n < cap - 1) dst[n++] = *s++;
    dst[n] = '\0';
}

static void sset(char *dst, int cap, const char *s) { dst[0] = '\0'; scat(dst, cap, s); }

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void path_join(char *out, int cap, const char *base, const char *name) {
    sset(out, cap, base);
    int n = sl(out);
    if (n > 0 && out[n - 1] != '/') scat(out, cap, "/");
    scat(out, cap, name);
}

static void set_status(const char *s, uint32_t col) {
    sset(g_status, sizeof(g_status), s);
    g_status_col = col;
    g_status_until_ms = time_ms() + 3000;
}

/* -- icons, same loading/blit pattern as mdfm.c ------------------------- */

static void icons_load(void) {
    for (int i = 0; i < ICON_COUNT; i++) g_icon_ok[i] = 0;
    g_icons = ilib_open(ICONS_PATH);
    if (!g_icons) return;
    for (int i = 0; i < ICON_COUNT; i++)
        if (ilib_load_image(g_icons, (uint16_t)i, &g_icon_img[i]) == ILIB_OK) g_icon_ok[i] = 1;
}

static void icons_free(void) {
    for (int i = 0; i < ICON_COUNT; i++) if (g_icon_ok[i]) ilib_free_image(&g_icon_img[i]);
    if (g_icons) ilib_close(g_icons);
}

static void draw_icon(int id, int x, int y) {
    if (id < 0 || id >= ICON_COUNT || !g_icon_ok[id]) return;
    ilib_image_t *img = &g_icon_img[id];
    if (img->width == 0 || img->height == 0) return;

    for (int dy = 0; dy < ICON_SIZE; dy++) {
        int py = y + dy;
        if (py < 0 || py >= g_H) continue;
        int sy = (dy * img->height) / ICON_SIZE;
        for (int dx = 0; dx < ICON_SIZE; dx++) {
            int px = x + dx;
            if (px < 0 || px >= g_W) continue;
            int sx = (dx * img->width) / ICON_SIZE;
            const uint8_t *p = &img->pixels[((size_t)sy * img->width + sx) * 4];
            uint8_t r = p[0], gc = p[1], b = p[2], a = p[3];
            if (a == 0) continue;
            uint32_t *dst = &g_buf[(size_t)py * g_W + px];
            if (a == 255) {
                *dst = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)gc << 8) | b;
            } else {
                uint32_t old = *dst;
                uint8_t or_ = (uint8_t)(old >> 16), og = (uint8_t)(old >> 8), ob = (uint8_t)old;
                uint8_t nr = (uint8_t)((r * a + or_ * (255 - a)) / 255);
                uint8_t ng = (uint8_t)((gc * a + og * (255 - a)) / 255);
                uint8_t nb = (uint8_t)((b * a + ob * (255 - a)) / 255);
                *dst = 0xFF000000u | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | nb;
            }
        }
    }
}

static int icon_for_entry(const entry_t *e) {
    if (e->is_dir) return ICON_FOLDER_CLOSED;
    const char *name = e->name;
    int len = sl(name);
    int dot = -1;
    for (int i = len - 1; i >= 0; i--) { if (name[i] == '.') { dot = i; break; } if (name[i] == '/') break; }
    if (dot < 0) return ICON_UNKNOWN_FILE;
    const char *ext = name + dot + 1;
    if (str_eq(ext, "txt") || str_eq(ext, "md") || str_eq(ext, "log")) return ICON_RICH_DOC;
    if (str_eq(ext, "doc") || str_eq(ext, "docx")) return ICON_WORD;
    if (str_eq(ext, "mp3") || str_eq(ext, "wav") || str_eq(ext, "ogg")) return ICON_MUSIC;
    if (str_eq(ext, "zip") || str_eq(ext, "tar") || str_eq(ext, "gz")) return ICON_ARCHIVE;
    if (str_eq(ext, "bin") || str_eq(ext, "exe") || str_eq(ext, "elf") || str_eq(ext, "sqr")) return ICON_BIN;
    if (str_eq(ext, "html") || str_eq(ext, "htm")) return ICON_IE;
    return ICON_UNKNOWN_FILE;
}

static int ends_with_sqr(const char *s) {
    int n = sl(s);
    return n >= 4 && s[n-4]=='.' && s[n-3]=='s' && s[n-2]=='q' && s[n-1]=='r';
}

/* -- drawing ------------------------------------------------------------ */

static void fill_rect(int x, int y, int w, int h, uint32_t col) {
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > g_W) x1 = g_W;
    if (y1 > g_H) y1 = g_H;
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

static void draw_text_clip(int x, int y, const char *s, uint32_t col, int clip_x1) {
    if (!g_font || !s) return;
    int cx = x;
    while (*s) {
        fnt_glyph_t *gl = fnt_get_glyph(g_font, (uint32_t)(unsigned char)*s);
        if (gl) {
            if (cx + gl->width > clip_x1) break;
            for (int dy = 0; dy < gl->bitmap_height; dy++)
                for (int dx = 0; dx < gl->bitmap_width; dx++)
                    if (fnt_get_pixel(gl, dx, dy)) {
                        int px = cx + dx, py = y + dy;
                        if (px >= 0 && px < g_W && py >= 0 && py < g_H)
                            g_buf[(size_t)py * g_W + px] = col;
                    }
            cx += gl->width;
        }
        s++;
    }
}

static void draw_text(int x, int y, const char *s, uint32_t col) { draw_text_clip(x, y, s, col, g_W); }

/* Text over an arbitrary background needs to stay legible -- a 1px black
 * offset copy underneath reads fine against any of this UI's dark tones
 * without needing real alpha blending. */
static void draw_label_shadowed(int x, int y, const char *s, uint32_t col) {
    draw_text_clip(x + 1, y + 1, s, C_LABEL_SHADOW, g_W);
    draw_text_clip(x, y, s, col, g_W);
}

static void fit_label(const char *s, char *out, int cap, int max_w) {
    int n = 0; while (s[n] && n < cap - 1) n++;
    for (int i = 0; i <= n; i++) out[i] = (i < n) ? s[i] : '\0';
    if (text_w(out) <= max_w) return;
    int len = n;
    while (len > 0) {
        out[len] = '\0';
        char tmp[64]; int tn = 0;
        while (out[tn] && tn < (int)sizeof(tmp) - 3) { tmp[tn] = out[tn]; tn++; }
        tmp[tn] = '.'; tmp[tn+1] = '.'; tmp[tn+2] = '\0';
        if (text_w(tmp) <= max_w || len == 1) {
            int i2 = 0; while (tmp[i2] && i2 < cap - 1) { out[i2] = tmp[i2]; i2++; }
            out[i2] = '\0';
            return;
        }
        len--;
    }
}

/* -- grid layout ---------------------------------------------------------
 * Column-major (fill a column top-to-bottom before starting the next),
 * the classic desktop-icon convention -- avoids a ragged last row eating
 * into whatever margin is reserved at the bottom edge. */
static int cols_visible(void) {
    int usable_h = g_H - g_res_top - g_res_bottom - GRID_PAD * 2;
    int rows = usable_h / CELL_H;
    return rows < 1 ? 1 : rows;
}

static void cell_pos(int idx, int *out_x, int *out_y) {
    int rows = cols_visible();
    int col = idx / rows;
    int row = idx % rows;
    *out_x = g_res_left + GRID_PAD + col * CELL_W;
    *out_y = g_res_top + GRID_PAD + row * CELL_H;
}

static int cell_at(int mx, int my) {
    int rows = cols_visible();
    int cx = mx - (g_res_left + GRID_PAD);
    int cy = my - (g_res_top + GRID_PAD);
    if (cx < 0 || cy < 0) return -1;
    int col = cx / CELL_W, row = cy / CELL_H;
    if (row >= rows) return -1;
    int idx = col * rows + row;
    if (idx < 0 || idx >= g_nentries) return -1;
    return idx;
}

/* -- filesystem ----------------------------------------------------------
 * Same opendir/readdir/closedir semantics as mdfm.c/ls.c/rm.c. */
static void refresh_listing(void) {
    g_nentries = 0;
    int d = opendir(ROOT_PATH);
    if (d < 0) { set_status("Cannot read root directory.", C_STATUS_ERR); return; }

    char name[NAME_CAP]; int is_dir = 0; uint32_t size = 0;
    while (g_nentries < MAX_ENTRIES) {
        int rc = readdir(d, name, sizeof(name), &is_dir, &size);
        if (rc <= 0) break;
        if (str_eq(name, ".") || str_eq(name, "..")) continue;
        entry_t *e = &g_entries[g_nentries];
        sset(e->name, sizeof(e->name), name);
        e->is_dir = is_dir;
        e->size = size;
        g_nentries++;
    }
    closedir(d);
}

static void compute_reserved(wm_win_info_t *wins, int n) {
    int top = 0, bottom = 0, left = 0, right = 0;
    for (int i = 0; i < n; i++) {
        if (!wins[i].is_dock) continue;
        switch (wins[i].dock_edge) {
            case WM_DOCK_EDGE_TOP:    top    += wins[i].h; break;
            case WM_DOCK_EDGE_BOTTOM: bottom += wins[i].h; break;
            case WM_DOCK_EDGE_LEFT:   left   += wins[i].w; break;
            case WM_DOCK_EDGE_RIGHT:  right  += wins[i].w; break;
        }
    }
    g_res_top = top; g_res_bottom = bottom; g_res_left = left; g_res_right = right;
}

/* -- launching -------------------------------------------------------- */

static void open_entry(int idx) {
    if (idx < 0 || idx >= g_nentries) return;
    entry_t *e = &g_entries[idx];
    char path[PATH_CAP];
    path_join(path, sizeof(path), ROOT_PATH, e->name);

    if (e->is_dir) {
        int pid = fork();
        if (pid < 0) { set_status("fork() failed.", C_STATUS_ERR); return; }
        if (pid == 0) {
            char *argv[3]; argv[0] = MDFM_PATH; argv[1] = path; argv[2] = NULL;
            execve(MDFM_PATH, argv, NULL);
            exit(1);
        }
        set_status("Opening in File Manager...", C_STATUS);
        return;
    }

    if (!ends_with_sqr(e->name)) {
        set_status("No default app for this file type yet.", C_STATUS_ERR);
        return;
    }

    int pid = fork();
    if (pid < 0) { set_status("fork() failed.", C_STATUS_ERR); return; }
    if (pid == 0) {
        char *argv[2]; argv[0] = path; argv[1] = NULL;
        execve(path, argv, NULL);
        exit(1);
    }
    set_status("Launched.", C_STATUS);
}

/* -- render -------------------------------------------------------------- */

static void render(void) {
    fill_rect(0, 0, g_W, g_H, C_BG);

    for (int i = 0; i < g_nentries; i++) {
        int cx, cy; cell_pos(i, &cx, &cy);
        if (cy + CELL_H > g_H - g_res_bottom) continue; /* would land under a reserved edge */

        entry_t *e = &g_entries[i];
        int icon_x = cx + (CELL_W - ICON_SIZE) / 2;
        int icon_y = cy + 6;

        if (i == g_sel) {
            fill_rect(cx + 3, cy, CELL_W - 6, ICON_SIZE + LABEL_H + 12, C_SEL_BG);
            frame_rect(cx + 3, cy, CELL_W - 6, ICON_SIZE + LABEL_H + 12, C_SEL_EDGE);
        }

        draw_icon(icon_for_entry(e), icon_x, icon_y);

        char label[64];
        fit_label(e->name, label, sizeof(label), CELL_W - 8);
        int lx = cx + (CELL_W - text_w(label)) / 2;
        draw_label_shadowed(lx, icon_y + ICON_SIZE + 6, label, C_LABEL);
    }

    if (g_status[0] && time_ms() < g_status_until_ms) {
        draw_label_shadowed(g_res_left + GRID_PAD, g_H - g_res_bottom - GRID_PAD - 14, g_status, g_status_col);
    }
}

/* -- input --------------------------------------------------------------- */

static void handle_mouse(const wm_mouse_event_t *m) {
    if (m->type != WM_MOUSE_UP || !(m->changed & WM_MBTN_LEFT)) return;

    int idx = cell_at(m->x, m->y);
    if (idx < 0) { g_sel = -1; return; }

    uint64_t now = time_ms();
    int is_dclick = (idx == g_last_click_idx) && (now - g_last_click_ms) < DCLICK_MS;
    g_last_click_idx = idx;
    g_last_click_ms = now;

    g_sel = idx;
    if (is_dclick) { open_entry(idx); g_last_click_idx = -1; /* don't chain into a triple-click */ }
}

static void handle_key(const KeyboardEventData *k) {
    if ((char)k->ascii == 27) g_sel = -1; /* Esc: deselect */
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

/* -- entry point ----------------------------------------------------------
 * See WM_FLAG_DESKTOP in wm_protocol.h: this window is always full-screen
 * (req.w/h are ignored), always drawn behind every other window, and
 * can't be minimized/maximized/fullscreened/moved/resized -- so unlike
 * every other client in this codebase, the resize half of
 * wm_client_resize.h is mostly moot here. It's still used for the same
 * "-1 = torn down elsewhere, exit cleanly" detection every other client
 * gets, and to read back the actual screen size mdw granted (the CREATE
 * response itself doesn't carry w/h -- see wm_desktop_create()'s comment
 * in wm_protocol.h). */
int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    wm_shm_t *mb = wm_ipc_connect(3000);
    if (!mb) { puts("mddesktop: mdw never came up (waited 3s)\n"); return 1; }

    wm_response_t resp;
    if (wm_desktop_create(mb, "Desktop", &resp) != 0 || !resp.success) {
        printf("mddesktop: create failed: %s\n", resp.message);
        return 1;
    }

    int shmfd = shm_open(resp.shm_name, O_RDWR, 0, 0);
    if (shmfd < 0) { puts("mddesktop: shm_open failed\n"); return 1; }
    g_buf = (uint32_t *)mmap(NULL, (size_t)resp.shm_size, PROT_R | PROT_W, MAP_SHARED, shmfd);
    if (g_buf == MAP_FAILED) { puts("mddesktop: mmap failed\n"); return 1; }

    wm_key_ring_t *keys = NULL;
    int keyfd = shm_open(resp.key_shm_name, O_RDWR, 0, 0);
    if (keyfd >= 0) {
        void *p = mmap(NULL, (size_t)resp.key_shm_size, PROT_R | PROT_W, MAP_SHARED, keyfd);
        if (p != MAP_FAILED) keys = (wm_key_ring_t *)p;
    }
    wm_mouse_ring_t *mice = NULL;
    int mousefd = shm_open(resp.mouse_shm_name, O_RDWR, 0, 0);
    if (mousefd >= 0) {
        void *p = mmap(NULL, (size_t)resp.mouse_shm_size, PROT_R | PROT_W, MAP_SHARED, mousefd);
        if (p != MAP_FAILED) mice = (wm_mouse_ring_t *)p;
    }
    if (!mice) { puts("mddesktop: no mouse ring (mdw too old?)\n"); return 1; }

    /* Learn the actual screen size mdw granted us (see the CREATE-response
     * caveat in the comment above) and any dock-reserved margins, in one
     * shot. */
    wm_win_info_t wins[WM_MAX_WINDOWS];
    int nwins = wm_list_windows(mb, wins);
    if (nwins < 0) { puts("mddesktop: couldn't read back screen size\n"); return 1; }
    for (int i = 0; i < nwins; i++) {
        if (wins[i].win_id != resp.win_id) continue;
        g_W = wins[i].w; g_H = wins[i].h;
        break;
    }
    if (g_W <= 0 || g_H <= 0) { puts("mddesktop: bad screen geometry\n"); return 1; }
    compute_reserved(wins, nwins);

    wm_client_surface_t surf;
    wm_client_surface_init(&surf, g_buf, resp.shm_name, resp.shm_size, g_W, g_H);

    g_font = try_load_font(FONT_PRIMARY);
    if (!g_font) g_font = try_load_font(FONT_FALLBACK);
    icons_load();

    refresh_listing();

    render();
    wm_damage(mb, resp.win_id, 0, 0, g_W, g_H);

    int dirty = 0;
    int quit = 0;
    uint64_t next_tick = time_ms();
    uint64_t next_resize_poll = time_ms();
    uint64_t next_rescan = time_ms() + 3000;

    while (!quit) {
        wm_mouse_event_t m;
        while (wm_mouse_ring_pop(mice, &m)) { handle_mouse(&m); dirty = 1; }
        if (keys) {
            KeyboardEventData k;
            while (wm_key_ring_pop(keys, &k)) { handle_key(&k); dirty = 1; }
        }

        /* Mostly just close-detection here -- see the entry-point comment
         * above for why this window is never actually resized. */
        if (time_ms() >= next_resize_poll) {
            int rc = wm_client_surface_poll(mb, resp.win_id, &surf);
            if (rc < 0) { quit = 1; break; }
            if (rc > 0) { g_buf = surf.buf; g_W = surf.w; g_H = surf.h; dirty = 1; }
            next_resize_poll = time_ms() + 500;
        }

        if (time_ms() >= next_rescan) {
            int before = g_nentries;
            refresh_listing();
            if (g_nentries != before) dirty = 1;
            next_rescan = time_ms() + 3000;
        }

        if (g_status[0] && time_ms() >= g_status_until_ms) { g_status[0] = '\0'; dirty = 1; }

        if (dirty) {
            render();
            wm_damage(mb, resp.win_id, 0, 0, g_W, g_H);
            dirty = 0;
        }

        next_tick += 16;
        while (time_ms() < next_tick) yield();
        if (time_ms() > next_tick + 200) next_tick = time_ms();
    }

    if (surf.buf) munmap(surf.buf, (size_t)surf.shm_size);
    if (g_font) fnt_free_font(g_font);
    icons_free();
    return 0;
}