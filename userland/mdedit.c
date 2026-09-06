/*
 * mdedit.c — ModuOS graphical text editor
 *
 * Standalone fullscreen app: owns its own NodGL device/context and draws
 * straight into the screen texture's mapped backbuffer.
 *  This is the "simple app" pattern NodGL.h describes (see its
 * "Simple Global API" comment) taken one level down to the Device/Context
 * API so we get direct pixel access + font rendering.
 *
 * Usage:  mdedit [path]
 *   No path -> starts an untitled empty buffer; Ctrl+S will prompt for
 *              a filename.
 *
 * Keys:
 *   Arrows            move cursor
 *   Home / End        start / end of line
 *   Page Up/Down      scroll one screen
 *   Backspace/Delete  delete char before/after cursor
 *   Enter             split line
 *   Tab               insert spaces
 *   Ctrl+S  (0x13)    save (prompts for filename if none yet)
 *   Ctrl+Q  (0x11)    quit (asks again if there are unsaved changes)
 *   Esc               cancel filename prompt
 *
 * KEY CONSTANTS: matched against the real events.h. Note the arrow keys
 * are KEY_ARROW_UP/DOWN/LEFT/RIGHT (not KEY_UP/DOWN/LEFT/RIGHT). There's
 * no KEY_S / KEY_Q in this KeyCode enum (letter keys aren't broken out —
 * see its "Add more as needed" comment), so Ctrl+S / Ctrl+Q are detected
 * two ways at once: the classic ASCII control-code convention
 * (Ctrl+letter = letter - 'A' + 1, so Ctrl+S=0x13, Ctrl+Q=0x11) OR
 * modifiers & MOD_CTRL together with ->ascii=='s'/'q' (in case ->ascii
 * carries the plain letter alongside a separate modifier flag instead).
 * Printable characters, Backspace, Enter and Tab are matched on ->ascii
 * as well as ->keycode,
 * so basic typing/editing is robust either way.
 *
 * Copyright © 2026 New Technologies Software — GPL v2.0
 */

#include "libc.h"
#include "NodGL.h"
#include "lib_fnt.h"
#include "../include/moduos/kernel/events/events.h"

/* ============================================================
   Assets / config
   ============================================================ */
#define FONT_PRIMARY   "/ModuOS/shared/assets/fonts/Terminus.fnt"
#define FONT_FALLBACK  "/ModuOS/shared/assets/fonts/Unicode.fnt"
#define TAB_WIDTH      4
#define MARGIN_X       6
#define MARGIN_Y       4
#define GUTTER_DIGITS  5              /* room for up to 99999 line numbers */

/* Bake GUTTER_DIGITS into a literal format string at compile time — this
 * codebase's snprintf() only reads a *literal* digit-run for field width,
 * it does NOT support '*' (pulling width from a va_arg), so "%*u" would
 * silently misparse and desync the rest of the argument list. */
#define STR2(x) #x
#define STR(x) STR2(x)
#define GUTTER_FMT "%" STR(GUTTER_DIGITS) "u"

/* Colors (0xAARRGGBB) */
#define COL_BG          0xFF14181F
#define COL_TEXT        0xFFDCE2EE
#define COL_GUTTER_BG   0xFF0F1218
#define COL_GUTTER_TXT  0xFF565E70
#define COL_CURSOR      0xFF2F6FE0
#define COL_CURSOR_TEXT 0xFFF2F5FA
#define COL_STATUS_BG   0xFF1B2130
#define COL_STATUS_TXT  0xFFDCE2EE
#define COL_STATUS_MOD  0xFFE0483F

/* ============================================================
   Text buffer: array of growable lines
   ============================================================ */
typedef struct {
    char   *data;
    size_t  len;
    size_t  cap;
} line_t;

typedef struct {
    line_t *lines;
    size_t  count;
    size_t  cap;
} buffer_t;

static void line_reserve(line_t *ln, size_t need) {
    if (need <= ln->cap) return;
    size_t ncap = ln->cap ? ln->cap * 2 : 32;
    while (ncap < need) ncap *= 2;
    char *nd = (char *)realloc(ln->data, ncap);
    if (!nd) return; /* out of memory: silently refuse growth */
    ln->data = nd;
    ln->cap = ncap;
}

static void line_set(line_t *ln, const char *s, size_t len) {
    line_reserve(ln, len + 1);
    memcpy(ln->data, s, len);
    ln->data[len] = 0;
    ln->len = len;
}

static void line_insert_char(line_t *ln, size_t pos, char c) {
    if (pos > ln->len) pos = ln->len;
    line_reserve(ln, ln->len + 2);
    memmove(ln->data + pos + 1, ln->data + pos, ln->len - pos + 1);
    ln->data[pos] = c;
    ln->len++;
}

static void line_delete_char(line_t *ln, size_t pos) {
    if (pos >= ln->len) return;
    memmove(ln->data + pos, ln->data + pos + 1, ln->len - pos);
    ln->len--;
}

static void buffer_reserve(buffer_t *buf, size_t need) {
    if (need <= buf->cap) return;
    size_t ncap = buf->cap ? buf->cap * 2 : 64;
    while (ncap < need) ncap *= 2;
    line_t *nl = (line_t *)realloc(buf->lines, ncap * sizeof(line_t));
    if (!nl) return;
    buf->lines = nl;
    buf->cap = ncap;
}

/* Insert a new (empty) line at index `at`, shifting the rest down. */
static void buffer_insert_line(buffer_t *buf, size_t at) {
    if (at > buf->count) at = buf->count;
    buffer_reserve(buf, buf->count + 1);
    memmove(&buf->lines[at + 1], &buf->lines[at], (buf->count - at) * sizeof(line_t));
    buf->lines[at].data = NULL;
    buf->lines[at].len = 0;
    buf->lines[at].cap = 0;
    line_set(&buf->lines[at], "", 0);
    buf->count++;
}

/* Remove line `at`, freeing it. */
static void buffer_delete_line(buffer_t *buf, size_t at) {
    if (at >= buf->count) return;
    free(buf->lines[at].data);
    memmove(&buf->lines[at], &buf->lines[at + 1], (buf->count - at - 1) * sizeof(line_t));
    buf->count--;
}

static void buffer_free(buffer_t *buf) {
    for (size_t i = 0; i < buf->count; i++) free(buf->lines[i].data);
    free(buf->lines);
    buf->lines = NULL;
    buf->count = buf->cap = 0;
}

/* ============================================================
   File load / save
   ============================================================ */
static void buffer_load_file(buffer_t *buf, const char *path) {
    buffer_free(buf);

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        buffer_insert_line(buf, 0); /* new empty document */
        return;
    }

    long fsz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (fsz < 0) fsz = 0;

    char *data = NULL;
    size_t got = 0;
    if (fsz > 0) {
        data = (char *)malloc((size_t)fsz);
        if (data) {
            while (got < (size_t)fsz) {
                ssize_t r = read(fd, data + got, (size_t)fsz - got);
                if (r <= 0) break;
                got += (size_t)r;
            }
        }
    }
    close(fd);

    if (!data || got == 0) {
        if (data) free(data);
        buffer_insert_line(buf, 0);
        return;
    }

    size_t line_start = 0;
    for (size_t i = 0; i < got; i++) {
        if (data[i] == '\n') {
            size_t end = i;
            if (end > line_start && data[end - 1] == '\r') end--; /* strip CR */
            buffer_insert_line(buf, buf->count);
            line_set(&buf->lines[buf->count - 1], data + line_start, end - line_start);
            line_start = i + 1;
        }
    }
    if (line_start < got) {
        size_t end = got;
        if (end > line_start && data[end - 1] == '\r') end--;
        buffer_insert_line(buf, buf->count);
        line_set(&buf->lines[buf->count - 1], data + line_start, end - line_start);
    }
    free(data);

    if (buf->count == 0) buffer_insert_line(buf, 0);
}

/* Returns 0 on success, -1 on failure. */
static int buffer_save_file(buffer_t *buf, const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    for (size_t i = 0; i < buf->count; i++) {
        line_t *ln = &buf->lines[i];
        if (ln->len > 0 && write(fd, ln->data, ln->len) != (ssize_t)ln->len) { close(fd); return -1; }
        char nl = '\n';
        if (write(fd, &nl, 1) != 1) { close(fd); return -1; }
    }
    close(fd);
    return 0;
}

/* ============================================================
   Editor state
   ============================================================ */
typedef enum { MODE_EDIT, MODE_PROMPT_SAVE } editor_mode_t;

static struct {
    NodGL_Device  device;
    NodGL_Context ctx;
    NodGL_Texture screen_tex;
    uint8_t      *bb;
    uint32_t      bb_pitch;
    uint32_t      sw, sh;

    fnt_font_t *font;
    int glyph_w, glyph_h;

    buffer_t buf;
    size_t   cx, cy;        /* cursor: column, row (buffer coords) */
    size_t   top_line;      /* first visible row */
    size_t   left_col;      /* first visible column */
    int      pref_col;      /* remembered column for up/down through short lines */

    int text_x0, text_y0;   /* top-left of the text area (past gutter/margins) */
    int rows_visible, cols_visible;

    char filename[256];
    int  has_filename;
    int  modified;
    int  quit_confirm_pending;

    editor_mode_t mode;
    char prompt_buf[256];
    size_t prompt_len;

    char status_msg[128];

    /* Damage tracking: single bounding dirty rect (screen space),
     * union everything that changed this frame into
     * one rect, then only clear/redraw/present *that*. Redrawing the
     * full screen on every keystroke was the whole source of the lag:
     * with no GPU, every pixel is a CPU store, so a full-screen repaint
     * + full-screen present on every single character is enormously
     * more work than touching the one or two rows that actually changed. */
    int dirty_valid;
    int dirty_x0, dirty_y0, dirty_x1, dirty_y1;

    int quit;
} g;

/* ============================================================
   Pixel / text drawing (backbuffer space)
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

/* Draw text starting at (x,y), clipped to [clip_x0,clip_x1). Returns
 * how many source characters were actually consumed before clip_x1 —
 * unused by callers here but harmless to compute. */
static void draw_text_clip(int x, int y, int clip_x0, int clip_x1, const char *s, size_t n, uint32_t col) {
    if (!g.font || !s) return;
    int cx = x;
    uint8_t r = (col >> 16) & 0xFF, gv = (col >> 8) & 0xFF, b = col & 0xFF;
    for (size_t i = 0; i < n && s[i]; i++) {
        if (cx >= clip_x1) break;
        fnt_glyph_t *gl = fnt_get_glyph(g.font, (uint32_t)(unsigned char)s[i]);
        if (gl) {
            for (int dy = 0; dy < gl->bitmap_height; dy++)
                for (int dx = 0; dx < gl->bitmap_width; dx++)
                    if (cx + dx >= clip_x0 && cx + dx < clip_x1 && fnt_get_pixel(gl, dx, dy))
                        blend(cx + dx, y + dy, r, gv, b, 255);
            cx += g.glyph_w; /* fixed-width grid regardless of glyph->width */
        } else {
            cx += g.glyph_w;
        }
    }
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
   Layout
   ============================================================ */
static void recompute_layout(void) {
    int gutter_w = (GUTTER_DIGITS + 1) * g.glyph_w; /* +1 char gap */
    g.text_x0 = MARGIN_X + gutter_w;
    g.text_y0 = MARGIN_Y;
    int status_h = g.glyph_h + 6;
    int usable_h = (int)g.sh - status_h - MARGIN_Y;
    int usable_w = (int)g.sw - g.text_x0 - MARGIN_X;
    g.rows_visible = usable_h / g.glyph_h;
    g.cols_visible = usable_w / g.glyph_w;
    if (g.rows_visible < 1) g.rows_visible = 1;
    if (g.cols_visible < 1) g.cols_visible = 1;
}

static void scroll_to_cursor(void) {
    if (g.cy < g.top_line) g.top_line = g.cy;
    if (g.cy >= g.top_line + (size_t)g.rows_visible) g.top_line = g.cy - g.rows_visible + 1;
    if (g.cx < g.left_col) g.left_col = g.cx;
    if (g.cx >= g.left_col + (size_t)g.cols_visible) g.left_col = g.cx - g.cols_visible + 1;
}

/* ============================================================
   Damage tracking (screen space) — see the `dirty_*` fields' comment.
   ============================================================ */
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

static void mark_full_dirty(void) { mark_dirty(0, 0, (int)g.sw, (int)g.sh); }

/* Marks one text row, in full-row-width bands aligned to the row grid —
 * this alignment is what lets render_region() below redraw whole rows
 * only, with no partial-row vertical clipping to worry about. */
static void mark_row_dirty(size_t li) {
    if (li < g.top_line) return;
    size_t row = li - g.top_line;
    if (row >= (size_t)g.rows_visible) return;
    int py = g.text_y0 + (int)row * g.glyph_h;
    mark_dirty(0, py, (int)g.sw, g.glyph_h);
}

static void mark_status_dirty(void) {
    int status_h = g.glyph_h + 6;
    mark_dirty(0, (int)g.sh - status_h, (int)g.sw, status_h);
}

/* ============================================================
   Rendering — redraws/uploads/presents ONLY [rx0,ry0)-[rx1,ry1).
   Every mark_*_dirty() call above produces rects aligned to row/status
   boundaries, so the bounding union passed in here is always made up
   of whole rows and/or the whole status bar — never a partial row —
   which is what makes the simple "skip rows outside the band" check
   below safe.
   ============================================================ */
static void render_region(int rx0, int ry0, int rx1, int ry1) {
    fill(rx0, ry0, rx1 - rx0, ry1 - ry0, COL_BG);

    int gutter_w = g.text_x0 - MARGIN_X;
    int status_h = g.glyph_h + 6;
    int status_y = (int)g.sh - status_h;

    int gutter_bottom = status_y < ry1 ? status_y : ry1;
    if (gutter_bottom > ry0) fill(0, ry0, gutter_w, gutter_bottom - ry0, COL_GUTTER_BG);

    for (int row = 0; row < g.rows_visible; row++) {
        int py = g.text_y0 + row * g.glyph_h;
        if (py >= ry1 || py + g.glyph_h <= ry0) continue; /* outside this band */
        size_t li = g.top_line + (size_t)row;
        if (li >= g.buf.count) break;

        char numbuf[16];
        snprintf(numbuf, sizeof(numbuf), GUTTER_FMT, (unsigned)(li + 1));
        draw_text_clip(MARGIN_X, py, 0, gutter_w, numbuf, strlen(numbuf), COL_GUTTER_TXT);

        line_t *ln = &g.buf.lines[li];
        const char *vis = (g.left_col < ln->len) ? ln->data + g.left_col : "";
        size_t vis_len = (g.left_col < ln->len) ? ln->len - g.left_col : 0;
        draw_text_clip(g.text_x0, py, g.text_x0, (int)g.sw - MARGIN_X, vis, vis_len, COL_TEXT);

        if (li == g.cy && g.cx >= g.left_col && g.cx < g.left_col + (size_t)g.cols_visible) {
            int ccx = g.text_x0 + (int)(g.cx - g.left_col) * g.glyph_w;
            fill(ccx, py, g.glyph_w, g.glyph_h, COL_CURSOR);
            if (g.cx < ln->len) {
                char ch[2] = { ln->data[g.cx], 0 };
                draw_text_clip(ccx, py, ccx, ccx + g.glyph_w, ch, 1, COL_CURSOR_TEXT);
            }
        }
    }

    if (status_y < ry1) {
        fill(0, status_y, (int)g.sw, status_h, COL_STATUS_BG);

        if (g.mode == MODE_PROMPT_SAVE) {
            char line[300];
            snprintf(line, sizeof(line), "Save as: %s_", g.prompt_buf);
            draw_text_clip(MARGIN_X, status_y + 3, MARGIN_X, (int)g.sw - MARGIN_X, line, strlen(line), COL_STATUS_TXT);
        } else {
            char left[200];
            const char *name = g.has_filename ? g.filename : "[untitled]";
            snprintf(left, sizeof(left), "%s%s  Ln %u, Col %u",
                     name, g.modified ? " *" : "",
                     (unsigned)(g.cy + 1), (unsigned)(g.cx + 1));
            uint32_t lcol = g.modified ? COL_STATUS_MOD : COL_STATUS_TXT;
            draw_text_clip(MARGIN_X, status_y + 3, MARGIN_X, (int)g.sw - MARGIN_X, left, strlen(left), lcol);

            const char *right = g.status_msg[0] ? g.status_msg : "^S Save  ^Q Quit  ^Shift+S Save As";
            size_t rlen = strlen(right);
            int rx = (int)g.sw - MARGIN_X - (int)rlen * g.glyph_w;
            if (rx < MARGIN_X) rx = MARGIN_X;
            draw_text_clip(rx, status_y + 3, MARGIN_X, (int)g.sw - MARGIN_X, right, rlen, COL_STATUS_TXT);
        }
    }

    /* Rect-scoped upload + present — NodGL_PresentContext would flush the
     * full viewport regardless of what we drew here, undoing the point
     * of only touching [rx0,ry0)-[rx1,ry1) above (see NodGL.h's own note
     * on NodGL_PresentContextRect). */
    NodGL_DrawTexture(g.ctx, g.screen_tex, rx0, ry0, rx0, ry0,
                       (uint32_t)(rx1 - rx0), (uint32_t)(ry1 - ry0));
    NodGL_PresentContextRect(g.ctx, (uint32_t)rx0, (uint32_t)ry0,
                              (uint32_t)(rx1 - rx0), (uint32_t)(ry1 - ry0), 1);
}

/* ============================================================
   Editing operations
   ============================================================ */
static void set_status(const char *msg) {
    safe_strcpy(g.status_msg, sizeof(g.status_msg), msg);
}

static void editor_insert_char(char c) {
    line_t *ln = &g.buf.lines[g.cy];
    line_insert_char(ln, g.cx, c);
    g.cx++;
    g.modified = 1;
    g.status_msg[0] = 0;
}

static void editor_insert_newline(void) {
    line_t *ln = &g.buf.lines[g.cy];
    size_t tail_len = ln->len - g.cx;
    buffer_insert_line(&g.buf, g.cy + 1);
    line_set(&g.buf.lines[g.cy + 1], ln->data + g.cx, tail_len);
    ln->data[g.cx] = 0;
    ln->len = g.cx;
    g.cy++;
    g.cx = 0;
    g.modified = 1;
    g.status_msg[0] = 0;
}

static void editor_backspace(void) {
    if (g.cx > 0) {
        line_delete_char(&g.buf.lines[g.cy], g.cx - 1);
        g.cx--;
        g.modified = 1;
    } else if (g.cy > 0) {
        line_t *prev = &g.buf.lines[g.cy - 1];
        line_t *cur = &g.buf.lines[g.cy];
        size_t new_cx = prev->len;
        line_reserve(prev, prev->len + cur->len + 1);
        memcpy(prev->data + prev->len, cur->data, cur->len + 1);
        prev->len += cur->len;
        buffer_delete_line(&g.buf, g.cy);
        g.cy--;
        g.cx = new_cx;
        g.modified = 1;
    }
    g.status_msg[0] = 0;
}

static void editor_delete_forward(void) {
    line_t *ln = &g.buf.lines[g.cy];
    if (g.cx < ln->len) {
        line_delete_char(ln, g.cx);
        g.modified = 1;
    } else if (g.cy + 1 < g.buf.count) {
        line_t *next = &g.buf.lines[g.cy + 1];
        line_reserve(ln, ln->len + next->len + 1);
        memcpy(ln->data + ln->len, next->data, next->len + 1);
        ln->len += next->len;
        buffer_delete_line(&g.buf, g.cy + 1);
        g.modified = 1;
    }
    g.status_msg[0] = 0;
}

static void editor_insert_tab(void) {
    for (int i = 0; i < TAB_WIDTH; i++) editor_insert_char(' ');
}

static void cursor_clamp_col(void) {
    size_t len = g.buf.lines[g.cy].len;
    if (g.cx > len) g.cx = len;
}

static void cursor_left(void) {
    if (g.cx > 0) g.cx--;
    else if (g.cy > 0) { g.cy--; g.cx = g.buf.lines[g.cy].len; }
    g.pref_col = (int)g.cx;
}
static void cursor_right(void) {
    line_t *ln = &g.buf.lines[g.cy];
    if (g.cx < ln->len) g.cx++;
    else if (g.cy + 1 < g.buf.count) { g.cy++; g.cx = 0; }
    g.pref_col = (int)g.cx;
}
static void cursor_up(void) {
    if (g.cy == 0) return;
    g.cy--;
    g.cx = (size_t)g.pref_col;
    cursor_clamp_col();
}
static void cursor_down(void) {
    if (g.cy + 1 >= g.buf.count) return;
    g.cy++;
    g.cx = (size_t)g.pref_col;
    cursor_clamp_col();
}
static void cursor_home(void) { g.cx = 0; g.pref_col = 0; }
static void cursor_end(void)  { g.cx = g.buf.lines[g.cy].len; g.pref_col = (int)g.cx; }
static void cursor_page_up(void) {
    if ((size_t)g.rows_visible >= g.cy) g.cy = 0;
    else g.cy -= (size_t)g.rows_visible;
    cursor_clamp_col();
}
static void cursor_page_down(void) {
    g.cy += (size_t)g.rows_visible;
    if (g.cy >= g.buf.count) g.cy = g.buf.count - 1;
    cursor_clamp_col();
}

static void do_save(void) {
    if (buffer_save_file(&g.buf, g.filename) == 0) {
        g.modified = 0;
        set_status("Saved");
    } else {
        set_status("Save failed!");
    }
    g.quit_confirm_pending = 0;
}

static void begin_save_prompt(void) {
    g.mode = MODE_PROMPT_SAVE;
    /* Pre-fill with the current path (if any) so Save As is an edit,
     * not a blank retype; also what fires when there's no name yet. */
    safe_strcpy(g.prompt_buf, sizeof(g.prompt_buf), g.has_filename ? g.filename : "");
    g.prompt_len = strlen(g.prompt_buf);
}

static void editor_request_save(void) {
    if (g.has_filename) do_save();
    else begin_save_prompt();
}

static void editor_request_save_as(void) {
    begin_save_prompt();
}

static void editor_request_quit(void) {
    if (!g.modified || g.quit_confirm_pending) {
        g.quit = 1;
        return;
    }
    g.quit_confirm_pending = 1;
    set_status("Unsaved changes! ^Q again to discard, ^S to save.");
}

/* ============================================================
   Input
   ============================================================ */
static void handle_prompt_key(const KeyboardEventData *k) {
    if (k->ascii == '\r' || k->ascii == '\n' || k->keycode == KEY_ENTER) {
        if (g.prompt_len > 0) {
            safe_strcpy(g.filename, sizeof(g.filename), g.prompt_buf);
            g.has_filename = 1;
            g.mode = MODE_EDIT;
            do_save();
        }
        return;
    }
    if (k->keycode == KEY_ESCAPE || k->ascii == 27) {
        g.mode = MODE_EDIT;
        set_status("Save cancelled");
        return;
    }
    if ((k->ascii == '\b' || k->ascii == 127 || k->keycode == KEY_BACKSPACE) && g.prompt_len > 0) {
        g.prompt_len--;
        g.prompt_buf[g.prompt_len] = 0;
        return;
    }
    if (k->ascii >= 32 && k->ascii < 127 && g.prompt_len + 1 < sizeof(g.prompt_buf)) {
        g.prompt_buf[g.prompt_len++] = k->ascii;
        g.prompt_buf[g.prompt_len] = 0;
    }
}

static void editor_handle_key(const KeyboardEventData *k) {
    if (g.mode == MODE_PROMPT_SAVE) {
        handle_prompt_key(k);
        /* Everything the prompt touches (its own text, or falling back
         * to MODE_EDIT's status line) lives inside the status bar rect. */
        mark_status_dirty();
        return;
    }

    /* Ctrl+S / Ctrl+Shift+S / Ctrl+Q — see the KEY CONSTANTS note at the
     * top of this file for why this checks two different encodings at
     * once. Shift+S is checked before plain S since ctrl_held+shift_held
     * would otherwise also satisfy the plain-save condition below. */
    int ctrl_held  = (k->modifiers & MOD_CTRL) != 0;
    int shift_held = (k->modifiers & MOD_SHIFT) != 0;
    int is_s = (k->ascii == 's' || k->ascii == 'S');
    int is_q = (k->ascii == 'q' || k->ascii == 'Q');
    if (ctrl_held && shift_held && is_s) { editor_request_save_as(); mark_status_dirty(); return; }
    if (k->ascii == 0x13 || (ctrl_held && is_s)) { editor_request_save(); mark_status_dirty(); return; }
    if (k->ascii == 0x11 || (ctrl_held && is_q)) { editor_request_quit(); mark_status_dirty(); return; }

    /* Any other key clears a pending quit-confirm and the status message. */
    int was_confirm_pending = g.quit_confirm_pending;
    g.quit_confirm_pending = 0;

    size_t old_cy = g.cy;
    size_t old_top_line = g.top_line;
    size_t old_left_col = g.left_col;
    size_t old_count = g.buf.count;

    switch (k->keycode) {
        case KEY_ARROW_LEFT:  cursor_left();       goto scroll;
        case KEY_ARROW_RIGHT: cursor_right();      goto scroll;
        case KEY_ARROW_UP:    cursor_up();         goto scroll;
        case KEY_ARROW_DOWN:  cursor_down();       goto scroll;
        case KEY_HOME:      cursor_home();       goto scroll;
        case KEY_END:       cursor_end();        goto scroll;
        case KEY_PAGE_UP:   cursor_page_up();    goto scroll;
        case KEY_PAGE_DOWN: cursor_page_down();  goto scroll;
        default: break;
    }

    if (k->ascii == '\b' || k->ascii == 127 || k->keycode == KEY_BACKSPACE) { editor_backspace(); goto scroll; }
    if (k->keycode == KEY_DELETE) { editor_delete_forward(); goto scroll; }
    if (k->ascii == '\r' || k->ascii == '\n' || k->keycode == KEY_ENTER) { editor_insert_newline(); goto scroll; }
    if (k->ascii == '\t' || k->keycode == KEY_TAB) { editor_insert_tab(); goto scroll; }
    if (k->ascii >= 32 && k->ascii < 127) { editor_insert_char((char)k->ascii); goto scroll; }

    /* Unrecognized key (e.g. a bare modifier press): nothing visible
     * changed unless it just cleared a pending quit-confirm message. */
    if (was_confirm_pending) { g.status_msg[0] = 0; mark_status_dirty(); }
    return;

scroll:
    scroll_to_cursor();
    /* Scrolling or a line being added/removed can shift everything below
     * it, so fall back to a full repaint for those. Otherwise (the
     * common case: typing, or moving the cursor without scrolling) only
     * the row the cursor left and the row it's on now actually changed. */
    if (g.top_line != old_top_line || g.left_col != old_left_col || g.buf.count != old_count) {
        mark_full_dirty();
    } else {
        mark_row_dirty(old_cy);
        mark_row_dirty(g.cy);
    }
    mark_status_dirty(); /* Ln/Col and the modified flag change most keystrokes */
}

/* ============================================================
   Entry point
   ============================================================ */
int md_main(long argc, char **argv) {
    memset(&g, 0, sizeof(g));
    g.mode = MODE_EDIT;
    g.pref_col = 0;

    int efd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    if (efd < 0) { printf("mdedit: no event device\n"); return 2; }

    if (NodGL_CreateDevice(NodGL_FEATURE_LEVEL_1_0, &g.device, &g.ctx, NULL) != NodGL_OK) {
        printf("mdedit: NodGL_CreateDevice failed\n");
        close(efd);
        return 1;
    }
    NodGL_GetScreenResolution(g.device, &g.sw, &g.sh);

    NodGL_TextureDesc td; memset(&td, 0, sizeof(td));
    td.width = g.sw; td.height = g.sh;
    td.format = NodGL_FORMAT_R8G8B8A8_UNORM; td.mip_levels = 1;
    if (NodGL_CreateTexture(g.device, &td, &g.screen_tex) != NodGL_OK) {
        printf("mdedit: NodGL_CreateTexture failed\n");
        NodGL_ReleaseDevice(g.device); close(efd);
        return 1;
    }
    if (NodGL_MapResource(g.ctx, g.screen_tex, (void **)&g.bb, &g.bb_pitch) != NodGL_OK) {
        printf("mdedit: NodGL_MapResource failed\n");
        NodGL_ReleaseResource(g.device, g.screen_tex);
        NodGL_ReleaseDevice(g.device); close(efd);
        return 1;
    }

    g.font = try_load_font(FONT_PRIMARY);
    if (!g.font) g.font = try_load_font(FONT_FALLBACK);
    if (!g.font) {
        printf("mdedit: no font available\n");
        NodGL_UnmapResource(g.ctx, g.screen_tex);
        NodGL_ReleaseResource(g.device, g.screen_tex);
        NodGL_ReleaseDevice(g.device); close(efd);
        return 1;
    }
    g.glyph_w = g.font->header.glyph_width;
    g.glyph_h = g.font->header.glyph_height;
    if (g.glyph_w <= 0) g.glyph_w = 8;
    if (g.glyph_h <= 0) g.glyph_h = 16;

    if (argc > 1 && argv[1] && argv[1][0]) {
        safe_strcpy(g.filename, sizeof(g.filename), argv[1]);
        g.has_filename = 1;
        buffer_load_file(&g.buf, g.filename);
    } else {
        buffer_insert_line(&g.buf, 0);
    }

    recompute_layout();
    scroll_to_cursor();
    mark_full_dirty(); /* prime the display with one full paint */

    while (!g.quit) {
        Event ev;
        while (read(efd, &ev, sizeof(ev)) > 0) {
            if (ev.type == EVENT_KEY_PRESSED) editor_handle_key(&ev.data.keyboard);
        }
        if (g.quit) break;

        if (g.dirty_valid) {
            render_region(g.dirty_x0, g.dirty_y0, g.dirty_x1, g.dirty_y1);
            g.dirty_valid = 0;
        }
        /* No damage this iteration -> no clear, no blit, no present, just
         * poll input again 
         */
        yield();
    }

    buffer_free(&g.buf);
    fnt_free_font(g.font);
    NodGL_UnmapResource(g.ctx, g.screen_tex);
    NodGL_ReleaseResource(g.device, g.screen_tex);
    NodGL_ReleaseDevice(g.device);
    close(efd);
    input_flush();
    return 0;
}