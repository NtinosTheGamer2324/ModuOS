// mddino.c -- Chrome Dino ("T-Rex runner") for ModuOS / mdw.
//
// * Sprites: /appdata/mddino/assets.bmp (Chrome's 1x offline sprite sheet,
//   1233px wide). Loaded once, converted to ARGB. No FNT needed -- the score
//   digits, "HI" and "GAME OVER" all come from the sprite sheet itself.
// * Pure integer math (no float/SSE), 60 Hz fixed timestep, Chrome's own
//   physics constants, collision boxes, gap formula and speed curve.
// * Logical canvas is 600x150 like Chrome. The window is created at that
//   size, but if mdw resizes/maximizes it we remap the buffer and integer-
//   scale + center the game.
//
// Controls: SPACE / UP / W / left click = jump (and restart after Game Over)
//           DOWN / S                    = duck (hold-ish) / fast-fall in air
#include "libc/libc.h"
#include "ilib.h"
#include "wm_protocol.h"

#define SPRITE_PATH "/appdata/mddino/assets.bmp"

#define GW        600
#define GH        150
#define TREX_X    50
#define GROUND_Y  93      /* GH - 47 - 10 */
#define HORIZON_Y 127
#define TICK_HZ   60

/* ---- sprite-sheet coordinates (1x sheet, same as Chrome's LDPI) ---- */
#define SP_TREX_X     848
#define SP_TREX_Y     2
#define SP_PTERO_X    134
#define SP_CLOUD_X    86
#define SP_CACT_S_X   228
#define SP_CACT_L_X   332
#define SP_HORIZ_Y    54
#define SP_TEXT_X     655   /* digits "0123456789HI", 10x13 each */
#define SP_OVER_Y     15    /* "GAME OVER" 191x11 */
#define SP_RESTART_X  2
#define SP_RESTART_Y  2

/* ====================================================================
 *  Tiny helpers
 * ==================================================================== */
static uint32_t rng = 2463534242u;
static uint32_t rnd32(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static int rnd_range(int a, int b) { return a + (int)(rnd32() % (uint32_t)(b - a + 1)); }

/* JS Math.round() on a milli-unit value (v/1000, round half up) */
static int round_m(int v) {
    int n = v + 500;
    return n >= 0 ? n / 1000 : -((-n + 999) / 1000);
}

/* ====================================================================
 *  Sprite sheet (BMP -> ARGB)
 * ==================================================================== */
static uint32_t *spr = NULL;
static int spr_w = 0, spr_h = 0;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static int lum(uint32_t c) { return (int)((((c >> 16) & 255) * 30 + ((c >> 8) & 255) * 59 + (c & 255) * 11) / 100); }

static uint8_t *read_file(const char *path, size_t *out_sz) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return NULL;
    long fsz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (fsz <= 0 || fsz > 32 * 1024 * 1024) { close(fd); return NULL; }
    uint8_t *data = (uint8_t *)malloc((size_t)fsz);
    if (!data) { close(fd); return NULL; }
    size_t got = 0;
    while (got < (size_t)fsz) {
        ssize_t r = read(fd, data + got, (size_t)fsz - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (got != (size_t)fsz) { free(data); return NULL; }
    *out_sz = got;
    return data;
}

/* Supports 1/4/8-bit paletted, 24-bit, 32-bit; top-down or bottom-up. */
static int load_bmp(const char *path) {
    size_t size = 0;
    uint8_t *f = read_file(path, &size);
    if (!f) { printf("mddino: cannot read %s\n", path); return -1; }
    if (size < 54 || f[0] != 'B' || f[1] != 'M') { printf("mddino: not a BMP\n"); free(f); return -1; }

    uint32_t off  = rd32(f + 10);
    uint32_t dib  = rd32(f + 14);
    int32_t  w    = (int32_t)rd32(f + 18);
    int32_t  h    = (int32_t)rd32(f + 22);
    uint32_t bpp  = rd16(f + 28);
    uint32_t comp = rd32(f + 30);
    uint32_t ncol = rd32(f + 46);
    int bottom_up = h > 0;
    if (h < 0) h = -h;

    if (dib < 40 || w <= 0 || h <= 0 || (comp != 0 && comp != 3) ||
        !(bpp == 1 || bpp == 4 || bpp == 8 || bpp == 24 || bpp == 32)) {
        printf("mddino: unsupported BMP (bpp=%u comp=%u)\n", (unsigned)bpp, (unsigned)comp);
        free(f); return -1;
    }
    const uint8_t *pal = f + 14 + dib;
    if (bpp <= 8 && ncol == 0) ncol = 1u << bpp;

    size_t stride = (((size_t)w * bpp + 31) / 32) * 4;
    if ((size_t)off + stride * (size_t)h > size) { printf("mddino: BMP truncated\n"); free(f); return -1; }

    uint32_t *pix = (uint32_t *)malloc((size_t)w * h * 4);
    if (!pix) { free(f); return -1; }

    uint32_t amin = 255, amax = 0;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = f + off + (size_t)(bottom_up ? (h - 1 - y) : y) * stride;
        for (int x = 0; x < w; x++) {
            uint32_t r = 0, g = 0, b = 0, a = 255, idx;
            if (bpp == 32)      { const uint8_t *p = row + x * 4; b = p[0]; g = p[1]; r = p[2]; a = p[3]; }
            else if (bpp == 24) { const uint8_t *p = row + x * 3; b = p[0]; g = p[1]; r = p[2]; }
            else {
                if (bpp == 8)      idx = row[x];
                else if (bpp == 4) idx = (x & 1) ? (row[x >> 1] & 15) : (row[x >> 1] >> 4);
                else               idx = (row[x >> 3] >> (7 - (x & 7))) & 1;
                if (idx < ncol) { const uint8_t *pe = pal + idx * 4; b = pe[0]; g = pe[1]; r = pe[2]; }
            }
            if (bpp == 32) { if (a < amin) amin = a; if (a > amax) amax = a; }
            pix[(size_t)y * w + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    free(f);

    int real_alpha = (bpp == 32 && amax > 0 && amin < 255);
    if (!real_alpha) {
        /* No usable alpha channel -> derive one. */
        uint32_t bg = pix[0] & 0xFFFFFF;
        size_t n = (size_t)w * h;
        if (lum(bg) < 128) {
            /* dark background (e.g. transparency flattened to black): colour-key it */
            for (size_t i = 0; i < n; i++)
                pix[i] = ((pix[i] & 0xFFFFFF) == bg) ? 0u : (0xFF000000u | (pix[i] & 0xFFFFFF));
        } else {
            /* light background: luminance -> coverage, drawn in Chrome's #535353 */
            for (size_t i = 0; i < n; i++) {
                int a = (255 - lum(pix[i])) * 255 / 172;
                if (a > 255) a = 255;
                if (a < 10) a = 0;
                pix[i] = a ? (((uint32_t)a << 24) | 0x535353u) : 0u;
            }
        }
    }
    spr = pix; spr_w = w; spr_h = h;
    printf("mddino: sprite sheet %dx%d, %u-bit%s\n", w, h, (unsigned)bpp, real_alpha ? ", alpha" : "");
    return 0;
}

/* ====================================================================
 *  Back buffer + blit
 * ==================================================================== */
static uint32_t *back = NULL;   /* GW*GH logical canvas */

static void blit(int sx, int sy, int sw, int sh, int dx, int dy) {
    int x0 = 0, y0 = 0, x1 = sw, y1 = sh;
    if (dx < 0) x0 = -dx;
    if (dy < 0) y0 = -dy;
    if (dx + x1 > GW) x1 = GW - dx;
    if (dy + y1 > GH) y1 = GH - dy;
    if (sx + x1 > spr_w) x1 = spr_w - sx;
    if (sy + y1 > spr_h) y1 = spr_h - sy;
    for (int y = y0; y < y1; y++) {
        const uint32_t *sp = spr + (size_t)(sy + y) * spr_w + sx;
        uint32_t *dp = back + (size_t)(dy + y) * GW + dx;
        for (int x = x0; x < x1; x++) {
            uint32_t p = sp[x];
            uint32_t a = p >> 24;
            if (a == 255) { dp[x] = p; continue; }
            if (!a) continue;
            uint32_t dd = dp[x];
            uint32_t r = (((p >> 16) & 255) * a + ((dd >> 16) & 255) * (255 - a)) / 255;
            uint32_t g = (((p >> 8) & 255) * a + ((dd >> 8) & 255) * (255 - a)) / 255;
            uint32_t b = ((p & 255) * a + (dd & 255) * (255 - a)) / 255;
            dp[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

/* ====================================================================
 *  Game state
 * ==================================================================== */
enum { ST_WAITING, ST_RUNNING, ST_CRASHED };
static int state = ST_WAITING;
static int dirty = 1;

static int  speed_m = 6000;         /* px/tick * 1000 */
static int64_t distance_m = 0;      /* px * 1000      */
static int  run_ticks = 0, g_tick = 0, crashed_ticks = 0;
static int  hi_score = 0;

/* trex */
static int t_y = GROUND_Y, t_v = 0;     /* t_v in milli-px/tick */
static int t_jumping = 0, t_drop = 0, t_reached = 0, t_ducking = 0;
static int t_duck_until = 0, t_anim = 0;

/* horizon */
static int hz_off = 0, hz_src0 = 2, hz_src1 = 2;

/* clouds */
typedef struct { int x, y, gap; } cloud_t;
static cloud_t clouds[6];
static int n_clouds = 0;

/* obstacles: type 0 = small cactus, 1 = large cactus, 2 = pterodactyl */
typedef struct { int type, size, x, y, w, h, gap, off_m, frame, ftick; } obst_t;
#define MAX_OBS 6
static obst_t obs[MAX_OBS];
static int n_obs = 0;

/* ---- collision boxes (straight from Chrome) ---- */
typedef struct { int x, y, w, h; } box_t;
static const box_t TREX_RUN[6]  = {{22,0,17,16},{1,18,30,9},{10,35,14,8},{1,24,29,5},{5,30,21,4},{9,34,15,4}};
static const box_t TREX_DUCK[1] = {{1,18,55,25}};
static const box_t CACT_S[3]    = {{0,7,5,27},{4,0,6,34},{10,4,7,14}};
static const box_t CACT_L[3]    = {{0,12,7,38},{8,0,7,49},{13,10,10,38}};
static const box_t PTERO_B[5]   = {{15,15,16,5},{18,21,24,6},{2,14,4,3},{6,10,4,7},{10,8,6,9}};

static int boxhit(box_t a, box_t b) {
    return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.h + a.y > b.y;
}

static int check_collision(const obst_t *o) {
    int dw = t_ducking ? 59 : 44;
    box_t tb = { TREX_X + 1, t_y + 1, dw - 2, 47 - 2 };
    box_t ob = { o->x + 1, o->y + 1, o->w - 2, o->h - 2 };
    if (!boxhit(tb, ob)) return 0;

    const box_t *tbx = t_ducking ? TREX_DUCK : TREX_RUN;
    int tn = t_ducking ? 1 : 6;
    box_t obx[5]; int on;
    if (o->type == 2)      { on = 5; for (int i = 0; i < 5; i++) obx[i] = PTERO_B[i]; }
    else {
        on = 3;
        const box_t *src = (o->type == 0) ? CACT_S : CACT_L;
        for (int i = 0; i < 3; i++) obx[i] = src[i];
        if (o->size > 1) {
            obx[1].w = o->w - obx[0].w - obx[2].w;
            obx[2].x = o->w - obx[2].w;
        }
    }
    for (int t = 0; t < tn; t++) {
        box_t at = { tbx[t].x + tb.x, tbx[t].y + tb.y, tbx[t].w, tbx[t].h };
        for (int i = 0; i < on; i++) {
            box_t ao = { obx[i].x + ob.x, obx[i].y + ob.y, obx[i].w, obx[i].h };
            if (boxhit(at, ao)) return 1;
        }
    }
    return 0;
}

/* ====================================================================
 *  Game logic
 * ==================================================================== */
static void add_cloud(void) {
    if (n_clouds >= 6) return;
    cloud_t c = { GW, rnd_range(30, 71), rnd_range(100, 400) };
    clouds[n_clouds++] = c;
}

static void add_obstacle(void) {
    if (n_obs >= MAX_OBS) return;
    static const int ptero_y[3] = { 100, 75, 50 };
    obst_t o; memset(&o, 0, sizeof(o));
    int t = rnd_range(0, 2);
    if (t == 2 && speed_m < 8500) t = rnd_range(0, 1);
    o.type = t;
    if (t == 2) {
        o.size = 1; o.w = 46; o.h = 40;
        o.y = ptero_y[rnd_range(0, 2)];
        o.off_m = (rnd32() & 1) ? 800 : -800;
    } else {
        int bw = (t == 0) ? 17 : 25;
        int multi = (t == 0) ? 4000 : 7000;
        o.size = rnd_range(1, 3);
        if (o.size > 1 && speed_m < multi) o.size = 1;
        o.w = bw * o.size;
        o.h = (t == 0) ? 35 : 50;
        o.y = (t == 0) ? 105 : 90;
    }
    o.x = GW - o.w;
    int base = (t == 2) ? 150 : 120;
    int mg = (o.w * speed_m) / 1000 + (base * 6) / 10;   /* gap coefficient 0.6 */
    o.gap = rnd_range(mg, mg * 3 / 2);
    obs[n_obs++] = o;
}

static void start_jump(void) {
    t_jumping = 1; t_drop = 0; t_reached = 0; t_ducking = 0;
    t_v = -10000 - speed_m / 10;
}

static void reset_game(void) {
    speed_m = 6000; distance_m = 0; run_ticks = 0; crashed_ticks = 0;
    t_y = GROUND_Y; t_v = 0; t_jumping = t_drop = t_reached = t_ducking = 0;
    t_duck_until = 0; t_anim = 0;
    n_obs = 0; n_clouds = 0; add_cloud();
    state = ST_RUNNING; dirty = 1;
}

static int score_now(void) { return (int)(distance_m / 40000); }

static void do_jump(void) {
    if (state == ST_WAITING) { reset_game(); start_jump(); }
    else if (state == ST_RUNNING) { if (!t_jumping) start_jump(); }
    else if (state == ST_CRASHED) { if (crashed_ticks >= 72) reset_game(); } /* ~1.2s lockout */
    dirty = 1;
}

static void do_duck(void) {
    if (state != ST_RUNNING) return;
    if (t_jumping) t_drop = 1;
    else t_duck_until = g_tick + 30;
}

static void step(void) {
    if (state == ST_CRASHED) { crashed_ticks++; return; }
    if (state != ST_RUNNING) return;
    g_tick++; run_ticks++; t_anim++;

    if (speed_m < 13000) speed_m += 1;     /* acceleration 0.001/tick */
    distance_m += speed_m;

    /* trex */
    if (t_jumping) {
        t_y += round_m(t_drop ? t_v * 3 : t_v);
        t_v += 600;
        if (t_y < 63 || t_drop) t_reached = 1;
        if (t_y < 30 || t_drop) { if (t_reached && t_v < -5000) t_v = -5000; }
        if (t_y > GROUND_Y) { t_y = GROUND_Y; t_v = 0; t_jumping = 0; t_drop = 0; }
    }
    t_ducking = (!t_jumping && g_tick < t_duck_until);

    /* horizon */
    hz_off += speed_m / 1000;
    if (hz_off >= GW) { hz_off -= GW; hz_src0 = hz_src1; hz_src1 = (rnd32() & 1) ? 602 : 2; }

    /* clouds */
    for (int i = 0; i < n_clouds; i++) clouds[i].x -= 1;
    while (n_clouds > 0 && clouds[0].x + 46 <= 0) {
        for (int i = 1; i < n_clouds; i++) clouds[i - 1] = clouds[i];
        n_clouds--;
    }
    if (n_clouds < 6 && (n_clouds == 0 || (GW - clouds[n_clouds - 1].x) > clouds[n_clouds - 1].gap) && (rnd32() & 1))
        add_cloud();

    /* obstacles */
    for (int i = 0; i < n_obs; i++) {
        obs[i].x -= (speed_m + obs[i].off_m) / 1000;
        if (obs[i].type == 2 && ++obs[i].ftick >= 10) { obs[i].ftick = 0; obs[i].frame ^= 1; }
    }
    while (n_obs > 0 && obs[0].x + obs[0].w <= 0) {
        for (int i = 1; i < n_obs; i++) obs[i - 1] = obs[i];
        n_obs--;
    }
    if (run_ticks > 180) {                  /* 3s clear time */
        if (n_obs == 0) add_obstacle();
        else {
            obst_t *l = &obs[n_obs - 1];
            if (l->x + l->w + l->gap < GW) add_obstacle();
        }
    }

    /* collision: only the nearest obstacles matter */
    for (int i = 0; i < n_obs; i++) {
        if (obs[i].x > TREX_X + 60) break;
        if (check_collision(&obs[i])) {
            state = ST_CRASHED; crashed_ticks = 0;
            if (score_now() > hi_score) hi_score = score_now();
            dirty = 1;
            break;
        }
    }
}

/* ====================================================================
 *  Rendering
 * ==================================================================== */
static void draw_digit(int d, int dx, int dy) { blit(SP_TEXT_X + d * 10, 2, 10, 13, dx, dy); }

static void draw_number5(int v, int x, int y) {
    v %= 100000;
    for (int i = 4; i >= 0; i--) { draw_digit(v % 10, x + i * 11, y); v /= 10; }
}

static void render(void) {
    for (int i = 0; i < GW * GH; i++) back[i] = 0xFFFFFFFFu;

    for (int i = 0; i < n_clouds; i++) blit(SP_CLOUD_X, 2, 46, 14, clouds[i].x, clouds[i].y);

    blit(hz_src0, SP_HORIZ_Y, GW, 12, -hz_off, HORIZON_Y);
    blit(hz_src1, SP_HORIZ_Y, GW, 12, GW - hz_off, HORIZON_Y);

    for (int i = 0; i < n_obs; i++) {
        obst_t *o = &obs[i];
        if (o->type == 2)
            blit(SP_PTERO_X + o->frame * 46, 2, 46, 40, o->x, o->y);
        else if (o->type == 0)
            blit(SP_CACT_S_X + (17 * o->size * (o->size - 1)) / 2, 2, o->w, 35, o->x, o->y);
        else
            blit(SP_CACT_L_X + (25 * o->size * (o->size - 1)) / 2, 2, o->w, 50, o->x, o->y);
    }

    /* trex */
    {
        int off, w = 44;
        if (state == ST_CRASHED)       off = 220;
        else if (state == ST_WAITING)  off = 0;
        else if (t_jumping)            off = 0;
        else if (t_ducking)            { w = 59; off = ((t_anim / 8) & 1) ? 323 : 264; }
        else                           off = ((t_anim / 5) & 1) ? 132 : 88;
        blit(SP_TREX_X + off, SP_TREX_Y, w, 47, TREX_X, t_y);
    }

    /* score + hi score */
    draw_number5(score_now(), GW - 11 * 6, 5);
    if (hi_score > 0) {
        int hx = GW - 11 * 6 - 110;
        draw_digit(10, hx, 5); draw_digit(11, hx + 11, 5);
        draw_number5(hi_score, hx + 33, 5);
    }

    if (state == ST_CRASHED) {
        blit(SP_TEXT_X, SP_OVER_Y, 191, 11, (GW - 191) / 2, (GH - 25) / 3);
        blit(SP_RESTART_X, SP_RESTART_Y, 36, 32, (GW - 36) / 2, GH / 2);
    }
}

/* ====================================================================
 *  Window / mdw plumbing
 * ==================================================================== */
static wm_shm_t *mb = NULL;
static int       win_id = -1;
static uint32_t *buf = NULL;
static int       bw = GW, bh = GH;
static uint64_t  cur_size = 0;
static char      cur_name[64];
static int       layout_dirty = 1;

/* ---- profiling (set DINO_PROFILE 0 to silence) ---- */
#define DINO_PROFILE   1
/* Minimum ms between presents. 33 = 30 fps, 16 = ~60 fps. If mdw can't keep
 * up, a lower present rate is smoother than a stalled one. */
#define PRESENT_MIN_MS 33
static uint64_t prof_render = 0, prof_copy = 0, prof_dmg = 0;
static unsigned prof_frames = 0;

static void present(void) {
    uint64_t pa = time_ms();
    int s = bw / GW, s2 = bh / GH;
    if (s2 < s) s = s2;
    if (s < 1) s = 1;
    int ox = (bw - GW * s) / 2, oy = (bh - GH * s) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;

    if (layout_dirty) {
        for (size_t i = 0; i < (size_t)bw * bh; i++) buf[i] = 0xFFFFFFFFu;
        layout_dirty = 0;
    }

    int cw = GW * s; if (ox + cw > bw) cw = bw - ox;
    int ch = GH * s; if (oy + ch > bh) ch = bh - oy;

    for (int y = 0; y < GH; y++) {
        const uint32_t *srow = back + (size_t)y * GW;
        for (int ry = 0; ry < s; ry++) {
            int dy = oy + y * s + ry;
            if (dy >= bh) break;
            uint32_t *drow = buf + (size_t)dy * bw + ox;
            if (s == 1) {
                memcpy(drow, srow, (size_t)cw * 4);
            } else if (ry == 0) {
                for (int x = 0; x < GW; x++) {
                    uint32_t c = srow[x];
                    for (int rx = 0; rx < s; rx++) {
                        int dx = x * s + rx;
                        if (dx < cw) drow[dx] = c;
                    }
                }
            } else {
                memcpy(drow, buf + (size_t)(dy - 1) * bw + ox, (size_t)cw * 4);
            }
        }
    }
    uint64_t pb = time_ms();
    wm_damage(mb, win_id, ox, oy, cw, ch);
    prof_copy += pb - pa;
    prof_dmg  += time_ms() - pb;
}

/* Returns 0 if our window no longer exists, 1 otherwise. Sets *minimized. */
static int poll_window(int *minimized) {
    wm_win_info_t list[WM_MAX_WINDOWS];
    int n = wm_list_windows(mb, list);
    if (n < 0) return 1; /* timeout -- try again later */
    for (int i = 0; i < n; i++) {
        if (list[i].win_id != win_id) continue;
        *minimized = (list[i].state == WM_STATE_MINIMIZED);
        if (strcmp(list[i].shm_name, cur_name) != 0) {
            int fd = shm_open(list[i].shm_name, O_RDWR, 0, 0);
            if (fd >= 0) {
                uint32_t *nb = (uint32_t *)mmap(NULL, (size_t)list[i].shm_size, PROT_R | PROT_W, MAP_SHARED, fd);
                if (nb != MAP_FAILED) {
                    munmap(buf, (size_t)cur_size);
                    buf = nb; cur_size = list[i].shm_size;
                    bw = list[i].w; bh = list[i].h;
                    memcpy(cur_name, list[i].shm_name, 64);
                    layout_dirty = 1; dirty = 1;
                }
            }
        }
        return 1;
    }
    return 0;
}

/* ---- Keyboard adapter: the ONLY place that touches KeyboardEventData ---- */
enum { ACT_NONE = 0, ACT_JUMP = 1, ACT_DUCK = 2 };
static int key_to_action(const KeyboardEventData *k) {
    if (k->keycode == KEY_SPACE || k->keycode == KEY_ARROW_UP || k->keycode == KEY_ENTER ||
        k->ascii == ' ' || k->ascii == 'w' || k->ascii == 'W')
        return ACT_JUMP;
    if (k->keycode == KEY_ARROW_DOWN || k->ascii == 's' || k->ascii == 'S')
        return ACT_DUCK;
    return ACT_NONE;
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    if (load_bmp(SPRITE_PATH) != 0) return 1;

    back = (uint32_t *)malloc((size_t)GW * GH * 4);
    if (!back) { puts("mddino: out of memory\n"); return 1; }

    mb = wm_ipc_connect(3000);
    if (!mb) { puts("mddino: mdw never came up\n"); return 1; }

    wm_request_t req; memset(&req, 0, sizeof(req));
    req.cmd = WM_CMD_CREATE;
    req.w = GW; req.h = GH;
    req.flags = 0;
    req.pid = (uint32_t)getpid();
    { const char *t = "Dino"; int i = 0; while (t[i] && i < 63) { req.title[i] = t[i]; i++; } req.title[i] = 0; }

    wm_response_t resp;
    if (wm_ipc_call(mb, &req, &resp, 3000) != 0 || !resp.success) {
        printf("mddino: create failed: %s\n", resp.message);
        return 1;
    }
    win_id = resp.win_id;

    int fd = shm_open(resp.shm_name, O_RDWR, 0, 0);
    if (fd < 0) { puts("mddino: shm_open failed\n"); return 1; }
    buf = (uint32_t *)mmap(NULL, (size_t)resp.shm_size, PROT_R | PROT_W, MAP_SHARED, fd);
    if (buf == MAP_FAILED) { puts("mddino: mmap failed\n"); return 1; }
    cur_size = resp.shm_size;
    memcpy(cur_name, resp.shm_name, 64);

    wm_key_ring_t *keys = NULL;
    int kfd = shm_open(resp.key_shm_name, O_RDWR, 0, 0);
    if (kfd >= 0) {
        void *p = mmap(NULL, (size_t)resp.key_shm_size, PROT_R | PROT_W, MAP_SHARED, kfd);
        if (p != MAP_FAILED) keys = (wm_key_ring_t *)p;
    }
    wm_mouse_ring_t *mice = NULL;
    int mfd = shm_open(resp.mouse_shm_name, O_RDWR, 0, 0);
    if (mfd >= 0) {
        void *p = mmap(NULL, (size_t)resp.mouse_shm_size, PROT_R | PROT_W, MAP_SHARED, mfd);
        if (p != MAP_FAILED) mice = (wm_mouse_ring_t *)p;
    }

    rng ^= (uint32_t)time_ms() | 1u;
    state = ST_WAITING;
    n_clouds = 0; add_cloud();

    uint64_t last = time_ms(), next_poll = last + 500;
    int acc = 0, paused = 0;
    uint64_t last_present = 0, prof_t0 = last;

    for (;;) {
        uint64_t now = time_ms();
        uint64_t dt = now - last;
        last = now;
        if (dt > 250) dt = 250;

        /* ---- input ---- */
        if (keys) {
            KeyboardEventData k;
            while (wm_key_ring_pop(keys, &k)) {
                int a = key_to_action(&k);
                if (a == ACT_JUMP) do_jump();
                else if (a == ACT_DUCK) do_duck();
            }
        }
        if (mice) {
            wm_mouse_event_t me;
            while (wm_mouse_ring_pop(mice, &me))
                if (me.type == WM_MOUSE_DOWN && (me.changed & WM_MBTN_LEFT)) do_jump();
        }

        /* ---- window state (resize remap / minimized / closed) ---- */
        if (now >= next_poll) {
            int minimized = 0;
            if (!poll_window(&minimized)) break;          /* window was closed */
            paused = minimized;
            next_poll = now + (paused ? 200 : 500);
        }
        if (paused) { acc = 0; yield(); continue; }

        /* ---- fixed 60 Hz simulation ---- */
        acc += (int)dt * TICK_HZ;
        int ticks = 0;
        while (acc >= 1000 && ticks < 6) { acc -= 1000; step(); ticks++; }
        if (acc >= 1000) acc = 0;
        if (ticks > 0 && state == ST_RUNNING) dirty = 1;

        if (dirty && (now - last_present) >= PRESENT_MIN_MS) {
            uint64_t r0 = time_ms();
            render();
            prof_render += time_ms() - r0;
            present();
            prof_frames++;
            dirty = 0;
            last_present = now;
        } else yield();

#if DINO_PROFILE
        if (now - prof_t0 >= 1000) {
            printf("dino: %u fps | per-second totals ms: render=%u copy=%u damage=%u\n",
                   prof_frames, (unsigned)prof_render, (unsigned)prof_copy, (unsigned)prof_dmg);
            prof_frames = 0; prof_render = prof_copy = prof_dmg = 0; prof_t0 = now;
        }
#endif
    }

    return 0;
}