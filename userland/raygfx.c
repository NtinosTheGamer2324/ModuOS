#include "libc.h"
#include "string.h"
#include "../include/moduos/kernel/events/events.h"

/*
 * raygfx.sqr
 *
 * Userland port of the kernel RaycasterFPS, but rendered with real graphics via
 * $/dev/graphics/video0 (MD64API GRP) and controlled via $/dev/input/event0.
 *
 * Controls:
 *   W/A/S/D: move
 *   Left/Right arrows: turn
 *   ESC: quit
 */

#define MAP_W 24
#define MAP_H 24

/* Internal render resolution (then scaled to full framebuffer) */
#define R_W 320
#define R_H 200

#define FOV_DEG 60
#define MAX_DEPTH_TENTHS (20*10)

/* Speeds tuned for ~60fps timestep */
#define MOVE_SPEED_TENTHS 18
#define ROT_SPEED_DEG 3

/* Map from kernel RaycasterFPS (1=wall,2=door,0=empty) */
static int g_map[MAP_H][MAP_W] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,1},
    {1,0,1,1,1,0,1,0,1,0,1,1,1,0,1,0,1,1,1,0,1,1,0,1},
    {1,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,1,0,1},
    {1,0,1,0,2,0,1,1,1,0,1,0,1,1,1,0,2,0,1,0,2,1,0,1},
    {1,0,0,0,0,0,0,0,1,0,0,0,0,0,1,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,0,1,1,0,1,1,1,1,0,1,1,1,0,1,1,1,0,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,1,1,1,1,0,1,1,1,1,1,1,1,0,1,1,1,1,1,0,1,1,1},
    {1,0,1,0,0,0,0,1,0,0,0,0,0,1,0,0,0,0,0,1,0,0,0,1},
    {1,0,1,0,2,1,0,1,0,2,0,2,0,1,0,1,2,0,1,0,2,1,0,1},
    {1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,0,1,1,1,0,1,1,1,1,1,1,0,1,1,1,0,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,1,1,1,0,1,0,1,1,1,1,1,1,1,0,1,0,1,1,1,0,1,1},
    {1,0,1,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1},
    {1,0,1,0,2,0,1,1,1,0,1,0,1,1,1,0,1,2,1,0,2,1,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,1,1,1,1,0,1,1,1,1,1,1,1,1,0,1,1,1,1,0,1,1,1},
    {1,0,1,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1},
    {1,0,1,0,2,1,0,1,0,2,0,2,0,1,0,1,2,0,1,0,2,1,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}
};

/* Kernel's sine table (scaled by 100) */
static const int sin_table[361] = {
    0, 2, 3, 5, 7, 9, 10, 12, 14, 16, 17, 19, 21, 22, 24, 26, 28, 29, 31, 33,
    34, 36, 37, 39, 41, 42, 44, 45, 47, 48, 50, 52, 53, 54, 56, 57, 59, 60, 62, 63,
    64, 66, 67, 68, 69, 71, 72, 73, 74, 75, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86,
    87, 87, 88, 89, 90, 91, 91, 92, 93, 93, 94, 95, 95, 96, 96, 97, 97, 97, 98, 98,
    98, 99, 99, 99, 99, 99, 100, 100, 100, 100, 100, 100, 100, 99, 99, 99, 99, 99, 98, 98,
    98, 97, 97, 97, 96, 96, 95, 95, 94, 93, 93, 92, 91, 91, 90, 89, 88, 87, 87, 86,
    85, 84, 83, 82, 81, 80, 79, 78, 77, 75, 74, 73, 72, 71, 69, 68, 67, 66, 64, 63,
    62, 60, 59, 57, 56, 54, 53, 52, 50, 48, 47, 45, 44, 42, 41, 39, 37, 36, 34, 33,
    31, 29, 28, 26, 24, 22, 21, 19, 17, 16, 14, 12, 10, 9, 7, 5, 3, 2, 0, -2,
    -3, -5, -7, -9, -10, -12, -14, -16, -17, -19, -21, -22, -24, -26, -28, -29, -31, -33, -34, -36,
    -37, -39, -41, -42, -44, -45, -47, -48, -50, -52, -53, -54, -56, -57, -59, -60, -62, -63, -64, -66,
    -67, -68, -69, -71, -72, -73, -74, -75, -77, -78, -79, -80, -81, -82, -83, -84, -85, -86, -87, -87,
    -88, -89, -90, -91, -91, -92, -93, -93, -94, -95, -95, -96, -96, -97, -97, -97, -98, -98, -98, -99,
    -99, -99, -99, -99, -100, -100, -100, -100, -100, -100, -100, -99, -99, -99, -99, -99, -98, -98, -98, -97,
    -97, -97, -96, -96, -95, -95, -94, -93, -93, -92, -91, -91, -90, -89, -88, -87, -87, -86, -85, -84,
    -83, -82, -81, -80, -79, -78, -77, -75, -74, -73, -72, -71, -69, -68, -67, -66, -64, -63, -62, -60,
    -59, -57, -56, -54, -53, -52, -50, -48, -47, -45, -44, -42, -41, -39, -37, -36, -34, -33, -31, -29,
    -28, -26, -24, -22, -21, -19, -17, -16, -14, -12, -10, -9, -7, -5, -3, -2, 0
};

static int isin(int a){ while(a<0)a+=360; while(a>=360)a-=360; return sin_table[a]; }
static int icos(int a){ return isin(a+90); }

typedef struct {
    int x; /* scaled by 100 */
    int y;
    int angle;
} Player;

typedef struct {
    md64api_grp_video_info_t vi;

    /* Render target: userland backbuffer (tightly packed). */
    uint8_t *bb;
    uint32_t bb_pitch;

    uint32_t fmt;
    uint32_t scale;
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

static void fb_put_px(Gfx *g, int x, int y, uint32_t c) {
    if (!g || !g->bb) return;
    if ((unsigned)x >= R_W || (unsigned)y >= R_H) return;
    uint32_t sx = g->off_x + (uint32_t)x * g->scale;
    uint32_t sy = g->off_y + (uint32_t)y * g->scale;

    /* Guard: the scaled output must still be in the framebuffer bounds. */
    if (sx >= g->vi.width || sy >= g->vi.height) return;

    if (g->fmt == MD64API_GRP_FMT_XRGB8888) {
        for (uint32_t yy=0; yy<g->scale && (sy + yy) < g->vi.height; yy++) {
            uint32_t *r = (uint32_t*)(g->bb + (uint64_t)(sy + yy) * g->bb_pitch);
            for (uint32_t xx=0; xx<g->scale && (sx + xx) < g->vi.width; xx++) r[sx + xx] = c;
        }
    } else {
        uint16_t cc = (uint16_t)c;
        for (uint32_t yy=0; yy<g->scale && (sy + yy) < g->vi.height; yy++) {
            uint16_t *r = (uint16_t*)(g->bb + (uint64_t)(sy + yy) * g->bb_pitch);
            for (uint32_t xx=0; xx<g->scale && (sx + xx) < g->vi.width; xx++) r[sx + xx] = cc;
        }
    }
}

static void fb_clear(Gfx *g, uint32_t c) {
    if (!g || !g->bb) return;

    if (g->fmt == MD64API_GRP_FMT_XRGB8888) {
        for (uint32_t y=0; y<g->vi.height; y++) {
            uint32_t *row = (uint32_t*)(g->bb + (uint64_t)y * g->bb_pitch);
            for (uint32_t x=0; x<g->vi.width; x++) row[x] = c;
        }
    } else {
        uint16_t cc = (uint16_t)c;
        for (uint32_t y=0; y<g->vi.height; y++) {
            uint16_t *row = (uint16_t*)(g->bb + (uint64_t)y * g->bb_pitch);
            for (uint32_t x=0; x<g->vi.width; x++) row[x] = cc;
        }
    }
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
          (g->fmt == MD64API_GRP_FMT_RGB565 && g->vi.bpp == 16))) return -3;

    /* allocate a tightly-packed backbuffer (presented via gfx_blit) */
    uint32_t bpp_bytes = (g->fmt == MD64API_GRP_FMT_RGB565) ? 2u : 4u;
    g->bb_pitch = g->vi.width * bpp_bytes;
    uint32_t buf_size = g->bb_pitch * g->vi.height;
    g->bb = (uint8_t*)malloc(buf_size);
    if (!g->bb) return -4;

    /* choose integer scale */
    uint32_t s1 = g->vi.width / R_W;
    uint32_t s2 = g->vi.height / R_H;
    g->scale = (s1 < s2) ? s1 : s2;
    if (g->scale < 1) g->scale = 1;

    uint32_t out_w = R_W * g->scale;
    uint32_t out_h = R_H * g->scale;
    g->off_x = (g->vi.width  > out_w) ? (g->vi.width  - out_w)/2 : 0;
    g->off_y = (g->vi.height > out_h) ? (g->vi.height - out_h)/2 : 0;
    return 0;
}

/* Raycast like kernel (distance in 1/10th units) */
static int cast_ray(int px, int py, int angle, int *hit_type) {
    int dx = icos(angle);
    int dy = isin(angle);

    for (int dist = 0; dist < MAX_DEPTH_TENTHS; dist += 2) {
        int test_x = (px + (dx * dist / 100)) / 100;
        int test_y = (py + (dy * dist / 100)) / 100;

        if (test_x < 0 || test_x >= MAP_W || test_y < 0 || test_y >= MAP_H) {
            *hit_type = 1;
            return dist;
        }
        if (g_map[test_y][test_x] != 0) {
            *hit_type = g_map[test_y][test_x];
            return dist;
        }
    }

    *hit_type = 0;
    return MAX_DEPTH_TENTHS;
}

static uint32_t wall_color(Gfx *g, int hit_type, int dist) {
    /* simple shading by dist */
    int shade = 255 - (dist * 255 / (MAX_DEPTH_TENTHS + 1));
    if (shade < 20) shade = 20;

    uint8_t r=shade, gr=shade, b=shade;
    if (hit_type == 2) { r = shade; gr = (uint8_t)(shade/2); b = 0; }

    if (g->fmt == MD64API_GRP_FMT_RGB565) return (uint32_t)rgb565(r, gr, b);
    return xrgb(r, gr, b);
}

static void hud_put_digit(Gfx *g, int x, int y, char ch, uint32_t fg) {
    /* tiny 3x5 font for digits and a few letters, drawn in internal pixels */
    static const uint8_t dig[10][5] = {
        {0x7,0x5,0x5,0x5,0x7}, /*0*/
        {0x2,0x6,0x2,0x2,0x7}, /*1*/
        {0x7,0x1,0x7,0x4,0x7}, /*2*/
        {0x7,0x1,0x7,0x1,0x7}, /*3*/
        {0x5,0x5,0x7,0x1,0x1}, /*4*/
        {0x7,0x4,0x7,0x1,0x7}, /*5*/
        {0x7,0x4,0x7,0x5,0x7}, /*6*/
        {0x7,0x1,0x1,0x1,0x1}, /*7*/
        {0x7,0x5,0x7,0x5,0x7}, /*8*/
        {0x7,0x5,0x7,0x1,0x7}, /*9*/
    };
    uint8_t rows[5] = {0,0,0,0,0};

    if (ch >= '0' && ch <= '9') {
        for (int i=0;i<5;i++) rows[i] = dig[ch-'0'][i];
    } else if (ch == 'F') {
        rows[0]=0x7; rows[1]=0x4; rows[2]=0x7; rows[3]=0x4; rows[4]=0x4;
    } else if (ch == 'S') {
        rows[0]=0x7; rows[1]=0x4; rows[2]=0x7; rows[3]=0x1; rows[4]=0x7;
    } else if (ch == 'P') { /* crude P */
        rows[0]=0x7; rows[1]=0x5; rows[2]=0x7; rows[3]=0x4; rows[4]=0x4;
    } else if (ch == 'A') {
        rows[0]=0x2; rows[1]=0x5; rows[2]=0x7; rows[3]=0x5; rows[4]=0x5;
    } else if (ch == 'X') {
        rows[0]=0x5; rows[1]=0x5; rows[2]=0x2; rows[3]=0x5; rows[4]=0x5;
    } else if (ch == 'Y') {
        rows[0]=0x5; rows[1]=0x5; rows[2]=0x2; rows[3]=0x2; rows[4]=0x2;
    } else if (ch == ':') {
        rows[0]=0x0; rows[1]=0x2; rows[2]=0x0; rows[3]=0x2; rows[4]=0x0;
    } else if (ch == ' ') {
        return;
    } else {
        /* unknown: small box */
        rows[0]=0x7; rows[1]=0x1; rows[2]=0x1; rows[3]=0x1; rows[4]=0x7;
    }

    for (int yy=0; yy<5; yy++) {
        for (int xx=0; xx<3; xx++) {
            if (rows[yy] & (1<<(2-xx))) {
                fb_put_px(g, x+xx, y+yy, fg);
            }
        }
    }
}

static void hud_put_int(Gfx *g, int x, int y, int v, uint32_t fg) {
    char buf[16];
    int n = 0;
    if (v == 0) {
        buf[n++] = '0';
    } else {
        int sign = (v < 0);
        unsigned u = (unsigned)(sign ? -v : v);
        char tmp[16];
        int m=0;
        while (u && m < 15) { tmp[m++] = (char)('0' + (u % 10)); u /= 10; }
        if (sign) buf[n++] = '-';
        while (m--) buf[n++] = tmp[m];
    }
    for (int i=0; i<n; i++) {
        hud_put_digit(g, x + i*4, y, buf[i], fg);
    }
}

static int gfx_present(Gfx *g) {
    if (!g || !g->bb) return -1;
    return gfx_blit(g->bb,
                    (uint16_t)g->vi.width, (uint16_t)g->vi.height,
                    0, 0,
                    (uint16_t)g->bb_pitch,
                    (uint16_t)g->fmt);
}

static void render_frame(Gfx *gfx, const Player *p, int fps) {
    uint32_t sky = (gfx->fmt == MD64API_GRP_FMT_RGB565) ? (uint32_t)rgb565(80,140,220) : xrgb(80,140,220);
    uint32_t floorc = (gfx->fmt == MD64API_GRP_FMT_RGB565) ? (uint32_t)rgb565(30,30,35) : xrgb(30,30,35);

    fb_clear(gfx, (gfx->fmt == MD64API_GRP_FMT_RGB565) ? (uint32_t)rgb565(0,0,0) : xrgb(0,0,0));

    /* draw sky/floor */
    for (int y=0; y<R_H/2; y++) {
        for (int x=0; x<R_W; x++) fb_put_px(gfx, x, y, sky);
    }
    for (int y=R_H/2; y<R_H; y++) {
        for (int x=0; x<R_W; x++) fb_put_px(gfx, x, y, floorc);
    }

    int angle_step_tenths = (FOV_DEG * 10) / R_W;
    int start_angle = p->angle - (FOV_DEG / 2);

    for (int x=0; x<R_W; x++) {
        int ray_angle = start_angle + (x * angle_step_tenths / 10);
        while (ray_angle < 0) ray_angle += 360;
        while (ray_angle >= 360) ray_angle -= 360;

        int hit_type;
        int dist = cast_ray(p->x, p->y, ray_angle, &hit_type);

        /* avoid div by 0 and fish-eye a bit */
        int d = dist + 10;
        int wall_h = (R_H * 120) / d;
        if (wall_h > R_H) wall_h = R_H;

        int top = (R_H/2) - (wall_h/2);
        int bot = top + wall_h;
        if (top < 0) top = 0;
        if (bot > R_H) bot = R_H;

        uint32_t wc = wall_color(gfx, hit_type, dist);
        for (int y=top; y<bot; y++) {
            fb_put_px(gfx, x, y, wc);
        }
    }

    /* HUD (top-left, in internal pixels) */
    uint32_t fg = (gfx->fmt == MD64API_GRP_FMT_RGB565) ? (uint32_t)rgb565(255,255,255) : xrgb(255,255,255);
    uint32_t fg2 = (gfx->fmt == MD64API_GRP_FMT_RGB565) ? (uint32_t)rgb565(255,220,80) : xrgb(255,220,80);

    hud_put_digit(gfx, 2, 2, 'F', fg);
    hud_put_digit(gfx, 6, 2, 'P', fg);
    hud_put_digit(gfx,10, 2, 'S', fg);
    hud_put_digit(gfx,14, 2, ':', fg);
    hud_put_int(gfx, 18, 2, fps, fg2);

    hud_put_digit(gfx, 2, 10, 'X', fg);
    hud_put_digit(gfx, 6, 10, ':', fg);
    hud_put_int(gfx, 10, 10, p->x/100, fg2);

    hud_put_digit(gfx, 2, 18, 'Y', fg);
    hud_put_digit(gfx, 6, 18, ':', fg);
    hud_put_int(gfx, 10, 18, p->y/100, fg2);

    hud_put_digit(gfx, 2, 26, 'A', fg);
    hud_put_digit(gfx, 6, 26, ':', fg);
    hud_put_int(gfx, 10, 26, p->angle, fg2);
}

static int evt_open(void) {
    return open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
}

typedef struct {
    int quit;
} Input;

/*
 * ASCII version behavior: act on KEY_PRESSED events only.
 * This avoids relying on key-up events / held state.
 */
static int input_handle(Player *p, Input *in, const Event *e) {
    if (!p || !in || !e) return 0;
    if (e->type != EVENT_KEY_PRESSED) return 0;

    KeyCode kc = e->data.keyboard.keycode;
    char c = e->data.keyboard.ascii;

    if (kc == KEY_ESCAPE) { in->quit = 1; return 0; }

    /* Rotation: arrows */
    if (kc == KEY_ARROW_LEFT) {
        p->angle -= ROT_SPEED_DEG;
        if (p->angle < 0) p->angle += 360;
        return 1;
    }
    if (kc == KEY_ARROW_RIGHT) {
        p->angle += ROT_SPEED_DEG;
        if (p->angle >= 360) p->angle -= 360;
        return 1;
    }

    int dx = icos(p->angle);
    int dy = isin(p->angle);

    int nx = p->x;
    int ny = p->y;

    if (c == 'w' || c == 'W') { nx += (dx * MOVE_SPEED_TENTHS) / 10; ny += (dy * MOVE_SPEED_TENTHS) / 10; }
    else if (c == 's' || c == 'S') { nx -= (dx * MOVE_SPEED_TENTHS) / 10; ny -= (dy * MOVE_SPEED_TENTHS) / 10; }
    else if (c == 'a' || c == 'A') { nx += (dy * MOVE_SPEED_TENTHS) / 10; ny -= (dx * MOVE_SPEED_TENTHS) / 10; }
    else if (c == 'd' || c == 'D') { nx -= (dy * MOVE_SPEED_TENTHS) / 10; ny += (dx * MOVE_SPEED_TENTHS) / 10; }
    else return 0;

    int mx = nx / 100;
    int my = ny / 100;
    if (mx >= 0 && mx < MAP_W && my >= 0 && my < MAP_H && g_map[my][mx] == 0) {
        p->x = nx;
        p->y = ny;
        return 1;
    }

    return 0;
}

/* movement is applied in input_handle() per keypress (ASCII version style) */

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    puts_raw("raygfx - framebuffer raycaster (userland)\n");

    Gfx gfx;
    int gr = gfx_init(&gfx);
    if (gr != 0) {
        printf("raygfx: gfx_init failed (%d)\n", gr);
        sleep(2);
        return 1;
    }

    int efd = evt_open();
    if (efd < 0) {
        puts_raw("raygfx: cannot open $/dev/input/event0\n");
        sleep(2);
        return 2;
    }

    Player p = { .x = 150, .y = 150, .angle = 0 };
    Input in;
    memset(&in, 0, sizeof(in));

    int dirty = 1; /* render first frame */

    uint64_t last = time_ms();
    uint64_t fps_last = last;
    int fps = 0;
    int fps_counter = 0;

    while (!in.quit) {
        /* poll events */
        Event ev;
        while (read(efd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (input_handle(&p, &in, &ev)) dirty = 1;
        }

        uint64_t now = time_ms();
        if (now - last >= 16) {
            int fps_changed = 0;
            fps_counter++;
            if (now - fps_last >= 1000) {
                fps = fps_counter;
                fps_counter = 0;
                fps_last = now;
                fps_changed = 1;
            }

            if (dirty || fps_changed) {
                render_frame(&gfx, &p, fps);
                gfx_present(&gfx);
                dirty = 0;
            }

            last = now;
        }

        yield();
    }

    close(efd);
    return 0;
}
