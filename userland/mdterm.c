/*
 * mdterm.c - terminal emulator for mdw.
 *
 * The shell runs on a kernel pty ($/dev/ptmx), so the line discipline owns
 * echo, ^C/^\ delivery, canonical editing and termios. mdterm only parses
 * the byte stream the shell writes (VT100/xterm subset, UTF-8) and turns
 * mdw key events into the bytes a terminal would send.
 *
 * Sections: term_* emulator core, ren_* renderer, shell_* pty child,
 * wmc_* mdw window client, key_* input translation, app_* main loop.
 *
 * Copyright (c) 2026 New Technologies Software - GPL v2.0
 */

#include "libc/libc.h"
#include "libc/tty.h"
#include "wm_protocol.h"
#include "lib_fnt.h"

#define MDTERM_FONT_PATH    "/ModuOS/shared/assets/fonts/Terminus.fnt"
#define MDTERM_SHELL_PATH   "/Apps/zenith5.1.sqr"
#define MDTERM_TITLE        "Terminal"

#define MDTERM_DEF_COLS     80
#define MDTERM_DEF_ROWS     25
#define MDTERM_MAX_COLS     240
#define MDTERM_MAX_ROWS     100
#define MDTERM_SB_LINES     1000
#define MDTERM_MAX_PARAMS   16
#define MDTERM_FONT_MAX     (4 * 1024 * 1024)

#define MDTERM_SYNC_MS      100
#define MDTERM_BLINK_MS     530
#define MDTERM_FRAME_MS     16
#define MDTERM_PUMP_MAX     8192

/*
 * Cell colors. Palette indices (0..255) carry no flag bits; the flags let
 * "default" follow the theme and truecolor skip the palette.
 */
#define COL_DEFAULT         0x01000000u
#define COL_TRUECOLOR       0x02000000u
#define COL_DEF_FG          0xFFD8DEEAu
#define COL_DEF_BG          0xFF0C0F14u

#define TERM_ATTR_BOLD      0x01
#define TERM_ATTR_REVERSE   0x02
#define TERM_ATTR_UNDERLINE 0x04

#define BOX_U               0x1
#define BOX_D               0x2
#define BOX_L               0x4
#define BOX_R               0x8

typedef struct {
    uint32_t cp;
    uint32_t fg;
    uint32_t bg;
    uint8_t  attr;
} term_cell_t;

typedef struct {
    int      cx, cy;
    uint32_t fg, bg;
    uint8_t  attr;
} term_saved_t;

typedef enum {
    TERM_PS_GROUND,
    TERM_PS_ESC,
    TERM_PS_CSI,
    TERM_PS_STRING,
    TERM_PS_STRING_ESC,
    TERM_PS_SKIP1
} term_pstate_t;

typedef struct {
    term_cell_t *screen[2];         /* [0] primary, [1] alternate */
    int          alt;
    int          cols, rows;
    int          cx, cy;
    int          top, bot;          /* scroll region, inclusive */
    uint32_t     fg, bg;
    uint8_t      attr;
    int          wrap_pending;
    int          autowrap;
    int          cursor_visible;
    int          app_cursor;
    term_saved_t saved;
    term_saved_t alt_saved;

    uint8_t      dirty[MDTERM_MAX_ROWS];
    int          all_dirty;

    term_pstate_t pstate;
    int          params[MDTERM_MAX_PARAMS];
    int          nparams;
    int          priv;
    int          interm;
    uint32_t     utf_cp;
    int          utf_need;
    int          swallow_bs;

    term_cell_t *sb[MDTERM_SB_LINES];
    int          sb_cols[MDTERM_SB_LINES];
    int          sb_start;
    int          sb_count;
    int          sb_view;           /* lines scrolled back; 0 = live */
} term_t;

typedef struct {
    fnt_font_t *font;
    int         cell_w, cell_h;
    uint32_t   *pixels;
    int         width, height;
    int         blink_on;
    uint64_t    blink_at;
    int         cur_mode;           /* last drawn: 0 none, 1 block, 2 hollow */
    int         cur_x, cur_y;
} ren_t;

typedef struct {
    int master;
    int pid;
    int status;
    int running;
} shell_t;

typedef struct {
    wm_shm_t       *mailbox;
    int32_t         win_id;
    wm_key_ring_t  *keys;
    size_t          keys_size;
    uint32_t       *pixels;
    size_t          pixels_size;
    char            shm_name[64];
    int             focused;
    int             gone;
    uint64_t        sync_at;
} wmc_t;

typedef struct {
    int quit;
    int await_key;
} app_t;

typedef struct {
    uint16_t cp;
    uint8_t  arms;
} ren_box_t;

static term_t  term;
static ren_t   ren;
static shell_t shell = { -1, -1, 0, 0 };
static wmc_t   wmc;
static app_t   app;

static void shell_write(const void *buf, size_t n);

/* ------------------------------------------------------------------ */
/* util                                                                */
/* ------------------------------------------------------------------ */

static void util_copy_str(char *dst, size_t size, const char *src) {
    size_t i = 0;
    if (!size) return;
    while (src[i] && i + 1 < size) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static size_t util_fmt_uint(char *out, unsigned v) {
    char tmp[12];
    size_t k = 0, n = 0;
    do {
        tmp[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (k) out[n++] = tmp[--k];
    return n;
}

static int util_clamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ------------------------------------------------------------------ */
/* term: grid                                                          */
/* ------------------------------------------------------------------ */

static term_cell_t *term_row(int r) {
    return term.screen[term.alt] + (size_t)r * (size_t)term.cols;
}

static void term_mark_row(int r) {
    if (r >= 0 && r < term.rows) term.dirty[r] = 1;
}

static void term_mark_all(void) {
    term.all_dirty = 1;
}

/* Erased cells take the pen background (BCE), as full-screen apps expect. */
static void term_cell_erase(term_cell_t *c) {
    c->cp = ' ';
    c->fg = COL_DEFAULT;
    c->bg = term.bg;
    c->attr = 0;
}

static void term_erase_cells(int r, int c0, int c1) {
    term_cell_t *row = term_row(r);
    c0 = util_clamp(c0, 0, term.cols);
    c1 = util_clamp(c1, 0, term.cols);
    for (int c = c0; c < c1; c++) term_cell_erase(&row[c]);
    term_mark_row(r);
}

static void term_pen_reset(void) {
    term.fg = COL_DEFAULT;
    term.bg = COL_DEFAULT;
    term.attr = 0;
}

/* ------------------------------------------------------------------ */
/* term: scrollback                                                    */
/* ------------------------------------------------------------------ */

static void term_sb_clear(void) {
    for (int i = 0; i < term.sb_count; i++)
        free(term.sb[(term.sb_start + i) % MDTERM_SB_LINES]);
    term.sb_start = 0;
    term.sb_count = 0;
    term.sb_view = 0;
}

static void term_sb_push(const term_cell_t *row, int cols) {
    term_cell_t *copy = (term_cell_t *)malloc((size_t)cols * sizeof(*copy));
    if (!copy) return;
    memcpy(copy, row, (size_t)cols * sizeof(*copy));

    if (term.sb_count == MDTERM_SB_LINES) {
        free(term.sb[term.sb_start]);
        term.sb_start = (term.sb_start + 1) % MDTERM_SB_LINES;
        term.sb_count--;
    }
    int idx = (term.sb_start + term.sb_count) % MDTERM_SB_LINES;
    term.sb[idx] = copy;
    term.sb_cols[idx] = cols;
    term.sb_count++;

    /* Keep a scrolled-back view anchored on the same text. */
    if (term.sb_view > 0) {
        if (term.sb_view < term.sb_count) term.sb_view++;
        term_mark_all();
    }
}

static void term_view_scroll(int delta) {
    if (term.alt) return;
    int v = util_clamp(term.sb_view + delta, 0, term.sb_count);
    if (v == term.sb_view) return;
    term.sb_view = v;
    term_mark_all();
}

static void term_view_reset(void) {
    if (term.sb_view == 0) return;
    term.sb_view = 0;
    term_mark_all();
}

/* Row source for the renderer: scrollback lines first, then the live grid. */
static const term_cell_t *term_view_row(int r, int *cols_out) {
    int v = term.sb_count - term.sb_view + r;
    if (v < term.sb_count) {
        int idx = (term.sb_start + v) % MDTERM_SB_LINES;
        *cols_out = term.sb_cols[idx];
        return term.sb[idx];
    }
    *cols_out = term.cols;
    return term_row(v - term.sb_count);
}

/* ------------------------------------------------------------------ */
/* term: scrolling and cursor                                          */
/* ------------------------------------------------------------------ */

static void term_scroll_up(int top, int bot, int n) {
    int h = bot - top + 1;
    if (n > h) n = h;
    if (n <= 0) return;
    size_t row_bytes = (size_t)term.cols * sizeof(term_cell_t);

    if (top == 0 && !term.alt)
        for (int i = 0; i < n; i++) term_sb_push(term_row(i), term.cols);

    for (int r = top; r + n <= bot; r++)
        memcpy(term_row(r), term_row(r + n), row_bytes);
    for (int r = bot - n + 1; r <= bot; r++) term_erase_cells(r, 0, term.cols);
    for (int r = top; r <= bot; r++) term_mark_row(r);
}

static void term_scroll_down(int top, int bot, int n) {
    int h = bot - top + 1;
    if (n > h) n = h;
    if (n <= 0) return;
    size_t row_bytes = (size_t)term.cols * sizeof(term_cell_t);

    for (int r = bot; r - n >= top; r--)
        memcpy(term_row(r), term_row(r - n), row_bytes);
    for (int r = top; r < top + n; r++) term_erase_cells(r, 0, term.cols);
    for (int r = top; r <= bot; r++) term_mark_row(r);
}

static void term_linefeed(void) {
    if (term.cy == term.bot) term_scroll_up(term.top, term.bot, 1);
    else if (term.cy < term.rows - 1) term.cy++;
}

static void term_reverse_index(void) {
    if (term.cy == term.top) term_scroll_down(term.top, term.bot, 1);
    else if (term.cy > 0) term.cy--;
}

static void term_put_cp(uint32_t cp) {
    if (term.wrap_pending) {
        term.wrap_pending = 0;
        term.cx = 0;
        term_linefeed();
    }
    term_cell_t *cell = &term_row(term.cy)[term.cx];
    cell->cp = cp;
    cell->fg = term.fg;
    cell->bg = term.bg;
    cell->attr = term.attr;
    term_mark_row(term.cy);

    if (term.cx + 1 >= term.cols) term.wrap_pending = term.autowrap;
    else term.cx++;
}

static void term_cursor_save(term_saved_t *s) {
    s->cx = term.cx;
    s->cy = term.cy;
    s->fg = term.fg;
    s->bg = term.bg;
    s->attr = term.attr;
}

static void term_cursor_restore(const term_saved_t *s) {
    term.cx = util_clamp(s->cx, 0, term.cols - 1);
    term.cy = util_clamp(s->cy, 0, term.rows - 1);
    term.fg = s->fg;
    term.bg = s->bg;
    term.attr = s->attr;
    term.wrap_pending = 0;
}

/* ------------------------------------------------------------------ */
/* term: erase / edit                                                  */
/* ------------------------------------------------------------------ */

static void term_erase_display(int mode) {
    if (mode == 0) {
        term_erase_cells(term.cy, term.cx, term.cols);
        for (int r = term.cy + 1; r < term.rows; r++)
            term_erase_cells(r, 0, term.cols);
    } else if (mode == 1) {
        for (int r = 0; r < term.cy; r++) term_erase_cells(r, 0, term.cols);
        term_erase_cells(term.cy, 0, term.cx + 1);
    } else {
        for (int r = 0; r < term.rows; r++) term_erase_cells(r, 0, term.cols);
        if (mode == 3) term_sb_clear();
    }
}

static void term_erase_line(int mode) {
    if (mode == 0) term_erase_cells(term.cy, term.cx, term.cols);
    else if (mode == 1) term_erase_cells(term.cy, 0, term.cx + 1);
    else term_erase_cells(term.cy, 0, term.cols);
}

static void term_delete_chars(int n) {
    term_cell_t *row = term_row(term.cy);
    n = util_clamp(n, 0, term.cols - term.cx);
    for (int c = term.cx; c < term.cols; c++) {
        if (c + n < term.cols) row[c] = row[c + n];
        else term_cell_erase(&row[c]);
    }
    term_mark_row(term.cy);
}

static void term_insert_chars(int n) {
    term_cell_t *row = term_row(term.cy);
    n = util_clamp(n, 0, term.cols - term.cx);
    for (int c = term.cols - 1; c >= term.cx; c--) {
        if (c - n >= term.cx) row[c] = row[c - n];
        else term_cell_erase(&row[c]);
    }
    term_mark_row(term.cy);
}

/* ------------------------------------------------------------------ */
/* term: modes, reset, resize                                          */
/* ------------------------------------------------------------------ */

static void term_alt_enter(int save_cursor) {
    if (term.alt) return;
    if (save_cursor) term_cursor_save(&term.alt_saved);
    term.alt = 1;
    term.sb_view = 0;
    uint32_t pen_bg = term.bg;
    term.bg = COL_DEFAULT;
    for (int r = 0; r < term.rows; r++) term_erase_cells(r, 0, term.cols);
    term.bg = pen_bg;
    term_mark_all();
}

static void term_alt_leave(int restore_cursor) {
    if (!term.alt) return;
    term.alt = 0;
    if (restore_cursor) term_cursor_restore(&term.alt_saved);
    term_mark_all();
}

static void term_set_private_mode(int mode, int on) {
    switch (mode) {
    case 1:    term.app_cursor = on; break;
    case 7:    term.autowrap = on; break;
    case 25:   term.cursor_visible = on; break;
    case 47:
    case 1047: if (on) term_alt_enter(0); else term_alt_leave(0); break;
    case 1049: if (on) term_alt_enter(1); else term_alt_leave(1); break;
    default:   break;
    }
}

static void term_reset(void) {
    term_alt_leave(0);
    term_pen_reset();
    term.top = 0;
    term.bot = term.rows - 1;
    term.autowrap = 1;
    term.cursor_visible = 1;
    term.app_cursor = 0;
    term.wrap_pending = 0;
    term.pstate = TERM_PS_GROUND;
    term.utf_need = 0;
    term.swallow_bs = 0;
    for (int r = 0; r < term.rows; r++) term_erase_cells(r, 0, term.cols);
    term.cx = 0;
    term.cy = 0;
    memset(&term.saved, 0, sizeof(term.saved));
    term.saved.fg = COL_DEFAULT;
    term.saved.bg = COL_DEFAULT;
    term.alt_saved = term.saved;
    term_view_reset();
    term_mark_all();
}

/*
 * No reflow: wrapped lines are truncated and the shell redraws its prompt.
 * Rows pushed off the top by a shrinking window go to scrollback so the
 * cursor line stays visible.
 */
static int term_resize(int ncols, int nrows) {
    ncols = util_clamp(ncols, 1, MDTERM_MAX_COLS);
    nrows = util_clamp(nrows, 1, MDTERM_MAX_ROWS);
    if (term.screen[0] && ncols == term.cols && nrows == term.rows) return 0;

    size_t count = (size_t)ncols * (size_t)nrows;
    term_cell_t *fresh[2];
    fresh[0] = (term_cell_t *)malloc(count * sizeof(term_cell_t));
    fresh[1] = (term_cell_t *)malloc(count * sizeof(term_cell_t));
    if (!fresh[0] || !fresh[1]) {
        free(fresh[0]);
        free(fresh[1]);
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        for (int s = 0; s < 2; s++) {
            fresh[s][i].cp = ' ';
            fresh[s][i].fg = COL_DEFAULT;
            fresh[s][i].bg = COL_DEFAULT;
            fresh[s][i].attr = 0;
        }
    }

    int old_cols = term.cols, old_rows = term.rows;
    int shift = (term.cy >= nrows) ? term.cy - nrows + 1 : 0;

    for (int s = 0; s < 2; s++) {
        if (!term.screen[s]) continue;
        int sh = (s == term.alt) ? shift : 0;
        int copy_cols = old_cols < ncols ? old_cols : ncols;
        for (int r = 0; r < sh && r < old_rows; r++) {
            if (s == 0)
                term_sb_push(term.screen[s] + (size_t)r * (size_t)old_cols, old_cols);
        }
        for (int r = sh; r < old_rows && r - sh < nrows; r++) {
            memcpy(fresh[s] + (size_t)(r - sh) * (size_t)ncols,
                   term.screen[s] + (size_t)r * (size_t)old_cols,
                   (size_t)copy_cols * sizeof(term_cell_t));
        }
    }

    free(term.screen[0]);
    free(term.screen[1]);
    term.screen[0] = fresh[0];
    term.screen[1] = fresh[1];
    term.cols = ncols;
    term.rows = nrows;
    term.cy -= shift;
    term.cx = util_clamp(term.cx, 0, ncols - 1);
    term.cy = util_clamp(term.cy, 0, nrows - 1);
    term.top = 0;
    term.bot = nrows - 1;
    term.wrap_pending = 0;
    term.saved.cx = util_clamp(term.saved.cx, 0, ncols - 1);
    term.saved.cy = util_clamp(term.saved.cy, 0, nrows - 1);
    term.alt_saved.cx = util_clamp(term.alt_saved.cx, 0, ncols - 1);
    term.alt_saved.cy = util_clamp(term.alt_saved.cy, 0, nrows - 1);
    term.sb_view = 0;
    term_mark_all();
    return 0;
}

static int term_init(void) {
    term.autowrap = 1;
    term.cursor_visible = 1;
    term_pen_reset();
    term.saved.fg = COL_DEFAULT;
    term.saved.bg = COL_DEFAULT;
    term.alt_saved = term.saved;
    return term_resize(MDTERM_DEF_COLS, MDTERM_DEF_ROWS);
}

/* ------------------------------------------------------------------ */
/* term: escape sequence parser                                        */
/* ------------------------------------------------------------------ */

static int term_arg(int i, int def) {
    if (i >= term.nparams || term.params[i] <= 0) return def;
    return term.params[i];
}

static int term_arg_raw(int i) {
    return i < term.nparams ? term.params[i] : 0;
}

static uint32_t term_rgb(int r, int g, int b) {
    return COL_TRUECOLOR | ((uint32_t)util_clamp(r, 0, 255) << 16) |
           ((uint32_t)util_clamp(g, 0, 255) << 8) | (uint32_t)util_clamp(b, 0, 255);
}

/* Returns extra params consumed by 38/48 at index i, or 0 if malformed. */
static int term_sgr_extended(int i, uint32_t *col) {
    if (i + 1 >= term.nparams) return 0;
    int kind = term.params[i + 1];
    if (kind == 5 && i + 2 < term.nparams) {
        *col = (uint32_t)util_clamp(term.params[i + 2], 0, 255);
        return 2;
    }
    if (kind == 2 && i + 4 < term.nparams) {
        *col = term_rgb(term.params[i + 2], term.params[i + 3], term.params[i + 4]);
        return 4;
    }
    return 0;
}

static void term_sgr(void) {
    if (term.nparams == 0) {
        term_pen_reset();
        return;
    }
    for (int i = 0; i < term.nparams; i++) {
        int a = term.params[i];
        if (a == 0) term_pen_reset();
        else if (a == 1) term.attr |= TERM_ATTR_BOLD;
        else if (a == 4) term.attr |= TERM_ATTR_UNDERLINE;
        else if (a == 7) term.attr |= TERM_ATTR_REVERSE;
        else if (a == 22) term.attr &= (uint8_t)~TERM_ATTR_BOLD;
        else if (a == 24) term.attr &= (uint8_t)~TERM_ATTR_UNDERLINE;
        else if (a == 27) term.attr &= (uint8_t)~TERM_ATTR_REVERSE;
        else if (a >= 30 && a <= 37) term.fg = (uint32_t)(a - 30);
        else if (a == 39) term.fg = COL_DEFAULT;
        else if (a >= 40 && a <= 47) term.bg = (uint32_t)(a - 40);
        else if (a == 49) term.bg = COL_DEFAULT;
        else if (a >= 90 && a <= 97) term.fg = (uint32_t)(a - 90 + 8);
        else if (a >= 100 && a <= 107) term.bg = (uint32_t)(a - 100 + 8);
        else if (a == 38 || a == 48) {
            uint32_t col = 0;
            int used = term_sgr_extended(i, &col);
            if (!used) break;
            if (a == 38) term.fg = col;
            else term.bg = col;
            i += used;
        }
    }
}

static void term_reply_cpr(void) {
    char b[24];
    size_t n = 0;
    b[n++] = 0x1B;
    b[n++] = '[';
    n += util_fmt_uint(b + n, (unsigned)(term.cy + 1));
    b[n++] = ';';
    n += util_fmt_uint(b + n, (unsigned)(term.cx + 1));
    b[n++] = 'R';
    shell_write(b, n);
}

static void term_set_region(void) {
    int top = term_arg(0, 1) - 1;
    int bot = term_arg(1, term.rows) - 1;
    if (top < 0 || bot >= term.rows || top >= bot) {
        top = 0;
        bot = term.rows - 1;
    }
    term.top = top;
    term.bot = bot;
    term.cx = 0;
    term.cy = 0;
    term.wrap_pending = 0;
}

static void term_csi_modes(int on) {
    if (term.priv != '?') return;
    for (int i = 0; i < term.nparams; i++) term_set_private_mode(term.params[i], on);
}

static void term_csi_dispatch(uint8_t f) {
    int n = term_arg(0, 1);
    term.wrap_pending = 0;

    switch (f) {
    case 'A': {
        int lim = term.cy >= term.top ? term.top : 0;
        term.cy = term.cy - n < lim ? lim : term.cy - n;
        break;
    }
    case 'B': {
        int lim = term.cy <= term.bot ? term.bot : term.rows - 1;
        term.cy = term.cy + n > lim ? lim : term.cy + n;
        break;
    }
    case 'C': term.cx += n; break;
    case 'D': term.cx -= n; break;
    case 'E': term.cy += n; term.cx = 0; break;
    case 'F': term.cy -= n; term.cx = 0; break;
    case 'G': term.cx = n - 1; break;
    case 'd': term.cy = n - 1; break;
    case 'H':
    case 'f': term.cy = term_arg(0, 1) - 1; term.cx = term_arg(1, 1) - 1; break;
    case 'J':
        term_erase_display(term_arg_raw(0));
        /* zenith's clear sends ESC[2J\b and expects the cursor homed. */
        if (term_arg_raw(0) >= 2) { term.cx = 0; term.cy = 0; }
        term.swallow_bs = 1;
        break;
    case 'K': term_erase_line(term_arg_raw(0)); break;
    case 'L':
        if (term.cy >= term.top && term.cy <= term.bot)
            term_scroll_down(term.cy, term.bot, n);
        break;
    case 'M':
        if (term.cy >= term.top && term.cy <= term.bot)
            term_scroll_up(term.cy, term.bot, n);
        break;
    case 'P': term_delete_chars(n); break;
    case '@': term_insert_chars(n); break;
    case 'X': term_erase_cells(term.cy, term.cx, term.cx + n); break;
    case 'S': term_scroll_up(term.top, term.bot, n); break;
    case 'T': term_scroll_down(term.top, term.bot, n); break;
    case 'r': term_set_region(); break;
    case 's': term_cursor_save(&term.saved); break;
    case 'u': term_cursor_restore(&term.saved); break;
    case 'h': term_csi_modes(1); break;
    case 'l': term_csi_modes(0); break;
    case 'n':
        if (term.priv) break;
        if (term_arg_raw(0) == 5) shell_write("\x1b[0n", 4);
        else if (term_arg_raw(0) == 6) term_reply_cpr();
        break;
    case 'c':
        if (!term.priv) shell_write("\x1b[?6c", 5);
        break;
    case 'm':
        term_sgr();
        /* zenith emits ESC[..m\b; the kernel VT swallows that \b, so do we. */
        term.swallow_bs = 1;
        break;
    default:
        break;
    }

    term.cx = util_clamp(term.cx, 0, term.cols - 1);
    term.cy = util_clamp(term.cy, 0, term.rows - 1);
}

static void term_exec_ctl(uint8_t c) {
    switch (c) {
    case '\n':
    case 0x0B:
    case 0x0C:
        term_linefeed();
        term.wrap_pending = 0;
        break;
    case '\r':
        term.cx = 0;
        term.wrap_pending = 0;
        break;
    case '\b':
        if (term.cx > 0) term.cx--;
        term.wrap_pending = 0;
        break;
    case '\t': {
        int nx = (term.cx + 8) & ~7;
        term.cx = nx >= term.cols ? term.cols - 1 : nx;
        term.wrap_pending = 0;
        break;
    }
    default:
        break;
    }
}

static void term_csi_begin(void) {
    term.pstate = TERM_PS_CSI;
    term.nparams = 0;
    term.priv = 0;
    term.interm = 0;
    memset(term.params, 0, sizeof(term.params));
}

static void term_csi_byte(uint8_t c) {
    if (c == 0x1B) {
        term.pstate = TERM_PS_ESC;
    } else if (c < 0x20) {
        term_exec_ctl(c);
    } else if (c >= '0' && c <= '9') {
        if (term.nparams == 0) term.nparams = 1;
        int *p = &term.params[term.nparams - 1];
        if (*p < 10000) *p = *p * 10 + (c - '0');
    } else if (c == ';' || c == ':') {
        if (term.nparams == 0) term.nparams = 1;
        if (term.nparams < MDTERM_MAX_PARAMS) term.nparams++;
    } else if (c == '?' || c == '>' || c == '<' || c == '=') {
        term.priv = c;
    } else if (c >= 0x20 && c <= 0x2F) {
        term.interm = 1;
    } else if (c >= 0x40 && c <= 0x7E) {
        term.pstate = TERM_PS_GROUND;
        if (!term.interm) term_csi_dispatch(c);
    } else {
        term.pstate = TERM_PS_GROUND;
    }
}

static void term_esc_byte(uint8_t c) {
    if (c < 0x20 && c != 0x1B) {
        term_exec_ctl(c);
        return;
    }
    term.pstate = TERM_PS_GROUND;
    switch (c) {
    case 0x1B: term.pstate = TERM_PS_ESC; break;
    case '[':  term_csi_begin(); break;
    case ']':
    case 'P':
    case 'X':
    case '^':
    case '_':  term.pstate = TERM_PS_STRING; break;
    case '(':
    case ')':
    case '*':
    case '+':
    case '#':
    case '%':  term.pstate = TERM_PS_SKIP1; break;
    case '7':  term_cursor_save(&term.saved); break;
    case '8':  term_cursor_restore(&term.saved); break;
    case 'D':  term_linefeed(); break;
    case 'E':  term.cx = 0; term_linefeed(); break;
    case 'M':  term_reverse_index(); break;
    case 'c':  term_reset(); break;
    default:   break;
    }
}

static void term_utf8_byte(uint8_t c) {
    if (c < 0x80) {
        if (term.utf_need) {
            term.utf_need = 0;
            term_put_cp(0xFFFD);
        }
        term_put_cp(c);
        return;
    }
    if ((c & 0xC0) == 0x80) {
        if (!term.utf_need) {
            term_put_cp(0xFFFD);
            return;
        }
        term.utf_cp = (term.utf_cp << 6) | (c & 0x3Fu);
        if (--term.utf_need == 0)
            term_put_cp(term.utf_cp > 0x10FFFF ? 0xFFFD : term.utf_cp);
        return;
    }
    if (term.utf_need) term_put_cp(0xFFFD);
    if ((c & 0xE0) == 0xC0) { term.utf_cp = c & 0x1Fu; term.utf_need = 1; }
    else if ((c & 0xF0) == 0xE0) { term.utf_cp = c & 0x0Fu; term.utf_need = 2; }
    else if ((c & 0xF8) == 0xF0) { term.utf_cp = c & 0x07u; term.utf_need = 3; }
    else { term.utf_need = 0; term_put_cp(0xFFFD); }
}

static void term_ground_byte(uint8_t c) {
    if (term.swallow_bs) {
        term.swallow_bs = 0;
        if (c == 0x08) return;
    }
    if (c == 0x1B) {
        term.utf_need = 0;
        term.pstate = TERM_PS_ESC;
    } else if (c < 0x20) {
        term.utf_need = 0;
        term_exec_ctl(c);
    } else if (c != 0x7F) {
        term_utf8_byte(c);
    }
}

static void term_feed_byte(uint8_t c) {
    switch (term.pstate) {
    case TERM_PS_STRING:
        if (c == 0x07 || c == 0x18 || c == 0x1A) term.pstate = TERM_PS_GROUND;
        else if (c == 0x1B) term.pstate = TERM_PS_STRING_ESC;
        return;
    case TERM_PS_STRING_ESC:
        if (c == '\\') {
            term.pstate = TERM_PS_GROUND;
        } else {
            term.pstate = TERM_PS_ESC;
            term_esc_byte(c);
        }
        return;
    case TERM_PS_SKIP1:
        term.pstate = TERM_PS_GROUND;
        return;
    case TERM_PS_ESC:
        term_esc_byte(c);
        return;
    case TERM_PS_CSI:
        if (c == 0x18 || c == 0x1A) term.pstate = TERM_PS_GROUND;
        else term_csi_byte(c);
        return;
    case TERM_PS_GROUND:
    default:
        term_ground_byte(c);
        return;
    }
}

static void term_feed_string(const char *s) {
    while (*s) term_feed_byte((uint8_t)*s++);
}

static void term_feed_int(int v) {
    char b[16];
    size_t n = 0;
    if (v < 0) {
        b[n++] = '-';
        v = -v;
    }
    n += util_fmt_uint(b + n, (unsigned)v);
    b[n] = 0;
    term_feed_string(b);
}

/* ------------------------------------------------------------------ */
/* ren: renderer                                                       */
/* ------------------------------------------------------------------ */

static const uint32_t ren_ansi[16] = {
    0xFF0C0F14, 0xFFCC3B3B, 0xFF3FB950, 0xFFD29922,
    0xFF4C8DFF, 0xFFB267E6, 0xFF39C5CF, 0xFFD8DEEA,
    0xFF5A6270, 0xFFFF6B6B, 0xFF6BE675, 0xFFF2CC60,
    0xFF79B8FF, 0xFFD2A8FF, 0xFF56D4DD, 0xFFFFFFFF,
};

/* Used only when the font lacks the glyph; heavy/double map to light. */
static const ren_box_t ren_box_tab[] = {
    { 0x2500, BOX_L | BOX_R },         { 0x2501, BOX_L | BOX_R },
    { 0x2502, BOX_U | BOX_D },         { 0x2503, BOX_U | BOX_D },
    { 0x250C, BOX_D | BOX_R },         { 0x250F, BOX_D | BOX_R },
    { 0x2510, BOX_D | BOX_L },         { 0x2513, BOX_D | BOX_L },
    { 0x2514, BOX_U | BOX_R },         { 0x2517, BOX_U | BOX_R },
    { 0x2518, BOX_U | BOX_L },         { 0x251B, BOX_U | BOX_L },
    { 0x251C, BOX_U | BOX_D | BOX_R }, { 0x2523, BOX_U | BOX_D | BOX_R },
    { 0x2524, BOX_U | BOX_D | BOX_L }, { 0x252B, BOX_U | BOX_D | BOX_L },
    { 0x252C, BOX_D | BOX_L | BOX_R }, { 0x2533, BOX_D | BOX_L | BOX_R },
    { 0x2534, BOX_U | BOX_L | BOX_R }, { 0x253B, BOX_U | BOX_L | BOX_R },
    { 0x253C, 0xF },                   { 0x254B, 0xF },
    { 0x2550, BOX_L | BOX_R },         { 0x2551, BOX_U | BOX_D },
    { 0x2554, BOX_D | BOX_R },         { 0x2557, BOX_D | BOX_L },
    { 0x255A, BOX_U | BOX_R },         { 0x255D, BOX_U | BOX_L },
    { 0x2560, BOX_U | BOX_D | BOX_R }, { 0x2563, BOX_U | BOX_D | BOX_L },
    { 0x2566, BOX_D | BOX_L | BOX_R }, { 0x2569, BOX_U | BOX_L | BOX_R },
    { 0x256C, 0xF },
};

static uint32_t ren_palette(unsigned idx) {
    if (idx < 16) return ren_ansi[idx];
    if (idx < 232) {
        idx -= 16;
        unsigned r = idx / 36, g = (idx / 6) % 6, b = idx % 6;
        r = r ? 55 + r * 40 : 0;
        g = g ? 55 + g * 40 : 0;
        b = b ? 55 + b * 40 : 0;
        return 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    unsigned v = 8 + (idx - 232) * 10;
    return 0xFF000000u | (v << 16) | (v << 8) | v;
}

static uint32_t ren_resolve(uint32_t c, int is_fg, int bold) {
    if (c & COL_TRUECOLOR) return 0xFF000000u | (c & 0x00FFFFFFu);
    if (c & COL_DEFAULT) return is_fg ? COL_DEF_FG : COL_DEF_BG;
    unsigned idx = c & 0xFF;
    if (is_fg && bold && idx < 8) idx += 8;
    return ren_palette(idx);
}

static void ren_fill(int x, int y, int w, int h, uint32_t col) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > ren.width) w = ren.width - x;
    if (y + h > ren.height) h = ren.height - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        uint32_t *p = ren.pixels + (size_t)(y + j) * (size_t)ren.width + (size_t)x;
        for (int i = 0; i < w; i++) p[i] = col;
    }
}

static unsigned ren_box_arms(uint32_t cp) {
    for (size_t i = 0; i < sizeof(ren_box_tab) / sizeof(ren_box_tab[0]); i++)
        if (ren_box_tab[i].cp == cp) return ren_box_tab[i].arms;
    return 0;
}

static void ren_draw_box(unsigned arms, int x, int y, uint32_t col) {
    int mx = x + ren.cell_w / 2;
    int my = y + ren.cell_h / 2;
    if (arms & BOX_U) ren_fill(mx, y, 1, my - y + 1, col);
    if (arms & BOX_D) ren_fill(mx, my, 1, y + ren.cell_h - my, col);
    if (arms & BOX_L) ren_fill(x, my, mx - x + 1, 1, col);
    if (arms & BOX_R) ren_fill(mx, my, x + ren.cell_w - mx, 1, col);
}

static void ren_draw_glyph(uint32_t cp, int x, int y, uint32_t fg) {
    fnt_glyph_t *g = fnt_get_glyph(ren.font, cp);
    if (!g) {
        unsigned arms = ren_box_arms(cp);
        if (arms) {
            ren_draw_box(arms, x, y, fg);
            return;
        }
        g = fnt_get_glyph(ren.font, '?');
        if (!g) return;
    }
    int gw = g->bitmap_width < ren.cell_w ? g->bitmap_width : ren.cell_w;
    int gh = g->bitmap_height < ren.cell_h ? g->bitmap_height : ren.cell_h;
    for (int dy = 0; dy < gh; dy++) {
        if (y + dy < 0 || y + dy >= ren.height) continue;
        uint32_t *p = ren.pixels + (size_t)(y + dy) * (size_t)ren.width;
        for (int dx = 0; dx < gw; dx++) {
            if (x + dx < 0 || x + dx >= ren.width) continue;
            if (fnt_get_pixel(g, dx, dy)) p[x + dx] = fg;
        }
    }
}

static void ren_draw_row(int r) {
    static const term_cell_t blank = { ' ', COL_DEFAULT, COL_DEFAULT, 0 };
    int line_cols = 0;
    const term_cell_t *line = term_view_row(r, &line_cols);
    int y = r * ren.cell_h;

    for (int c = 0; c < term.cols; c++) {
        const term_cell_t *cell = c < line_cols ? &line[c] : &blank;
        uint32_t fg = ren_resolve(cell->fg, 1, cell->attr & TERM_ATTR_BOLD);
        uint32_t bg = ren_resolve(cell->bg, 0, 0);
        if (cell->attr & TERM_ATTR_REVERSE) {
            uint32_t t = fg;
            fg = bg;
            bg = t;
        }
        int here = (ren.cur_mode && r == term.cy && c == term.cx) ? ren.cur_mode : 0;
        if (here == 1) {
            uint32_t t = fg;
            fg = bg;
            bg = t;
        }
        int x = c * ren.cell_w;
        ren_fill(x, y, ren.cell_w, ren.cell_h, bg);
        if (cell->cp != ' ') ren_draw_glyph(cell->cp, x, y, fg);
        if (cell->attr & TERM_ATTR_UNDERLINE)
            ren_fill(x, y + ren.cell_h - 1, ren.cell_w, 1, fg);
        if (here == 2) {
            uint32_t edge = COL_DEF_FG;
            ren_fill(x, y, ren.cell_w, 1, edge);
            ren_fill(x, y + ren.cell_h - 1, ren.cell_w, 1, edge);
            ren_fill(x, y, 1, ren.cell_h, edge);
            ren_fill(x + ren.cell_w - 1, y, 1, ren.cell_h, edge);
        }
    }
    int used = term.cols * ren.cell_w;
    ren_fill(used, y, ren.width - used, ren.cell_h, COL_DEF_BG);
}

static void ren_cursor_wake(uint64_t now) {
    ren.blink_on = 1;
    ren.blink_at = now;
}

static void ren_tick(uint64_t now) {
    if (now - ren.blink_at < MDTERM_BLINK_MS) return;
    ren.blink_at = now;
    ren.blink_on = !ren.blink_on;
}

/* Returns 1 if a damage rect was sent. */
static int ren_frame(void) {
    int mode;
    if (!term.cursor_visible || term.sb_view > 0) mode = 0;
    else if (wmc.focused) mode = ren.blink_on ? 1 : 0;
    else mode = 2;

    int moved = mode && (term.cx != ren.cur_x || term.cy != ren.cur_y);
    if (mode != ren.cur_mode || moved) {
        term_mark_row(ren.cur_y);
        term_mark_row(term.cy);
    }
    ren.cur_mode = mode;
    ren.cur_x = term.cx;
    ren.cur_y = term.cy;

    int full = term.all_dirty;
    int lo = term.rows, hi = -1;
    if (full) ren_fill(0, 0, ren.width, ren.height, COL_DEF_BG);
    for (int r = 0; r < term.rows; r++) {
        if (!full && !term.dirty[r]) continue;
        ren_draw_row(r);
        if (r < lo) lo = r;
        if (r > hi) hi = r;
    }
    memset(term.dirty, 0, sizeof(term.dirty));
    term.all_dirty = 0;
    if (hi < 0) return 0;

    int y = full ? 0 : lo * ren.cell_h;
    int h = full ? ren.height : (hi - lo + 1) * ren.cell_h;
    if (wm_damage(wmc.mailbox, wmc.win_id, 0, y, ren.width, h) != 0)
        term.all_dirty = 1;
    return 1;
}

static void ren_attach(uint32_t *pixels, int w, int h, size_t bytes) {
    if ((size_t)w * (size_t)h * 4 > bytes) h = (int)(bytes / ((size_t)w * 4));
    ren.pixels = pixels;
    ren.width = w;
    ren.height = h;
}

static int ren_load_font(void) {
    int fd = open(MDTERM_FONT_PATH, O_RDONLY, 0);
    if (fd < 0) return -1;
    long size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (size <= 0 || size > MDTERM_FONT_MAX) {
        close(fd);
        return -1;
    }
    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (!data) {
        close(fd);
        return -1;
    }
    size_t got = 0;
    while (got < (size_t)size) {
        ssize_t r = read(fd, data + got, (size_t)size - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    ren.font = (got == (size_t)size) ? fnt_load_font(data, got) : NULL;
    free(data);
    if (!ren.font) return -1;

    int w = ren.font->header.glyph_width;
    int h = ren.font->header.glyph_height;
    if (w < 4 || w > 32 || h < 8 || h > 64) {
        w = 8;
        h = 16;
    }
    ren.cell_w = w;
    ren.cell_h = h;
    ren.blink_on = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* shell: pty child                                                    */
/* ------------------------------------------------------------------ */

static void shell_write(const void *buf, size_t n) {
    if (shell.master < 0 || !shell.running) return;
    const char *p = (const char *)buf;
    while (n) {
        ssize_t w = write(shell.master, p, n);
        if (w <= 0) break;
        p += w;
        n -= (size_t)w;
    }
}

static void shell_sync_winsize(void) {
    if (shell.master < 0) return;
    winsize_t ws = { (uint16_t)term.rows, (uint16_t)term.cols,
                     (uint16_t)ren.width, (uint16_t)ren.height };
    tty_setwinsize(shell.master, &ws);
}

/* Same sequence getty uses: new session, slave becomes the controlling tty. */
static int shell_spawn(void) {
    char slave[32];
    int m = tty_openpty(slave, sizeof slave);
    if (m < 0) return -1;
    shell.master = m;
    shell_sync_winsize();

    int pid = fork();
    if (pid < 0) {
        close(m);
        shell.master = -1;
        return -1;
    }
    if (pid == 0) {
        /* Do NOT close(m) here. ModuOS's fd table is one global flat table,
         * so 'm' is the PARENT's slot: closing it would tear the pty down
         * (slave open then fails, tty_set_ctty returns -EIO, exit 4). */
        if (setsid() < 0) _exit(2);
        int s = open(slave, O_RDWR, 0);
        if (s < 0) _exit(3);
        if (tty_set_ctty(s) < 0) _exit(4);
        /* No dup2 onto 0/1/2: ModuOS routes stdin/stdout/stderr through the
         * process's controlling tty (tty_proc_read/write), and fd_dup2() on
         * the global fd table crashes the kernel here. Keep 's' open so the
         * slave stays referenced until the shell takes over. */
        char *argv[] = { (char *)MDTERM_SHELL_PATH, NULL };
        char *envp[] = { (char *)"TERM=linux", NULL };
        execve(MDTERM_SHELL_PATH, argv, envp);
        _exit(127);
    }
    shell.pid = pid;
    shell.running = 1;
    int one = 1;
    tty_ioctl(m, TTY_IOC_NONBLOCK, &one, sizeof one);
    return 0;
}

/* Bounded per call so a flooding child can't starve input and rendering. */
static size_t shell_pump(void) {
    char buf[1024];
    size_t total = 0;
    while (total < MDTERM_PUMP_MAX) {
        ssize_t n = read(shell.master, buf, sizeof buf);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) term_feed_byte((uint8_t)buf[i]);
        total += (size_t)n;
    }
    return total;
}

/* Returns 1 once, when the child is found dead. */
static int shell_reap(void) {
    if (!shell.running) return 0;
    int st = 0;
    pid_t r = waitpid(shell.pid, &st, WNOHANG);
    if (r == 0) return 0;
    shell.running = 0;
    shell.status = (r == shell.pid) ? st : -1;
    return 1;
}

static void shell_shutdown(void) {
    if (shell.master >= 0) {
        close(shell.master);          /* hangs up the slave */
        shell.master = -1;
    }
    if (shell.running) {
        kill(shell.pid, SIGHUP);
        kill(shell.pid, SIGTERM);
        shell.running = 0;
    }
}

/* ------------------------------------------------------------------ */
/* wmc: mdw window client                                              */
/* ------------------------------------------------------------------ */

/* shm_open handles are single-use for mmap, so every remap reopens by name. */
static void *wmc_map(const char *name, size_t size) {
    int fd = shm_open(name, O_RDWR, 0, 0);
    if (fd < 0) return NULL;
    void *p = mmap(NULL, size, PROT_R | PROT_W, MAP_SHARED, fd);
    return p == MAP_FAILED ? NULL : p;
}

static int wmc_open(void) {
    static wm_response_t resp;

    wmc.mailbox = wm_ipc_connect(3000);
    if (!wmc.mailbox) {
        printf("mdterm: couldn't connect to mdw (is it running?)\n");
        return -1;
    }

    wm_request_t req;
    memset(&req, 0, sizeof(req));
    req.cmd = WM_CMD_CREATE;
    req.w = (uint32_t)(term.cols * ren.cell_w);
    req.h = (uint32_t)(term.rows * ren.cell_h);
    req.pid = (uint32_t)getpid();
    util_copy_str(req.title, sizeof(req.title), MDTERM_TITLE);

    if (wm_ipc_call(wmc.mailbox, &req, &resp, 3000) != 0 || !resp.success) {
        printf("mdterm: window create failed: %s\n",
               resp.message[0] ? resp.message : "(no response)");
        return -1;
    }
    wmc.win_id = resp.win_id;

    wmc.pixels_size = (size_t)resp.shm_size;
    wmc.pixels = (uint32_t *)wmc_map(resp.shm_name, wmc.pixels_size);
    wmc.keys_size = (size_t)resp.key_shm_size;
    wmc.keys = (wm_key_ring_t *)wmc_map(resp.key_shm_name, wmc.keys_size);
    if (!wmc.pixels || !wmc.keys) {
        printf("mdterm: couldn't map window buffers\n");
        return -1;
    }
    util_copy_str(wmc.shm_name, sizeof(wmc.shm_name), resp.shm_name);
    ren_attach(wmc.pixels, (int)req.w, (int)req.h, wmc.pixels_size);
    wmc.focused = 1;
    wm_set_cursor(wmc.mailbox, wmc.win_id, WM_CURSOR_TEXT);
    return 0;
}

/* mdw swaps the pixel segment on maximize/restore/resize; follow it. */
static void wmc_remap(const wm_win_info_t *me) {
    if (me->w < ren.cell_w || me->h < ren.cell_h) return;
    uint32_t *np = (uint32_t *)wmc_map(me->shm_name, (size_t)me->shm_size);
    if (!np) return;

    if (wmc.pixels) munmap(wmc.pixels, wmc.pixels_size);
    wmc.pixels = np;
    wmc.pixels_size = (size_t)me->shm_size;
    util_copy_str(wmc.shm_name, sizeof(wmc.shm_name), me->shm_name);
    ren_attach(np, me->w, me->h, wmc.pixels_size);

    term_resize(me->w / ren.cell_w, me->h / ren.cell_h);
    shell_sync_winsize();
    term_mark_all();
}

/*
 * The protocol has no close or resize notification, so poll the window list.
 * A missing win_id means the user closed us. CLOSE is skipped in that case:
 * the slot may already belong to another client.
 */
static void wmc_sync(void) {
    static wm_win_info_t infos[WM_MAX_WINDOWS];
    int n = wm_list_windows(wmc.mailbox, infos);
    if (n < 0) return;

    const wm_win_info_t *me = NULL;
    for (int i = 0; i < n; i++) {
        if (infos[i].win_id == wmc.win_id) {
            me = &infos[i];
            break;
        }
    }
    if (!me) {
        wmc.gone = 1;
        return;
    }

    int focused = me->focused != 0;
    if (focused != wmc.focused) {
        wmc.focused = focused;
        ren_cursor_wake(time_ms());
    }
    if (strcmp(me->shm_name, wmc.shm_name) != 0 ||
        me->w != ren.width || me->h != ren.height)
        wmc_remap(me);
}

static void wmc_close(void) {
    if (!wmc.gone && wmc.mailbox) {
        wm_request_t req;
        wm_response_t resp;
        memset(&req, 0, sizeof(req));
        req.cmd = WM_CMD_CLOSE;
        req.win_id = wmc.win_id;
        wm_ipc_call(wmc.mailbox, &req, &resp, 2000);
    }
    if (wmc.keys) munmap(wmc.keys, wmc.keys_size);
    if (wmc.pixels) munmap(wmc.pixels, wmc.pixels_size);
}

/* ------------------------------------------------------------------ */
/* key: mdw key events -> terminal bytes                               */
/* ------------------------------------------------------------------ */

static size_t key_fmt_csi(char *out, int num, char fin, int mod) {
    size_t n = 0;
    out[n++] = 0x1B;
    out[n++] = '[';
    if (num > 0) n += util_fmt_uint(out + n, (unsigned)num);
    if (mod > 1) {
        if (num == 0) out[n++] = '1';
        out[n++] = ';';
        n += util_fmt_uint(out + n, (unsigned)mod);
    }
    out[n++] = fin;
    return n;
}

static size_t key_fmt_cursor(char *out, char fin, int mod) {
    if (mod > 1) return key_fmt_csi(out, 0, fin, mod);
    out[0] = 0x1B;
    out[1] = term.app_cursor ? 'O' : '[';
    out[2] = fin;
    return 3;
}

static size_t key_fmt_ss3(char *out, char fin, int mod) {
    if (mod > 1) return key_fmt_csi(out, 0, fin, mod);
    out[0] = 0x1B;
    out[1] = 'O';
    out[2] = fin;
    return 3;
}

static int key_ctrl_char(char c) {
    if (c >= 'a' && c <= 'z') return c - 'a' + 1;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 1;
    switch (c) {
    case '@':
    case ' ':
    case '2':  return 0;
    case '[':  return 27;
    case '\\': return 28;
    case ']':  return 29;
    case '^':
    case '6':  return 30;
    case '_':
    case '-':  return 31;
    case '?':  return 127;
    default:   return -1;
    }
}

static int key_is_modifier(KeyCode k) {
    return k == KEY_LEFT_SHIFT || k == KEY_RIGHT_SHIFT || k == KEY_LEFT_CTRL ||
           k == KEY_RIGHT_CTRL || k == KEY_LEFT_ALT || k == KEY_RIGHT_ALT ||
           k == KEY_CAPS_LOCK || k == KEY_NUM_LOCK || k == KEY_SCROLL_LOCK ||
           k == KEY_LEFT_SUPER || k == KEY_RIGHT_SUPER;
}

/* Returns the byte count written to out (>= 16 bytes of room required). */
static size_t key_translate(const KeyboardEventData *k, char *out) {
    unsigned m = k->modifiers;
    int shift = (m & MOD_SHIFT) != 0;
    int alt = (m & MOD_ALT) != 0;
    int ctrl = (m & MOD_CTRL) != 0;
    int mod = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
    int c = -1;

    switch (k->keycode) {
    case KEY_ARROW_UP:    return key_fmt_cursor(out, 'A', mod);
    case KEY_ARROW_DOWN:  return key_fmt_cursor(out, 'B', mod);
    case KEY_ARROW_RIGHT: return key_fmt_cursor(out, 'C', mod);
    case KEY_ARROW_LEFT:  return key_fmt_cursor(out, 'D', mod);
    case KEY_HOME:        return key_fmt_cursor(out, 'H', mod);
    case KEY_END:         return key_fmt_cursor(out, 'F', mod);
    case KEY_INSERT:      return key_fmt_csi(out, 2, '~', mod);
    case KEY_DELETE:      return key_fmt_csi(out, 3, '~', mod);
    case KEY_PAGE_UP:     return key_fmt_csi(out, 5, '~', mod);
    case KEY_PAGE_DOWN:   return key_fmt_csi(out, 6, '~', mod);
    case KEY_F1:          return key_fmt_ss3(out, 'P', mod);
    case KEY_F2:          return key_fmt_ss3(out, 'Q', mod);
    case KEY_F3:          return key_fmt_ss3(out, 'R', mod);
    case KEY_F4:          return key_fmt_ss3(out, 'S', mod);
    case KEY_F5:          return key_fmt_csi(out, 15, '~', mod);
    case KEY_F6:          return key_fmt_csi(out, 17, '~', mod);
    case KEY_F7:          return key_fmt_csi(out, 18, '~', mod);
    case KEY_F8:          return key_fmt_csi(out, 19, '~', mod);
    case KEY_F9:          return key_fmt_csi(out, 20, '~', mod);
    case KEY_F10:         return key_fmt_csi(out, 21, '~', mod);
    case KEY_F11:         return key_fmt_csi(out, 23, '~', mod);
    case KEY_F12:         return key_fmt_csi(out, 24, '~', mod);
    case KEY_ENTER:       c = '\r'; break;
    case KEY_BACKSPACE:   c = ctrl ? 0x08 : 0x7F; break;
    case KEY_ESCAPE:      c = 0x1B; break;
    case KEY_SPACE:       c = ctrl ? 0 : ' '; break;
    case KEY_TAB:
        if (shift) return key_fmt_csi(out, 0, 'Z', 1);
        c = '\t';
        break;
    default:
        break;
    }

    if (c < 0) {
        if (!k->ascii || (m & MOD_SUPER)) return 0;
        c = (unsigned char)k->ascii;
        if (ctrl) {
            int cc = key_ctrl_char(k->ascii);
            if (cc >= 0) c = cc;
        }
    }
    size_t n = 0;
    if (alt) out[n++] = 0x1B;
    out[n++] = (char)c;
    return n;
}

static void key_handle(const KeyboardEventData *k) {
    int half = term.rows / 2 > 0 ? term.rows / 2 : 1;
    if ((k->modifiers & MOD_SHIFT) && k->keycode == KEY_PAGE_UP) {
        term_view_scroll(half);
        return;
    }
    if ((k->modifiers & MOD_SHIFT) && k->keycode == KEY_PAGE_DOWN) {
        term_view_scroll(-half);
        return;
    }

    char buf[16];
    size_t n = key_translate(k, buf);
    if (!n) return;
    term_view_reset();
    shell_write(buf, n);
    ren_cursor_wake(time_ms());
}

/* ------------------------------------------------------------------ */
/* app: main loop                                                      */
/* ------------------------------------------------------------------ */

static void app_on_shell_exit(void) {
    while (shell_pump() > 0) {
    }
    if (shell.status == 0) {
        app.quit = 1;
        return;
    }
    term_feed_string("\r\n[process exited with status ");
    term_feed_int((shell.status >> 8) & 0xFF);
    term_feed_string("]\r\nPress any key to close.\r\n");
    app.await_key = 1;
}

static void app_run(void) {
    uint64_t last_frame = 0;

    while (!app.quit) {
        uint64_t now = time_ms();

        KeyboardEventData k;
        while (wm_key_ring_pop(wmc.keys, &k)) {
            if (shell.running) key_handle(&k);
            else if (app.await_key && !key_is_modifier(k.keycode)) app.quit = 1;
        }

        if (shell.running) {
            if (shell_pump() > 0) ren_cursor_wake(now);
            if (shell_reap()) app_on_shell_exit();
        }

        if (now - wmc.sync_at >= MDTERM_SYNC_MS) {
            wmc.sync_at = now;
            wmc_sync();
            if (wmc.gone) break;
        }

        ren_tick(now);
        if (now - last_frame >= MDTERM_FRAME_MS) {
            ren_frame();
            last_frame = now;
        }
        yield();
    }
}

int md_main(long argc, char **argv) {
    (void)argc;
    (void)argv;

    if (ren_load_font() != 0) {
        printf("mdterm: couldn't load font %s\n", MDTERM_FONT_PATH);
        return 1;
    }
    if (term_init() != 0) {
        printf("mdterm: out of memory\n");
        return 1;
    }
    if (wmc_open() != 0) return 1;

    term_mark_all();
    if (shell_spawn() != 0) {
        term_feed_string("mdterm: cannot open $/dev/ptmx (kernel TTY missing?)\r\n");
        app.await_key = 1;
    }
    ren_frame();

    app_run();

    shell_shutdown();
    wmc_close();
    term_sb_clear();
    return 0;
}