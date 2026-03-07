#include "libc.h"
#include "NodGL.h"
#include "string.h"
#include "../include/moduos/kernel/events/events.h"

/*
 * gfxclock.sqr
 *
 * Analog + digital clock rendered in framebuffer graphics mode.
 *
 * Controls:
 *   ESC: quit
 */

typedef struct {
    md64api_grp_video_info_t vi;
    uint8_t *bb;
    uint32_t bb_pitch;
    uint32_t fmt;
    uint32_t bpp_bytes;
} Gfx;

static uint32_t pack_xrgb8888(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static uint16_t pack_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t rr = (uint16_t)((r * 31u) / 255u);
    uint16_t gg = (uint16_t)((g * 63u) / 255u);
    uint16_t bb = (uint16_t)((b * 31u) / 255u);
    return (uint16_t)((rr << 11) | (gg << 5) | (bb));
}

static void put_px(Gfx *g, int x, int y, uint32_t c) {
    if (!g || !g->bb) return;
    if ((unsigned)x >= g->vi.width || (unsigned)y >= g->vi.height) return;
    uint8_t *row = g->bb + (uint64_t)y * g->bb_pitch;
    if (g->bpp_bytes == 4) {
        ((uint32_t*)row)[x] = c;
    } else {
        ((uint16_t*)row)[x] = (uint16_t)c;
    }
}

static void clear(Gfx *g, uint32_t c) {
    if (!g || !g->bb) return;
    for (uint32_t y = 0; y < g->vi.height; y++) {
        uint8_t *row = g->bb + (uint64_t)y * g->bb_pitch;
        if (g->bpp_bytes == 4) {
            uint32_t *p = (uint32_t*)row;
            for (uint32_t x = 0; x < g->vi.width; x++) p[x] = c;
        } else {
            uint16_t *p = (uint16_t*)row;
            for (uint32_t x = 0; x < g->vi.width; x++) p[x] = (uint16_t)c;
        }
    }
}

static int iabs(int v) { return (v < 0) ? -v : v; }

/* Simple integer Bresenham line */
static void draw_line(Gfx *g, int x0, int y0, int x1, int y1, uint32_t c) {
    int dx = iabs(x1 - x0);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = -iabs(y1 - y0);
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        put_px(g, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Midpoint circle (outline) */
static void draw_circle(Gfx *g, int cx, int cy, int r, uint32_t c) {
    int x = r;
    int y = 0;
    int err = 0;

    while (x >= y) {
        put_px(g, cx + x, cy + y, c);
        put_px(g, cx + y, cy + x, c);
        put_px(g, cx - y, cy + x, c);
        put_px(g, cx - x, cy + y, c);
        put_px(g, cx - x, cy - y, c);
        put_px(g, cx - y, cy - x, c);
        put_px(g, cx + y, cy - x, c);
        put_px(g, cx + x, cy - y, c);

        y++;
        if (err <= 0) {
            err += 2*y + 1;
        }
        if (err > 0) {
            x--;
            err -= 2*x + 1;
        }
    }
}

/* Tiny 3x5 font digits + ':' and '-' (enough for HH:MM:SS and YYYY-MM-DD) */
static const uint8_t font_3x5_digits[12][5] = {
    /* 0 */ {0b111,0b101,0b101,0b101,0b111},
    /* 1 */ {0b010,0b110,0b010,0b010,0b111},
    /* 2 */ {0b111,0b001,0b111,0b100,0b111},
    /* 3 */ {0b111,0b001,0b111,0b001,0b111},
    /* 4 */ {0b101,0b101,0b111,0b001,0b001},
    /* 5 */ {0b111,0b100,0b111,0b001,0b111},
    /* 6 */ {0b111,0b100,0b111,0b101,0b111},
    /* 7 */ {0b111,0b001,0b001,0b001,0b001},
    /* 8 */ {0b111,0b101,0b111,0b101,0b111},
    /* 9 */ {0b111,0b101,0b111,0b001,0b111},
    /* : */ {0b000,0b010,0b000,0b010,0b000},
    /* - */ {0b000,0b000,0b111,0b000,0b000},
};

static void draw_glyph_3x5(Gfx *g, int x, int y, char ch, int scale, uint32_t c) {
    int idx = -1;
    if (ch >= '0' && ch <= '9') idx = (int)(ch - '0');
    else if (ch == ':') idx = 10;
    else if (ch == '-') idx = 11;
    if (idx < 0) return;

    for (int yy = 0; yy < 5; yy++) {
        uint8_t row = font_3x5_digits[idx][yy];
        for (int xx = 0; xx < 3; xx++) {
            if (row & (1u << (2-xx))) {
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        put_px(g, x + xx*scale + sx, y + yy*scale + sy, c);
            }
        }
    }
}

static void draw_text_3x5(Gfx *g, int x, int y, const char *s, int scale, uint32_t c) {
    int cx = x;
    for (size_t i = 0; s && s[i]; i++) {
        draw_glyph_3x5(g, cx, y, s[i], scale, c);
        cx += (3*scale) + scale; // glyph + spacing
    }
}

/* Very small float-free sin/cos approximation via lookup table (0..59) */
static const int16_t sin60[60] = {
      0,  107,  214,  319,  420,  515,  604,  684,  756,  818,
    870,  912,  943,  963,  972,  970,  957,  933,  898,  852,
    796,  729,  652,  566,  471,  369,  260,  146,   30,  -86,
   -201, -312, -417, -515, -604, -684, -756, -818, -870, -912,
   -943, -963, -972, -970, -957, -933, -898, -852, -796, -729,
   -652, -566, -471, -369, -260, -146,  -30,   86,  201,  312,
};
static const int16_t cos60[60] = {
    1000,  994,  977,  948,  908,  857,  796,  725,  647,  561,
     471,  375,  276,  174,   70,  -37, -144, -250, -353, -453,
    -548, -637, -719, -793, -857, -911, -953, -983, -999, -1000,
    -987, -959, -918, -864, -798, -721, -634, -540, -440, -336,
    -230, -123,  -16,   91,  197,  300,  399,  493,  581,  662,
     734,  797,  850,  892,  923,  942,  951,  948,  934,  909,
};

/* Map "tick" (0..59) to an angle where 0 is at 12 o'clock */
static void hand_endpoint(int cx, int cy, int len, int tick0_59, int *out_x, int *out_y) {
    int t = tick0_59 % 60;
    // rotate so 0 is at top: cos->x, sin->y, but y grows down => use -sin
    int x = (cos60[t] * len) / 1000;
    int y = -(sin60[t] * len) / 1000;
    if (out_x) *out_x = cx + x;
    if (out_y) *out_y = cy + y;
}

// Interpolate between tick t and t+1 using ms_in_sec (0..999) for smoother second hand.
static void hand_endpoint_smooth(int cx, int cy, int len, int tick0_59, uint16_t ms_in_sec,
                                int *out_x, int *out_y) {
    int t0 = tick0_59 % 60;
    int t1 = (t0 + 1) % 60;

    int w1 = (int)ms_in_sec;          // 0..999
    int w0 = 1000 - w1;

    int sinv = (sin60[t0] * w0 + sin60[t1] * w1) / 1000;
    int cosv = (cos60[t0] * w0 + cos60[t1] * w1) / 1000;

    int x = (cosv * len) / 1000;
    int y = -(sinv * len) / 1000;
    if (out_x) *out_x = cx + x;
    if (out_y) *out_y = cy + y;
}

static NodGL_Device g_device = NULL;
static NodGL_Context g_ctx = NULL;
static NodGL_Texture g_backbuffer_tex = 0;

static int gfx_init(Gfx *g) {
    memset(g, 0, sizeof(*g));

    if (NodGL_CreateDevice(NodGL_FEATURE_LEVEL_1_0, &g_device, &g_ctx, NULL) != NodGL_OK) {
        return -1;
    }

    uint32_t screen_w, screen_h;
    NodGL_GetScreenResolution(g_device, &screen_w, &screen_h);

    g->vi.width = screen_w;
    g->vi.height = screen_h;
    g->vi.bpp = 32;
    g->fmt = MD64API_GRP_FMT_XRGB8888;
    g->bpp_bytes = 4;

    NodGL_TextureDesc tex_desc = {
        .width = screen_w,
        .height = screen_h,
        .format = NodGL_FORMAT_R8G8B8A8_UNORM,
        .mip_levels = 1,
        .initial_data = NULL,
        .initial_data_size = 0
    };

    if (NodGL_CreateTexture(g_device, &tex_desc, &g_backbuffer_tex) != NodGL_OK) {
        NodGL_ReleaseDevice(g_device);
        return -2;
    }

    if (NodGL_MapResource(g_ctx, g_backbuffer_tex, (void**)&g->bb, &g->bb_pitch) != NodGL_OK) {
        NodGL_ReleaseResource(g_device, g_backbuffer_tex);
        NodGL_ReleaseDevice(g_device);
        return -4;
    }

    return 0;
}

static void gfx_present(Gfx *g) {
    NodGL_DrawTexture(g_ctx, g_backbuffer_tex, 0, 0, 0, 0, g->vi.width, g->vi.height);
    NodGL_PresentContext(g_ctx, 0);
}

static void format_time(char out[16], uint8_t hh, uint8_t mm, uint8_t ss) {
    out[0] = (char)('0' + (hh / 10));
    out[1] = (char)('0' + (hh % 10));
    out[2] = ':';
    out[3] = (char)('0' + (mm / 10));
    out[4] = (char)('0' + (mm % 10));
    out[5] = ':';
    out[6] = (char)('0' + (ss / 10));
    out[7] = (char)('0' + (ss % 10));
    out[8] = 0;
}

static void format_date(char out[16], uint16_t yyyy, uint8_t mo, uint8_t dd) {
    // YYYY-MM-DD
    out[0] = (char)('0' + (yyyy / 1000) % 10);
    out[1] = (char)('0' + (yyyy / 100) % 10);
    out[2] = (char)('0' + (yyyy / 10) % 10);
    out[3] = (char)('0' + (yyyy / 1) % 10);
    out[4] = '-';
    out[5] = (char)('0' + (mo / 10));
    out[6] = (char)('0' + (mo % 10));
    out[7] = '-';
    out[8] = (char)('0' + (dd / 10));
    out[9] = (char)('0' + (dd % 10));
    out[10] = 0;
}

static void render(Gfx *g, uint8_t hh, uint8_t mm, uint8_t ss,
                   uint16_t yyyy, uint8_t mo, uint8_t dd,
                   uint16_t ms_in_sec) {

    uint32_t bg = (g->fmt == MD64API_GRP_FMT_RGB565) ? pack_rgb565(10, 12, 18) : pack_xrgb8888(10, 12, 18);
    uint32_t fg = (g->fmt == MD64API_GRP_FMT_RGB565) ? pack_rgb565(230, 230, 240) : pack_xrgb8888(230, 230, 240);
    uint32_t accent = (g->fmt == MD64API_GRP_FMT_RGB565) ? pack_rgb565(255, 80, 80) : pack_xrgb8888(255, 80, 80);
    uint32_t dim = (g->fmt == MD64API_GRP_FMT_RGB565) ? pack_rgb565(90, 100, 120) : pack_xrgb8888(90, 100, 120);

    clear(g, bg);

    int cx = (int)g->vi.width / 2;
    int cy = (int)g->vi.height / 2 - 40;
    int r = (int)((g->vi.height < g->vi.width ? g->vi.height : g->vi.width) / 3);
    if (r < 60) r = 60;

    // Face
    draw_circle(g, cx, cy, r, fg);
    draw_circle(g, cx, cy, r-1, dim);

    // Numerals 12/3/6/9
    {
        int nscale = 4;
        // "12" at top
        draw_text_3x5(g, cx - (2 * ((3*nscale)+nscale))/2, cy - r + 14, "12", nscale, fg);
        // "3" right
        draw_text_3x5(g, cx + r - 16, cy - (5*nscale)/2, "3", nscale, fg);
        // "6" bottom
        draw_text_3x5(g, cx - ((3*nscale)+nscale)/2, cy + r - 16, "6", nscale, fg);
        // "9" left
        draw_text_3x5(g, cx - r + 10, cy - (5*nscale)/2, "9", nscale, fg);
    }

    // Minute dots (subtle)
    for (int t = 0; t < 60; t++) {
        int x0,y0;
        hand_endpoint(cx, cy, r-3, t, &x0, &y0);
        put_px(g, x0, y0, (t % 5 == 0) ? fg : dim);
    }

    // Tick marks every 5 minutes (longer)
    for (int t = 0; t < 60; t += 5) {
        int x0,y0,x1,y1;
        hand_endpoint(cx, cy, r-2, t, &x0, &y0);
        hand_endpoint(cx, cy, r-10, t, &x1, &y1);
        draw_line(g, x0, y0, x1, y1, fg);
    }

    // Hands
    int sx,sy,mx,my,hx,hy;

    // Smooth second hand (interpolated)
    hand_endpoint_smooth(cx, cy, r-12, (int)ss, ms_in_sec, &sx, &sy);
    hand_endpoint(cx, cy, r-22, (int)mm, &mx, &my);

    // hour: map to 0..59 with minute influence
    int hour12 = (int)(hh % 12);
    int hour_tick = (hour12 * 5) + ((int)mm / 12);
    hand_endpoint(cx, cy, r-38, hour_tick, &hx, &hy);

    draw_line(g, cx, cy, hx, hy, fg);
    draw_line(g, cx, cy, mx, my, fg);
    // Second hand: draw base position, plus a small "tail" (also smooth)
    draw_line(g, cx, cy, sx, sy, accent);
    {
        int tx, ty;
        hand_endpoint_smooth(cx, cy, -12, (int)ss, ms_in_sec, &tx, &ty);
        draw_line(g, cx, cy, tx, ty, dim);
    }

    // Center cap
    draw_circle(g, cx, cy, 3, fg);

    // Digital time below
    char tbuf[16];
    format_time(tbuf, hh, mm, ss);

    char dbuf[16];
    format_date(dbuf, yyyy, mo, dd);

    int scale = 6;
    int text_w = (int)strlen(tbuf) * ((3*scale)+scale);
    int tx = (int)g->vi.width/2 - text_w/2;
    int ty = cy + r + 20;
    draw_text_3x5(g, tx, ty, tbuf, scale, fg);

    // Date line
    scale = 4;
    text_w = (int)strlen(dbuf) * ((3*scale)+scale);
    tx = (int)g->vi.width/2 - text_w/2;
    ty += 5*6 + 10;
    draw_text_3x5(g, tx, ty, dbuf, scale, dim);

    // Hint
    const char *hint = "ESC to quit";
    scale = 3;
    text_w = (int)strlen(hint) * ((3*scale)+scale);
    tx = (int)g->vi.width/2 - text_w/2;
    ty += 5*4 + 14;
    draw_text_3x5(g, tx, ty, hint, scale, dim);
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    Gfx g;
    int gr = gfx_init(&g);
    if (gr != 0) {
        printf("gfxclock: graphics init failed (%d). Need framebuffer graphics mode.\n", gr);
        return 1;
    }

    int efd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    if (efd < 0) {
        puts_raw("gfxclock: cannot open $/dev/input/event0\n");
        NodGL_ReleaseResource(g_device, g_backbuffer_tex);
        NodGL_ReleaseDevice(g_device);
        return 2;
    }

    md64api_sysinfo_data_u info;
    memset(&info, 0, sizeof(info));
    int sysinfo_fd = open("$/dev/md64api/sysinfo", 0, 0);

    uint8_t last_s = 255;
    uint16_t last_ms_bucket = 0;
    uint64_t sec_epoch_ms = 0;

    while (1) {
        // input
        Event ev;
        while (read(efd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (ev.type == EVENT_KEY_PRESSED && ev.data.keyboard.keycode == KEY_ESCAPE) {
                close(efd);
                if (g.bb) free(g.bb);
                return 0;
            }
        }

        // Use SYS_TIME for smooth animation pacing.
        uint64_t ms = time_ms();

        // Render at ~60 FPS for a smooth sweeping second hand.
        uint16_t bucket = (uint16_t)(ms / 16u);
        if (bucket != last_ms_bucket) {
            last_ms_bucket = bucket;

            if (sysinfo_fd >= 0 && (lseek(sysinfo_fd, 0, 0), read(sysinfo_fd, &info, sizeof(info))) > 0) {
                // Re-sync the sub-second phase when the RTC second changes.
                // This avoids jitter caused by SYS_TIME not being aligned to RTC seconds.
                if (info.rtc_second != last_s) {
                    last_s = info.rtc_second;
                    sec_epoch_ms = ms;
                }

                uint16_t ms_in_sec = (uint16_t)(ms - sec_epoch_ms);
                if (ms_in_sec >= 1000) ms_in_sec %= 1000;

                render(&g,
                       info.rtc_hour, info.rtc_minute, info.rtc_second,
                       info.rtc_year, info.rtc_month, info.rtc_day,
                       ms_in_sec);
                gfx_present(&g);
            }
        }

        yield();
    }

    NodGL_ReleaseResource(g_device, g_backbuffer_tex);
    NodGL_ReleaseDevice(g_device);
}
