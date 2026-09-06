// SPDX-License-Identifier: GPL-2.0-only
//
// ntosiux_ttyman.c - NTOSIUX TTY Manager
// UserFS invoke-based virtual terminal multiplexer with graphical output.
//
// Output goes through NodGL + lib_FNT — never to fd 1/2 (those are kprint).

#include "libc.h"
#include "nodgl.h"
#include "lib_FNT.h"

// Input events (KeyCode, Event, KeyboardEventData, EVENT_* constants) come
// from this shared header. It's just struct/enum definitions — userland
// gets input by opening $/dev/input/event0 and read()-ing Event records
// off it, same as any other file. There is no kernel function to call
// for this; see handle_keyboard_input() below.
#include "../include/moduos/kernel/events/events.h"

// ================================================================
// Config
// ================================================================

#define MAX_TTYS        6
#define MAX_TTY_NAME    32
#define RING_BUF_SIZE   4096
#define TTY_MAX_COLS    256
#define TTY_MAX_ROWS    128
#define FONT_SCALE      1       // set to 2 for HiDPI

// ================================================================
// Colour palette — standard 16-colour ANSI (0xAARRGGBB)
// ================================================================

static const uint32_t g_ansi16[16] = {
    0xFF1E1E1E, // 0  Black
    0xFFCC0000, // 1  Red
    0xFF00CC00, // 2  Green
    0xFFCC8800, // 3  Yellow
    0xFF0000CC, // 4  Blue
    0xFFCC00CC, // 5  Magenta
    0xFF00CCCC, // 6  Cyan
    0xFFCCCCCC, // 7  White
    0xFF555555, // 8  Bright Black
    0xFFFF5555, // 9  Bright Red
    0xFF55FF55, // 10 Bright Green
    0xFFFFFF55, // 11 Bright Yellow
    0xFF5555FF, // 12 Bright Blue
    0xFFFF55FF, // 13 Bright Magenta
    0xFF55FFFF, // 14 Bright Cyan
    0xFFFFFFFF, // 15 Bright White
};

// Build 256-colour xterm palette entry on demand.
// Indices 0-15: same as g_ansi16.
// Indices 16-231: 6x6x6 colour cube.
// Indices 232-255: greyscale ramp.
static uint32_t ansi256_color(int idx) {
    if (idx < 16)  return g_ansi16[idx];
    if (idx < 232) {
        idx -= 16;
        int b = idx % 6; idx /= 6;
        int g = idx % 6; idx /= 6;
        int r = idx;
        uint8_t rv = r ? (55 + r * 40) : 0;
        uint8_t gv = g ? (55 + g * 40) : 0;
        uint8_t bv = b ? (55 + b * 40) : 0;
        return 0xFF000000 | ((uint32_t)rv << 16) | ((uint32_t)gv << 8) | bv;
    }
    // Greyscale 232-255
    uint8_t v = (uint8_t)(8 + (idx - 232) * 10);
    return 0xFF000000 | ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
}

// ================================================================
// Terminal cell
// ================================================================

typedef struct {
    uint32_t codepoint;
    uint32_t fg, bg;
    uint8_t  bold      : 1;
    uint8_t  underline : 1;
    uint8_t  reverse   : 1;
    uint8_t  dirty     : 1;
} tty_cell_t;

// ================================================================
// ANSI escape parser
// ================================================================

#define ANSI_PARAM_MAX 16

typedef enum {
    PARSER_GROUND,    // normal text
    PARSER_ESC,       // saw 0x1B
    PARSER_CSI,       // saw ESC [
    PARSER_OSC,       // saw ESC ] — collect until ST/BEL, then discard
} ansi_state_t;

typedef struct {
    ansi_state_t state;
    int  params[ANSI_PARAM_MAX];
    int  param_count;
    int  cur_digit;     // accumulator for current numeric param
    int  cur_valid;     // have we seen at least one digit for cur param?
    char inter[8];      // intermediate bytes (e.g. '?', '>')
    int  inter_len;
} ansi_parser_t;

// ================================================================
// Per-TTY screen buffer
// ================================================================

typedef struct {
    tty_cell_t cells[TTY_MAX_ROWS][TTY_MAX_COLS];
    int cols, rows;
    int cur_col, cur_row;
    int saved_col, saved_row;
    int scroll_top, scroll_bottom;  // 0-based inclusive
    int cursor_visible;

    // Current SGR state
    uint32_t fg, bg;
    uint8_t  bold, underline, reverse;

    // Next-colour override (for 38;2 / 38;5 sequences)
    int  next_fg_idx;   // -1 = use fg directly
    int  next_bg_idx;

    ansi_parser_t parser;
    int dirty;  // any cell changed since last flush
} tty_screen_t;

// ================================================================
// Global render state (shared across all TTYs, only one displayed)
// ================================================================

static NodGL_Device  g_device;
static NodGL_Context g_ctx;
static fnt_font_t   *g_font;
static int           g_font_w;
static int           g_font_h;
static uint32_t      g_screen_w;
static uint32_t      g_screen_h;

// ================================================================
// Ring buffer type (used for each TTY's stdin/stdout/stderr)
// ================================================================

typedef struct {
    uint8_t  buf[RING_BUF_SIZE];
    uint32_t r, w, count;
} ring_t;

static void ring_push(ring_t *rb, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (rb->count >= RING_BUF_SIZE) break;
        rb->buf[rb->w] = data[i];
        rb->w = (rb->w + 1) % RING_BUF_SIZE;
        rb->count++;
    }
}

static size_t ring_pop(ring_t *rb, uint8_t *out, size_t maxlen) {
    size_t n = 0;
    while (n < maxlen && rb->count > 0) {
        out[n++] = rb->buf[rb->r];
        rb->r = (rb->r + 1) % RING_BUF_SIZE;
        rb->count--;
    }
    return n;
}

// ================================================================
// Per-TTY slot
// ================================================================

typedef struct {
    char        name[MAX_TTY_NAME];
    int         active;
    int         shell_pid;
    ring_t      stdin_ring;
    ring_t      stdout_ring;
    ring_t      stderr_ring;
    tty_screen_t screen;
} vtty_t;

static vtty_t ttys[MAX_TTYS];
static int    current_tty = 0;

// Input device fd — opened once in md_main(), read from every tick in
// handle_keyboard_input(). -1 until init_input() succeeds.
static int    g_input_fd = -1;

// ================================================================
// Screen buffer helpers
// ================================================================

static void screen_init(tty_screen_t *s, int cols, int rows) {
    memset(s, 0, sizeof(*s));
    s->cols           = cols;
    s->rows           = rows;
    s->fg             = g_ansi16[7];   // white
    s->bg             = g_ansi16[0];   // black
    s->scroll_top     = 0;
    s->scroll_bottom  = rows - 1;
    s->cursor_visible = 1;
    s->next_fg_idx    = -1;
    s->next_bg_idx    = -1;

    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            s->cells[r][c].codepoint = ' ';
            s->cells[r][c].fg    = s->fg;
            s->cells[r][c].bg    = s->bg;
            s->cells[r][c].dirty = 1;
        }
    s->dirty = 1;
}

static void screen_set_cell(tty_screen_t *s, int col, int row, uint32_t cp) {
    if (col < 0 || col >= s->cols || row < 0 || row >= s->rows) return;
    tty_cell_t *c = &s->cells[row][col];

    uint32_t fg = s->fg;
    uint32_t bg = s->bg;
    if (s->reverse) { uint32_t t = fg; fg = bg; bg = t; }

    if (c->codepoint == cp && c->fg == fg && c->bg == bg &&
        c->bold == s->bold && c->underline == s->underline) return;

    c->codepoint = cp;
    c->fg        = fg;
    c->bg        = bg;
    c->bold      = s->bold;
    c->underline = s->underline;
    c->reverse   = s->reverse;
    c->dirty     = 1;
    s->dirty     = 1;
}

// Scroll the scroll region up by n lines, filling blank lines at the bottom.
static void screen_scroll_up(tty_screen_t *s, int n) {
    if (n <= 0) return;
    int top = s->scroll_top, bot = s->scroll_bottom;
    for (int row = top; row <= bot - n; row++)
        memcpy(s->cells[row], s->cells[row + n], sizeof(s->cells[0]));
    for (int row = bot - n + 1; row <= bot; row++)
        for (int col = 0; col < s->cols; col++) {
            s->cells[row][col].codepoint = ' ';
            s->cells[row][col].fg    = s->fg;
            s->cells[row][col].bg    = s->bg;
            s->cells[row][col].dirty = 1;
        }
    s->dirty = 1;
}

static void screen_scroll_down(tty_screen_t *s, int n) {
    if (n <= 0) return;
    int top = s->scroll_top, bot = s->scroll_bottom;
    for (int row = bot; row >= top + n; row--)
        memcpy(s->cells[row], s->cells[row - n], sizeof(s->cells[0]));
    for (int row = top; row < top + n; row++)
        for (int col = 0; col < s->cols; col++) {
            s->cells[row][col].codepoint = ' ';
            s->cells[row][col].fg    = s->fg;
            s->cells[row][col].bg    = s->bg;
            s->cells[row][col].dirty = 1;
        }
    s->dirty = 1;
}

static void screen_erase_line(tty_screen_t *s, int row, int col_start, int col_end) {
    for (int c = col_start; c <= col_end && c < s->cols; c++) {
        s->cells[row][c].codepoint = ' ';
        s->cells[row][c].fg    = s->fg;
        s->cells[row][c].bg    = s->bg;
        s->cells[row][c].dirty = 1;
    }
    s->dirty = 1;
}

static void screen_erase_display(tty_screen_t *s, int from_row, int to_row) {
    for (int r = from_row; r <= to_row && r < s->rows; r++)
        screen_erase_line(s, r, 0, s->cols - 1);
}

// Advance cursor, wrapping and scrolling as needed.
static void screen_advance_cursor(tty_screen_t *s) {
    s->cur_col++;
    if (s->cur_col >= s->cols) {
        s->cur_col = 0;
        s->cur_row++;
        if (s->cur_row > s->scroll_bottom) {
            s->cur_row = s->scroll_bottom;
            screen_scroll_up(s, 1);
        }
    }
}

// ================================================================
// SGR (Select Graphic Rendition) — ESC [ ... m
// ================================================================

static void apply_sgr(tty_screen_t *s, int *params, int count) {
    if (count == 0) {
        // ESC[m — full reset
        s->fg        = g_ansi16[7];
        s->bg        = g_ansi16[0];
        s->bold      = 0;
        s->underline = 0;
        s->reverse   = 0;
        return;
    }

    for (int i = 0; i < count; ) {
        int p = params[i];
        switch (p) {
            case 0:
                s->fg        = g_ansi16[7];
                s->bg        = g_ansi16[0];
                s->bold      = 0;
                s->underline = 0;
                s->reverse   = 0;
                break;
            case 1:  s->bold      = 1; break;
            case 4:  s->underline = 1; break;
            case 7:  s->reverse   = 1; break;
            case 22: s->bold      = 0; break;
            case 24: s->underline = 0; break;
            case 27: s->reverse   = 0; break;

            // Foreground 30-37 (normal), 90-97 (bright)
            case 30: case 31: case 32: case 33:
            case 34: case 35: case 36: case 37:
                s->fg = g_ansi16[p - 30 + (s->bold ? 8 : 0)];
                break;
            case 39: s->fg = g_ansi16[7]; break;
            case 90: case 91: case 92: case 93:
            case 94: case 95: case 96: case 97:
                s->fg = g_ansi16[p - 90 + 8];
                break;

            // Background 40-47 (normal), 100-107 (bright)
            case 40: case 41: case 42: case 43:
            case 44: case 45: case 46: case 47:
                s->bg = g_ansi16[p - 40];
                break;
            case 49: s->bg = g_ansi16[0]; break;
            case 100: case 101: case 102: case 103:
            case 104: case 105: case 106: case 107:
                s->bg = g_ansi16[p - 100 + 8];
                break;

            // 256-colour and truecolour foreground: ESC[38;5;n or ESC[38;2;r;g;b
            case 38:
                if (i + 1 < count && params[i + 1] == 5 && i + 2 < count) {
                    s->fg = ansi256_color(params[i + 2]);
                    i += 2;
                } else if (i + 1 < count && params[i + 1] == 2 && i + 4 < count) {
                    uint8_t r = (uint8_t)params[i + 2];
                    uint8_t g = (uint8_t)params[i + 3];
                    uint8_t b = (uint8_t)params[i + 4];
                    s->fg = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                    i += 4;
                }
                break;

            // 256-colour and truecolour background: ESC[48;5;n or ESC[48;2;r;g;b
            case 48:
                if (i + 1 < count && params[i + 1] == 5 && i + 2 < count) {
                    s->bg = ansi256_color(params[i + 2]);
                    i += 2;
                } else if (i + 1 < count && params[i + 1] == 2 && i + 4 < count) {
                    uint8_t r = (uint8_t)params[i + 2];
                    uint8_t g = (uint8_t)params[i + 3];
                    uint8_t b = (uint8_t)params[i + 4];
                    s->bg = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                    i += 4;
                }
                break;

            default: break;
        }
        i++;
    }
}

// ================================================================
// CSI dispatch — called when a complete CSI sequence is ready
// ================================================================

static void dispatch_csi(tty_screen_t *s, ansi_parser_t *p, char final) {
    int *pm    = p->params;
    int  count = p->param_count;

    // Helper: param with default
    #define P(i, def) ((i) < count ? pm[i] : (def))

    switch (final) {

        // ---- Cursor movement ----
        case 'A': s->cur_row -= P(0, 1); if (s->cur_row < 0) s->cur_row = 0; break;
        case 'B': s->cur_row += P(0, 1); if (s->cur_row >= s->rows) s->cur_row = s->rows - 1; break;
        case 'C': s->cur_col += P(0, 1); if (s->cur_col >= s->cols) s->cur_col = s->cols - 1; break;
        case 'D': s->cur_col -= P(0, 1); if (s->cur_col < 0) s->cur_col = 0; break;

        case 'E': // Cursor next line
            s->cur_row += P(0, 1);
            s->cur_col = 0;
            if (s->cur_row >= s->rows) s->cur_row = s->rows - 1;
            break;
        case 'F': // Cursor previous line
            s->cur_row -= P(0, 1);
            s->cur_col = 0;
            if (s->cur_row < 0) s->cur_row = 0;
            break;
        case 'G': // Cursor horizontal absolute
            s->cur_col = P(0, 1) - 1;
            if (s->cur_col < 0) s->cur_col = 0;
            if (s->cur_col >= s->cols) s->cur_col = s->cols - 1;
            break;

        case 'H': // Cursor position (row, col, 1-based)
        case 'f':
            s->cur_row = P(0, 1) - 1;
            s->cur_col = P(1, 1) - 1;
            if (s->cur_row < 0) s->cur_row = 0;
            if (s->cur_row >= s->rows) s->cur_row = s->rows - 1;
            if (s->cur_col < 0) s->cur_col = 0;
            if (s->cur_col >= s->cols) s->cur_col = s->cols - 1;
            break;

        case 'd': // Line position absolute (VPA)
            s->cur_row = P(0, 1) - 1;
            if (s->cur_row < 0) s->cur_row = 0;
            if (s->cur_row >= s->rows) s->cur_row = s->rows - 1;
            break;

        // ---- Erase ----
        case 'J': { // Erase in display
            int n = P(0, 0);
            if (n == 0) screen_erase_display(s, s->cur_row, s->rows - 1);
            else if (n == 1) screen_erase_display(s, 0, s->cur_row);
            else if (n == 2) screen_erase_display(s, 0, s->rows - 1);
            break;
        }
        case 'K': { // Erase in line
            int n = P(0, 0);
            if (n == 0) screen_erase_line(s, s->cur_row, s->cur_col, s->cols - 1);
            else if (n == 1) screen_erase_line(s, s->cur_row, 0, s->cur_col);
            else if (n == 2) screen_erase_line(s, s->cur_row, 0, s->cols - 1);
            break;
        }

        // ---- Scroll ----
        case 'S': screen_scroll_up(s,   P(0, 1)); break;
        case 'T': screen_scroll_down(s, P(0, 1)); break;

        // ---- Insert / delete ----
        case 'L': screen_scroll_down(s, P(0, 1)); break; // Insert line
        case 'M': screen_scroll_up(s,   P(0, 1)); break; // Delete line
        case 'P': { // Delete character
            int n = P(0, 1);
            for (int c = s->cur_col; c < s->cols - n; c++)
                s->cells[s->cur_row][c] = s->cells[s->cur_row][c + n];
            screen_erase_line(s, s->cur_row, s->cols - n, s->cols - 1);
            break;
        }
        case '@': { // Insert character
            int n = P(0, 1);
            for (int c = s->cols - 1; c >= s->cur_col + n; c--)
                s->cells[s->cur_row][c] = s->cells[s->cur_row][c - n];
            screen_erase_line(s, s->cur_row, s->cur_col, s->cur_col + n - 1);
            break;
        }

        // ---- Scroll region ----
        case 'r': // DECSTBM
            s->scroll_top    = P(0, 1) - 1;
            s->scroll_bottom = P(1, s->rows) - 1;
            if (s->scroll_top < 0) s->scroll_top = 0;
            if (s->scroll_bottom >= s->rows) s->scroll_bottom = s->rows - 1;
            s->cur_col = 0; s->cur_row = 0;
            break;

        // ---- SGR ----
        case 'm':
            apply_sgr(s, pm, count);
            break;

        // ---- Save / restore cursor ----
        case 's': s->saved_col = s->cur_col; s->saved_row = s->cur_row; break;
        case 'u': s->cur_col   = s->saved_col; s->cur_row  = s->saved_row; break;

        // ---- Private modes (ESC[?...h / ESC[?...l) ----
        case 'h':
        case 'l': {
            int enable = (final == 'h');
            // Only care about ?25 (cursor visibility) and ?1049 (alt screen — ignore)
            if (p->inter[0] == '?') {
                for (int i = 0; i < count; i++) {
                    if (pm[i] == 25) s->cursor_visible = enable;
                    // 1049 alt screen: we don't maintain a separate alt buffer,
                    // just clear and reset on enable so apps get a clean slate.
                    if (pm[i] == 1049 && enable) {
                        screen_erase_display(s, 0, s->rows - 1);
                        s->cur_col = s->cur_row = 0;
                    }
                }
            }
            break;
        }

        // ---- Cursor shape (DECSCUSR) — accept and ignore ----
        case 'q': break;

        // Anything else: silently ignored.
        default: break;
    }

    #undef P
}

// ================================================================
// ANSI parser — feed one byte at a time
// ================================================================

static void parser_reset(ansi_parser_t *p) {
    p->state       = PARSER_GROUND;
    p->param_count = 0;
    p->cur_digit   = 0;
    p->cur_valid   = 0;
    p->inter_len   = 0;
    memset(p->inter, 0, sizeof(p->inter));
}

static void parser_commit_param(ansi_parser_t *p) {
    if (p->param_count < ANSI_PARAM_MAX) {
        p->params[p->param_count++] = p->cur_valid ? p->cur_digit : 0;
    }
    p->cur_digit = 0;
    p->cur_valid = 0;
}

// Feed one decoded codepoint (UTF-8 assumed for now, treating each byte
// in 0x80-0xFF as a continuation/placeholder — full UTF-8 decode left
// as an exercise since most terminal output is ASCII anyway).
static void screen_put_char(tty_screen_t *s, uint32_t cp) {
    ansi_parser_t *p = &s->parser;

    // Handle C0 controls first (they're processed in GROUND and CSI states)
    if (cp < 0x20 || cp == 0x7F) {
        switch (cp) {
            case '\r':
                s->cur_col = 0;
                return;
            case '\n':
                s->cur_row++;
                if (s->cur_row > s->scroll_bottom) {
                    s->cur_row = s->scroll_bottom;
                    screen_scroll_up(s, 1);
                }
                return;
            case '\t': {
                // Advance to next tab stop (every 8 cols)
                int next = (s->cur_col / 8 + 1) * 8;
                if (next >= s->cols) next = s->cols - 1;
                for (int c = s->cur_col; c < next; c++)
                    screen_set_cell(s, c, s->cur_row, ' ');
                s->cur_col = next;
                return;
            }
            case '\b':
                if (s->cur_col > 0) s->cur_col--;
                return;
            case 0x1B:
                if (p->state == PARSER_CSI) {
                    // Interrupted CSI — start fresh ESC
                    parser_reset(p);
                }
                p->state = PARSER_ESC;
                return;
            case 0x07: // BEL — ignore (or you could flash the screen)
                if (p->state == PARSER_OSC) parser_reset(p);
                return;
            case 0x9B: // 8-bit CSI introducer
                parser_reset(p);
                p->state = PARSER_CSI;
                return;
            default:
                return; // ignore other C0
        }
    }

    switch (p->state) {
        case PARSER_GROUND:
            screen_set_cell(s, s->cur_col, s->cur_row, cp);
            screen_advance_cursor(s);
            break;

        case PARSER_ESC:
            switch (cp) {
                case '[':
                    parser_reset(p);
                    p->state = PARSER_CSI;
                    break;
                case ']':
                    parser_reset(p);
                    p->state = PARSER_OSC;
                    break;
                case '7': // Save cursor
                    s->saved_col = s->cur_col;
                    s->saved_row = s->cur_row;
                    p->state = PARSER_GROUND;
                    break;
                case '8': // Restore cursor
                    s->cur_col = s->saved_col;
                    s->cur_row = s->saved_row;
                    p->state = PARSER_GROUND;
                    break;
                case 'M': // Reverse index (scroll down)
                    if (s->cur_row == s->scroll_top)
                        screen_scroll_down(s, 1);
                    else if (s->cur_row > 0)
                        s->cur_row--;
                    p->state = PARSER_GROUND;
                    break;
                case 'c': // Full reset
                    screen_init(s, s->cols, s->rows);
                    p->state = PARSER_GROUND;
                    break;
                default:
                    p->state = PARSER_GROUND;
                    break;
            }
            break;

        case PARSER_CSI:
            if (cp >= '0' && cp <= '9') {
                p->cur_digit = p->cur_digit * 10 + (int)(cp - '0');
                p->cur_valid = 1;
            } else if (cp == ';') {
                parser_commit_param(p);
            } else if (cp >= 0x20 && cp <= 0x2F) {
                // Intermediate byte (space, '!', '"', '#', '$', '%', '&',
                // '\'', '(', ')', '*', '+', ',', '-', '.', '/')
                if (p->inter_len < (int)(sizeof(p->inter) - 1))
                    p->inter[p->inter_len++] = (char)cp;
            } else if (cp >= 0x40 && cp <= 0x7E) {
                // Final byte — commit last param and dispatch
                parser_commit_param(p);
                dispatch_csi(s, p, (char)cp);
                parser_reset(p);
            } else {
                // Anything else (another ESC, C1, etc.) cancels the sequence
                parser_reset(p);
                // Re-process as ground if it's a printable char
                if (cp >= 0x20) {
                    screen_set_cell(s, s->cur_col, s->cur_row, cp);
                    screen_advance_cursor(s);
                }
            }
            break;

        case PARSER_OSC:
            // Collect until BEL (0x07, handled above) or ST (ESC \).
            // We discard all OSC content (title setting etc.).
            if (cp == '\\') parser_reset(p); // ST
            // BEL case handled in C0 block above
            break;
    }
}

// Feed a raw byte buffer (from the stdout/stderr ring) into the screen.
static void screen_feed(tty_screen_t *s, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++)
        screen_put_char(s, (uint32_t)data[i]);
}

// ================================================================
// Rendering — rasterise dirty cells via NodGL + lib_FNT
// ================================================================

static void render_cell(tty_screen_t *s, int col, int row) {
    tty_cell_t *c = &s->cells[row][col];
    int px = col * g_font_w;
    int py = row * g_font_h;

    // Background rectangle
    NodGL_FillRect(px, py, (uint32_t)g_font_w, (uint32_t)g_font_h, c->bg);

    // Glyph
    if (c->codepoint != 0 && c->codepoint != ' ') {
        fnt_glyph_t *glyph = fnt_get_glyph(g_font, c->codepoint);
        if (glyph) {
            uint32_t fg = c->fg;
            for (int gy = 0; gy < glyph->bitmap_height; gy++) {
                for (int gx = 0; gx < glyph->bitmap_width; gx++) {
                    if (fnt_get_pixel(glyph, gx, gy)) {
                        // Single-pixel plot via FillRect 1x1
                        NodGL_FillRect(px + gx, py + gy, 1, 1, fg);
                    }
                }
            }
        }
    }

    // Underline
    if (c->underline) {
        int uly = py + g_font_h - 2;
        NodGL_FillRect(px, uly, (uint32_t)g_font_w, 1, c->fg);
    }

    c->dirty = 0;
}

static void render_cursor(tty_screen_t *s) {
    if (!s->cursor_visible) return;
    int col = s->cur_col, row = s->cur_row;
    if (col < 0 || col >= s->cols || row < 0 || row >= s->rows) return;

    int px = col * g_font_w;
    int py = row * g_font_h + g_font_h - 2;
    // Simple underline cursor in bright white
    NodGL_FillRect(px, py, (uint32_t)g_font_w, 2, 0xFFFFFFFF);
}

// Repaint only dirty cells.
static void tty_flush(tty_screen_t *s) {
    if (!s->dirty) return;
    for (int row = 0; row < s->rows; row++)
        for (int col = 0; col < s->cols; col++)
            if (s->cells[row][col].dirty)
                render_cell(s, col, row);
    render_cursor(s);
    NodGL_Present();
    s->dirty = 0;
}

// Full repaint (after TTY switch).
static void tty_repaint_all(tty_screen_t *s) {
    for (int row = 0; row < s->rows; row++)
        for (int col = 0; col < s->cols; col++)
            s->cells[row][col].dirty = 1;
    s->dirty = 1;
    tty_flush(s);
}

// ================================================================
// Invoke callbacks
// ================================================================

// stdin: shell reads keyboard bytes
static ssize_t stdin_invoke(void *ctx,
                            const void *in_buf,  size_t in_size,
                            void       *out_buf, size_t out_size)
{
    vtty_t *tty = (vtty_t *)ctx;
    if (!tty || !tty->active) return -1;
    (void)in_buf; (void)in_size;
    size_t n = ring_pop(&tty->stdin_ring, (uint8_t *)out_buf, out_size);
    return (ssize_t)n;
}

// stdout: shell pushes output bytes, we parse them into the screen buffer
static ssize_t stdout_invoke(void *ctx,
                             const void *in_buf,  size_t in_size,
                             void       *out_buf, size_t out_size)
{
    vtty_t *tty = (vtty_t *)ctx;
    if (!tty || !tty->active) return -1;
    (void)out_buf; (void)out_size;
    screen_feed(&tty->screen, (const uint8_t *)in_buf, in_size);
    // If this is the current TTY, flush immediately; otherwise mark dirty
    // and let the TTY switch handle it.
    if (&ttys[current_tty] == tty)
        tty_flush(&tty->screen);
    return (ssize_t)in_size;
}

// stderr: same as stdout (just rendered identically for now)
static ssize_t stderr_invoke(void *ctx,
                             const void *in_buf,  size_t in_size,
                             void       *out_buf, size_t out_size)
{
    return stdout_invoke(ctx, in_buf, in_size, out_buf, out_size);
}

// ----------------------------------------------------------------
// Control node request/response (same pattern as userman)
// ----------------------------------------------------------------

#define TTYMAN_CMD_SWITCH   1
#define TTYMAN_CMD_QUERY    2

typedef struct { int cmd; int a; } ttyman_req_t;
typedef struct { int success; int pid; int active; } ttyman_resp_t;

static ssize_t ctl_invoke(void *ctx,
                          const void *in_buf,  size_t in_size,
                          void       *out_buf, size_t out_size)
{
    (void)ctx;
    if (in_size  != sizeof(ttyman_req_t))  return -1;
    if (out_size != sizeof(ttyman_resp_t)) return -1;

    const ttyman_req_t *req  = (const ttyman_req_t *)in_buf;
    ttyman_resp_t      *resp = (ttyman_resp_t *)out_buf;
    memset(resp, 0, sizeof(*resp));

    switch (req->cmd) {
        case TTYMAN_CMD_SWITCH: {
            int idx = req->a;
            if (idx < 0 || idx >= MAX_TTYS || !ttys[idx].active) {
                resp->success = -1; break;
            }
            current_tty = idx;
            tty_repaint_all(&ttys[idx].screen);
            resp->success = 0;
            break;
        }
        case TTYMAN_CMD_QUERY: {
            int idx = req->a;
            if (idx < 0 || idx >= MAX_TTYS) { resp->success = -1; break; }
            resp->active  = ttys[idx].active;
            resp->pid     = ttys[idx].shell_pid;
            resp->success = 0;
            break;
        }
        default:
            resp->success = -2;
            break;
    }
    return sizeof(ttyman_resp_t);
}

// ================================================================
// Node registration helper
// ================================================================

static int register_invoke_node(const char *path,
                                ssize_t (*fn)(void *, const void *, size_t,
                                              void *, size_t),
                                void *ctx)
{
    userfs_user_node_t node;
    memset(&node, 0, sizeof(node));
    node.path       = path;
    node.owner_id   = "ttyman";
    node.perms      = USERFS_PERM_READ_WRITE | USERFS_PERM_INVOKE;
    node.ops.invoke = fn;
    node.ctx        = ctx;
    return userfs_register(&node);
}

// ================================================================
// Shell spawning
// ================================================================

static int spawn_shell_on_tty(int idx) {
    if (idx < 0 || idx >= MAX_TTYS || !ttys[idx].active) return -1;
    vtty_t *tty = &ttys[idx];

    int pid = fork();
    if (pid < 0) return -2;

    if (pid == 0) {
        char sin[128], sout[128], serr[128];
        sprintf(sin,  "$/user/ttyman/%s/stdin",  tty->name);
        sprintf(sout, "$/user/ttyman/%s/stdout", tty->name);
        sprintf(serr, "$/user/ttyman/%s/stderr", tty->name);

        int in_fd  = open(sin,  O_RDWR, 0);
        int out_fd = open(sout, O_RDWR, 0);
        int err_fd = open(serr, O_RDWR, 0);

        if (in_fd < 0 || out_fd < 0 || err_fd < 0) { exit(1); }

        if (in_fd  != 0) { dup2(in_fd,  0); if (in_fd  > 2) close(in_fd);  }
        if (out_fd != 1) { dup2(out_fd, 1); if (out_fd > 2) close(out_fd); }
        if (err_fd != 2) { dup2(err_fd, 2); if (err_fd > 2) close(err_fd); }

        char *argv[] = { "/Apps/zenith5.1.sqr", NULL };
        char *envp[] = {
            "PATH=/ModuOS/System64:/Apps/",
            "HOME=/",
            "TERM=ntosiux-256color",
            NULL
        };
        execve("/Apps/zenith5.1.sqr", argv, envp);
        exit(1);
    }

    tty->shell_pid = pid;
    return pid;
}

// ================================================================
// Keyboard input -> current TTY stdin ring
// ================================================================

// Translate a KeyCode + modifiers into the VT byte sequence the shell
// expects, writing into out[].  Returns number of bytes written (0 if
// the key produces no output, e.g. bare modifier keys).
static int keyevent_to_vt(const KeyboardEventData *k, uint8_t *out) {
    // Printable ASCII — just forward it, respecting Ctrl combos.
    if (k->ascii != 0) {
        if (k->modifiers & MOD_CTRL) {
            // Ctrl+A..Z -> 0x01..0x1A
            char c = k->ascii;
            if (c >= 'a' && c <= 'z') { out[0] = (uint8_t)(c - 'a' + 1); return 1; }
            if (c >= 'A' && c <= 'Z') { out[0] = (uint8_t)(c - 'A' + 1); return 1; }
            if (c == '[')  { out[0] = 0x1B; return 1; } // Ctrl+[ = ESC
            if (c == '\\') { out[0] = 0x1C; return 1; }
            if (c == ']')  { out[0] = 0x1D; return 1; }
        }
        out[0] = (uint8_t)k->ascii;
        return 1;
    }

    // Special keys — emit standard VT/xterm escape sequences.
    // ESC [ ... notation (CSI sequences).
    switch (k->keycode) {
        case KEY_ESCAPE:      out[0] = 0x1B; return 1;
        case KEY_BACKSPACE:   out[0] = 0x7F; return 1;  // DEL (xterm default)
        case KEY_TAB:
            if (k->modifiers & MOD_SHIFT) {
                // Shift+Tab = CSI Z (backtab)
                out[0]=0x1B; out[1]='['; out[2]='Z'; return 3;
            }
            out[0] = '\t'; return 1;
        case KEY_ENTER:       out[0] = '\r'; return 1;

        // Arrow keys
        case KEY_ARROW_UP:    out[0]=0x1B; out[1]='['; out[2]='A'; return 3;
        case KEY_ARROW_DOWN:  out[0]=0x1B; out[1]='['; out[2]='B'; return 3;
        case KEY_ARROW_RIGHT: out[0]=0x1B; out[1]='['; out[2]='C'; return 3;
        case KEY_ARROW_LEFT:  out[0]=0x1B; out[1]='['; out[2]='D'; return 3;

        // Navigation
        case KEY_HOME:        out[0]=0x1B; out[1]='['; out[2]='H'; return 3;
        case KEY_END:         out[0]=0x1B; out[1]='['; out[2]='F'; return 3;
        case KEY_INSERT:      out[0]=0x1B; out[1]='['; out[2]='2'; out[3]='~'; return 4;
        case KEY_DELETE:      out[0]=0x1B; out[1]='['; out[2]='3'; out[3]='~'; return 4;
        case KEY_PAGE_UP:     out[0]=0x1B; out[1]='['; out[2]='5'; out[3]='~'; return 4;
        case KEY_PAGE_DOWN:   out[0]=0x1B; out[1]='['; out[2]='6'; out[3]='~'; return 4;

        // Function keys F1-F12 (xterm sequences)
        case KEY_F1:  out[0]=0x1B; out[1]='O'; out[2]='P'; return 3;
        case KEY_F2:  out[0]=0x1B; out[1]='O'; out[2]='Q'; return 3;
        case KEY_F3:  out[0]=0x1B; out[1]='O'; out[2]='R'; return 3;
        case KEY_F4:  out[0]=0x1B; out[1]='O'; out[2]='S'; return 3;
        case KEY_F5:  out[0]=0x1B; out[1]='['; out[2]='1'; out[3]='5'; out[4]='~'; return 5;
        case KEY_F6:  out[0]=0x1B; out[1]='['; out[2]='1'; out[3]='7'; out[4]='~'; return 5;
        case KEY_F7:  out[0]=0x1B; out[1]='['; out[2]='1'; out[3]='8'; out[4]='~'; return 5;
        case KEY_F8:  out[0]=0x1B; out[1]='['; out[2]='1'; out[3]='9'; out[4]='~'; return 5;
        case KEY_F9:  out[0]=0x1B; out[1]='['; out[2]='2'; out[3]='0'; out[4]='~'; return 5;
        case KEY_F10: out[0]=0x1B; out[1]='['; out[2]='2'; out[3]='1'; out[4]='~'; return 5;
        case KEY_F11: out[0]=0x1B; out[1]='['; out[2]='2'; out[3]='3'; out[4]='~'; return 5;
        case KEY_F12: out[0]=0x1B; out[1]='['; out[2]='2'; out[3]='4'; out[4]='~'; return 5;

        // Bare modifier keys produce nothing
        case KEY_LEFT_SHIFT:  case KEY_RIGHT_SHIFT:
        case KEY_LEFT_CTRL:   case KEY_RIGHT_CTRL:
        case KEY_LEFT_ALT:    case KEY_RIGHT_ALT:
        case KEY_CAPS_LOCK:   case KEY_NUM_LOCK:
        case KEY_SCROLL_LOCK:
            return 0;

        default: return 0;
    }
}

static void handle_keyboard_input(void) {
    if (g_input_fd < 0) return; // input device never opened successfully

    Event ev;
    // Drain all pending events this tick. The fd is opened O_NONBLOCK, so
    // read() returns <= 0 once the queue is empty (0 == no data, -1 == would
    // block / error) — same pattern as teseraris.c's input loop.
    while (read(g_input_fd, &ev, sizeof(ev)) > 0) {
        // We only care about key-press events; releases are ignored.
        if (ev.type != EVENT_KEY_PRESSED)
            continue;

        const KeyboardEventData *k = &ev.data.keyboard;

        // Alt+F1-F6: TTY switch.  Alt modifier + F-key, no other modifiers.
        if ((k->modifiers & MOD_ALT) && !(k->modifiers & MOD_CTRL)) {
            int new_tty = -1;
            switch (k->keycode) {
                case KEY_F1: new_tty = 0; break;
                case KEY_F2: new_tty = 1; break;
                case KEY_F3: new_tty = 2; break;
                case KEY_F4: new_tty = 3; break;
                case KEY_F5: new_tty = 4; break;
                case KEY_F6: new_tty = 5; break;
                default: break;
            }
            if (new_tty >= 0 && new_tty < MAX_TTYS && ttys[new_tty].active) {
                current_tty = new_tty;
                tty_repaint_all(&ttys[current_tty].screen);
                continue; // hotkey consumed
            }
        }

        // Translate to VT bytes and push into the current TTY's stdin ring.
        uint8_t vt[8];
        int n = keyevent_to_vt(k, vt);
        if (n > 0)
            ring_push(&ttys[current_tty].stdin_ring, vt, (size_t)n);
    }
}

// ================================================================
// NodGL + font initialisation
// ================================================================

// Load font from disk into a malloc'd buffer, pass to fnt_load_font.
// Returns NULL on failure.
static fnt_font_t *load_font_from_file(const char *path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return NULL;

    // Determine file size via lseek
    long size = lseek(fd, 0, 2); // SEEK_END = 2
    if (size <= 0) { close(fd); return NULL; }
    lseek(fd, 0, 0); // SEEK_SET = 0

    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (!data) { close(fd); return NULL; }

    ssize_t got = read(fd, data, (size_t)size);
    close(fd);
    if (got != size) { free(data); return NULL; }

    fnt_font_t *font = fnt_load_font(data, (size_t)size);
    free(data); // fnt_load_font copies what it needs
    return font;
}

static int init_graphics(void) {
    NodGL_FeatureLevel actual;
    if (NodGL_CreateDevice(NodGL_FEATURE_LEVEL_1_0, &g_device, &g_ctx, &actual) != NodGL_OK)
        return -1;

    NodGL_GetScreenResolution(g_device, &g_screen_w, &g_screen_h);

    // Try to load the system font; fall back to a built-in 8x16 stub if missing.
    g_font = load_font_from_file("/ModuOS/shared/assets/fonts/Terminus.fnt");
    if (!g_font) {
        // If no font file exists the render functions will produce blank glyphs —
        // the terminal is still functional, just invisible text.
        // A real implementation would embed a fallback bitmap here.
        g_font_w = 8;
        g_font_h = 16;
    } else {
        g_font_w = (int)g_font->header.glyph_width  * FONT_SCALE;
        g_font_h = (int)g_font->header.glyph_height * FONT_SCALE;
        if (g_font_w < 1) g_font_w = 1;
        if (g_font_h < 1) g_font_h = 1;
    }

    return 0;
}

// Open the input device and stash the fd in g_input_fd. O_NONBLOCK so
// handle_keyboard_input() can poll it every tick without stalling the
// render loop. Returns 0 on success, -1 on failure.
static int init_input(void) {
    g_input_fd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    return (g_input_fd < 0) ? -1 : 0;
}

// ================================================================
// Entry point
// ================================================================

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    // Note: we never write to fd 1 or 2 here — those go to kprint.
    // All diagnostic output is suppressed in a shipping build.
    // During development you can re-enable the printf calls below.

    if (init_graphics() < 0) return 1;
    if (init_input() < 0) return 1;

    int cols = (int)(g_screen_w / (uint32_t)g_font_w);
    int rows = (int)(g_screen_h / (uint32_t)g_font_h);
    if (cols > TTY_MAX_COLS) cols = TTY_MAX_COLS;
    if (rows > TTY_MAX_ROWS) rows = TTY_MAX_ROWS;
    if (cols < 1) cols = 80;
    if (rows < 1) rows = 24;

    memset(ttys, 0, sizeof(ttys));
    for (int i = 0; i < MAX_TTYS; i++)
        ttys[i].shell_pid = -1;

    // Register ctl node
    if (register_invoke_node("ttyman/ctl", ctl_invoke, NULL) < 0) return 1;

    // Create TTY slots + register their three invoke nodes each
    for (int i = 0; i < MAX_TTYS; i++) {
        vtty_t *tty = &ttys[i];
        tty->name[0] = 't'; tty->name[1] = 't';
        tty->name[2] = 'y'; tty->name[3] = '1' + (char)i;
        tty->name[4] = '\0';
        tty->active    = 1;
        tty->shell_pid = -1;

        screen_init(&tty->screen, cols, rows);

        char sin[64], sout[64], serr[64];
        sprintf(sin,  "ttyman/%s/stdin",  tty->name);
        sprintf(sout, "ttyman/%s/stdout", tty->name);
        sprintf(serr, "ttyman/%s/stderr", tty->name);

        if (register_invoke_node(sin,  stdin_invoke,  tty) < 0 ||
            register_invoke_node(sout, stdout_invoke, tty) < 0 ||
            register_invoke_node(serr, stderr_invoke, tty) < 0)
            return 1;
    }

    // Spawn shell on tty1, paint it
    current_tty = 0;
    if (spawn_shell_on_tty(0) < 0) return 1;

    NodGL_Clear(g_ansi16[0]);
    tty_repaint_all(&ttys[0].screen);

    // Main loop
    for (;;) {
        handle_keyboard_input();

        // Respawn dead shells
        for (int i = 0; i < MAX_TTYS; i++) {
            if (!ttys[i].active || ttys[i].shell_pid <= 0) continue;
            int status = 0;
            if (waitpid(ttys[i].shell_pid, &status, WNOHANG) > 0) {
                ttys[i].shell_pid = -1;
                spawn_shell_on_tty(i);
            }
        }

        yield();
    }

    return 0;
}