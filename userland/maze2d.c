/*
 * maze2d.c — Top-down 2D maze game for ModuOS
 *
 * Find the exit. Each level the maze gets bigger.
 *
 * Controls:
 *   WASD / Arrow keys  — Move
 *   Q / ESC            — Quit
 *   M                  — Toggle full map reveal (debug)
 *
 * Build like snakegfx.c; entry point is md_main().
 */

#include "libc.h"
#include "NodGL.h"
#include "../include/moduos/kernel/events/events.h"

/* ═══════════════════════════════════════════════════════════════════
   Map config
   ═══════════════════════════════════════════════════════════════════ */

#define MAP_W       31      /* must be odd */
#define MAP_H       31      /* must be odd */
#define MAX_MAP     (MAP_W * MAP_H)

#define T_WALL      0
#define T_EMPTY     1
#define T_EXIT      2

/* ═══════════════════════════════════════════════════════════════════
   Colours
   ═══════════════════════════════════════════════════════════════════ */

#define C_BG        0x0A0A0Eu
#define C_WALL      0x2A2A3Au
#define C_WALL_HI   0x3A3A5Au   /* lighter edge on wall top */
#define C_FLOOR     0x13131Au
#define C_PLAYER    0x44FFAAu
#define C_PLAYER_S  0x226644u   /* player shadow */
#define C_EXIT      0x00FFCCu
#define C_EXIT_DIM  0x005544u
#define C_HUD       0xCCCCCCu
#define C_YELLOW    0xFFFF00u
#define C_OVERLAY_BG 0x080810u

/* ═══════════════════════════════════════════════════════════════════
   Types
   ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  tile[MAX_MAP];

    /* Player position in world-space pixels */
    float    px, py;

    /* Velocity (for smooth deceleration) */
    float    vx, vy;

    int      level;
    uint32_t steps;
    int      won;
    uint32_t seed;
    int      show_map;  /* debug: reveal whole maze */
} Game;

/* ═══════════════════════════════════════════════════════════════════
   Globals
   ═══════════════════════════════════════════════════════════════════ */

static Game          G;
static NodGL_Device  g_dev;
static NodGL_Context g_ctx;
static uint32_t      g_sw, g_sh;

/* Tile size in pixels (computed from screen size) */
static int g_tile;

/* Camera offset (top-left of viewport in world pixels) */
static float g_cam_x, g_cam_y;

/* Input state */
static int g_key_up, g_key_down, g_key_left, g_key_right;

/* ═══════════════════════════════════════════════════════════════════
   Helpers
   ═══════════════════════════════════════════════════════════════════ */

static inline float fabs_f(float x) { return x < 0.f ? -x : x; }
static inline int   abs_i(int x)    { return x < 0   ? -x :  x; }

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static uint32_t rng_next(void) {
    G.seed = G.seed * 1664525u + 1013904223u;
    return G.seed;
}
static int rng_range(int lo, int hi) {
    return lo + (int)(rng_next() % (uint32_t)(hi - lo));
}

static inline uint8_t map_get(int x, int y) {
    if (x < 0 || x >= MAP_W || y < 0 || y >= MAP_H) return T_WALL;
    return G.tile[y * MAP_W + x];
}
static inline void map_set(int x, int y, uint8_t v) {
    if (x < 0 || x >= MAP_W || y < 0 || y >= MAP_H) return;
    G.tile[y * MAP_W + x] = v;
}

/* ═══════════════════════════════════════════════════════════════════
   Maze generation (iterative DFS carver)
   ═══════════════════════════════════════════════════════════════════ */

static uint8_t carve_visited[MAX_MAP];

static void carve(int sx, int sy) {
    static int stk_x[MAX_MAP / 4 + 4];
    static int stk_y[MAX_MAP / 4 + 4];
    int sp = 0;

    carve_visited[sy * MAP_W + sx] = 1;
    stk_x[sp] = sx; stk_y[sp] = sy; sp++;

    /* Directions in cell-space (step 2 to skip wall cells) */
    static const int DX[4] = { 2, 0,-2, 0};
    static const int DY[4] = { 0, 2, 0,-2};

    while (sp > 0) {
        int x = stk_x[sp-1], y = stk_y[sp-1];

        /* Shuffle directions */
        int ord[4] = {0,1,2,3};
        for (int i = 3; i > 0; i--) {
            int j = (int)(rng_next() % (uint32_t)(i + 1));
            int t = ord[i]; ord[i] = ord[j]; ord[j] = t;
        }

        int moved = 0;
        for (int i = 0; i < 4; i++) {
            int d  = ord[i];
            int nx = x + DX[d];
            int ny = y + DY[d];
            if (nx <= 0 || nx >= MAP_W-1 || ny <= 0 || ny >= MAP_H-1) continue;
            if (carve_visited[ny * MAP_W + nx]) continue;

            /* Knock out the wall between */
            map_set(x + DX[d]/2, y + DY[d]/2, T_EMPTY);
            map_set(nx, ny, T_EMPTY);
            carve_visited[ny * MAP_W + nx] = 1;
            stk_x[sp] = nx; stk_y[sp] = ny; sp++;
            moved = 1;
            break;
        }
        if (!moved) sp--;
    }
}

static void level_generate(void) {
    /* Fill with walls */
    for (int i = 0; i < MAX_MAP; i++) G.tile[i] = T_WALL;
    memset(carve_visited, 0, sizeof(carve_visited));

    /* Carve from (1,1) */
    map_set(1, 1, T_EMPTY);
    carve(1, 1);

    /* Place exit — find a far empty cell from bottom-right */
    int best_x = MAP_W - 2, best_y = MAP_H - 2;
    while (map_get(best_x, best_y) != T_EMPTY && best_x > 1) best_x -= 2;
    while (map_get(best_x, best_y) != T_EMPTY && best_y > 1) best_y -= 2;
    map_set(best_x, best_y, T_EXIT);

    /* Player starts at cell (1,1), centred in that tile */
    G.px = (1.5f) * (float)g_tile;
    G.py = (1.5f) * (float)g_tile;
    G.vx = 0.f;
    G.vy = 0.f;
    G.steps = 0;
    G.won   = 0;
}

/* ═══════════════════════════════════════════════════════════════════
   Tiny 5×7 bitmap font (digits + A-Z + punctuation)
   ═══════════════════════════════════════════════════════════════════ */

static const uint8_t g_font5x7[][7] = {
    {0xE,0x11,0x13,0x15,0x19,0x11,0xE},  /* 0 */
    {0x4,0xC,0x4,0x4,0x4,0x4,0xE},       /* 1 */
    {0xE,0x11,0x1,0x2,0x4,0x8,0x1F},     /* 2 */
    {0x1F,0x2,0x4,0x2,0x1,0x11,0xE},     /* 3 */
    {0x2,0x6,0xA,0x12,0x1F,0x2,0x2},     /* 4 */
    {0x1F,0x10,0x1E,0x1,0x1,0x11,0xE},   /* 5 */
    {0x6,0x8,0x10,0x1E,0x11,0x11,0xE},   /* 6 */
    {0x1F,0x1,0x2,0x4,0x8,0x8,0x8},      /* 7 */
    {0xE,0x11,0x11,0xE,0x11,0x11,0xE},   /* 8 */
    {0xE,0x11,0x11,0xF,0x1,0x2,0xC},     /* 9 */
    {0xE,0x11,0x11,0x1F,0x11,0x11,0x11}, /* A */
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},{0xE,0x11,0x10,0x10,0x10,0x11,0xE},
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},{0xE,0x11,0x10,0x17,0x11,0x11,0xF},
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},{0xE,0x4,0x4,0x4,0x4,0x4,0xE},
    {0x7,0x2,0x2,0x2,0x2,0x12,0xC},      {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},{0x11,0x1B,0x15,0x11,0x11,0x11,0x11},
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11},{0xE,0x11,0x11,0x11,0x11,0x11,0xE},
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},{0xE,0x11,0x11,0x11,0x15,0x12,0xD},
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},{0xF,0x10,0x10,0xE,0x1,0x1,0x1E},
    {0x1F,0x4,0x4,0x4,0x4,0x4,0x4},      {0x11,0x11,0x11,0x11,0x11,0x11,0xE},
    {0x11,0x11,0x11,0x11,0x11,0xA,0x4},  {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},
    {0x11,0xA,0xA,0x4,0xA,0xA,0x11},     {0x11,0x11,0xA,0x4,0x4,0x4,0x4},
    {0x1F,0x1,0x2,0x4,0x8,0x10,0x1F},    /* Z */
    {0x0,0x4,0x0,0x0,0x0,0x4,0x0},       /* : */
    {0x4,0x4,0x4,0x4,0x4,0x0,0x4},       /* ! */
    {0x0,0x0,0x0,0x1F,0x0,0x0,0x0},      /* - */
    {0x0,0x0,0x0,0x0,0x0,0x0,0x4},       /* . */
    {0x1,0x1,0x2,0x4,0x8,0x10,0x10},     /* / */
};

static int font_idx(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
    if (c >= 'a' && c <= 'z') return 10 + (c - 'a');
    if (c == ':') return 36; if (c == '!') return 37;
    if (c == '-') return 38; if (c == '.') return 39;
    if (c == '/') return 40;
    return -1;
}

static void draw_glyph(int px, int py, char c, int s, uint32_t col) {
    int idx = font_idx(c);
    if (idx < 0) return;
    const uint8_t *rows = g_font5x7[idx];
    for (int row = 0; row < 7; row++)
        for (int bit = 0; bit < 5; bit++)
            if (rows[row] & (1 << (4 - bit)))
                NodGL_FillRectContext(g_ctx,
                    px + bit*s, py + row*s, (uint32_t)s, (uint32_t)s, col);
}

static int draw_str(int px, int py, const char *s, int scale, uint32_t col) {
    int x = px;
    for (; *s; s++) {
        if (*s == ' ') { x += (5+1)*scale; continue; }
        draw_glyph(x, py, *s, scale, col);
        x += (5+1)*scale;
    }
    return x;
}

static void draw_int(int px, int py, int v, int scale, uint32_t col) {
    char buf[16]; itoa(v, buf, 10);
    draw_str(px, py, buf, scale, col);
}

/* ═══════════════════════════════════════════════════════════════════
   Rendering
   ═══════════════════════════════════════════════════════════════════ */

/* Camera: keep player centred, clamped to map bounds */
static void update_camera(void) {
    int world_w = MAP_W * g_tile;
    int world_h = MAP_H * g_tile;

    g_cam_x = G.px - (float)g_sw * 0.5f;
    g_cam_y = G.py - (float)g_sh * 0.5f;

    if (g_cam_x < 0.f) g_cam_x = 0.f;
    if (g_cam_y < 0.f) g_cam_y = 0.f;
    if (g_cam_x > (float)(world_w - (int)g_sw)) g_cam_x = (float)(world_w - (int)g_sw);
    if (g_cam_y > (float)(world_h - (int)g_sh)) g_cam_y = (float)(world_h - (int)g_sh);

    /* If world smaller than screen, centre it */
    if (world_w <= (int)g_sw) g_cam_x = (float)(world_w - (int)g_sw) * 0.5f;
    if (world_h <= (int)g_sh) g_cam_y = (float)(world_h - (int)g_sh) * 0.5f;
}

static void fill(int x, int y, int w, int h, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    NodGL_FillRectContext(g_ctx, x, y, (uint32_t)w, (uint32_t)h, c);
}

/* Pulse helper for exit glow — uses time_ms, returns 0..255 */
static uint8_t pulse(void) {
    uint32_t t = (uint32_t)(time_ms() % 1000);
    /* triangle wave 0→255→0 */
    return (uint8_t)(t < 500 ? t * 255 / 500 : (1000 - t) * 255 / 500);
}

static void render(void) {
    update_camera();

    /* Clear background */
    NodGL_FillRectContext(g_ctx, 0, 0, g_sw, g_sh, C_BG);

    /* Visible tile range */
    int tx0 = (int)(g_cam_x / g_tile);
    int ty0 = (int)(g_cam_y / g_tile);
    int tx1 = tx0 + (int)g_sw / g_tile + 2;
    int ty1 = ty0 + (int)g_sh / g_tile + 2;
    if (tx0 < 0) tx0 = 0;
    if (ty0 < 0) ty0 = 0;
    if (tx1 > MAP_W) tx1 = MAP_W;
    if (ty1 > MAP_H) ty1 = MAP_H;

    uint8_t p = pulse();
    uint32_t exit_col = (uint32_t)(0x002200u
        | ((uint32_t)(p / 2) << 16)
        | ((uint32_t)p       <<  8)
        | (uint32_t)(p / 3));

    for (int ty = ty0; ty < ty1; ty++) {
        for (int tx = tx0; tx < tx1; tx++) {
            int sx = (int)((float)(tx * g_tile) - g_cam_x);
            int sy = (int)((float)(ty * g_tile) - g_cam_y);
            uint8_t t = map_get(tx, ty);

            if (t == T_WALL) {
                /* Wall body */
                fill(sx, sy, g_tile, g_tile, C_WALL);
                /* Subtle top-left highlight */
                fill(sx, sy, g_tile, 1, C_WALL_HI);
                fill(sx, sy, 1, g_tile, C_WALL_HI);
            } else {
                /* Floor */
                fill(sx, sy, g_tile, g_tile, C_FLOOR);

                if (t == T_EXIT) {
                    /* Glowing exit marker — inner diamond */
                    int cx = sx + g_tile/2;
                    int cy2= sy + g_tile/2;
                    int r  = g_tile / 3;
                    /* Draw a simple filled square glow for now */
                    fill(cx - r, cy2 - r, r*2, r*2, exit_col);
                    /* bright centre dot */
                    int dot = g_tile / 6;
                    if (dot < 1) dot = 1;
                    fill(cx - dot, cy2 - dot, dot*2, dot*2, C_EXIT);
                }
            }
        }
    }

    /* Player */
    int psx = (int)(G.px - g_cam_x);
    int psy = (int)(G.py - g_cam_y);
    int pr  = g_tile / 3;
    if (pr < 2) pr = 2;

    /* Shadow */
    fill(psx - pr + 2, psy - pr + 2, pr*2, pr*2, C_PLAYER_S);
    /* Body */
    fill(psx - pr, psy - pr, pr*2, pr*2, C_PLAYER);
    /* Eye dot */
    int er = pr / 3; if (er < 1) er = 1;
    fill(psx - er, psy - er, er*2, er*2, 0x002200u);

    /* HUD */
    draw_str(6, 6, "LEVEL", 2, C_HUD);
    draw_int(66, 6, G.level + 1, 2, C_YELLOW);
    draw_str((int)g_sw/2 - 54, 6, "STEPS", 2, C_HUD);
    draw_int((int)g_sw/2 + 12, 6, (int)G.steps, 2, 0xFFFFFF);
}

/* ═══════════════════════════════════════════════════════════════════
   Overlay
   ═══════════════════════════════════════════════════════════════════ */

static int str_w(const char *s, int scale) {
    int w = 0;
    for (; *s; s++) w += (5+1)*scale;
    return w;
}

static void draw_overlay(const char *l1, const char *l2, const char *l3) {
    int ow = 300, oh = 90;
    int ox = ((int)g_sw - ow) / 2;
    int oy = ((int)g_sh - oh) / 2;
    fill(ox, oy, ow, oh, C_OVERLAY_BG);
    fill(ox, oy, ow, 2, 0x0066AA);
    fill(ox, oy+oh-2, ow, 2, 0x0066AA);
    fill(ox, oy, 2, oh, 0x0066AA);
    fill(ox+ow-2, oy, 2, oh, 0x0066AA);
    draw_str(ox + (ow - str_w(l1,2))/2, oy + 10, l1, 2, 0xFFFFFF);
    draw_str(ox + (ow - str_w(l2,2))/2, oy + 36, l2, 2, C_YELLOW);
    draw_str(ox + (ow - str_w(l3,2))/2, oy + 62, l3, 2, 0x88AAFF);
}

/* ═══════════════════════════════════════════════════════════════════
   Movement & collision
   ═══════════════════════════════════════════════════════════════════ */

#define PLAYER_RADIUS 0.35f   /* in tile units */
#define ACCEL         0.18f   /* tile/frame² */
#define FRICTION      0.75f   /* velocity multiplier per frame */
#define MAX_SPEED     0.22f   /* tile/frame */

static int tile_solid(int tx, int ty) {
    return map_get(tx, ty) == T_WALL;
}

static void physics_update(void) {
    float tile_f = (float)g_tile;

    /* Apply velocity */
    float nx = G.px + G.vx * tile_f;
    float ny = G.py + G.vy * tile_f;

    /* Collision: separate axes */
    float r = PLAYER_RADIUS * tile_f;

    /* X axis */
    if (!tile_solid((int)((nx + r) / tile_f), (int)(G.py / tile_f)) &&
        !tile_solid((int)((nx + r) / tile_f), (int)((G.py - r + 1.f) / tile_f)) &&
        !tile_solid((int)((nx - r) / tile_f), (int)(G.py / tile_f)) &&
        !tile_solid((int)((nx - r) / tile_f), (int)((G.py - r + 1.f) / tile_f)))
        G.px = nx;
    else
        G.vx = 0.f;

    /* Y axis */
    if (!tile_solid((int)(G.px / tile_f), (int)((ny + r) / tile_f)) &&
        !tile_solid((int)((G.px - r + 1.f) / tile_f), (int)((ny + r) / tile_f)) &&
        !tile_solid((int)(G.px / tile_f), (int)((ny - r) / tile_f)) &&
        !tile_solid((int)((G.px - r + 1.f) / tile_f), (int)((ny - r) / tile_f)))
        G.py = ny;
    else
        G.vy = 0.f;

    /* Friction */
    G.vx *= FRICTION;
    G.vy *= FRICTION;
    if (fabs_f(G.vx) < 0.001f) G.vx = 0.f;
    if (fabs_f(G.vy) < 0.001f) G.vy = 0.f;

    /* Count steps when meaningfully moving */
    if (fabs_f(G.vx) > 0.005f || fabs_f(G.vy) > 0.005f)
        G.steps++;

    /* Check exit */
    int ptx = (int)(G.px / (float)g_tile);
    int pty = (int)(G.py / (float)g_tile);
    if (map_get(ptx, pty) == T_EXIT) G.won = 1;
}

static void apply_input(void) {
    float ax = 0.f, ay = 0.f;
    if (g_key_up)    ay -= ACCEL;
    if (g_key_down)  ay += ACCEL;
    if (g_key_left)  ax -= ACCEL;
    if (g_key_right) ax += ACCEL;

    /* Normalise diagonal */
    if (ax != 0.f && ay != 0.f) {
        ax *= 0.7071f;
        ay *= 0.7071f;
    }

    G.vx = clampf(G.vx + ax, -MAX_SPEED, MAX_SPEED);
    G.vy = clampf(G.vy + ay, -MAX_SPEED, MAX_SPEED);
}

/* ═══════════════════════════════════════════════════════════════════
   Input
   ═══════════════════════════════════════════════════════════════════ */

static void handle_event(const Event *e, int *quit, int *restart) {
    if (e->type != EVENT_KEY_PRESSED && e->type != EVENT_KEY_RELEASED) return;
    int pressed = (e->type == EVENT_KEY_PRESSED);
    KeyCode kc  = e->data.keyboard.keycode;
    char    c   = e->data.keyboard.ascii;

    if (pressed) {
        if (kc == KEY_ESCAPE)        { *quit = 1; return; }
        if (c == 'q' || c == 'Q')    { *quit = 1; return; }
        if (c == 'm' || c == 'M')    { G.show_map ^= 1; return; }
        if (G.won) {
            if (kc == KEY_ENTER || c == '\r') { *restart = 1; return; }
        }
    }

    /* Arrow keys */
    if (kc == KEY_ARROW_UP    || c == 'w' || c == 'W') g_key_up    = pressed;
    if (kc == KEY_ARROW_DOWN  || c == 's' || c == 'S') g_key_down  = pressed;
    if (kc == KEY_ARROW_LEFT  || c == 'a' || c == 'A') g_key_left  = pressed;
    if (kc == KEY_ARROW_RIGHT || c == 'd' || c == 'D') g_key_right = pressed;
}

/* ═══════════════════════════════════════════════════════════════════
   Entry point
   ═══════════════════════════════════════════════════════════════════ */

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    if (NodGL_CreateDevice(NodGL_FEATURE_LEVEL_1_0, &g_dev, &g_ctx, NULL) != NodGL_OK) {
        puts_raw("maze2d: NodGL init failed\n");
        sleep(2);
        return 1;
    }
    NodGL_GetScreenResolution(g_dev, &g_sw, &g_sh);

    /* Tile size: fit MAP_W×MAP_H tiles on screen with a little margin */
    g_tile = (int)g_sw / (MAP_W + 2);
    {
        int th = (int)g_sh / (MAP_H + 2);
        if (th < g_tile) g_tile = th;
    }
    if (g_tile < 8)  g_tile = 8;
    if (g_tile > 32) g_tile = 32;

    int efd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    if (efd < 0) {
        puts_raw("maze2d: cannot open event0\n");
        NodGL_ReleaseDevice(g_dev);
        return 2;
    }

    G.seed  = (uint32_t)(time_ms() & 0x7FFFFFFFu);
    G.level = 0;
    level_generate();

    /* Title screen */
    NodGL_FillRectContext(g_ctx, 0, 0, g_sw, g_sh, C_BG);
    draw_overlay("MAZE 2D", "FIND THE EXIT", "ENTER TO START");
    NodGL_PresentContext(g_ctx, 0);

    /* Wait for Enter */
    {
        int ready = 0;
        while (!ready) {
            Event ev;
            while (read(efd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                if (ev.type != EVENT_KEY_PRESSED) continue;
                KeyCode kc = ev.data.keyboard.keycode;
                char ac = ev.data.keyboard.ascii;
                if (kc == KEY_ENTER || ac == '\r') ready = 1;
                if (kc == KEY_ESCAPE || ac == 'q' || ac == 'Q') {
                    close(efd);
                    NodGL_ReleaseDevice(g_dev);
                    return 0;
                }
            }
            yield();
        }
    }

    int quit = 0;

    while (!quit) {
        uint64_t frame_start = time_ms();

        /* Input */
        int restart = 0;
        Event ev;
        while (read(efd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            handle_event(&ev, &quit, &restart);
            if (quit) break;
        }
        if (quit) break;

        if (restart) {
            G.level++;
            level_generate();
        }

        /* Update */
        if (!G.won) {
            apply_input();
            physics_update();
        }

        /* Render */
        render();

        if (G.won) {
            char lmsg[32];
            lmsg[0]='L'; lmsg[1]='E'; lmsg[2]='V'; lmsg[3]='E';
            lmsg[4]='L'; lmsg[5]=' '; itoa(G.level + 1, lmsg + 6, 10);
            draw_overlay("YOU ESCAPED!", lmsg, "ENTER FOR NEXT");
        }

        NodGL_PresentContext(g_ctx, 0);

        /* ~60fps cap */
        uint64_t elapsed = time_ms() - frame_start;
        if (elapsed < 16) {
            uint64_t end = time_ms() + (16 - elapsed);
            while (time_ms() < end) yield();
        }
    }

    close(efd);
    NodGL_ReleaseDevice(g_dev);
    return 0;
}