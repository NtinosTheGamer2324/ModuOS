#include "libc.h"
#include "NodGL.h"
#include "../include/moduos/kernel/events/events.h"

/* ── Grid ─────────────────────────────────────────────────────────── */
#define GW 32
#define GH 24
#define MAX_LEN (GW * GH)

/* ── Colors (XRGB8888) ────────────────────────────────────────────── */
#define C_BG      0x000000u
#define C_BORDER  0x222222u
#define C_GRID    0x0A0A0Au
#define C_HEAD    0x00FF44u
#define C_BODY    0x00AA33u
#define C_FOOD    0xFF3030u
#define C_TEXT    0xFFFFFFu
#define C_YELLOW  0xFFFF00u
#define C_OVERLAY 0x111111u

/* ── Types ────────────────────────────────────────────────────────── */
typedef struct { int x, y; } Pt;
typedef enum   { R=0, D=1, L=2, U=3 } Dir;
static int g_needs_redraw = 0;

/* ── Static state (keep off stack) ───────────────────────────────── */
static Pt   g_body[MAX_LEN];
static int  g_len;
static Dir  g_dir, g_next_dir;
static Pt   g_food;
static int  g_score, g_hiscore;
static int  g_over, g_paused;
static uint32_t g_seed;

/* ── NodGL handles ────────────────────────────────────────────────── */
static NodGL_Device  g_dev;
static NodGL_Context g_ctx;

/* ── Screen / cell geometry ───────────────────────────────────────── */
static uint32_t g_sw, g_sh;
static uint32_t g_cell;
static uint32_t g_bx, g_by;   /* board origin in pixels */
static uint32_t g_bw, g_bh;   /* board size in pixels   */

/* ═══════════════════════════════════════════════════════════════════
   Tiny 5x7 font (digits + A-Z + some punctuation)
   Each glyph is 5 bytes, one per row, bits 4..0 = columns left→right
   ═══════════════════════════════════════════════════════════════════ */
static const uint8_t g_font5x7[][7] = {
    /* 0 */ {0xE,0x11,0x13,0x15,0x19,0x11,0xE},
    /* 1 */ {0x4,0xC,0x4,0x4,0x4,0x4,0xE},
    /* 2 */ {0xE,0x11,0x1,0x2,0x4,0x8,0x1F},
    /* 3 */ {0x1F,0x2,0x4,0x2,0x1,0x11,0xE},
    /* 4 */ {0x2,0x6,0xA,0x12,0x1F,0x2,0x2},
    /* 5 */ {0x1F,0x10,0x1E,0x1,0x1,0x11,0xE},
    /* 6 */ {0x6,0x8,0x10,0x1E,0x11,0x11,0xE},
    /* 7 */ {0x1F,0x1,0x2,0x4,0x8,0x8,0x8},
    /* 8 */ {0xE,0x11,0x11,0xE,0x11,0x11,0xE},
    /* 9 */ {0xE,0x11,0x11,0xF,0x1,0x2,0xC},
    /* A */ {0xE,0x11,0x11,0x1F,0x11,0x11,0x11},
    /* B */ {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    /* C */ {0xE,0x11,0x10,0x10,0x10,0x11,0xE},
    /* D */ {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    /* E */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    /* F */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    /* G */ {0xE,0x11,0x10,0x17,0x11,0x11,0xF},
    /* H */ {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    /* I */ {0xE,0x4,0x4,0x4,0x4,0x4,0xE},
    /* J */ {0x7,0x2,0x2,0x2,0x2,0x12,0xC},
    /* K */ {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    /* L */ {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    /* M */ {0x11,0x1B,0x15,0x11,0x11,0x11,0x11},
    /* N */ {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    /* O */ {0xE,0x11,0x11,0x11,0x11,0x11,0xE},
    /* P */ {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    /* Q */ {0xE,0x11,0x11,0x11,0x15,0x12,0xD},
    /* R */ {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    /* S */ {0xF,0x10,0x10,0xE,0x1,0x1,0x1E},
    /* T */ {0x1F,0x4,0x4,0x4,0x4,0x4,0x4},
    /* U */ {0x11,0x11,0x11,0x11,0x11,0x11,0xE},
    /* V */ {0x11,0x11,0x11,0x11,0x11,0xA,0x4},
    /* W */ {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},
    /* X */ {0x11,0xA,0xA,0x4,0xA,0xA,0x11},
    /* Y */ {0x11,0x11,0xA,0x4,0x4,0x4,0x4},
    /* Z */ {0x1F,0x1,0x2,0x4,0x8,0x10,0x1F},
    /* : */ {0x0,0x4,0x0,0x0,0x0,0x4,0x0},
    /* ! */ {0x4,0x4,0x4,0x4,0x4,0x0,0x4},
    /* - */ {0x0,0x0,0x0,0x1F,0x0,0x0,0x0},
    /* . */ {0x0,0x0,0x0,0x0,0x0,0x0,0x4},
    /* / */ {0x1,0x1,0x2,0x4,0x8,0x10,0x10},
};

static int font_idx(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
    if (c >= 'a' && c <= 'z') return 10 + (c - 'a');
    if (c == ':') return 36;
    if (c == '!') return 37;
    if (c == '-') return 38;
    if (c == '.') return 39;
    if (c == '/') return 40;
    return -1;
}

/* Draw a single glyph at pixel (px,py), scale s, color col */
static void draw_glyph(int px, int py, char c, int s, uint32_t col) {
    int idx = font_idx(c);
    if (idx < 0) return;
    const uint8_t *rows = g_font5x7[idx];
    for (int row = 0; row < 7; row++) {
        for (int bit = 0; bit < 5; bit++) {
            if (rows[row] & (1 << (4 - bit))) {
                NodGL_FillRectContext(g_ctx,
                    px + bit * s, py + row * s, (uint32_t)s, (uint32_t)s, col);
            }
        }
    }
}

/* Draw a string; returns x after last glyph */
static int draw_str(int px, int py, const char *s, int scale, uint32_t col) {
    int x = px;
    for (; *s; s++) {
        if (*s == ' ') { x += (5 + 1) * scale; continue; }
        draw_glyph(x, py, *s, scale, col);
        x += (5 + 1) * scale;
    }
    return x;
}

static int draw_int(int px, int py, int v, int scale, uint32_t col) {
    char buf[16];
    itoa(v, buf, 10);
    return draw_str(px, py, buf, scale, col);
}

/* ── RNG ──────────────────────────────────────────────────────────── */
static uint32_t rnd(void) {
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed;
}

/* ── Helpers ──────────────────────────────────────────────────────── */
static int hits_body(Pt p, int skip_tail) {
    int end = skip_tail ? g_len - 1 : g_len;
    for (int i = 0; i < end; i++)
        if (g_body[i].x == p.x && g_body[i].y == p.y) return 1;
    return 0;
}

static void place_food(void) {
    Pt f;
    do { f.x = (int)(rnd() % GW); f.y = (int)(rnd() % GH); }
    while (hits_body(f, 0));
    g_food = f;
}

/* ── Game logic ───────────────────────────────────────────────────── */
static void game_reset(void) {
    g_len  = 4;
    g_dir  = R;
    g_next_dir = R;
    g_over = 0;
    g_paused = 0;
    g_score = 0;
    int sx = GW / 2, sy = GH / 2;
    for (int i = 0; i < g_len; i++) {
        g_body[i].x = sx - i;
        g_body[i].y = sy;
    }
    place_food();
}

/* Returns 1 if snake moved, 0 if game over */
static int game_tick(void) {
    if (g_over || g_paused) return 0;
    g_dir = g_next_dir;

    Pt nh = g_body[0];
    switch (g_dir) {
        case R: nh.x++; break;
        case L: nh.x--; break;
        case D: nh.y++; break;
        case U: nh.y--; break;
    }

    /* wall collision */
    if (nh.x < 0 || nh.x >= GW || nh.y < 0 || nh.y >= GH) { g_over = 1; return 0; }
    /* self collision (allow tail slot — it will move away) */
    if (hits_body(nh, 1)) { g_over = 1; return 0; }

    int ate = (nh.x == g_food.x && nh.y == g_food.y);

    /* shift body */
    if (!ate) g_len = g_len; /* no grow */
    else {
        g_score += 10;
        if (g_score > g_hiscore) g_hiscore = g_score;
        if (g_len < MAX_LEN) g_len++;
        place_food();
    }

    for (int i = g_len - 1; i > 0; i--) g_body[i] = g_body[i-1];
    g_body[0] = nh;
    return 1;
}

/* ── Drawing ──────────────────────────────────────────────────────── */
static void fill(int x, int y, uint32_t w, uint32_t h, uint32_t c) {
    NodGL_FillRectContext(g_ctx, x, y, w, h, c);
}

static void draw_board_bg(void) {
    /* outer border */
    fill(0, 0, g_sw, g_sh, C_BG);
    fill((int)g_bx - 2, (int)g_by - 2, g_bw + 4, g_bh + 4, C_BORDER);
    /* grid */
    for (int gy = 0; gy < GH; gy++) {
        for (int gx = 0; gx < GW; gx++) {
            uint32_t px = g_bx + (uint32_t)gx * g_cell;
            uint32_t py = g_by + (uint32_t)gy * g_cell;
            fill((int)px, (int)py, g_cell - 1, g_cell - 1, C_GRID);
        }
    }
}

static void draw_cell(int gx, int gy, uint32_t c) {
    int px = (int)g_bx + gx * (int)g_cell;
    int py = (int)g_by + gy * (int)g_cell;
    uint32_t inner = g_cell > 2 ? g_cell - 2 : g_cell;
    fill(px + 1, py + 1, inner, inner, c);
}

static void draw_snake_and_food(void) {
    draw_cell(g_food.x, g_food.y, C_FOOD);
    for (int i = g_len - 1; i >= 0; i--)
        draw_cell(g_body[i].x, g_body[i].y, i == 0 ? C_HEAD : C_BODY);
}

static void draw_hud(void) {
    /* HUD bar above board */
    int hy = (int)g_by - 24;
    if (hy < 0) hy = 0;
    fill(0, hy, g_sw, 20, C_BG);

    draw_str(4, hy + 2, "SCORE:", 2, C_TEXT);
    int ax = draw_int(76, hy + 2, g_score, 2, C_YELLOW);

    draw_str(ax + 16, hy + 2, "BEST:", 2, C_TEXT);
    draw_int(ax + 16 + 60, hy + 2, g_hiscore, 2, C_YELLOW);

    if (g_paused)
        draw_str((int)g_sw / 2 - 40, hy + 2, "PAUSED", 2, 0x00CCFF);
}

static void draw_overlay(const char *line1, const char *line2) {
    /* semi-transparent-ish dark box in centre */
    uint32_t ow = 260, oh = 80;
    int ox = (int)(g_sw - ow) / 2;
    int oy = (int)(g_sh - oh) / 2;
    fill(ox, oy, ow, oh, C_OVERLAY);
    /* two lines of text */
    int tw1 = (int)strlen(line1) * 6 * 2;
    int tw2 = (int)strlen(line2) * 6 * 2;
    draw_str(ox + ((int)ow - tw1) / 2, oy + 12, line1, 2, C_TEXT);
    draw_str(ox + ((int)ow - tw2) / 2, oy + 44, line2, 2, C_YELLOW);
}

static void full_redraw(void) {
    draw_board_bg();
    draw_snake_and_food();
    draw_hud();
    NodGL_PresentContext(g_ctx, 0);
}

/* ── Input ────────────────────────────────────────────────────────── */
static void handle_key(const Event *e, int *quit) {
    if (e->type != EVENT_KEY_PRESSED) return;
    KeyCode kc = e->data.keyboard.keycode;
    char    c  = e->data.keyboard.ascii;

    if (kc == KEY_ESCAPE || c == 'q' || c == 'Q') { *quit = 1; return; }

    if (g_over) {
        if (kc == KEY_ENTER || c == '\n' || c == '\r') {
            game_reset();
            full_redraw();   /* clears overlay + old snake in one shot */
            g_needs_redraw = 0;
        }
        return;
    }

    if (c == 'p' || c == 'P') {
        g_paused ^= 1;
        g_needs_redraw = 1;
        return;
    }

    if ((kc == KEY_ARROW_UP    || c=='w'||c=='W') && g_dir != D) g_next_dir = U;
    if ((kc == KEY_ARROW_DOWN  || c=='s'||c=='S') && g_dir != U) g_next_dir = D;
    if ((kc == KEY_ARROW_LEFT  || c=='a'||c=='A') && g_dir != R) g_next_dir = L;
    if ((kc == KEY_ARROW_RIGHT || c=='d'||c=='D') && g_dir != L) g_next_dir = R;
}

static void sleep_ms(uint32_t ms) {
    uint64_t end = time_ms() + ms;
    while (time_ms() < end) yield();
}

/* ── Entry point ──────────────────────────────────────────────────── */
int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    if (NodGL_CreateDevice(NodGL_FEATURE_LEVEL_1_0, &g_dev, &g_ctx, NULL) != NodGL_OK) {
        puts_raw("snakegfx: NodGL init failed\n");
        sleep(2);
        return 1;
    }

    NodGL_GetScreenResolution(g_dev, &g_sw, &g_sh);

    uint32_t avail_h = g_sh > 32 ? g_sh - 32 : g_sh;
    uint32_t cw = g_sw / GW;
    uint32_t ch = avail_h / GH;
    g_cell = cw < ch ? cw : ch;
    if (g_cell < 4)  g_cell = 4;
    if (g_cell > 28) g_cell = 28;

    g_bw = (uint32_t)GW * g_cell;
    g_bh = (uint32_t)GH * g_cell;
    g_bx = (g_sw - g_bw) / 2;
    g_by = (g_sh - g_bh) / 2 + 12;

    int efd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    if (efd < 0) {
        puts_raw("snakegfx: cannot open event0\n");
        free(g_ctx);
        NodGL_ReleaseDevice(g_dev);
        sleep(2);
        return 2;
    }

    g_seed    = (uint32_t)(time_ms() & 0x7FFFFFFFu);
    g_hiscore = 0;
    game_reset();
    full_redraw();

    uint64_t last_tick = time_ms();
    uint64_t last_hud  = time_ms();
    int quit = 0;

    while (!quit) {
        uint64_t frame_start = time_ms();
        int dirty = 0;
    
        Event ev;
        while (read(efd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev))
            handle_key(&ev, &quit);
    
        if (g_needs_redraw) {
            draw_hud();
            NodGL_PresentContext(g_ctx, 0);
            g_needs_redraw = 0;
            dirty = 0;
        }
    
        uint64_t now = time_ms();
    
        uint32_t interval = 200;
        if (g_score > 50)  interval = 170;
        if (g_score > 150) interval = 140;
        if (g_score > 300) interval = 110;
        if (g_score > 500) interval = 85;
    
        if (!g_over && !g_paused && now - last_tick >= interval) {
            Pt prev_tail = g_body[g_len - 1];
            int prev_len = g_len;
            int moved = game_tick();
        
            if (moved) {
                if (g_len == prev_len)
                    draw_cell(prev_tail.x, prev_tail.y, C_GRID);
                draw_cell(g_body[1].x, g_body[1].y, C_BODY);
                draw_cell(g_body[0].x, g_body[0].y, C_HEAD);
                draw_cell(g_food.x, g_food.y, C_FOOD);
                dirty = 1;
            }
        
            if (g_over) {
                full_redraw();
                draw_overlay("GAME OVER", "ENTER TO RESTART");
                dirty = 1;
            }
        
            last_tick = now;
        }
    
        if (now - last_hud >= 250 && !g_over) {
            draw_hud();
            dirty = 1;
            last_hud = now;
        }
    
        if (dirty)
            NodGL_PresentContext(g_ctx, 0);
    
        uint64_t elapsed = time_ms() - frame_start;
        if (elapsed < 16) sleep_ms((uint32_t)(16 - elapsed));
    }

    close(efd);
    free(g_ctx);
    NodGL_ReleaseDevice(g_dev);
    return 0;
}