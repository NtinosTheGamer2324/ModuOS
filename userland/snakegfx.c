#include "libc.h"
#include "string.h"
#include "../include/moduos/kernel/events/events.h"

/*
 * snakegfx.sqr
 *
 * Userland port of the kernel Snake (eatfruit.c) game, but rendered in graphics mode
 * via MD64API GRP ($/dev/graphics/video0).
 *
 * Input: $/dev/input/event0 (structured events)
 *   - WASD / hjkl / arrow-key escape sequences if your shell sends them as ASCII
 *   - p = pause
 *   - q or ESC = quit (ESC might arrive as 0x1B)
 */

#define GAME_W 40
#define GAME_H 25
#define MAX_SNAKE 1000

typedef enum { DIR_UP=0, DIR_DOWN=1, DIR_LEFT=2, DIR_RIGHT=3 } Dir;

typedef struct { int x, y; } Pt;

typedef struct {
    Pt body[MAX_SNAKE];
    int len;
    Dir dir;
} Snake;

typedef struct {
    Snake s;
    Pt food;
    int score;
    int over;
    int paused;
} Game;

/*
 * IMPORTANT: keep large state off the user stack.
 * The process stack is small; putting Game on the stack overflows instantly.
 */
static Game g_game;

static uint32_t rng_seed = 1;
static uint32_t rnd_u32(void) {
    rng_seed = (rng_seed * 1103515245u + 12345u) & 0x7fffffffu;
    return rng_seed;
}

static int snake_hits(const Snake *s, Pt p) {
    for (int i = 0; i < s->len; i++) {
        if (s->body[i].x == p.x && s->body[i].y == p.y) return 1;
    }
    return 0;
}

static void spawn_food(Game *g) {
    do {
        g->food.x = (int)(rnd_u32() % GAME_W);
        g->food.y = (int)(rnd_u32() % GAME_H);
    } while (snake_hits(&g->s, g->food));
}

static void game_init(Game *g) {
    memset(g, 0, sizeof(*g));
    g->s.len = 3;
    g->s.dir = DIR_RIGHT;
    int sx = GAME_W / 2;
    int sy = GAME_H / 2;
    g->s.body[0] = (Pt){sx, sy};
    g->s.body[1] = (Pt){sx-1, sy};
    g->s.body[2] = (Pt){sx-2, sy};
    g->score = 0;
    g->over = 0;
    g->paused = 0;
    spawn_food(g);
}

static int game_step(Game *g, Pt *old_head, Pt *old_tail, Pt *old_food, int *ate_food) {
    if (old_head) *old_head = g->s.body[0];
    if (old_tail) *old_tail = g->s.body[g->s.len - 1];
    if (old_food) *old_food = g->food;
    if (ate_food) *ate_food = 0;

    if (g->over || g->paused) return 0;

    Pt nh = g->s.body[0];
    switch (g->s.dir) {
        case DIR_UP:    nh.y--; break;
        case DIR_DOWN:  nh.y++; break;
        case DIR_LEFT:  nh.x--; break;
        case DIR_RIGHT: nh.x++; break;
    }

    if (nh.x < 0 || nh.x >= GAME_W || nh.y < 0 || nh.y >= GAME_H) {
        g->over = 1;
        return 0;
    }
    if (snake_hits(&g->s, nh)) {
        g->over = 1;
        return 0;
    }

    int ate = (nh.x == g->food.x && nh.y == g->food.y);
    if (ate) {
        if (ate_food) *ate_food = 1;
        g->score += 10;
        if (g->s.len < MAX_SNAKE) g->s.len++;
        spawn_food(g);
    }

    for (int i = g->s.len - 1; i > 0; i--) {
        g->s.body[i] = g->s.body[i-1];
    }
    g->s.body[0] = nh;
    return 1;
}

/* ===========================
 * Graphics backend
 * =========================== */

typedef struct {
    md64api_grp_video_info_t vi;

    /* Render target: userland backbuffer (tightly packed). */
    uint8_t *bb;
    uint32_t bb_pitch;

    uint32_t fmt; /* md64api_grp_format_t */
    uint32_t cell_px;
    uint32_t off_x;
    uint32_t off_y;
} Gfx;

static uint32_t xrgb(uint8_t r,uint8_t g,uint8_t b){ return ((uint32_t)r<<16)|((uint32_t)g<<8)|b; }
static uint16_t rgb565(uint8_t r,uint8_t g,uint8_t b){
    uint16_t rr=(uint16_t)((r*31u)/255u);
    uint16_t gg=(uint16_t)((g*63u)/255u);
    uint16_t bb=(uint16_t)((b*31u)/255u);
    return (uint16_t)((rr<<11)|(gg<<5)|bb);
}

static void fb_put_rect(Gfx *g, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c) {
    if (!g || !g->bb) return;
    if (x >= g->vi.width || y >= g->vi.height) return;
    if (x + w > g->vi.width) w = g->vi.width - x;
    if (y + h > g->vi.height) h = g->vi.height - y;

    if (g->fmt == MD64API_GRP_FMT_XRGB8888 && g->vi.bpp == 32) {
        for (uint32_t yy=0; yy<h; yy++) {
            uint32_t *row = (uint32_t*)(g->bb + (uint64_t)(y+yy)*g->bb_pitch);
            for (uint32_t xx=0; xx<w; xx++) row[x+xx] = c;
        }
    } else if (g->fmt == MD64API_GRP_FMT_RGB565 && g->vi.bpp == 16) {
        uint16_t cc = (uint16_t)c;
        for (uint32_t yy=0; yy<h; yy++) {
            uint16_t *row = (uint16_t*)(g->bb + (uint64_t)(y+yy)*g->bb_pitch);
            for (uint32_t xx=0; xx<w; xx++) row[x+xx] = cc;
        }
    }
}

static void draw_cell(Gfx *g, int cx, int cy, uint32_t color) {
    uint32_t px = g->off_x + (uint32_t)cx * g->cell_px;
    uint32_t py = g->off_y + (uint32_t)cy * g->cell_px;
    fb_put_rect(g, px, py, g->cell_px-1, g->cell_px-1, color);
}

static uint32_t col_bg(Gfx *g){ return (g->fmt == MD64API_GRP_FMT_RGB565) ? (uint32_t)rgb565(8,8,12) : xrgb(8,8,12); }
static uint32_t col_grid(Gfx *g){ return (g->fmt == MD64API_GRP_FMT_RGB565) ? (uint32_t)rgb565(18,18,24) : xrgb(18,18,24); }
static uint32_t col_food(Gfx *g){ return (g->fmt == MD64API_GRP_FMT_RGB565) ? (uint32_t)rgb565(220,40,40) : xrgb(220,40,40); }
static uint32_t col_head(Gfx *g){ return (g->fmt == MD64API_GRP_FMT_RGB565) ? (uint32_t)rgb565(60,240,90) : xrgb(60,240,90); }
static uint32_t col_body(Gfx *g){ return (g->fmt == MD64API_GRP_FMT_RGB565) ? (uint32_t)rgb565(30,160,60) : xrgb(30,160,60); }

/* One-time full clear. This avoids visible redraw every frame. */
static int gfx_present_full(Gfx *g) {
    if (!g || !g->bb) return -1;
    return gfx_blit(g->bb,
                    (uint16_t)g->vi.width, (uint16_t)g->vi.height,
                    0, 0,
                    (uint16_t)g->bb_pitch,
                    (uint16_t)g->fmt);
}

static int gfx_present_cell(Gfx *g, int cx, int cy) {
    if (!g || !g->bb) return -1;
    if (cx < 0 || cy < 0 || cx >= GAME_W || cy >= GAME_H) return 0;

    uint32_t bpp = (g->fmt == MD64API_GRP_FMT_RGB565) ? 2u : 4u;
    uint32_t px = g->off_x + (uint32_t)cx * g->cell_px;
    uint32_t py = g->off_y + (uint32_t)cy * g->cell_px;
    uint32_t w = (g->cell_px > 0) ? (g->cell_px - 1) : 0;
    uint32_t h = (g->cell_px > 0) ? (g->cell_px - 1) : 0;
    if (w == 0 || h == 0) return 0;

    if (px >= g->vi.width || py >= g->vi.height) return 0;
    if (px + w > g->vi.width) w = g->vi.width - px;
    if (py + h > g->vi.height) h = g->vi.height - py;

    const uint8_t *src = g->bb + (uint64_t)py * g->bb_pitch + (uint64_t)px * bpp;
    uint32_t src_pitch = g->bb_pitch;

    return gfx_blit(src, (uint16_t)w, (uint16_t)h,
                    (uint16_t)px, (uint16_t)py,
                    (uint16_t)src_pitch,
                    (uint16_t)g->fmt);
}

static void draw_init(Gfx *g, const Game *game) {
    fb_put_rect(g, 0, 0, g->vi.width, g->vi.height, col_bg(g));
    fb_put_rect(g, g->off_x, g->off_y, GAME_W * g->cell_px, GAME_H * g->cell_px, col_grid(g));

    /* initial snake + food */
    draw_cell(g, game->food.x, game->food.y, col_food(g));
    for (int i = 0; i < game->s.len; i++) {
        draw_cell(g, game->s.body[i].x, game->s.body[i].y, (i==0)?col_head(g):col_body(g));
    }
}

/* Incremental updates for smooth rendering */
static void draw_update(Gfx *g, const Game *game, Pt old_head, Pt old_tail, Pt old_food, int ate) {
    /* erase tail if we did not grow */
    if (!ate) {
        draw_cell(g, old_tail.x, old_tail.y, col_grid(g));
        (void)gfx_present_cell(g, old_tail.x, old_tail.y);
    }

    /* old head becomes body (unless game over immediately) */
    if (!game->over) {
        draw_cell(g, old_head.x, old_head.y, col_body(g));
        (void)gfx_present_cell(g, old_head.x, old_head.y);

        draw_cell(g, game->s.body[0].x, game->s.body[0].y, col_head(g));
        (void)gfx_present_cell(g, game->s.body[0].x, game->s.body[0].y);
    }

    /* food moved? */
    if (ate) {
        draw_cell(g, old_food.x, old_food.y, col_grid(g));
        (void)gfx_present_cell(g, old_food.x, old_food.y);

        draw_cell(g, game->food.x, game->food.y, col_food(g));
        (void)gfx_present_cell(g, game->food.x, game->food.y);
    }

    /* HUD to console (optional) */
    printf("\rscore=%d len=%d %s %s  ", game->score, game->s.len,
           game->paused ? "[PAUSED]" : "        ",
           game->over ? "[GAME OVER]" : "          ");
}

static int gfx_init(Gfx *g) {
    memset(g, 0, sizeof(*g));
    if (md64api_grp_get_video0_info(&g->vi) != 0) return -1;

    if (g->vi.mode != MD64API_GRP_MODE_GRAPHICS || g->vi.fb_addr == 0) return -2;

    g->fmt = g->vi.fmt;
    if (g->fmt == MD64API_GRP_FMT_UNKNOWN) {
        if (g->vi.bpp == 32) g->fmt = MD64API_GRP_FMT_XRGB8888;
        else if (g->vi.bpp == 16) g->fmt = MD64API_GRP_FMT_RGB565;
    }

    if (!((g->fmt == MD64API_GRP_FMT_XRGB8888 && g->vi.bpp == 32) ||
          (g->fmt == MD64API_GRP_FMT_RGB565 && g->vi.bpp == 16))) {
        return -3;
    }

    /* allocate a tightly-packed backbuffer (presented via gfx_blit) */
    uint32_t bpp_bytes = (g->fmt == MD64API_GRP_FMT_RGB565) ? 2u : 4u;
    g->bb_pitch = g->vi.width * bpp_bytes;
    uint32_t buf_size = g->bb_pitch * g->vi.height;
    g->bb = (uint8_t*)malloc(buf_size);
    if (!g->bb) return -4;

    /* choose cell size to fit */
    uint32_t max_cell_w = g->vi.width / GAME_W;
    uint32_t max_cell_h = g->vi.height / GAME_H;
    g->cell_px = max_cell_w < max_cell_h ? max_cell_w : max_cell_h;
    if (g->cell_px < 8) g->cell_px = 8;
    if (g->cell_px > 32) g->cell_px = 32;

    uint32_t board_w = GAME_W * g->cell_px;
    uint32_t board_h = GAME_H * g->cell_px;
    g->off_x = (g->vi.width  > board_w) ? (g->vi.width  - board_w)/2 : 0;
    g->off_y = (g->vi.height > board_h) ? (g->vi.height - board_h)/2 : 0;

    return 0;
}

/* ===========================
 * Input
 * =========================== */

static int evt_open(void) {
    return open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
}

static void handle_event(Game *g, const Event *e, int *quit) {
    if (!g || !e || !quit) return;
    if (e->type != EVENT_KEY_PRESSED) return;

    KeyCode kc = e->data.keyboard.keycode;
    char c = e->data.keyboard.ascii;

    if (kc == KEY_ESCAPE || c == 0x1b || c == 'q' || c == 'Q') { *quit = 1; return; }

    if (g->over) {
        if (kc == KEY_ENTER || c == '\n') {
            game_init(g);
        }
        return;
    }

    if (c == 'p' || c == 'P') {
        g->paused = !g->paused;
        return;
    }

    /* Arrows */
    if (kc == KEY_ARROW_UP && g->s.dir != DIR_DOWN) g->s.dir = DIR_UP;
    else if (kc == KEY_ARROW_DOWN && g->s.dir != DIR_UP) g->s.dir = DIR_DOWN;
    else if (kc == KEY_ARROW_LEFT && g->s.dir != DIR_RIGHT) g->s.dir = DIR_LEFT;
    else if (kc == KEY_ARROW_RIGHT && g->s.dir != DIR_LEFT) g->s.dir = DIR_RIGHT;

    /* Fallback: WASD + hjkl */
    else if ((c=='w'||c=='W'||c=='k') && g->s.dir != DIR_DOWN) g->s.dir = DIR_UP;
    else if ((c=='s'||c=='S'||c=='j') && g->s.dir != DIR_UP) g->s.dir = DIR_DOWN;
    else if ((c=='a'||c=='A'||c=='h') && g->s.dir != DIR_RIGHT) g->s.dir = DIR_LEFT;
    else if ((c=='d'||c=='D'||c=='l') && g->s.dir != DIR_LEFT) g->s.dir = DIR_RIGHT;
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    puts_raw("snakegfx - Snake in userland (graphics)\n");

    Gfx gfx;
    int gr = gfx_init(&gfx);
    if (gr != 0) {
        printf("snakegfx: graphics init failed (%d). Need framebuffer graphics mode.\n", gr);
        sleep(2);
        return 1;
    }

    printf("snakegfx: video w=%u h=%u pitch=%u bpp=%u fmt=%u fb=0x%llx\n",
           (unsigned)gfx.vi.width, (unsigned)gfx.vi.height, (unsigned)gfx.vi.pitch,
           (unsigned)gfx.vi.bpp, (unsigned)gfx.fmt, (unsigned long long)gfx.vi.fb_addr);

    int efd = evt_open();
    if (efd < 0) {
        puts_raw("snakegfx: cannot open $/dev/input/event0\n");
        sleep(2);
        return 2;
    }
    puts_raw("snakegfx: opened event0 (nonblocking)\n");

    rng_seed = (uint32_t)(time_ms() & 0x7fffffff);

    game_init(&g_game);

    puts_raw("Controls: WASD (or HJKL), P pause, Q quit, ENTER restart\n");

    /* One-time draw */
    draw_init(&gfx, &g_game);
    gfx_present_full(&gfx);

    uint64_t last_tick = time_ms();
    uint64_t last_draw = 0;

    int quit = 0;
    uint64_t start = time_ms();
    while (!quit) {
        /* keep alive marker in case of instant exit debugging */
        if (time_ms() - start < 500) {
            // no-op
        }
        /* Non-blocking event poll */
        Event ev;
        while (read(efd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            handle_event(&g_game, &ev, &quit);
        }

        uint64_t now = time_ms();
        if (now - last_tick >= 120) {
            Pt old_head, old_tail, old_food;
            int ate = 0;
            int moved = game_step(&g_game, &old_head, &old_tail, &old_food, &ate);
            if (moved) {
                draw_update(&gfx, &g_game, old_head, old_tail, old_food, ate);
            }
            last_tick = now;
        }

        /* throttle HUD refresh if paused/over */
        if (now - last_draw >= 250) {
            printf("\rscore=%d len=%d %s %s  ", g_game.score, g_game.s.len,
                   g_game.paused ? "[PAUSED]" : "        ",
                   g_game.over ? "[GAME OVER]" : "          ");
            last_draw = now;
        }

        /* Yield a bit */
        yield();
    }

    close(efd);
    puts_raw("\nBye.\n");
    return 0;
}
