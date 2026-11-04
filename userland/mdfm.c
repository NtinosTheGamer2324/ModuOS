/*
 * mdfm.c — mdfm, the ModuOS graphical File Manager.
 *
 * An mdw client in the same style as mdparted_gui.c: draws into its own
 * SHM pixel buffer, reads clicks from the per-window mouse ring, reads
 * typing from the per-window key ring, and only repaints when something
 * actually changed.
 *
 * Layout:
 *   +----------+---------------------------------------------+
 *   | Sidebar  | Toolbar (path bar, Up, New Folder, Delete…)  |
 *   | pinned   +---------------------------------------------+
 *   | folders  | File list (name / dir marker / size)         |
 *   |          |                                               |
 *   |          +---------------------------------------------+
 *   |          | Status / log line                            |
 *   +----------+---------------------------------------------+
 *
 * Filesystem operations reuse the exact syscall-level semantics the
 * console tools already established:
 *   - opendir/readdir/closedir + stat()->is_directory,  same as ls.c/rm.c
 *   - mkdir(),                                          same as mkdir.c
 *   - unlink()/rmdir() (recursive delete),               same as rm.c
 *   - copy-then-unlink for "move" (no rename syscall),   same as mv.c
 *
 * Destructive actions (Delete) require a typed "yes" in the same
 * confirm-field flow mdparted_gui.c uses — a stray click can never
 * wipe a folder.
 *
 * Double-click a row to open it (directories navigate in, files are
 * just selected — mdfm does not attempt to exec/associate them here).
 *
 * Usage:
 *   mdfm            → opens at $/  (DevVFS root)
 *   mdfm <path>      → opens at that path
 */

#include "libc.h"
#include "string.h"
#include "lib_fnt.h"
#include "wm_protocol.h"
#include "wm_client_resize.h"
#include "ilib.h"
#include <stdint.h>

#define FONT_PRIMARY  "/ModuOS/shared/assets/fonts/Terminus.fnt"
#define FONT_FALLBACK "/ModuOS/shared/assets/fonts/Unicode.fnt"
#define ICONS_PATH    "/ModuOS/shared/assets/icons.ilib"

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

/* -- Limits ------------------------------------------------------------ */
#define MAX_ENTRIES     512
#define MAX_PINS        8
#define PATH_CAP        384
#define NAME_CAP        260

/* -- Window geometry ----------------------------------------------------
 * Two-column layout: fixed-width sidebar on the left, content pane fills
 * the rest.
 */
#define WIN_W       760
#define WIN_H       480

#define SIDEBAR_W   170

#define PAD           10
#define TOOLBAR_Y     8
#define TOOLBAR_H     26
#define PATHBAR_Y     40
#define PATHBAR_H     20
#define LIST_HDR_Y    68
#define LIST_Y        84
#define ROW_H         22
#define STATUS_H      22

#define BTN_W         78
#define BTN_H         22
#define BTN_GAP       6

#define SIDE_HDR_Y    10
#define SIDE_ITEM_Y0  28
#define SIDE_ITEM_H   26
#define SIDE_PAD      10

#define PROMPT_Y      (WIN_H - STATUS_H - 46)
#define FIELD_Y       (WIN_H - STATUS_H - 26)
#define FIELD_H       20

/* -- Palette (0xAARRGGBB, opaque: no WM_FLAG_HAS_ALPHA, fast blit path) */
#define C_BG                0xFF1B1F27u
#define C_SIDEBAR           0xFF171A21u
#define C_SIDEBAR_SEL       0xFF2F6FE0u
#define C_SIDEBAR_HOVER     0xFF232935u
#define C_ROW               0xFF1F242Eu
#define C_ROW_ALT           0xFF232935u
#define C_ROW_SEL           0xFF2F6FE0u
#define C_ROW_HOVER         0xFF2C3446u
#define C_TEXT              0xFFDCE2EEu
#define C_TEXT_DIM          0xFF8C94A6u
#define C_ACCENT            0xFF6FB2FFu
#define C_OK                0xFF5FD37Fu
#define C_WARN              0xFFE8C468u
#define C_ERR               0xFFE0483Fu
#define C_BTN               0xFF303848u
#define C_BTN_HOVER         0xFF3D4759u
#define C_BTN_DOWN          0xFF232B39u
#define C_BTN_DANGER        0xFF5A2E2Bu
#define C_BTN_DANGER_HOVER  0xFF7A3B37u
#define C_BTN_EDGE          0xFF454F63u
#define C_FIELD             0xFF11151Cu
#define C_RULE              0xFF39415Bu
#define C_DIR_ICON          0xFFE8C468u
#define C_FILE_ICON         0xFF8C94A6u

/* -- Tiny string helpers (same shape as mdparted_gui.c's) -------------- */

static int sl(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void scat(char *dst, int cap, const char *s) {
    int n = sl(dst);
    while (*s && n < cap - 1) dst[n++] = *s++;
    dst[n] = '\0';
}

static void scat_u(char *dst, int cap, uint64_t v) {
    char t[24];
    int n = 0;
    if (v == 0) t[n++] = '0';
    while (v > 0 && n < (int)sizeof(t)) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    int d = sl(dst);
    for (int i = n - 1; i >= 0 && d < cap - 1; i--) dst[d++] = t[i];
    dst[d] = '\0';
}

static void sset(char *dst, int cap, const char *s) { dst[0] = '\0'; scat(dst, cap, s); }

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void fmt_size(uint32_t bytes, char *out, int cap) {
    out[0] = '\0';
    if (bytes < 1024) { scat_u(out, cap, bytes); scat(out, cap, "B"); return; }
    uint32_t kb = bytes / 1024;
    if (kb < 1024) { scat_u(out, cap, kb); scat(out, cap, "K"); return; }
    uint32_t mb = kb / 1024;
    if (mb < 1024) { scat_u(out, cap, mb); scat(out, cap, "M"); return; }
    scat_u(out, cap, mb / 1024); scat(out, cap, "G");
}

/* -- Path helpers --------------------------------------------------------
 * Two root namespaces exist side by side in ModuOS: the plain Unix-style
 * tree rooted at "/" and DevVFS rooted at "$/". Both are just strings to
 * us — opendir/stat/mkdir etc. all take whichever prefix the path has.
 */

static int path_is_root(const char *p) {
    return str_eq(p, "/") || str_eq(p, "$/") || str_eq(p, "$");
}

/* Write ops (mkdir/rmdir/unlink/file create) only work in two places on
 * this kernel:
 *   - anywhere under "/"                       (the Unix-style tree)
 *   - "$/mnt/vDriveN-pX/..."                    (a mounted partition)
 * $/ itself and its other top-level children ($/dev/, $/user/, and a
 * bare $/mnt/ with no mounted-partition subpath) are kernel-special and
 * reject writes outright. */
static int path_supports_write(const char *p) {
    if (p[0] == '/') return 1; /* whole Unix tree */

    if (p[0] == '$' && p[1] == '/') {
        const char *rest = p + 2;
        const char *mnt = "mnt/";
        int i = 0;
        while (mnt[i] && rest[i] == mnt[i]) i++;
        if (mnt[i] != '\0') return 0;      /* not under $/mnt/ at all */
        rest += i;                         /* now past "mnt/" */
        if (!*rest) return 0;              /* bare "$/mnt/" — no partition */
        return 1;                          /* "$/mnt/vDriveN-pX..." */
    }
    return 0;
}

/* Join base + "/" + name into out, collapsing the double-slash that
 * would otherwise appear when base already ends in '/'. */
static void path_join(char *out, int cap, const char *base, const char *name) {
    sset(out, cap, base);
    int n = sl(out);
    if (n > 0 && out[n - 1] != '/') scat(out, cap, "/");
    scat(out, cap, name);
}

/* Strip the last path component, in place. "$/Apps/Foo" -> "$/Apps",
 * "/ModuOS/shared" -> "/ModuOS", "/ModuOS" -> "/", "$/dev" -> "$/".
 * Never strips past a bare "/" or "$/" root. */
static void path_parent(char *p) {
    int n = sl(p);
    if (n <= 1) return;
    if (str_eq(p, "$/")) return;

    /* Drop a single trailing slash first (not for roots). */
    if (n > 1 && p[n - 1] == '/') { p[n - 1] = '\0'; n--; }

    int i = n - 1;
    while (i > 0 && p[i] != '/') i--;

    if (i == 0) {
        /* Only one component below "/" — parent is "/". */
        sset(p, PATH_CAP, "/");
        return;
    }
    if (i == 1 && p[0] == '$') {
        /* "$/Foo" -> parent is "$/" */
        sset(p, PATH_CAP, "$/");
        return;
    }
    p[i] = '\0';
}

/* -- Model ----------------------------------------------------------- */

typedef struct {
    char     name[NAME_CAP];
    int      is_dir;
    uint32_t size;
} entry_t;

typedef struct {
    const char *label;
    const char *path;
    int         icon;
} pin_t;

static const pin_t g_pins[] = {
    { "/",                     "/",                 ICON_THIS_PC     },
    { "DevVFS ($/)",           "$/",                ICON_NETWORK     },
    { "Shared",                "/ModuOS/shared/",   ICON_FOLDER_OPEN },
    { "System64",              "/ModuOS/System64/", ICON_LOCKED_FOLDER },
    { "Apps",                  "/Apps/",            ICON_FOLDER_CLOSED },
};
#define PIN_COUNT ((int)(sizeof(g_pins) / sizeof(g_pins[0])))

/* -- Drawing into our own SHM window --------------------------------- */

static uint32_t   *g_buf;
static fnt_font_t *g_font;

/* Row width of the ACTUAL mapped buffer -- equals WIN_W normally, but
 * after MAXIMIZE/FULLSCREEN mdw reallocates to a bigger segment whose row
 * stride is the new (larger) width, not WIN_W. This UI's layout stays
 * fixed-size (letterboxed top-left); only pixel-indexing math needs the
 * real stride. */
static int g_stride = WIN_W;

static ilib_t      *g_icons;
static ilib_image_t g_icon_img[ICON_COUNT];
static int          g_icon_ok[ICON_COUNT];

static void icons_load(void) {
    for (int i = 0; i < ICON_COUNT; i++) g_icon_ok[i] = 0;

    g_icons = ilib_open(ICONS_PATH);
    if (!g_icons) return;

    for (int i = 0; i < ICON_COUNT; i++) {
        if (ilib_load_image(g_icons, (uint16_t)i, &g_icon_img[i]) == ILIB_OK)
            g_icon_ok[i] = 1;
    }
}

static void icons_free(void) {
    for (int i = 0; i < ICON_COUNT; i++)
        if (g_icon_ok[i]) ilib_free_image(&g_icon_img[i]);
    if (g_icons) ilib_close(g_icons);
}

/* Blit an RGBA ILIB icon at (x,y), alpha-composited onto the opaque
 * window buffer (the window itself is created without WM_FLAG_HAS_ALPHA,
 * so icons with soft edges still need per-pixel alpha blending here even
 * though the window as a whole stays on mdw's fast blit path).
 *
 * icons.ilib stores every icon at 32x32, but mdfm's UI is laid out for
 * ICON_SIZE (16x16) glyphs, so this always downsamples to ICON_SIZE on
 * the way out — nearest-neighbor is plenty at this scale and keeps the
 * loop simple. */
static void draw_icon(int id, int x, int y) {
    if (id < 0 || id >= ICON_COUNT || !g_icon_ok[id]) return;
    ilib_image_t *img = &g_icon_img[id];
    if (img->width == 0 || img->height == 0) return;

    for (int dy = 0; dy < ICON_SIZE; dy++) {
        int py = y + dy;
        if (py < 0 || py >= WIN_H) continue;
        int sy = (dy * img->height) / ICON_SIZE;

        for (int dx = 0; dx < ICON_SIZE; dx++) {
            int px = x + dx;
            if (px < 0 || px >= WIN_W) continue;
            int sx = (dx * img->width) / ICON_SIZE;

            const uint8_t *p = &img->pixels[((size_t)sy * img->width + sx) * 4];
            uint8_t r = p[0], g = p[1], b = p[2], a = p[3];
            if (a == 0) continue;

            uint32_t *dst = &g_buf[(size_t)py * g_stride + px];
            if (a == 255) {
                *dst = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            } else {
                uint32_t old = *dst;
                uint8_t or_ = (uint8_t)(old >> 16), og = (uint8_t)(old >> 8), ob = (uint8_t)old;
                uint8_t nr = (uint8_t)((r * a + or_ * (255 - a)) / 255);
                uint8_t ng = (uint8_t)((g * a + og * (255 - a)) / 255);
                uint8_t nb = (uint8_t)((b * a + ob * (255 - a)) / 255);
                *dst = 0xFF000000u | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | nb;
            }
        }
    }
}

static void fill_rect(int x, int y, int w, int h, uint32_t col) {
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > WIN_W) x1 = WIN_W;
    if (y1 > WIN_H) y1 = WIN_H;
    for (int yy = y; yy < y1; yy++)
        for (int xx = x; xx < x1; xx++)
            g_buf[(size_t)yy * g_stride + xx] = col;
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
    while (*s) {
        fnt_glyph_t *gl = fnt_get_glyph(g_font, (uint32_t)(unsigned char)*s);
        if (gl) w += gl->width;
        s++;
    }
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
                        if (px >= 0 && px < WIN_W && py >= 0 && py < WIN_H)
                            g_buf[(size_t)py * g_stride + px] = col;
                    }
            cx += gl->width;
        }
        s++;
    }
}

static void draw_text(int x, int y, const char *s, uint32_t col) {
    draw_text_clip(x, y, s, col, WIN_W);
}

static void draw_text_center(int x, int y, int w, const char *s, uint32_t col) {
    draw_text(x + (w - text_w(s)) / 2, y, s, col);
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

/* -- UI state -------------------------------------------------------- */

enum {
    BTN_UP = 0, BTN_NEWFOLDER, BTN_DELETE, BTN_REFRESH, BTN_QUIT,
    BTN_COUNT
};

typedef struct { int x, y, w, h; const char *label; int danger; } btn_t;
static btn_t g_btns[BTN_COUNT];

typedef struct { int x, y, w, h; } pin_rect_t;
static pin_rect_t g_pin_rects[PIN_COUNT];

typedef enum { MODE_IDLE, MODE_INPUT_NEWFOLDER, MODE_CONFIRM_DELETE } ui_mode_t;

#define LOG_LINES 1

static struct {
    char      cwd[PATH_CAP];

    entry_t   entries[MAX_ENTRIES];
    int       nentries;
    int       sel;        /* -1 = none */

    ui_mode_t mode;
    char      field[NAME_CAP];

    char      status[160];
    uint32_t  status_col;

    int       hover_btn;   /* BTN_* or -1 */
    int       hover_row;   /* 0-based or -1 */
    int       hover_pin;   /* pin index or -1 */
    int       active_btn;  /* button the press landed on, -1 = none */

    uint64_t  last_click_ms;
    int       last_click_row;

    int       dirty;
    int       quit;
} ui;

static void set_status(const char *text, uint32_t col) {
    sset(ui.status, sizeof(ui.status), text);
    ui.status_col = col;
    ui.dirty = 1;
}

/* -- Filesystem operations --------------------------------------------
 * Same syscall-level semantics as the console tools: opendir/readdir/
 * closedir + stat() (ls.c, rm.c), mkdir() (mkdir.c), unlink()/rmdir()
 * (rm.c's recursive delete), and copy-then-unlink for moves (mv.c) —
 * there is no rename syscall in this kernel.
 */

static int entry_cmp_dirs_first(const entry_t *a, const entry_t *b) {
    if (a->is_dir != b->is_dir) return b->is_dir - a->is_dir; /* dirs first */
    const char *x = a->name, *y = b->name;
    while (*x && *y) {
        if (*x != *y) return (unsigned char)*x - (unsigned char)*y;
        x++; y++;
    }
    return (unsigned char)*x - (unsigned char)*y;
}

static void sort_entries(void) {
    /* Simple insertion sort — directory listings here are small. */
    for (int i = 1; i < ui.nentries; i++) {
        entry_t key = ui.entries[i];
        int j = i - 1;
        while (j >= 0 && entry_cmp_dirs_first(&ui.entries[j], &key) > 0) {
            ui.entries[j + 1] = ui.entries[j];
            j--;
        }
        ui.entries[j + 1] = key;
    }
}

static void refresh_listing(void) {
    ui.nentries = 0;
    ui.sel = -1;

    int d = opendir(ui.cwd);
    if (d < 0) {
        char m[200];
        sset(m, sizeof(m), "Cannot open '");
        scat(m, sizeof(m), ui.cwd);
        scat(m, sizeof(m), "'.");
        set_status(m, C_ERR);
        return;
    }

    char name[NAME_CAP];
    int is_dir = 0;
    uint32_t size = 0;

    while (ui.nentries < MAX_ENTRIES) {
        int rc = readdir(d, name, sizeof(name), &is_dir, &size);
        if (rc <= 0) break;
        if (str_eq(name, ".") || str_eq(name, "..")) continue;

        entry_t *e = &ui.entries[ui.nentries];
        sset(e->name, sizeof(e->name), name);
        e->is_dir = is_dir;
        e->size   = size;
        ui.nentries++;
    }
    closedir(d);

    sort_entries();

    char m[200];
    sset(m, sizeof(m), ui.cwd);
    scat(m, sizeof(m), "  (");
    scat_u(m, sizeof(m), (uint64_t)ui.nentries);
    scat(m, sizeof(m), " items)");
    set_status(m, C_TEXT_DIM);
}

static void navigate_to(const char *path) {
    sset(ui.cwd, sizeof(ui.cwd), path);
    refresh_listing();
    ui.dirty = 1;
}

static void navigate_up(void) {
    if (path_is_root(ui.cwd)) { set_status("Already at root.", C_WARN); return; }
    char parent[PATH_CAP];
    sset(parent, sizeof(parent), ui.cwd);
    path_parent(parent);
    navigate_to(parent);
}

static void navigate_into(const char *name) {
    char child[PATH_CAP];
    path_join(child, sizeof(child), ui.cwd, name);
    navigate_to(child);
}

static void do_new_folder(const char *name) {
    if (!name || !*name) { set_status("New Folder: name cannot be empty.", C_ERR); return; }
    if (!path_supports_write(ui.cwd)) {
        set_status("Read-only here — writes only work under / or $/mnt/vDriveN-pX.", C_ERR);
        return;
    }
    char full[PATH_CAP];
    path_join(full, sizeof(full), ui.cwd, name);

    int rc = mkdir(full);
    if (rc != 0) {
        char m[200];
        sset(m, sizeof(m), "mkdir failed (rc=");
        scat_u(m, sizeof(m), (uint64_t)(uint32_t)(-rc));
        scat(m, sizeof(m), ") for '");
        scat(m, sizeof(m), full);
        scat(m, sizeof(m), "'.");
        set_status(m, C_ERR);
        return;
    }

    char m[200];
    sset(m, sizeof(m), "Created folder '");
    scat(m, sizeof(m), name);
    scat(m, sizeof(m), "'.");
    set_status(m, C_OK);
    refresh_listing();
}

/* Recursive delete — identical shape to rm.c's rm_recursive(), just
 * without the fs_file_info_t indirection since we already know is_dir
 * from the listing for the top-level call. */
static int fm_rm_recursive(const char *path, int is_dir) {
    if (!is_dir) return unlink(path);

    int d = opendir(path);
    if (d < 0) return -2;

    char name[NAME_CAP];
    int child_is_dir = 0;
    uint32_t sz = 0;

    while (1) {
        int rc = readdir(d, name, sizeof(name), &child_is_dir, &sz);
        if (rc <= 0) break;
        if (str_eq(name, ".") || str_eq(name, "..")) continue;

        char child[PATH_CAP];
        path_join(child, sizeof(child), path, name);

        int rrc = fm_rm_recursive(child, child_is_dir);
        if (rrc != 0) { closedir(d); return rrc; }
    }

    closedir(d);
    return rmdir(path);
}

static void do_delete_selected(void) {
    if (ui.sel < 0 || ui.sel >= ui.nentries) { set_status("Nothing selected.", C_WARN); return; }
    if (!path_supports_write(ui.cwd)) {
        set_status("Read-only here — writes only work under / or $/mnt/vDriveN-pX.", C_ERR);
        return;
    }

    entry_t *e = &ui.entries[ui.sel];
    char full[PATH_CAP];
    path_join(full, sizeof(full), ui.cwd, e->name);

    char nm[NAME_CAP];
    sset(nm, sizeof(nm), e->name);
    int was_dir = e->is_dir;

    int rc = fm_rm_recursive(full, was_dir);
    if (rc != 0) {
        char m[200];
        sset(m, sizeof(m), "Delete failed (rc=");
        scat_u(m, sizeof(m), (uint64_t)(uint32_t)(-rc));
        scat(m, sizeof(m), ") for '");
        scat(m, sizeof(m), nm);
        scat(m, sizeof(m), "'.");
        set_status(m, C_ERR);
        return;
    }

    char m[200];
    sset(m, sizeof(m), "Deleted '");
    scat(m, sizeof(m), nm);
    scat(m, sizeof(m), "'.");
    set_status(m, C_OK);
    refresh_listing();
}

/* -- Layout + hit testing -------------------------------------------- */

static void set_btn(int id, int x, int y, int w, int h, const char *label, int danger) {
    g_btns[id].x = x; g_btns[id].y = y; g_btns[id].w = w; g_btns[id].h = h;
    g_btns[id].label = label; g_btns[id].danger = danger;
}

static void layout_ui(void) {
    int x = SIDEBAR_W + PAD;
    set_btn(BTN_UP,        x, TOOLBAR_Y, 50,    BTN_H, "Up",     0); x += 50 + BTN_GAP;
    set_btn(BTN_NEWFOLDER, x, TOOLBAR_Y, BTN_W, BTN_H, "New Dir",0); x += BTN_W + BTN_GAP;
    set_btn(BTN_DELETE,    x, TOOLBAR_Y, BTN_W, BTN_H, "Delete", 1); x += BTN_W + BTN_GAP;
    set_btn(BTN_REFRESH,   x, TOOLBAR_Y, BTN_W, BTN_H, "Refresh",0);
    set_btn(BTN_QUIT, WIN_W - PAD - 60, TOOLBAR_Y, 60, BTN_H, "Quit", 0);

    int y = SIDE_ITEM_Y0;
    for (int i = 0; i < PIN_COUNT; i++) {
        g_pin_rects[i].x = 0;
        g_pin_rects[i].y = y;
        g_pin_rects[i].w = SIDEBAR_W;
        g_pin_rects[i].h = SIDE_ITEM_H;
        y += SIDE_ITEM_H;
    }
}

static int hit_button(int x, int y) {
    for (int i = 0; i < BTN_COUNT; i++) {
        btn_t *b = &g_btns[i];
        if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) return i;
    }
    return -1;
}

static int hit_pin(int x, int y) {
    if (x < 0 || x >= SIDEBAR_W) return -1;
    for (int i = 0; i < PIN_COUNT; i++) {
        pin_rect_t *r = &g_pin_rects[i];
        if (y >= r->y && y < r->y + r->h) return i;
    }
    return -1;
}

static int hit_row(int x, int y) {
    if (x < SIDEBAR_W + PAD || x > WIN_W - PAD) return -1;
    if (y < LIST_Y || y >= LIST_Y + ui.nentries * ROW_H) return -1;
    int row = (y - LIST_Y) / ROW_H;
    if (row < 0 || row >= ui.nentries) return -1;
    return row;
}

/* -- Rendering ------------------------------------------------------- */

static void draw_button(int id) {
    btn_t *b = &g_btns[id];
    int hot  = (ui.hover_btn == id);
    int down = (ui.active_btn == id && hot);

    uint32_t bg;
    if (down)            bg = C_BTN_DOWN;
    else if (b->danger)  bg = hot ? C_BTN_DANGER_HOVER : C_BTN_DANGER;
    else                 bg = hot ? C_BTN_HOVER : C_BTN;

    fill_rect(b->x, b->y, b->w, b->h, bg);
    frame_rect(b->x, b->y, b->w, b->h, hot ? C_ACCENT : C_BTN_EDGE);
    draw_text_center(b->x, b->y + (b->h - 8) / 2, b->w, b->label, C_TEXT);
}

static void draw_sidebar(void) {
    fill_rect(0, 0, SIDEBAR_W, WIN_H, C_SIDEBAR);
    draw_text(SIDE_PAD, SIDE_HDR_Y, "Pinned", C_TEXT_DIM);
    fill_rect(0, SIDE_HDR_Y + 14, SIDEBAR_W, 1, C_RULE);

    for (int i = 0; i < PIN_COUNT; i++) {
        pin_rect_t *r = &g_pin_rects[i];
        int is_cur   = str_eq(ui.cwd, g_pins[i].path);
        int is_hover = (ui.hover_pin == i);

        if (is_cur)        fill_rect(r->x, r->y, r->w, r->h, C_SIDEBAR_SEL);
        else if (is_hover)  fill_rect(r->x, r->y, r->w, r->h, C_SIDEBAR_HOVER);

        uint32_t col = is_cur ? C_TEXT : C_TEXT_DIM;
        draw_icon(g_pins[i].icon, SIDE_PAD, r->y + (r->h - ICON_SIZE) / 2);
        draw_text_clip(SIDE_PAD + ICON_SIZE + 6, r->y + (r->h - 8) / 2, g_pins[i].label, col,
                       SIDEBAR_W - 6);
    }

    fill_rect(SIDEBAR_W, 0, 1, WIN_H, C_RULE);
}

static void draw_toolbar(void) {
    for (int i = 0; i < BTN_COUNT; i++) draw_button(i);

    /* Path bar */
    fill_rect(SIDEBAR_W + PAD, PATHBAR_Y, WIN_W - SIDEBAR_W - PAD * 2, PATHBAR_H, C_FIELD);
    frame_rect(SIDEBAR_W + PAD, PATHBAR_Y, WIN_W - SIDEBAR_W - PAD * 2, PATHBAR_H, C_RULE);
    draw_text_clip(SIDEBAR_W + PAD + 6, PATHBAR_Y + 5, ui.cwd, C_ACCENT,
                   WIN_W - PAD - 6);

    /* List header */
    draw_text(SIDEBAR_W + PAD, LIST_HDR_Y, "Name", C_TEXT_DIM);
    draw_text(WIN_W - PAD - 60, LIST_HDR_Y, "Size", C_TEXT_DIM);
    fill_rect(SIDEBAR_W + PAD, LIST_HDR_Y + 14, WIN_W - SIDEBAR_W - PAD * 2, 1, C_RULE);
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

static void draw_list(void) {
    int list_x = SIDEBAR_W + PAD;
    int list_w = WIN_W - SIDEBAR_W - PAD * 2;

    if (ui.nentries == 0) {
        draw_text(list_x, LIST_Y + 2, "(empty)", C_TEXT_DIM);
        return;
    }

    for (int i = 0; i < ui.nentries; i++) {
        int y = LIST_Y + i * ROW_H;
        if (y + ROW_H > WIN_H - STATUS_H) break; /* below status bar */

        entry_t *e = &ui.entries[i];
        int is_sel   = (ui.sel == i);
        int is_hover = (ui.hover_row == i);

        uint32_t bg = (i % 2) ? C_ROW_ALT : C_ROW;
        if (is_sel)        bg = C_ROW_SEL;
        else if (is_hover) bg = C_ROW_HOVER;
        fill_rect(list_x, y, list_w, ROW_H, bg);

        draw_icon(icon_for_entry(e), list_x + 4, y + (ROW_H - ICON_SIZE) / 2);

        uint32_t text_col = is_sel ? C_TEXT : (e->is_dir ? C_TEXT : C_TEXT);
        draw_text_clip(list_x + ICON_SIZE + 12, y + (ROW_H - 8) / 2, e->name, text_col, list_x + list_w - 70);

        if (e->is_dir) {
            draw_text(list_x + list_w - 60, y + (ROW_H - 8) / 2, "<DIR>", C_TEXT_DIM);
        } else {
            char sz[16];
            fmt_size(e->size, sz, sizeof(sz));
            draw_text(list_x + list_w - 60, y + (ROW_H - 8) / 2, sz, C_TEXT_DIM);
        }
    }
}

static void draw_input_overlay(void) {
    if (ui.mode == MODE_IDLE) return;

    int x = SIDEBAR_W + PAD;
    int w = WIN_W - SIDEBAR_W - PAD * 2;

    const char *prompt = (ui.mode == MODE_INPUT_NEWFOLDER)
        ? "New folder name:"
        : "Type 'yes' to confirm delete, Esc to cancel:";
    uint32_t prompt_col = (ui.mode == MODE_CONFIRM_DELETE) ? C_WARN : C_TEXT_DIM;

    draw_text(x, PROMPT_Y, prompt, prompt_col);

    fill_rect(x, FIELD_Y, w, FIELD_H, C_FIELD);
    frame_rect(x, FIELD_Y, w, FIELD_H, C_ACCENT);
    draw_text(x + 6, FIELD_Y + 6, ui.field, C_TEXT);
}

static void draw_status(void) {
    fill_rect(SIDEBAR_W, WIN_H - STATUS_H, WIN_W - SIDEBAR_W, STATUS_H, C_ROW);
    fill_rect(SIDEBAR_W, WIN_H - STATUS_H, WIN_W - SIDEBAR_W, 1, C_RULE);
    draw_text_clip(SIDEBAR_W + PAD, WIN_H - STATUS_H + 6, ui.status, ui.status_col,
                   WIN_W - PAD);
}

static void render(void) {
    fill_rect(0, 0, WIN_W, WIN_H, C_BG);
    draw_sidebar();
    draw_toolbar();
    draw_list();
    draw_input_overlay();
    draw_status();
}

/* -- Actions ----------------------------------------------------------- */

static void begin_new_folder(void) {
    if (!path_supports_write(ui.cwd)) {
        set_status("Read-only here — writes only work under / or $/mnt/vDriveN-pX.", C_ERR);
        return;
    }
    ui.mode = MODE_INPUT_NEWFOLDER;
    ui.field[0] = '\0';
    ui.dirty = 1;
}

static void begin_confirm_delete(void) {
    if (ui.sel < 0 || ui.sel >= ui.nentries) { set_status("Nothing selected.", C_WARN); return; }
    if (!path_supports_write(ui.cwd)) {
        set_status("Read-only here — writes only work under / or $/mnt/vDriveN-pX.", C_ERR);
        return;
    }
    ui.mode = MODE_CONFIRM_DELETE;
    ui.field[0] = '\0';
    ui.dirty = 1;
}

static void end_input(void) {
    ui.mode = MODE_IDLE;
    ui.field[0] = '\0';
    ui.dirty = 1;
}

static void on_button(int id) {
    switch (id) {
        case BTN_UP:        navigate_up(); break;
        case BTN_NEWFOLDER: begin_new_folder(); break;
        case BTN_DELETE:    begin_confirm_delete(); break;
        case BTN_REFRESH:   refresh_listing(); break;
        case BTN_QUIT:      ui.quit = 1; break;
        default: break;
    }
    ui.dirty = 1;
}

/* -- Input ------------------------------------------------------------- */

#define DOUBLE_CLICK_MS 400

static void handle_mouse(const wm_mouse_event_t *m) {
    if (m->type == WM_MOUSE_LEAVE) {
        if (ui.hover_btn != -1 || ui.hover_row != -1 || ui.hover_pin != -1 || ui.active_btn != -1)
            ui.dirty = 1;
        ui.hover_btn = -1;
        ui.hover_row = -1;
        ui.hover_pin = -1;
        ui.active_btn = -1;
        return;
    }

    int hb = hit_button(m->x, m->y);
    int hp = (hb < 0) ? hit_pin(m->x, m->y) : -1;
    int hr = (hb < 0 && hp < 0) ? hit_row(m->x, m->y) : -1;
    if (hb != ui.hover_btn || hp != ui.hover_pin || hr != ui.hover_row) {
        ui.hover_btn = hb;
        ui.hover_pin = hp;
        ui.hover_row = hr;
        ui.dirty = 1;
    }

    if (m->type == WM_MOUSE_DOWN) {
        if (m->changed & WM_MBTN_LEFT) {
            ui.active_btn = hb;

            if (hb < 0 && hp >= 0) {
                navigate_to(g_pins[hp].path);
            } else if (hb < 0 && hr >= 0) {
                if (ui.sel == hr) {
                    uint64_t now = time_ms();
                    if (now - ui.last_click_ms < DOUBLE_CLICK_MS && ui.last_click_row == hr) {
                        if (ui.entries[hr].is_dir) navigate_into(ui.entries[hr].name);
                        ui.last_click_ms = 0;
                        ui.last_click_row = -1;
                    } else {
                        ui.last_click_ms = now;
                        ui.last_click_row = hr;
                    }
                } else {
                    ui.sel = hr;
                    ui.last_click_ms = time_ms();
                    ui.last_click_row = hr;
                }
            } else if (hb < 0 && hp < 0 && hr < 0) {
                ui.sel = -1;
            }
            ui.dirty = 1;
        }
        return;
    }

    if (m->type == WM_MOUSE_UP) {
        if (m->changed & WM_MBTN_LEFT) {
            int was = ui.active_btn;
            ui.active_btn = -1;
            if (was >= 0 && was == hb) on_button(was);
            ui.dirty = 1;
        }
        return;
    }
}

static void handle_key(const KeyboardEventData *k) {
    if (ui.mode == MODE_IDLE) return; /* nothing focusable outside a field */

    char c = (char)k->ascii;

    if (c == 27) { end_input(); set_status("Cancelled.", C_TEXT_DIM); return; }

    if (c == 8 || c == 127) { /* backspace / delete */
        int n = sl(ui.field);
        if (n > 0) { ui.field[n - 1] = '\0'; ui.dirty = 1; }
        return;
    }

    if (c == '\n' || c == '\r') {
        ui_mode_t was = ui.mode;
        char text[NAME_CAP]; sset(text, sizeof(text), ui.field);
        end_input();

        if (was == MODE_INPUT_NEWFOLDER) {
            do_new_folder(text);
        } else if (was == MODE_CONFIRM_DELETE) {
            if (!str_eq(text, "yes")) { set_status("Delete aborted.", C_TEXT_DIM); return; }
            do_delete_selected();
        }
        return;
    }

    if (c >= 32 && c < 127) {
        int n = sl(ui.field);
        if (n < (int)sizeof(ui.field) - 1) { ui.field[n] = c; ui.field[n + 1] = '\0'; ui.dirty = 1; }
    }
}

/* -- Entry point ------------------------------------------------------- */

int md_main(long argc, char **argv) {
    memset(&ui, 0, sizeof(ui));
    ui.sel           = -1;
    ui.hover_btn      = -1;
    ui.hover_row      = -1;
    ui.hover_pin      = -1;
    ui.active_btn     = -1;
    ui.last_click_row = -1;
    ui.mode           = MODE_IDLE;

    if (argc >= 2 && argv[1] && argv[1][0]) {
        sset(ui.cwd, sizeof(ui.cwd), argv[1]);
    } else {
        sset(ui.cwd, sizeof(ui.cwd), "/");
    }

    wm_shm_t *mb = wm_ipc_connect(3000); /* mdw may still be starting up — poll, don't fail on the first race */
    if (!mb) {
        puts("mdfm: mdw never came up (waited 3s)\n");
        return 1;
    }

    wm_request_t req; memset(&req, 0, sizeof(req));
    req.cmd   = WM_CMD_CREATE;
    req.w     = WIN_W;
    req.h     = WIN_H;
    req.flags = 0;                    /* fully opaque -> memcpy blit path in mdw */
    req.pid   = (uint32_t)getpid();   /* lets mdw reap this window if we crash */
    { const char *t = "File Manager"; int i = 0; while (t[i] && i < 63) { req.title[i] = t[i]; i++; } req.title[i] = 0; }

    wm_response_t resp;
    if (wm_ipc_call(mb, &req, &resp, 3000) != 0 || !resp.success) {
        printf("mdfm: create failed: %s\n", resp.message);
        return 1;
    }

    int shmfd = shm_open(resp.shm_name, O_RDWR, 0, 0);
    if (shmfd < 0) { puts("mdfm: shm_open failed\n"); return 1; }
    g_buf = (uint32_t *)mmap(NULL, (size_t)resp.shm_size, PROT_R | PROT_W, MAP_SHARED, shmfd);
    if (g_buf == MAP_FAILED) { puts("mdfm: mmap failed\n"); return 1; }

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
    if (!mice) { puts("mdfm: no mouse ring (mdw too old?)\n"); return 1; }
    if (!keys)   puts("mdfm: no key ring — typing disabled (New Dir/Delete confirm need it)\n");

    g_font = try_load_font(FONT_PRIMARY);
    if (!g_font) g_font = try_load_font(FONT_FALLBACK);
    icons_load();

    layout_ui();
    wm_set_cursor(mb, resp.win_id, WM_CURSOR_POINTER);

    refresh_listing();

    wm_client_surface_t surf;
    wm_client_surface_init(&surf, g_buf, resp.shm_name, resp.shm_size, WIN_W, WIN_H);

    render();
    wm_damage(mb, resp.win_id, 0, 0, WIN_W, WIN_H);

    /* Nothing here animates, so this app repaints only on input — same
     * pacing approach as mdparted_gui.c: sleep() is unreliable, so pace
     * with time_ms()+yield() instead. */
    uint64_t next_poll = time_ms();
    uint64_t next_resize_poll = time_ms();
    while (!ui.quit) {
        wm_mouse_event_t m;
        while (wm_mouse_ring_pop(mice, &m)) handle_mouse(&m);

        if (keys) {
            KeyboardEventData k;
            while (wm_key_ring_pop(keys, &k)) handle_key(&k);
        }

        /* mdw draws this window's titlebar itself, so its own maximize/
         * restore button never round-trips through our own wm_ipc_call --
         * polling wm_list_windows() is the only way we find out the
         * content buffer got reallocated. */
        if (time_ms() >= next_resize_poll) {
            int rc = wm_client_surface_poll(mb, resp.win_id, &surf);
            if (rc < 0) break; /* window torn down some other way */
            if (rc > 0) {
                g_buf = surf.buf;
                g_stride = surf.w;
                /* Fixed sidebar/toolbar/list layout: letterbox rather
                 * than reflow -- clear the whole (possibly now much
                 * bigger) buffer, then redraw the normal content
                 * anchored top-left. */
                wm_client_surface_clear(&surf, C_BG);
                ui.dirty = 1;
            }
            next_resize_poll = time_ms() + 200;
        }

        if (surf.minimized) {
            next_poll = time_ms();
            yield();
            continue;
        }

        if (ui.dirty) {
            render();
            wm_damage(mb, resp.win_id, 0, 0, WIN_W, WIN_H);
            ui.dirty = 0;
        }

        next_poll += 8;
        while (time_ms() < next_poll) yield();
        if (time_ms() > next_poll + 200) next_poll = time_ms(); /* resync after a long stall */
    }

    wm_request_t close_req; memset(&close_req, 0, sizeof(close_req));
    close_req.cmd    = WM_CMD_CLOSE;
    close_req.win_id = resp.win_id;
    wm_response_t cresp;
    wm_ipc_call(mb, &close_req, &cresp, 3000);

    if (surf.buf) munmap(surf.buf, (size_t)surf.shm_size); /* NOT resp.shm_size -- may be stale after a resize */
    if (g_font) fnt_free_font(g_font);
    icons_free();
    return 0;
}