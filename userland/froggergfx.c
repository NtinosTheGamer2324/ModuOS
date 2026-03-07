#include "libc.h"
#include "NodGL.h"
#include "string.h"
#include "../include/moduos/kernel/events/events.h"

/*
 * minesgfx.sqr
 *
 * Graphical Minesweeper (standalone userland app).
 *  - Uses $/dev/graphics/video0 via gfx_blit
 *  - Uses $/dev/input/event0 mouse events
 *
 * Controls:
 *  - Left click: reveal
 *  - Right click: flag/unflag
 *  - ESC: exit
 */

/* Embedded 8x8 font (ASCII 32..127). Source: public domain font8x8.
 * Copied from miniwm.c.
 */
static const uint8_t font8x8_basic[96][8] = {
    /* 0x20 ' ' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x21 '!' */ {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00},
    /* 0x22 '"' */ {0x36,0x36,0x24,0x00,0x00,0x00,0x00,0x00},
    /* 0x23 '#' */ {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},
    /* 0x24 '$' */ {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00},
    /* 0x25 '%' */ {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00},
    /* 0x '&' */ {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00},
    /* 0x27 '\''*/ {0x06,0x06,0x04,0x00,0x00,0x00,0x00,0x00},
    /* 0x28 '(' */ {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00},
    /* 0x29 ')' */ {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00},
    /* 0x2A '*' */ {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},
    /* 0x2B '+' */ {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00},
    /* 0x2C ',' */ {0x00,0x00,0x00,0x00,0x0C,0x0C,0x06,0x00},
    /* 0x2D '-' */ {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00},
    /* 0x2E '.' */ {0x00,0x00,0x00,0x00,0x0C,0x0C,0x00,0x00},
    /* 0x2F '/' */ {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00},
    /* 0x30 '0' */ {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00},
    /* 0x31 '1' */ {0x0C,0x0E,0x0F,0x0C,0x0C,0x0C,0x3F,0x00},
    /* 0x32 '2' */ {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00},
    /* 0x33 '3' */ {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00},
    /* 0x34 '4' */ {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00},
    /* 0x35 '5' */ {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00},
    /* 0x36 '6' */ {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00},
    /* 0x37 '7' */ {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00},
    /* 0x38 '8' */ {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00},
    /* 0x39 '9' */ {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00},
    /* 0x3A ':' */ {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00,0x00},
    /* 0x3B ';' */ {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x06,0x00},
    /* 0x3C '<' */ {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00},
    /* 0x3D '=' */ {0x00,0x00,0x3F,0x00,0x3F,0x00,0x00,0x00},
    /* 0x3E '>' */ {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00},
    /* 0x3F '?' */ {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00},
    /* 0x40 '@' */ {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00},
    /* 0x41 'A' */ {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00},
    /* 0x42 'B' */ {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00},
    /* 0x43 'C' */ {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00},
    /* 0x44 'D' */ {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00},
    /* 0x45 'E' */ {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00},
    /* 0x46 'F' */ {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00},
    /* 0x47 'G' */ {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00},
    /* 0x48 'H' */ {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00},
    /* 0x49 'I' */ {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    /* 0x4A 'J' */ {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00},
    /* 0x4B 'K' */ {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00},
    /* 0x4C 'L' */ {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00},
    /* 0x4D 'M' */ {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00},
    /* 0x4E 'N' */ {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00},
    /* 0x4F 'O' */ {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00},
    /* 0x50 'P' */ {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00},
    /* 0x51 'Q' */ {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00},
    /* 0x52 'R' */ {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00},
    /* 0x53 'S' */ {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00},
    /* 0x54 'T' */ {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    /* 0x55 'U' */ {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00},
    /* 0x56 'V' */ {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00},
    /* 0x57 'W' */ {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
    /* 0x58 'X' */ {0x63,0x36,0x1C,0x1C,0x1C,0x36,0x63,0x00},
    /* 0x59 'Y' */ {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00},
    /* 0x5A 'Z' */ {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00},
    /* 0x5B '[' */ {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00},
    /* 0x5C '\\' */ {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00},
    /* 0x5D ']' */ {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00},
    /* 0x5E '^' */ {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00},
    /* 0x5F '_' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
    /* 0x60 '`' */ {0x06,0x06,0x0C,0x00,0x00,0x00,0x00,0x00},
    /* 0x61 'a' */ {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00},
    /* 0x62 'b' */ {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00},
    /* 0x63 'c' */ {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00},
    /* 0x64 'd' */ {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00},
    /* 0x65 'e' */ {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00},
    /* 0x66 'f' */ {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00},
    /* 0x67 'g' */ {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F},
    /* 0x68 'h' */ {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00},
    /* 0x69 'i' */ {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00},
    /* 0x6A 'j' */ {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E},
    /* 0x6B 'k' */ {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00},
    /* 0x6C 'l' */ {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    /* 0x6D 'm' */ {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00},
    /* 0x6E 'n' */ {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00},
    /* 0x6F 'o' */ {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00},
    /* 0x70 'p' */ {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F},
    /* 0x71 'q' */ {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78},
    /* 0x72 'r' */ {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00},
    /* 0x73 's' */ {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00},
    /* 0x74 't' */ {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00},
    /* 0x75 'u' */ {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00},
    /* 0x76 'v' */ {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00},
    /* 0x77 'w' */ {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00},
    /* 0x78 'x' */ {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00},
    /* 0x79 'y' */ {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F},
    /* 0x7A 'z' */ {0x00,0x00,0x3F,0x19,0x0C,0x00,0x3F,0x00},
    /* 0x7B '{' */ {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00},
    /* 0x7C '|' */ {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},
    /* 0x7D '}' */ {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00},
    /* 0x7E '~' */ {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x7F DEL */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

static inline uint32_t pack_xrgb8888(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

#define CURSOR_W 16u
#define CURSOR_H 16u

typedef struct {
    uint32_t w;
    uint32_t h;
    uint8_t rgba[CURSOR_W * CURSOR_H * 4u];
} cursor_img_t;

static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline int32_t rds32(const uint8_t *p) { return (int32_t)rd32(p); }

static int load_cursor_bmp_rgba8888(const char *path, cursor_img_t *out) {
    fs_file_info_t st;
    if (stat(path, &st) != 0) return -1;
    if (st.is_directory) return -2;

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -3;

    uint32_t size = st.size;
    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) {
        close(fd);
        return -4;
    }

    uint32_t got = 0;
    while (got < size) {
        ssize_t n = read(fd, buf + got, size - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    close(fd);

    if (got != size || size < 54) {
        free(buf);
        return -5;
    }

    if (buf[0] != 'B' || buf[1] != 'M') {
        free(buf);
        return -6;
    }

    uint32_t pixel_off = rd32(buf + 10);
    uint32_t dib_size = rd32(buf + 14);
    if (dib_size < 40 || 14u + dib_size > size) {
        free(buf);
        return -7;
    }

    int32_t width = rds32(buf + 18);
    int32_t height = rds32(buf + 22);
    uint16_t planes = (uint16_t)(buf[26] | (buf[27] << 8));
    uint16_t bpp = (uint16_t)(buf[28] | (buf[29] << 8));

    if (planes != 1 || bpp != 32) {
        free(buf);
        return -8;
    }

    int top_down = 0;
    if (height < 0) {
        top_down = 1;
        height = -height;
    }

    if (width != (int32_t)CURSOR_W || height != (int32_t)CURSOR_H) {
        free(buf);
        return -9;
    }

    uint32_t src_row_stride = (uint32_t)width * 4u;
    if (pixel_off + (uint64_t)src_row_stride * (uint32_t)height > size) {
        free(buf);
        return -10;
    }

    out->w = CURSOR_W;
    out->h = CURSOR_H;

    for (uint32_t y = 0; y < CURSOR_H; y++) {
        uint32_t sy = top_down ? y : (CURSOR_H - 1u - y);
        const uint8_t *row = buf + pixel_off + (uint64_t)sy * src_row_stride;
        for (uint32_t x = 0; x < CURSOR_W; x++) {
            uint32_t px = rd32(row + x * 4u);
            uint8_t b = (uint8_t)(px & 0xFF);
            uint8_t g = (uint8_t)((px >> 8) & 0xFF);
            uint8_t r = (uint8_t)((px >> 16) & 0xFF);
            uint8_t a = (uint8_t)((px >> 24) & 0xFF);

            uint32_t di = (y * CURSOR_W + x) * 4u;
            out->rgba[di + 0] = r;
            out->rgba[di + 1] = g;
            out->rgba[di + 2] = b;
            out->rgba[di + 3] = a;
        }
    }

    free(buf);
    return 0;
}

static void alpha_blit_cursor_xrgb8888(uint8_t *dst, uint32_t pitch, uint32_t w, uint32_t h,
                                       const cursor_img_t *cur, int32_t x, int32_t y) {
    for (uint32_t cy = 0; cy < cur->h; cy++) {
        int32_t dy = y + (int32_t)cy;
        if ((uint32_t)dy >= h) continue;
        uint32_t *row = (uint32_t *)(dst + (uint64_t)dy * pitch);

        for (uint32_t cx = 0; cx < cur->w; cx++) {
            int32_t dx = x + (int32_t)cx;
            if ((uint32_t)dx >= w) continue;

            uint32_t si = (cy * cur->w + cx) * 4u;
            uint8_t sr = cur->rgba[si + 0];
            uint8_t sg = cur->rgba[si + 1];
            uint8_t sb = cur->rgba[si + 2];
            uint8_t sa = cur->rgba[si + 3];
            if (sa == 0) continue;

            uint32_t dp = row[dx];
            uint8_t dr = (uint8_t)((dp >> 16) & 0xFF);
            uint8_t dg = (uint8_t)((dp >> 8) & 0xFF);
            uint8_t db = (uint8_t)(dp & 0xFF);

            uint32_t a = sa;
            uint32_t ia = 255u - a;
            uint8_t or = (uint8_t)((sr * a + dr * ia) / 255u);
            uint8_t og = (uint8_t)((sg * a + dg * ia) / 255u);
            uint8_t ob = (uint8_t)((sb * a + db * ia) / 255u);

            row[dx] = pack_xrgb8888(or, og, ob);
        }
    }
}

static void fill_rect(uint8_t *fb, uint32_t pitch, uint32_t sw, uint32_t sh,
                      int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= (int32_t)sw || y >= (int32_t)sh) return;
    if (x + w > (int32_t)sw) w = (int32_t)sw - x;
    if (y + h > (int32_t)sh) h = (int32_t)sh - y;

    for (int32_t yy = 0; yy < h; yy++) {
        uint32_t *row = (uint32_t *)(fb + (uint64_t)(y + yy) * pitch);
        for (int32_t xx = 0; xx < w; xx++) row[x + xx] = c;
    }
}

static void put_px(uint8_t *fb, uint32_t pitch, uint32_t sw, uint32_t sh, int32_t x, int32_t y, uint32_t c) {
    if ((uint32_t)x >= sw || (uint32_t)y >= sh) return;
    uint32_t *row = (uint32_t *)(fb + (uint64_t)y * pitch);
    row[x] = c;
}

static void draw_char8(uint8_t *fb, uint32_t pitch, uint32_t sw, uint32_t sh,
                       int32_t x, int32_t y, char ch, uint32_t fg) {
    if (ch < 32 || ch > 127) ch = '?';
    const uint8_t *glyph = font8x8_basic[(int)ch - 32];
    for (int yy = 0; yy < 8; yy++) {
        uint8_t row = glyph[yy];
        for (int xx = 0; xx < 8; xx++) {
            if (row & (1u << xx)) {
                put_px(fb, pitch, sw, sh, x + xx, y + yy, fg);
            }
        }
    }
}

static void draw_text(uint8_t *fb, uint32_t pitch, uint32_t sw, uint32_t sh,
                      int32_t x, int32_t y, const char *s, uint32_t fg) {
    int32_t cx = x;
    for (size_t i = 0; s && s[i]; i++) {
        draw_char8(fb, pitch, sw, sh, cx, y, s[i], fg);
        cx += 8;
    }
}

/* Minesweeper logic (adapted from kernel mine_sweep.c) */
typedef enum { CELL_HIDDEN, CELL_REVEALED, CELL_FLAGGED } cell_state_t;

typedef struct {
    int mine;
    uint8_t adjacent;
    cell_state_t state;
} cell_t;

#define W 16
#define H 16
#define MINES 40

typedef struct {
    cell_t board[H][W];
    int mines_placed;
    int game_over;
    int win;

    uint64_t start_ms;   /* when timer started */
    uint64_t elapsed_s;  /* cached seconds */
} game_t;

static uint32_t rand_u32(uint32_t *s) {
    /* xorshift */
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static void compute_adj(game_t *g) {
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (g->board[y][x].mine) { g->board[y][x].adjacent = 0; continue; }
            int c = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (!dx && !dy) continue;
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                    if (g->board[ny][nx].mine) c++;
                }
            }
            g->board[y][x].adjacent = (uint8_t)c;
        }
    }
}

static void place_mines(game_t *g, int safe_x, int safe_y) {
    if (!g->start_ms) g->start_ms = time_ms();
    uint32_t seed = (uint32_t)time_ms();
    int placed = 0;
    while (placed < MINES) {
        int x = (int)(rand_u32(&seed) % W);
        int y = (int)(rand_u32(&seed) % H);
        if ((x == safe_x && y == safe_y) || g->board[y][x].mine) continue;
        g->board[y][x].mine = 1;
        placed++;
    }
    g->mines_placed = 1;
    compute_adj(g);
}

static void reveal(game_t *g, int x, int y) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    cell_t *c = &g->board[y][x];
    if (c->state == CELL_REVEALED || c->state == CELL_FLAGGED) return;

    c->state = CELL_REVEALED;
    if (c->mine) {
        g->game_over = 1;
        g->win = 0;
        return;
    }

    if (c->adjacent == 0) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (!dx && !dy) continue;
                reveal(g, x + dx, y + dy);
            }
        }
    }
}

static int check_win(game_t *g) {
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            cell_t *c = &g->board[y][x];
            if (!c->mine && c->state != CELL_REVEALED) return 0;
        }
    }
    return 1;
}

static int count_flagged_around(game_t *g, int x, int y) {
    int c = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (!dx && !dy) continue;
            int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
            if (g->board[ny][nx].state == CELL_FLAGGED) c++;
        }
    }
    return c;
}

static void chord_reveal(game_t *g, int x, int y) {
    cell_t *c = &g->board[y][x];
    if (c->state != CELL_REVEALED) return;
    if (c->adjacent == 0) return;

    int flags = count_flagged_around(g, x, y);
    if (flags != (int)c->adjacent) return;

    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (!dx && !dy) continue;
            int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
            if (g->board[ny][nx].state == CELL_FLAGGED) continue;
            reveal(g, nx, ny);
        }
    }
}

static void game_init(game_t *g) {
    memset(g, 0, sizeof(*g));
    g->start_ms = 0;
    g->elapsed_s = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            g->board[y][x].mine = 0;
            g->board[y][x].adjacent = 0;
            g->board[y][x].state = CELL_HIDDEN;
        }
    }
    g->mines_placed = 0;
    g->game_over = 0;
    g->win = 0;
}

static int count_flagged(game_t *g) {
    int c = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (g->board[y][x].state == CELL_FLAGGED) c++;
        }
    }
    return c;
}

static void draw_game(uint8_t *bb, uint32_t pitch, uint32_t sw, uint32_t sh, game_t *g, int32_t mx, int32_t my, int left_down) {
    (void)sh;

    /* Layout */
    const int tile = 24;
    const int ox = 32;
    const int oy = 48;

    /* Background (classic gray) */
    fill_rect(bb, pitch, sw, sh, 0, 0, (int32_t)sw, (int32_t)sh, pack_xrgb8888(190, 190, 190));

    /* Top bar */
    fill_rect(bb, pitch, sw, sh, 0, 0, (int32_t)sw, 40, pack_xrgb8888(160, 160, 160));
    fill_rect(bb, pitch, sw, sh, 0, 40, (int32_t)sw, 2, pack_xrgb8888(90, 90, 90));

    /* Smiley reset button (classic) */
    int sb = 26;
    int sx = (int)(sw / 2u) - sb / 2;
    int sy = 6;
    int over_smiley = (mx >= sx && my >= sy && mx < (sx + sb) && my < (sy + sb));

    uint32_t face = pack_xrgb8888(255, 220, 70);
    uint32_t outline = pack_xrgb8888(40, 40, 40);

    /* Button frame: raised normally, sunken when pressed over it */
    fill_rect(bb, pitch, sw, sh, sx, sy, sb, sb, pack_xrgb8888(200, 200, 200));
    if (left_down && over_smiley && !g->game_over && !g->win) {
        /* sunken */
        fill_rect(bb, pitch, sw, sh, sx, sy, sb, 2, pack_xrgb8888(110, 110, 110));
        fill_rect(bb, pitch, sw, sh, sx, sy, 2, sb, pack_xrgb8888(110, 110, 110));
        fill_rect(bb, pitch, sw, sh, sx, sy + sb - 2, sb, 2, pack_xrgb8888(240, 240, 240));
        fill_rect(bb, pitch, sw, sh, sx + sb - 2, sy, 2, sb, pack_xrgb8888(240, 240, 240));
    } else {
        /* raised */
        fill_rect(bb, pitch, sw, sh, sx, sy, sb, 2, pack_xrgb8888(240, 240, 240));
        fill_rect(bb, pitch, sw, sh, sx, sy, 2, sb, pack_xrgb8888(240, 240, 240));
        fill_rect(bb, pitch, sw, sh, sx, sy + sb - 2, sb, 2, pack_xrgb8888(110, 110, 110));
        fill_rect(bb, pitch, sw, sh, sx + sb - 2, sy, 2, sb, pack_xrgb8888(110, 110, 110));
    }

    /* face */
    fill_rect(bb, pitch, sw, sh, sx + 4, sy + 4, sb - 8, sb - 8, face);
    fill_rect(bb, pitch, sw, sh, sx + 4, sy + 4, sb - 8, 1, outline);
    fill_rect(bb, pitch, sw, sh, sx + 4, sy + sb - 5, sb - 8, 1, outline);
    fill_rect(bb, pitch, sw, sh, sx + 4, sy + 4, 1, sb - 8, outline);
    fill_rect(bb, pitch, sw, sh, sx + sb - 5, sy + 4, 1, sb - 8, outline);

    /* eyes */
    fill_rect(bb, pitch, sw, sh, sx + 9, sy + 11, 2, 2, outline);
    fill_rect(bb, pitch, sw, sh, sx + sb - 11, sy + 11, 2, 2, outline);

    /* mouth based on state */
    if (!g->game_over && !g->win && left_down) {
        /* surprised (classic: while holding left mouse anywhere) */
        fill_rect(bb, pitch, sw, sh, sx + 11, sy + 18, 1, 3, outline);
        fill_rect(bb, pitch, sw, sh, sx + 12, sy + 17, 1, 5, outline);
        fill_rect(bb, pitch, sw, sh, sx + 13, sy + 17, 1, 5, outline);
        fill_rect(bb, pitch, sw, sh, sx + 14, sy + 18, 1, 3, outline);
    } else if (g->game_over) {
        /* dead: X eyes + sad */
        fill_rect(bb, pitch, sw, sh, sx + 8, sy + 10, 4, 1, outline);
        fill_rect(bb, pitch, sw, sh, sx + 8, sy + 12, 4, 1, outline);
        fill_rect(bb, pitch, sw, sh, sx + sb - 12, sy + 10, 4, 1, outline);
        fill_rect(bb, pitch, sw, sh, sx + sb - 12, sy + 12, 4, 1, outline);
        fill_rect(bb, pitch, sw, sh, sx + 10, sy + 19, sb - 20, 1, outline);
        fill_rect(bb, pitch, sw, sh, sx + 9, sy + 18, 1, 1, outline);
        fill_rect(bb, pitch, sw, sh, sx + sb - 10, sy + 18, 1, 1, outline);
    } else if (g->win) {
        /* cool: sunglasses + smile */
        fill_rect(bb, pitch, sw, sh, sx + 7, sy + 11, sb - 14, 2, outline);
        fill_rect(bb, pitch, sw, sh, sx + 7, sy + 11, 2, 4, outline);
        fill_rect(bb, pitch, sw, sh, sx + sb - 9, sy + 11, 2, 4, outline);
        fill_rect(bb, pitch, sw, sh, sx + 10, sy + 19, sb - 20, 1, outline);
        fill_rect(bb, pitch, sw, sh, sx + 9, sy + 18, 1, 1, outline);
        fill_rect(bb, pitch, sw, sh, sx + sb - 10, sy + 18, 1, 1, outline);
    } else {
        /* normal smile */
        fill_rect(bb, pitch, sw, sh, sx + 10, sy + 19, sb - 20, 1, outline);
        fill_rect(bb, pitch, sw, sh, sx + 9, sy + 18, 1, 1, outline);
        fill_rect(bb, pitch, sw, sh, sx + sb - 10, sy + 18, 1, 1, outline);
    }

    draw_text(bb, pitch, sw, sh, 16, 12, "MD-Mine Sweeper", pack_xrgb8888(230, 230, 235));

    /* HUD: mines left + time */
    int flagged = count_flagged(g);
    int mines_left = MINES - flagged;
    char hud[64];
    strcpy(hud, "Mines: ");
    char num[16];
    itoa(mines_left, num, 10);
    strcat(hud, num);
    strcat(hud, "   Time: ");
    itoa((int)g->elapsed_s, num, 10);
    strcat(hud, num);
    draw_text(bb, pitch, sw, sh, 16, 24, hud, pack_xrgb8888(20, 20, 20));

    /* Grid */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int px = ox + x * tile;
            int py = oy + y * tile;

            cell_t *c = &g->board[y][x];
            /* Classic tile colors */
            uint32_t face = pack_xrgb8888(200, 200, 200);
            uint32_t shadow = pack_xrgb8888(120, 120, 120);
            uint32_t highlight = pack_xrgb8888(240, 240, 240);
            uint32_t revealed = pack_xrgb8888(170, 170, 170);

            if (c->state == CELL_REVEALED) {
                fill_rect(bb, pitch, sw, sh, px, py, tile - 1, tile - 1, revealed);
                /* inner border */
                fill_rect(bb, pitch, sw, sh, px, py, tile - 1, 1, shadow);
                fill_rect(bb, pitch, sw, sh, px, py, 1, tile - 1, shadow);
            } else {
                /* raised bevel */
                fill_rect(bb, pitch, sw, sh, px, py, tile - 1, tile - 1, face);
                fill_rect(bb, pitch, sw, sh, px, py, tile - 1, 2, highlight);
                fill_rect(bb, pitch, sw, sh, px, py, 2, tile - 1, highlight);
                fill_rect(bb, pitch, sw, sh, px, py + tile - 3, tile - 1, 2, shadow);
                fill_rect(bb, pitch, sw, sh, px + tile - 3, py, 2, tile - 1, shadow);
            }

            if (c->state == CELL_FLAGGED) {
                /* flag icon: pole + triangle */
                uint32_t pole = pack_xrgb8888(20, 20, 20);
                uint32_t red = pack_xrgb8888(220, 0, 0);
                /* pole */
                fill_rect(bb, pitch, sw, sh, px + 10, py + 6, 2, tile - 10, pole);
                fill_rect(bb, pitch, sw, sh, px + 8, py + tile - 6, 8, 2, pole);
                /* triangle */
                for (int yy = 0; yy < 8; yy++) {
                    int wtri = 8 - yy;
                    fill_rect(bb, pitch, sw, sh, px + 12, py + 6 + yy, wtri, 1, red);
                }
            } else if (c->state == CELL_REVEALED) {
                if (c->mine) {
                    draw_char8(bb, pitch, sw, sh, px + 8, py + 8, '*', pack_xrgb8888(255, 60, 60));
                } else if (c->adjacent) {
                    char ch = (char)('0' + c->adjacent);
                    uint32_t ncol = pack_xrgb8888(30, 200, 255);
                    if (c->adjacent == 1) ncol = pack_xrgb8888(80, 220, 80);
                    if (c->adjacent == 2) ncol = pack_xrgb8888(80, 120, 255);
                    if (c->adjacent >= 3) ncol = pack_xrgb8888(255, 200, 80);
                    draw_char8(bb, pitch, sw, sh, px + 8, py + 8, ch, ncol);
                }
            }

            /* hover highlight */
            int hx = (mx - ox) / tile;
            int hy = (my - oy) / tile;
            if (hx == x && hy == y) {
                fill_rect(bb, pitch, sw, sh, px, py, tile - 1, 2, pack_xrgb8888(255, 255, 255));
            }
        }
    }

    if (g->game_over) {
        draw_text(bb, pitch, sw, sh, 16, oy + H*tile + 16, "GAME OVER", pack_xrgb8888(200, 0, 0));
    } else if (g->win) {
        draw_text(bb, pitch, sw, sh, 16, oy + H*tile + 16, "YOU WIN!", pack_xrgb8888(0, 140, 0));
    }
}

static int streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static void debug_flag_all_mines(game_t *g) {
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (g->board[y][x].mine) {
                g->board[y][x].state = CELL_FLAGGED;
            }
        }
    }
}

int md_main(long argc, char **argv) {

    NodGL_Device device;
    NodGL_Context ctx;
    if (NodGL_CreateDevice(NodGL_FEATURE_LEVEL_1_0, &device, &ctx, NULL) != NodGL_OK) {
        puts_raw("froggergfx: NodGL init failed\n");
        return 1;
    }

    uint32_t screen_w, screen_h;
    NodGL_GetScreenResolution(device, &screen_w, &screen_h);

    NodGL_TextureDesc tex_desc = {
        .width = screen_w,
        .height = screen_h,
        .format = NodGL_FORMAT_R8G8B8A8_UNORM,
        .mip_levels = 1,
        .initial_data = NULL,
        .initial_data_size = 0
    };

    NodGL_Texture backbuffer_tex;
    if (NodGL_CreateTexture(device, &tex_desc, &backbuffer_tex) != NodGL_OK) {
        NodGL_ReleaseDevice(device);
        return 2;
    }

    uint8_t *bb;
    uint32_t pitch;
    if (NodGL_MapResource(ctx, backbuffer_tex, (void**)&bb, &pitch) != NodGL_OK) {
        NodGL_ReleaseResource(device, backbuffer_tex);
        NodGL_ReleaseDevice(device);
        return 2;
    }

    md64api_grp_video_info_t vi;
    vi.width = screen_w;
    vi.height = screen_h;
    vi.bpp = 32;

    int efd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    if (efd < 0) {
        NodGL_ReleaseResource(device, backbuffer_tex);
        NodGL_ReleaseDevice(device);
        puts_raw("froggergfx: cannot open event0\n");
        return 3;
    }

    int debug = 0;
    for (long i = 1; i < argc; i++) {
        if (streq(argv[i], "--debug")) debug = 1;
    }

    game_t g;
    game_init(&g);
    if (debug) {
        /* Place mines immediately so we can mark them. Use (0,0) as the forced-safe cell. */
        place_mines(&g, 0, 0);
        debug_flag_all_mines(&g);
    }

    int32_t mx = (int32_t)(vi.width / 2u);
    int32_t my = (int32_t)(vi.height / 2u);
    uint16_t buttons = 0, prev_buttons = 0;

    cursor_img_t cursor;
    int have_cursor = (load_cursor_bmp_rgba8888("/ModuOS/shared/usr/assets/mouse/arrow.bmp", &cursor) == 0);

    for (;;) {
        /* update timer */
        if (g.start_ms && !g.game_over && !g.win) {
            g.elapsed_s = (time_ms() - g.start_ms) / 1000u;
        }

        /* events */
        for (;;) {
            Event e;
            ssize_t n = read(efd, &e, sizeof(e));
            if (n != (ssize_t)sizeof(e)) break;

            if (e.type == EVENT_KEY_PRESSED && e.data.keyboard.keycode == KEY_ESCAPE) {
                close(efd);
                free(bb);
                return 0;
            }
            /* Restart: use CAPS LOCK keycode (exists as KEY_CAPS_LOCK in this tree).
             * If you later add letter keycodes, swap to KEY_R.
             */
            if (e.type == EVENT_KEY_PRESSED && e.data.keyboard.keycode == KEY_CAPS_LOCK) {
                game_init(&g);
            }

            if (e.type == EVENT_MOUSE_MOVE) {
                mx += e.data.mouse.delta_x;
                my += e.data.mouse.delta_y;
                if (mx < 0) mx = 0;
                if (my < 0) my = 0;
                if ((uint32_t)mx >= vi.width) mx = (int32_t)vi.width - 1;
                if ((uint32_t)my >= vi.height) my = (int32_t)vi.height - 1;
                buttons = e.data.mouse.buttons;
            } else if (e.type == EVENT_MOUSE_BUTTON) {
                buttons = e.data.mouse.buttons;
            }
        }

        int left_now = (buttons & 1) != 0;
        int right_now = (buttons & 2) != 0;
        int left_prev = (prev_buttons & 1) != 0;
        int right_prev = (prev_buttons & 2) != 0;
        int left_pressed = left_now && !left_prev;
        int right_pressed = right_now && !right_prev;
        int chord_pressed = (left_now && right_now) && !(left_prev && right_prev);

        /* Smiley button hit-test (must match draw_game top bar layout) */
        int sb = 26;
        int sx = (int)(vi.width / 2u) - sb / 2;
        int sy = 6;
        int over_smiley = (mx >= sx && my >= sy && mx < (sx + sb) && my < (sy + sb));
        if (left_pressed && over_smiley) {
            game_init(&g);
            prev_buttons = buttons;
            draw_game(bb, pitch, vi.width, vi.height, &g, mx, my, (buttons & 1) != 0);
            if (have_cursor) alpha_blit_cursor_xrgb8888(bb, pitch, vi.width, vi.height, &cursor, mx, my);
            NodGL_DrawTexture(ctx, backbuffer_tex, 0, 0, 0, 0, vi.width, vi.height);
            NodGL_PresentContext(ctx, 0);
            yield();
            continue;
        }

        const int tile = 24;
        const int ox = 32;
        const int oy = 48;
        int gx = (mx - ox) / tile;
        int gy = (my - oy) / tile;

        if (!g.game_over && !g.win) {
            if (left_pressed && gx >= 0 && gy >= 0 && gx < W && gy < H) {
                if (!g.mines_placed) place_mines(&g, gx, gy);
                reveal(&g, gx, gy);
                if (!g.game_over && check_win(&g)) {
                    g.win = 1;
                }
            }
            if (right_pressed && gx >= 0 && gy >= 0 && gx < W && gy < H) {
                cell_t *c = &g.board[gy][gx];
                if (c->state == CELL_HIDDEN) c->state = CELL_FLAGGED;
                else if (c->state == CELL_FLAGGED) c->state = CELL_HIDDEN;
            }
            if (chord_pressed && g.mines_placed && gx >= 0 && gy >= 0 && gx < W && gy < H) {
                chord_reveal(&g, gx, gy);
                if (!g.game_over && check_win(&g)) {
                    g.win = 1;
                }
            }
        }

        prev_buttons = buttons;

        draw_game(bb, pitch, vi.width, vi.height, &g, mx, my, (buttons & 1) != 0);
        if (have_cursor) {
            alpha_blit_cursor_xrgb8888(bb, pitch, vi.width, vi.height, &cursor, mx, my);
        }
        NodGL_DrawTexture(ctx, backbuffer_tex, 0, 0, 0, 0, vi.width, vi.height);
        NodGL_PresentContext(ctx, 0);
        yield();
    }

    NodGL_ReleaseResource(device, backbuffer_tex);
    NodGL_ReleaseDevice(device);
}



