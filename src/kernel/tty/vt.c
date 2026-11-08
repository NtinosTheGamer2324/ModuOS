// SPDX-License-Identifier: GPL-2.0-only
//
// vt.c - virtual consoles: VT100/xterm-subset emulator + cell grid + renderer.
//
// Each VT owns a rows x cols grid of packed cells and a small scrollback
// ring. Only the active VT in KD_TEXT mode draws to the display (through the
// VGA_* text-cell backend); the others keep updating their grid silently, so
// switching is just "repaint the grid".

#include "moduos/kernel/tty/tty.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/fs/devfs.h"

extern tty_t *tty_slot_alloc(int index, int kind);

/* ------------------------------------------------------------------ */
/* cell packing: cp:21 | fg:4 | bg:4 | attr:3                          */
/* ------------------------------------------------------------------ */

typedef uint32_t vcell_t;

#define A_UNDER 1u
#define A_REV   2u
#define A_BOLD  4u

#define CELL(cp, fg, bg, at) \
    ((vcell_t)((cp) & 0x1FFFFFu) | ((vcell_t)((fg) & 15u) << 21) | \
     ((vcell_t)((bg) & 15u) << 25) | ((vcell_t)((at) & 7u) << 29))
#define CELL_CP(c)  ((c) & 0x1FFFFFu)
#define CELL_FG(c)  (((c) >> 21) & 15u)
#define CELL_BG(c)  (((c) >> 25) & 15u)
#define CELL_AT(c)  (((c) >> 29) & 7u)

#define SB_LINES 128

enum { P_GROUND, P_ESC, P_CSI, P_OSC, P_OSC_ST, P_SKIP1 };

struct vt {
    int      num;                 /* 0-based */
    tty_t   *tty;

    int      rows, cols;
    vcell_t *cells;

    int      cx, cy;
    int      wrap_pending;
    int      top, bot;            /* scroll region, inclusive */

    uint8_t  fg, bg, attr;        /* current rendition (VGA palette idx) */
    int      sv_cx, sv_cy;
    uint8_t  sv_fg, sv_bg, sv_attr;

    int      cursor_visible;
    int      autowrap;

    int      mode;                /* KD_TEXT / KD_GRAPHICS */
    int      gfx_clients;

    /* parser */
    int      pstate;
    int      params[16];
    int      nparams;
    int      have_param;
    int      priv;
    uint32_t u8_cp;
    int      u8_need;
    int      swallow_bs;          /* legacy "ESC[..m\b" quirk */

    /* cursor overlay bookkeeping */
    int      cur_shown, cur_r, cur_c;

    /* scrollback */
    vcell_t *sb;
    int      sb_head, sb_count;
    int      view;
};

static vt_t  g_vt[TTY_NUM_VT];
static int   g_active;            /* 0-based */
static int   g_ready;

/* ------------------------------------------------------------------ */
/* colours                                                             */
/* ------------------------------------------------------------------ */

static const uint8_t ansi_to_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static uint8_t color16(int n) {            /* ANSI 0..15 -> VGA 0..15 */
    return (n < 8) ? ansi_to_vga[n] : (uint8_t)(ansi_to_vga[n - 8] | 8);
}

static uint8_t rgb_to_vga(int r, int g, int b) {
    int dr = r - g, dg = g - b;
    if (dr < 0) dr = -dr;
    if (dg < 0) dg = -dg;
    if (dr < 24 && dg < 24) {                       /* greys */
        int avg = (r + g + b) / 3;
        if (avg < 40)  return 0;
        if (avg < 120) return 8;
        if (avg < 200) return 7;
        return 15;
    }
    int idx = (r >= 128 ? 1 : 0) | (g >= 128 ? 2 : 0) | (b >= 128 ? 4 : 0);
    int bright = (r >= 200 || g >= 200 || b >= 200) ? 8 : 0;
    return (uint8_t)(ansi_to_vga[idx] | bright);
}

static uint8_t xterm256_to_vga(int n) {
    if (n < 0) n = 0;
    if (n < 16)  return color16(n);
    if (n < 232) {
        n -= 16;
        int b = n % 6, g = (n / 6) % 6, r = n / 36;
        return rgb_to_vga(r ? 55 + r * 40 : 0, g ? 55 + g * 40 : 0, b ? 55 + b * 40 : 0);
    }
    int v = 8 + (n - 232) * 10;
    return rgb_to_vga(v, v, v);
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */

static inline vcell_t *at(vt_t *v, int r, int c) { return &v->cells[r * v->cols + c]; }

static inline int vt_visible(vt_t *v) {
    return g_ready && (int)(v - g_vt) == g_active && v->mode == KD_TEXT;
}

static vcell_t blank(vt_t *v) { return CELL(' ', v->fg, v->bg, 0); }

/* cell as shown for (r,c) honouring scrollback view */
static vcell_t view_cell(vt_t *v, int r, int c) {
    int line = r - v->view;
    if (line >= 0) return *at(v, line, c);
    int k = -line;                                   /* lines back, 1 = newest */
    if (!v->sb || k > v->sb_count) return CELL(' ', 7, 0, 0);
    int idx = ((v->sb_head - k) % SB_LINES + SB_LINES) % SB_LINES;
    return v->sb[idx * v->cols + c];
}

static void draw_cell(vt_t *v, int r, int c) {
    vcell_t cell = (v->view ? view_cell(v, r, c) : *at(v, r, c));
    uint8_t fg = (uint8_t)CELL_FG(cell), bg = (uint8_t)CELL_BG(cell), a = (uint8_t)CELL_AT(cell);
    if ((a & A_BOLD) && fg < 8) fg |= 8;
    if (a & A_REV) { uint8_t t = fg; fg = bg; bg = t; }
    if (v->cur_shown && !v->view && r == v->cur_r && c == v->cur_c) { uint8_t t = fg; fg = bg; bg = t; }
    VGA_DrawCell(r, c, CELL_CP(cell), fg, bg, (a & A_UNDER) ? 1 : 0);
}

static void draw_all(vt_t *v) {
    VGA_DrawBatchBegin();
    for (int r = 0; r < v->rows; r++)
        for (int c = 0; c < v->cols; c++) draw_cell(v, r, c);
    VGA_DrawBatchEnd();
}

static void draw_span(vt_t *v, int r, int c0, int c1) {          /* [c0,c1) */
    if (!vt_visible(v) || v->view) return;
    for (int c = c0; c < c1; c++) draw_cell(v, r, c);
}

static void cursor_off(vt_t *v) {
    if (!v->cur_shown) return;
    v->cur_shown = 0;
    if (vt_visible(v) && v->cur_r < v->rows && v->cur_c < v->cols) draw_cell(v, v->cur_r, v->cur_c);
}

static void cursor_on(vt_t *v) {
    if (!vt_visible(v) || !v->cursor_visible || v->view) return;
    v->cur_shown = 1;
    v->cur_r = v->cy;
    v->cur_c = v->cx;
    draw_cell(v, v->cy, v->cx);
}

static void screen_fill_bg(uint8_t bg) { VGA_ClearScreenBG(bg); }

static void repaint(vt_t *v) {
    v->cur_shown = 0;
    screen_fill_bg(0);
    draw_all(v);
    cursor_on(v);
}

/* ------------------------------------------------------------------ */
/* grid operations                                                     */
/* ------------------------------------------------------------------ */

static void clear_cells(vt_t *v, int r, int c0, int c1) {         /* [c0,c1) */
    vcell_t b = blank(v);
    for (int c = c0; c < c1; c++) *at(v, r, c) = b;
}

static void sb_push_line(vt_t *v, int row) {
    if (!v->sb) return;
    memcpy(&v->sb[v->sb_head * v->cols], at(v, row, 0), (size_t)v->cols * sizeof(vcell_t));
    v->sb_head = (v->sb_head + 1) % SB_LINES;
    if (v->sb_count < SB_LINES) v->sb_count++;
}

/* n > 0: content moves up (new blank lines at bottom); n < 0: down */
static void scroll_region(vt_t *v, int top, int bot, int n) {
    int h = bot - top + 1;
    if (h <= 0 || n == 0) return;
    int cnt = n > 0 ? n : -n;
    if (cnt > h) cnt = h;

    int vis = vt_visible(v) && !v->view;
    if (vis) cursor_off(v);

    if (n > 0) {
        if (top == 0 && bot == v->rows - 1)
            for (int i = 0; i < cnt; i++) sb_push_line(v, i);
        if (cnt < h)
            memmove(at(v, top, 0), at(v, top + cnt, 0), (size_t)(h - cnt) * v->cols * sizeof(vcell_t));
        for (int r = bot - cnt + 1; r <= bot; r++) clear_cells(v, r, 0, v->cols);
    } else {
        if (cnt < h)
            memmove(at(v, top + cnt, 0), at(v, top, 0), (size_t)(h - cnt) * v->cols * sizeof(vcell_t));
        for (int r = top; r < top + cnt; r++) clear_cells(v, r, 0, v->cols);
    }

    if (vis) VGA_ScrollCells(top, bot, n > 0 ? cnt : -cnt, v->bg);
}

static void linefeed(vt_t *v) {
    if (v->cy == v->bot) scroll_region(v, v->top, v->bot, 1);
    else if (v->cy < v->rows - 1) v->cy++;
}

static void reverse_index(vt_t *v) {
    if (v->cy == v->top) scroll_region(v, v->top, v->bot, -1);
    else if (v->cy > 0) v->cy--;
}

static void put_cp(vt_t *v, uint32_t cp) {
    if (cp >= 0x300 && cp <= 0x36F) return;                 /* combining marks: ignore */
    if (v->wrap_pending && v->autowrap) {
        v->cx = 0;
        v->wrap_pending = 0;
        linefeed(v);
    }
    *at(v, v->cy, v->cx) = CELL(cp, v->fg, v->bg, v->attr);
    if (vt_visible(v) && !v->view) draw_cell(v, v->cy, v->cx);
    if (v->cx >= v->cols - 1) { if (v->autowrap) v->wrap_pending = 1; }
    else v->cx++;
}

static void erase_in_line(vt_t *v, int mode) {
    int c0 = 0, c1 = v->cols;
    if (mode == 0) c0 = v->cx;
    else if (mode == 1) c1 = v->cx + 1;
    clear_cells(v, v->cy, c0, c1);
    draw_span(v, v->cy, c0, c1);
}

static void erase_in_display(vt_t *v, int mode) {
    if (mode == 3) { v->sb_count = 0; v->sb_head = 0; return; }
    int r0 = 0, r1 = v->rows;                                   /* rows [r0,r1) fully */
    if (mode == 0) {
        erase_in_line(v, 0);
        r0 = v->cy + 1;
    } else if (mode == 1) {
        erase_in_line(v, 1);
        r1 = v->cy;
    }
    for (int r = r0; r < r1; r++) { clear_cells(v, r, 0, v->cols); draw_span(v, r, 0, v->cols); }
}

static void insert_chars(vt_t *v, int n) {
    if (n > v->cols - v->cx) n = v->cols - v->cx;
    vcell_t *row = at(v, v->cy, 0);
    for (int c = v->cols - 1; c >= v->cx + n; c--) row[c] = row[c - n];
    for (int c = v->cx; c < v->cx + n; c++) row[c] = blank(v);
    draw_span(v, v->cy, v->cx, v->cols);
}

static void delete_chars(vt_t *v, int n) {
    if (n > v->cols - v->cx) n = v->cols - v->cx;
    vcell_t *row = at(v, v->cy, 0);
    for (int c = v->cx; c + n < v->cols; c++) row[c] = row[c + n];
    for (int c = v->cols - n; c < v->cols; c++) row[c] = blank(v);
    draw_span(v, v->cy, v->cx, v->cols);
}

static void erase_chars(vt_t *v, int n) {
    int e = v->cx + n; if (e > v->cols) e = v->cols;
    clear_cells(v, v->cy, v->cx, e);
    draw_span(v, v->cy, v->cx, e);
}

static void reset_state(vt_t *v) {
    v->fg = 7; v->bg = 0; v->attr = 0;
    v->cx = v->cy = 0;
    v->wrap_pending = 0;
    v->top = 0; v->bot = v->rows - 1;
    v->cursor_visible = 1;
    v->autowrap = 1;
    v->pstate = P_GROUND;
    v->u8_need = 0;
    v->sv_cx = v->sv_cy = 0;
    v->sv_fg = 7; v->sv_bg = 0; v->sv_attr = 0;
}

/* ------------------------------------------------------------------ */
/* escape sequences                                                    */
/* ------------------------------------------------------------------ */

static inline int P(vt_t *v, int i, int def) {
    return (i < v->nparams && v->params[i] > 0) ? v->params[i] : def;
}

static void reply(vt_t *v, const char *s) {
    tty_receive_locked(v->tty, (const uint8_t *)s, strlen(s));
}

static void sgr(vt_t *v) {
    if (v->nparams == 0) { v->fg = 7; v->bg = 0; v->attr = 0; return; }
    for (int i = 0; i < v->nparams; i++) {
        int p = v->params[i];
        if (p < 0) p = 0;
        switch (p) {
            case 0:  v->fg = 7; v->bg = 0; v->attr = 0; break;
            case 1:  v->attr |= A_BOLD; break;
            case 4:  v->attr |= A_UNDER; break;
            case 7:  v->attr |= A_REV; break;
            case 22: v->attr &= (uint8_t)~A_BOLD; break;
            case 24: v->attr &= (uint8_t)~A_UNDER; break;
            case 27: v->attr &= (uint8_t)~A_REV; break;
            case 39: v->fg = 7; break;
            case 49: v->bg = 0; break;
            case 38: case 48: {
                uint8_t col = 7;
                if (i + 2 < v->nparams && v->params[i + 1] == 5) {
                    col = xterm256_to_vga(v->params[i + 2]); i += 2;
                } else if (i + 4 < v->nparams && v->params[i + 1] == 2) {
                    col = rgb_to_vga(v->params[i + 2], v->params[i + 3], v->params[i + 4]); i += 4;
                } else break;
                if (p == 38) v->fg = col; else v->bg = col;
                break;
            }
            default:
                if (p >= 30 && p <= 37)       v->fg = ansi_to_vga[p - 30];
                else if (p >= 40 && p <= 47)  v->bg = ansi_to_vga[p - 40];
                else if (p >= 90 && p <= 97)  v->fg = color16(p - 90 + 8);
                else if (p >= 100 && p <= 107) v->bg = color16(p - 100 + 8);
                break;
        }
    }
}

static void set_mode(vt_t *v, int on) {
    if (!v->priv) return;
    for (int i = 0; i < v->nparams; i++) {
        switch (v->params[i]) {
            case 7:  v->autowrap = on; if (!on) v->wrap_pending = 0; break;
            case 25: v->cursor_visible = on; break;
            default: break;      /* 1, 12, 47, 1049, 2004 ... accepted and ignored */
        }
    }
}

static void clampcur(vt_t *v) {
    if (v->cx < 0) v->cx = 0;
    if (v->cx >= v->cols) v->cx = v->cols - 1;
    if (v->cy < 0) v->cy = 0;
    if (v->cy >= v->rows) v->cy = v->rows - 1;
}

static void csi_dispatch(vt_t *v, uint8_t f) {
    if (v->have_param && v->nparams < 16) v->nparams++;
    int n;

    switch (f) {
        case 'A': v->cy -= P(v, 0, 1); if (v->cy < v->top && v->cy + P(v, 0, 1) >= v->top) v->cy = v->top; v->wrap_pending = 0; break;
        case 'B': n = P(v, 0, 1); v->cy += n; if (v->cy > v->bot && v->cy - n <= v->bot) v->cy = v->bot; v->wrap_pending = 0; break;
        case 'C': case 'a': v->cx += P(v, 0, 1); v->wrap_pending = 0; break;
        case 'D': v->cx -= P(v, 0, 1); v->wrap_pending = 0; break;
        case 'E': v->cy += P(v, 0, 1); v->cx = 0; v->wrap_pending = 0; break;
        case 'F': v->cy -= P(v, 0, 1); v->cx = 0; v->wrap_pending = 0; break;
        case 'G': case '`': v->cx = P(v, 0, 1) - 1; v->wrap_pending = 0; break;
        case 'd': v->cy = P(v, 0, 1) - 1; v->wrap_pending = 0; break;
        case 'e': v->cy += P(v, 0, 1); v->wrap_pending = 0; break;
        case 'H': case 'f': v->cy = P(v, 0, 1) - 1; v->cx = P(v, 1, 1) - 1; v->wrap_pending = 0; break;
        case 'J': erase_in_display(v, v->nparams ? v->params[0] : 0); break;
        case 'K': erase_in_line(v, v->nparams ? v->params[0] : 0); break;
        case 'L':
            if (v->cy >= v->top && v->cy <= v->bot) scroll_region(v, v->cy, v->bot, -P(v, 0, 1));
            v->cx = 0; v->wrap_pending = 0;
            break;
        case 'M':
            if (v->cy >= v->top && v->cy <= v->bot) scroll_region(v, v->cy, v->bot, P(v, 0, 1));
            v->cx = 0; v->wrap_pending = 0;
            break;
        case '@': insert_chars(v, P(v, 0, 1)); break;
        case 'P': delete_chars(v, P(v, 0, 1)); break;
        case 'X': erase_chars(v, P(v, 0, 1)); break;
        case 'S': scroll_region(v, v->top, v->bot, P(v, 0, 1)); break;
        case 'T': scroll_region(v, v->top, v->bot, -P(v, 0, 1)); break;
        case 'm': sgr(v); break;
        case 'h': set_mode(v, 1); break;
        case 'l': set_mode(v, 0); break;
        case 'r': {
            int t = P(v, 0, 1) - 1, b = P(v, 1, v->rows) - 1;
            if (b >= v->rows) b = v->rows - 1;
            if (t < b) { v->top = t; v->bot = b; v->cx = 0; v->cy = 0; v->wrap_pending = 0; }
            break;
        }
        case 's': v->sv_cx = v->cx; v->sv_cy = v->cy; v->sv_fg = v->fg; v->sv_bg = v->bg; v->sv_attr = v->attr; break;
        case 'u': v->cx = v->sv_cx; v->cy = v->sv_cy; v->fg = v->sv_fg; v->bg = v->sv_bg; v->attr = v->sv_attr; v->wrap_pending = 0; break;
        case 'n': {
            int q = v->nparams ? v->params[0] : 0;
            if (q == 5) reply(v, "\x1b[0n");
            else if (q == 6) {
                char b[24]; int len = 0;
                int r = v->cy + 1, c = v->cx + 1;
                b[len++] = 0x1b; b[len++] = '[';
                char t[8]; int tl = 0; int x = r; do { t[tl++] = (char)('0' + x % 10); x /= 10; } while (x);
                while (tl) b[len++] = t[--tl];
                b[len++] = ';';
                x = c; do { t[tl++] = (char)('0' + x % 10); x /= 10; } while (x);
                while (tl) b[len++] = t[--tl];
                b[len++] = 'R'; b[len] = 0;
                reply(v, b);
            }
            break;
        }
        case 'c': if (!v->priv) reply(v, "\x1b[?6c"); break;
        default: break;
    }
    clampcur(v);
    if (f == 'm') v->swallow_bs = 1;
}

static void esc_dispatch(vt_t *v, uint8_t c) {
    switch (c) {
        case '[': v->pstate = P_CSI; v->nparams = 0; v->have_param = 0; v->priv = 0;
                  for (int i = 0; i < 16; i++) v->params[i] = 0;
                  return;
        case ']': v->pstate = P_OSC; return;
        case '(': case ')': case '*': case '+': case '#': v->pstate = P_SKIP1; return;
        case '7': v->sv_cx = v->cx; v->sv_cy = v->cy; v->sv_fg = v->fg; v->sv_bg = v->bg; v->sv_attr = v->attr; break;
        case '8': v->cx = v->sv_cx; v->cy = v->sv_cy; v->fg = v->sv_fg; v->bg = v->sv_bg; v->attr = v->sv_attr; v->wrap_pending = 0; break;
        case 'M': reverse_index(v); break;
        case 'D': linefeed(v); break;
        case 'E': v->cx = 0; linefeed(v); v->wrap_pending = 0; break;
        case 'c': {
            reset_state(v);
            for (int r = 0; r < v->rows; r++) clear_cells(v, r, 0, v->cols);
            if (vt_visible(v)) { v->cur_shown = 0; screen_fill_bg(0); draw_all(v); }
            break;
        }
        default: break;
    }
    v->pstate = P_GROUND;
}

static void vt_feed(vt_t *v, uint8_t c) {
    switch (v->pstate) {
    case P_GROUND:
        if (v->swallow_bs) { v->swallow_bs = 0; if (c == 0x08) return; }
        if (c >= 0x80) {
            if (c >= 0xF0 && c < 0xF8)      { v->u8_cp = c & 0x07; v->u8_need = 3; }
            else if (c >= 0xE0)             { v->u8_cp = c & 0x0F; v->u8_need = 2; }
            else if (c >= 0xC0)             { v->u8_cp = c & 0x1F; v->u8_need = 1; }
            else if (v->u8_need) {
                v->u8_cp = (v->u8_cp << 6) | (c & 0x3F);
                if (--v->u8_need == 0) put_cp(v, v->u8_cp);
            }
            return;
        }
        v->u8_need = 0;
        if (c >= 0x20 && c != 0x7F) { put_cp(v, c); return; }
        switch (c) {
            case 0x08: v->wrap_pending = 0; if (v->cx > 0) v->cx--; break;
            case 0x09: v->wrap_pending = 0; v->cx = (v->cx | 7) + 1; if (v->cx >= v->cols) v->cx = v->cols - 1; break;
            case 0x0A: case 0x0B: case 0x0C: v->wrap_pending = 0; linefeed(v); break;
            case 0x0D: v->cx = 0; v->wrap_pending = 0; break;
            case 0x1B: v->pstate = P_ESC; break;
            default: break;
        }
        return;

    case P_ESC:
        if (c == 0x1B) return;
        if (c == 0x18 || c == 0x1A) { v->pstate = P_GROUND; return; }
        esc_dispatch(v, c);
        return;

    case P_CSI:
        if (c == 0x1B) { v->pstate = P_ESC; return; }
        if (c == 0x18 || c == 0x1A) { v->pstate = P_GROUND; return; }
        if (c >= '0' && c <= '9') {
            if (v->nparams < 16) {
                v->params[v->nparams] = v->params[v->nparams] * 10 + (c - '0');
                if (v->params[v->nparams] > 65535) v->params[v->nparams] = 65535;
                v->have_param = 1;
            }
            return;
        }
        if (c == ';' || c == ':') {
            if (v->nparams < 16) v->nparams++;
            v->have_param = 0;
            if (v->nparams < 16) v->params[v->nparams] = 0;
            return;
        }
        if (c >= '<' && c <= '?') { v->priv = 1; return; }
        if (c >= 0x20 && c <= 0x2F) return;                      /* intermediates */
        if (c >= 0x40 && c <= 0x7E) {
            v->pstate = P_GROUND;
            csi_dispatch(v, c);
            return;
        }
        return;

    case P_OSC:
        if (c == 0x07) v->pstate = P_GROUND;
        else if (c == 0x1B) v->pstate = P_OSC_ST;
        return;
    case P_OSC_ST:
        if (c == '\\') v->pstate = P_GROUND;
        else { v->pstate = P_ESC; vt_feed(v, c); }
        return;
    case P_SKIP1:
        v->pstate = P_GROUND;
        return;
    }
}

/* ------------------------------------------------------------------ */
/* public vt API                                                       */
/* ------------------------------------------------------------------ */

void vt_write_locked(vt_t *v, const uint8_t *buf, size_t n) {
    if (!v || !v->cells) return;
    if (v->view) { v->view = 0; if (vt_visible(v)) { v->cur_shown = 0; draw_all(v); } }

    int vis = vt_visible(v);
    if (vis) { cursor_off(v); VGA_DrawBatchBegin(); }
    for (size_t i = 0; i < n; i++) vt_feed(v, buf[i]);
    if (vis) { VGA_DrawBatchEnd(); cursor_on(v); }
}

void vt_scroll_view_locked(vt_t *v, int delta) {
    if (!v || !v->cells || !v->sb) return;
    int nv = v->view + delta;
    if (nv < 0) nv = 0;
    if (nv > v->sb_count) nv = v->sb_count;
    if (nv == v->view) return;
    if (vt_visible(v)) cursor_off(v);
    v->view = nv;
    if (vt_visible(v)) { draw_all(v); if (!nv) cursor_on(v); }
}

void vt_clear_locked(vt_t *v) {
    for (int r = 0; r < v->rows; r++) clear_cells(v, r, 0, v->cols);
    v->cx = v->cy = 0; v->wrap_pending = 0; v->view = 0;
    if (vt_visible(v)) { v->cur_shown = 0; screen_fill_bg(0); cursor_on(v); }
}

int vt_get_mode(vt_t *v) { return v ? v->mode : KD_TEXT; }
int vt_number(vt_t *v)   { return v ? v->num + 1 : 0; }
int vt_active(void)      { return g_active + 1; }

uint16_t vt_mode_mask(void) {
    uint16_t m = 0;
    for (int i = 0; i < TTY_NUM_VT; i++) if (g_vt[i].mode == KD_GRAPHICS) m |= (uint16_t)(1u << i);
    return m;
}

static void announce_switch(int vtnum, int became_active) {
    Event e;
    memset(&e, 0, sizeof(e));
    e.type = EVENT_VT_SWITCH;
    e.data.keyboard.scancode = (uint8_t)vtnum;
    e.data.keyboard.modifiers = (uint8_t)(became_active ? 1 : 0);
    devfs_input_push_event(&e);
}

void vt_set_mode_locked(vt_t *v, int kd) {
    if (!v) return;
    kd = (kd == KD_GRAPHICS) ? KD_GRAPHICS : KD_TEXT;
    if (v->mode == kd) return;
    if (vt_visible(v)) cursor_off(v);
    v->mode = kd;
    if (kd == KD_TEXT && (int)(v - g_vt) == g_active && g_ready) repaint(v);
}

int vt_switch_locked(int n) {
    int idx = n - 1;
    if (idx < 0 || idx >= TTY_NUM_VT) return -1;
    if (idx == g_active) return 0;

    vt_t *old = &g_vt[g_active];
    cursor_off(old);
    int old_gfx = (old->mode == KD_GRAPHICS);

    g_active = idx;
    vt_t *v = &g_vt[idx];

    if (old_gfx) announce_switch(old->num + 1, 0);
    if (v->mode == KD_GRAPHICS) {
        announce_switch(v->num + 1, 1);
    } else {
        vt_geometry_changed_locked();     /* no-op unless the display changed */
        repaint(v);
        announce_switch(v->num + 1, 1);
    }
    return 0;
}

int vt_switch(int n) {
    uint64_t f; tty_lock(&f);
    int r = vt_switch_locked(n);
    tty_unlock(f);
    return r;
}

void vt_redraw_active_locked(void) {
    vt_t *v = &g_vt[g_active];
    if (vt_visible(v)) repaint(v);
}

/* gfx clients ------------------------------------------------------- */

void vt_graphics_client_open(void) {
    uint64_t f; tty_lock(&f);
    vt_t *v = &g_vt[g_active];
    extern struct process *tty_current_process(void);
    tty_t *t = tty_for_process(tty_current_process(), 0);
    if (t && t->kind == TTY_KIND_VT && t->vt) v = t->vt;
    v->gfx_clients++;
    vt_set_mode_locked(v, KD_GRAPHICS);
    tty_unlock(f);
}

void vt_graphics_client_close(void) {
    uint64_t f; tty_lock(&f);
    extern struct process *tty_current_process(void);
    tty_t *t = tty_for_process(tty_current_process(), 0);
    vt_t *v = (t && t->kind == TTY_KIND_VT && t->vt) ? t->vt : &g_vt[g_active];
    if (v->gfx_clients > 0 && --v->gfx_clients == 0) vt_set_mode_locked(v, KD_TEXT);
    tty_unlock(f);
}

/* geometry / allocation --------------------------------------------- */

static int vt_alloc_buffers(vt_t *v, int rows, int cols) {
    vcell_t *nc = (vcell_t *)kmalloc((size_t)rows * cols * sizeof(vcell_t));
    if (!nc) return -1;
    vcell_t *nsb = (vcell_t *)kmalloc((size_t)SB_LINES * cols * sizeof(vcell_t));   /* best effort */

    for (int i = 0; i < rows * cols; i++) nc[i] = CELL(' ', 7, 0, 0);

    if (v->cells) {
        int rr = rows < v->rows ? rows : v->rows;
        int cc = cols < v->cols ? cols : v->cols;
        for (int r = 0; r < rr; r++)
            memcpy(&nc[r * cols], &v->cells[r * v->cols], (size_t)cc * sizeof(vcell_t));
        kfree(v->cells);
    }
    if (v->sb) kfree(v->sb);

    v->cells = nc;
    v->sb = nsb;
    v->sb_head = v->sb_count = 0;
    v->view = 0;
    v->rows = rows;
    v->cols = cols;
    v->top = 0; v->bot = rows - 1;
    clampcur(v);
    v->wrap_pending = 0;
    v->cur_shown = 0;
    if (v->tty) { v->tty->winsize.ws_row = (uint16_t)rows; v->tty->winsize.ws_col = (uint16_t)cols; }
    return 0;
}

static void query_geometry(int *rows, int *cols) {
    int r = 0, c = 0;
    VGA_GetTextGeometry(&r, &c);
    if (r < 4 || c < 20) { r = 25; c = 80; }
    if (r > 128) r = 128;
    if (c > 256) c = 256;
    *rows = r; *cols = c;
}

void vt_geometry_changed_locked(void) {
    int rows, cols;
    query_geometry(&rows, &cols);
    if (rows == g_vt[0].rows && cols == g_vt[0].cols) return;
    for (int i = 0; i < TTY_NUM_VT; i++) {
        vt_t *v = &g_vt[i];
        if (vt_alloc_buffers(v, rows, cols) != 0) continue;
    }
    if (g_ready) repaint(&g_vt[g_active]);
}

void vt_set_ready(int ready) {
    g_ready = ready;
    VGA_SetConsoleOwned(ready ? 1 : 0);
    if (ready) {
        vt_geometry_changed_locked();
        repaint(&g_vt[g_active]);
    }
}

void vt_init_all(void) {
    int rows, cols;
    query_geometry(&rows, &cols);

    for (int i = 0; i < TTY_NUM_VT; i++) {
        vt_t *v = &g_vt[i];
        memset(v, 0, sizeof(*v));
        v->num = i;
        v->tty = tty_slot_alloc(i, TTY_KIND_VT);
        if (!v->tty) continue;
        v->tty->vt = v;
        if (vt_alloc_buffers(v, rows, cols) != 0) {
            com_write_string(COM1_PORT, "[TTY] VT buffer allocation failed\n");
            continue;
        }
        reset_state(v);
        v->mode = KD_TEXT;
    }
    g_active = 0;
}
