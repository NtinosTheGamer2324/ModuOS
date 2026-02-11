#include "libc.h"
#include "string.h"
#include "../include/moduos/kernel/events/events.h"

/*
 * sysmon.sqr - simple graphical system monitor / task manager
 *
 * Shows: RAM usage, uptime, and process list.
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

static uint32_t rgb(Gfx *g, uint8_t r, uint8_t gg, uint8_t b) {
    return (g->fmt == MD64API_GRP_FMT_RGB565) ? (uint32_t)pack_rgb565(r, gg, b) : pack_xrgb8888(r, gg, b);
}

static void put_px(Gfx *g, int x, int y, uint32_t c) {
    if (!g || !g->bb) return;
    if ((unsigned)x >= g->vi.width || (unsigned)y >= g->vi.height) return;
    uint8_t *row = g->bb + (uint64_t)y * g->bb_pitch;
    if (g->bpp_bytes == 4) ((uint32_t*)row)[x] = c;
    else ((uint16_t*)row)[x] = (uint16_t)c;
}

static void fill_rect(Gfx *g, int x, int y, int w, int h, uint32_t c) {
    if (!g || !g->bb) return;
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)g->vi.width)  w = (int)g->vi.width - x;
    if (y + h > (int)g->vi.height) h = (int)g->vi.height - y;
    if (w <= 0 || h <= 0) return;

    for (int yy = 0; yy < h; yy++) {
        uint8_t *row = g->bb + (uint64_t)(y + yy) * g->bb_pitch;
        if (g->bpp_bytes == 4) {
            uint32_t *p = (uint32_t*)row + x;
            for (int xx = 0; xx < w; xx++) p[xx] = c;
        } else {
            uint16_t *p = (uint16_t*)row + x;
            for (int xx = 0; xx < w; xx++) p[xx] = (uint16_t)c;
        }
    }
}

/* Embedded 8x8 font (ASCII 32..127). Source: public domain font8x8.
 * We only include the subset we need by keeping all 96 glyphs.
 */
static const uint8_t font8x8_basic[96][8] = {
    /* 0x20 ' ' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x21 '!' */ {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00},
    /* 0x22 '"' */ {0x36,0x36,0x24,0x00,0x00,0x00,0x00,0x00},
    /* 0x23 '#' */ {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},
    /* 0x24 '$' */ {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00},
    /* 0x25 '%' */ {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00},
    /* 0x26 '&' */ {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00},
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
    /* 0x58 'X' */ {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00},
    /* 0x59 'Y' */ {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00},
    /* 0x5A 'Z' */ {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00},
    /* 0x5B '[' */ {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00},
    /* 0x5C '\\'*/ {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00},
    /* 0x5D ']' */ {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00},
    /* 0x5E '^' */ {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00},
    /* 0x5F '_' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
    /* 0x60 '`' */ {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00},
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
    /* 0x6E 'n' */ {0x00,0x00,0x1B,0x37,0x33,0x33,0x33,0x00},
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
    /* 0x7A 'z' */ {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00},
    /* 0x7B '{' */ {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00},
    /* 0x7C '|' */ {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},
    /* 0x7D '}' */ {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00},
    /* 0x7E '~' */ {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x7F DEL */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

static void draw_char8(Gfx *g, int x, int y, char ch, int scale, uint32_t fg, uint32_t bg) {
    if (ch < 32 || ch > 127) ch = '?';
    const uint8_t *glyph = font8x8_basic[(int)ch - 32];
    for (int yy = 0; yy < 8; yy++) {
        uint8_t row = glyph[yy];
        for (int xx = 0; xx < 8; xx++) {
            uint32_t col = (row & (1u << xx)) ? fg : bg;
            if (col == bg && bg == 0xFFFFFFFFu) continue; // optional transparent
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++)
                    put_px(g, x + xx*scale + sx, y + yy*scale + sy, col);
        }
    }
}

static void draw_text(Gfx *g, int x, int y, const char *s, int scale, uint32_t fg, uint32_t bg) {
    int cx = x;
    for (size_t i = 0; s && s[i]; i++) {
        draw_char8(g, cx, y, s[i], scale, fg, bg);
        cx += 8*scale;
    }
}

static void uitoa(char *out, unsigned v) {
    char tmp[16];
    int n = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v && n < 15) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = tmp[n-1-i];
    out[n] = 0;
}

static void fmt_uptime(char out[32], uint64_t ms) {
    uint64_t s = ms / 1000u;
    uint64_t m = s / 60u;
    uint64_t h = m / 60u;
    s %= 60u; m %= 60u;

    char hb[16], mb[16], sb[16];
    uitoa(hb, (unsigned)h);
    uitoa(mb, (unsigned)m);
    uitoa(sb, (unsigned)s);

    // H:MM:SS
    strcpy(out, hb);
    strcat(out, ":");
    if (m < 10) strcat(out, "0");
    strcat(out, mb);
    strcat(out, ":");
    if (s < 10) strcat(out, "0");
    strcat(out, sb);
}

static const char *state_name(uint32_t st) {
    switch (st) {
        case 0: return "READY";
        case 1: return "RUN";
        case 2: return "BLK";
        case 3: return "SLP";
        case 4: return "ZMB";
        default: return "?";
    }
}

static int gfx_init(Gfx *g) {
    memset(g, 0, sizeof(*g));
    if (md64api_grp_get_video0_info(&g->vi) != 0) return -1;
    if (g->vi.mode != MD64API_GRP_MODE_GRAPHICS) return -2;

    uint8_t fmt = g->vi.fmt;
    if (fmt == MD64API_GRP_FMT_UNKNOWN) {
        if (g->vi.bpp == 32) fmt = MD64API_GRP_FMT_XRGB8888;
        else if (g->vi.bpp == 16) fmt = MD64API_GRP_FMT_RGB565;
    }

    if (!(fmt == MD64API_GRP_FMT_XRGB8888 || fmt == MD64API_GRP_FMT_RGB565)) return -3;

    g->fmt = fmt;
    g->bpp_bytes = (fmt == MD64API_GRP_FMT_RGB565) ? 2u : 4u;
    g->bb_pitch = g->vi.width * g->bpp_bytes;

    uint64_t size = (uint64_t)g->bb_pitch * g->vi.height;
    g->bb = (uint8_t*)malloc((size_t)size);
    return g->bb ? 0 : -4;
}

static void present(Gfx *g) {
    (void)gfx_blit(g->bb, (uint16_t)g->vi.width, (uint16_t)g->vi.height,
                   0, 0, (uint16_t)g->bb_pitch, (uint16_t)g->fmt);
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    Gfx g;
    int gr = gfx_init(&g);
    if (gr != 0) {
        printf("sysmon: graphics init failed (%d)\n", gr);
        return 1;
    }

    int efd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    if (efd < 0) {
        puts_raw("sysmon: cannot open $/dev/input/event0\n");
        return 2;
    }

    md64api_sysinfo_data_u si;
    md64api_pid_info_u pi;

    uint64_t last_bucket = 0;

    // CPU% sampling (best-effort): total_time increments at PIT rate (configured to 100Hz in kernel logs).
    // We compute per-process delta_ticks over delta_ms.
    uint64_t last_total_time[MD64API_MAX_PROCESSES];
    memset(last_total_time, 0, sizeof(last_total_time));
    uint64_t last_sample_ms = time_ms();
    uint16_t last_cpu_pct10[MD64API_MAX_PROCESSES]; // tenths of a percent (e.g. 123 => 12.3%)
    for (int i = 0; i < MD64API_MAX_PROCESSES; i++) last_cpu_pct10[i] = 0;

    while (1) {
        // input is handled during draw (navigation)
        Event ev;

        uint64_t ms = time_ms();
        uint64_t bucket = ms / 200; // ~5 FPS
        if (bucket == last_bucket) { yield(); continue; }
        last_bucket = bucket;

        memset(&si, 0, sizeof(si));
        (void)get_system_info_u(&si);
        // Enumerate processes by probing PIDs.
        // This avoids depending on a full proc table dump and tends to be more stable.
        uint32_t pids[128];
        int pc = 0;

        uint64_t now_ms = ms;
        uint64_t dt_ms = now_ms - last_sample_ms;
        if (dt_ms == 0) dt_ms = 1;
        last_sample_ms = now_ms;

        for (uint32_t pid = 0; pid < MD64API_MAX_PROCESSES && pc < 128; pid++) {
            if (md64api_get_pid_info(pid, &pi) == 0) {
                // CPU% estimate (integer, tenths).
                // With PIT=100Hz: 100% CPU means dticks ~= dt_ms (because 100 ticks/sec => 1 tick per 10ms).
                uint64_t prev = last_total_time[pid];
                uint64_t cur = pi.total_time;
                last_total_time[pid] = cur;

                uint64_t dticks = (cur >= prev) ? (cur - prev) : 0;

                // pct = dticks * 1000 / dt_ms
                // pct10 = pct * 10 = dticks * 10000 / dt_ms
                uint64_t pct10 = (dt_ms ? (dticks * 10000ULL) / dt_ms : 0);
                if (pct10 > 1000) pct10 = 1000;
                last_cpu_pct10[pid] = (uint16_t)pct10;

                pids[pc++] = pid;
            }
        }

        uint32_t bg = rgb(&g, 12, 14, 18);
        uint32_t fg = rgb(&g, 230, 230, 240);
        uint32_t dim = rgb(&g, 120, 130, 150);
        uint32_t green = rgb(&g, 80, 220, 120);
        uint32_t red = rgb(&g, 255, 90, 90);

        fill_rect(&g, 0, 0, (int)g.vi.width, (int)g.vi.height, bg);

        // Header
        draw_text(&g, 16, 10, "System Monitor", 2, fg, bg);

        // RAM bar
        uint64_t total = si.sys_total_ram;
        uint64_t avail = si.sys_available_ram;
        uint64_t used = (total > avail) ? (total - avail) : 0;
        int bar_x = 16, bar_y = 40, bar_w = (int)g.vi.width - 32, bar_h = 14;
        fill_rect(&g, bar_x, bar_y, bar_w, bar_h, dim);
        int used_w = (total != 0) ? (int)((used * (uint64_t)bar_w) / total) : 0;
        if (used_w < 0) used_w = 0;
        if (used_w > bar_w) used_w = bar_w;
        fill_rect(&g, bar_x, bar_y, used_w, bar_h, (used_w > bar_w/2) ? red : green);

        char ub[16], tb[16];
        uitoa(ub, (unsigned)used);
        uitoa(tb, (unsigned)total);
        char ramline[64];
        strcpy(ramline, "RAM ");
        strcat(ramline, ub);
        strcat(ramline, "/");
        strcat(ramline, tb);
        strcat(ramline, " MB");
        draw_text(&g, 16, 60, ramline, 2, fg, bg);

        // Uptime
        char up[32];
        fmt_uptime(up, ms);
        char upline[64];
        strcpy(upline, "UP ");
        strcat(upline, up);
        draw_text(&g, 16, 80, upline, 2, fg, bg);

        // Process list header
        draw_text(&g, 16, 110, "PID   STATE  TICKS   NAME", 2, fg, bg);
        fill_rect(&g, 16, 124, (int)g.vi.width - 32, 1, dim);

        // Layout
        const int scale = 2;
        const int line_h = 8 * scale + 2;

        const int margin = 12;
        const int top_h = 96;

        int left_x = margin;
        int left_y = top_h + margin;
        int left_w = (int)g.vi.width * 2 / 3 - margin * 2;
        int left_h = (int)g.vi.height - left_y - margin;

        int right_x = left_x + left_w + margin;
        int right_y = left_y;
        int right_w = (int)g.vi.width - right_x - margin;
        int right_h = left_h;

        // Panels
        fill_rect(&g, left_x, left_y, left_w, left_h, rgb(&g, 18, 20, 26));
        fill_rect(&g, right_x, right_y, right_w, right_h, rgb(&g, 18, 20, 26));
        fill_rect(&g, left_x, left_y, left_w, 1, dim);
        fill_rect(&g, left_x, left_y + left_h - 1, left_w, 1, dim);
        fill_rect(&g, left_x, left_y, 1, left_h, dim);
        fill_rect(&g, left_x + left_w - 1, left_y, 1, left_h, dim);
        fill_rect(&g, right_x, right_y, right_w, 1, dim);
        fill_rect(&g, right_x, right_y + right_h - 1, right_w, 1, dim);
        fill_rect(&g, right_x, right_y, 1, right_h, dim);
        fill_rect(&g, right_x + right_w - 1, right_y, 1, right_h, dim);

        draw_text(&g, left_x + 8, left_y + 6, "Processes", scale, fg, rgb(&g, 18, 20, 26));
        draw_text(&g, right_x + 8, right_y + 6, "Details", scale, fg, rgb(&g, 18, 20, 26));

        // Process table header
        int header_y = left_y + 6 + line_h;
        fill_rect(&g, left_x + 1, header_y, left_w - 2, line_h, rgb(&g, 22, 25, 32));
        draw_text(&g, left_x + 8, header_y + 2, "PID", scale, dim, rgb(&g, 22, 25, 32));
        draw_text(&g, left_x + 8 + 6*8*scale, header_y + 2, "STATE", scale, dim, rgb(&g, 22, 25, 32));
        draw_text(&g, left_x + 8 + 14*8*scale, header_y + 2, "TICKS", scale, dim, rgb(&g, 22, 25, 32));
        draw_text(&g, left_x + 8 + 22*8*scale, header_y + 2, "CPU%", scale, dim, rgb(&g, 22, 25, 32));
        draw_text(&g, left_x + 8 + 28*8*scale, header_y + 2, "NAME", scale, dim, rgb(&g, 22, 25, 32));

        // Selection + scrolling state (static)
        static int sel = 0;
        static int scroll = 0;

        // Handle navigation
        while (read(efd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (ev.type == EVENT_KEY_PRESSED) {
                int kc = ev.data.keyboard.keycode;
                if (kc == KEY_ESCAPE) { close(efd); if (g.bb) free(g.bb); return 0; }
                if (kc == KEY_ARROW_DOWN) sel++;
                if (kc == KEY_ARROW_UP) sel--;
                if (kc == KEY_PAGE_DOWN) sel += 10;
                if (kc == KEY_PAGE_UP) sel -= 10;
            }
        }

        if (sel < 0) sel = 0;
        if (sel >= pc) sel = pc - 1;
        if (sel < 0) sel = 0;

        int list_y0 = header_y + line_h;
        int list_h = left_y + left_h - margin - list_y0;
        int rows = (list_h / line_h);
        if (rows < 1) rows = 1;

        if (sel < scroll) scroll = sel;
        if (sel >= scroll + rows) scroll = sel - rows + 1;
        if (scroll < 0) scroll = 0;
        if (scroll > pc - rows) scroll = pc - rows;
        if (scroll < 0) scroll = 0;

        // Draw rows
        for (int r = 0; r < rows; r++) {
            int idx = scroll + r;
            int ry = list_y0 + r * line_h;
            uint32_t row_bg = (r & 1) ? rgb(&g, 18, 20, 26) : rgb(&g, 20, 22, 28);
            if (idx == sel) row_bg = rgb(&g, 40, 60, 90);
            fill_rect(&g, left_x + 1, ry, left_w - 2, line_h, row_bg);
            if (idx >= pc) continue;

            if (md64api_get_pid_info(pids[idx], &pi) != 0) continue;

            char pidb[16], tickb[16];
            uitoa(pidb, pi.pid);
            uitoa(tickb, (unsigned)pi.total_time);

            draw_text(&g, left_x + 8, ry + 2, pidb, scale, fg, row_bg);
            draw_text(&g, left_x + 8 + 6*8*scale, ry + 2, state_name(pi.state), scale, fg, row_bg);
            draw_text(&g, left_x + 8 + 14*8*scale, ry + 2, tickb, scale, fg, row_bg);

            // CPU%
            {
                char cpb[16];
                uint16_t p10 = last_cpu_pct10[pi.pid];
                unsigned ip = p10 / 10;
                unsigned fp = p10 % 10;

                char nb[16];
                uitoa(nb, ip);

                // Build "<ip>.<fp>%" safely
                size_t n = 0;
                for (; nb[n] && n + 1 < sizeof(cpb); n++) cpb[n] = nb[n];
                if (n + 4 < sizeof(cpb)) {
                    cpb[n++] = '.';
                    cpb[n++] = (char)('0' + fp);
                    cpb[n++] = '%';
                    cpb[n] = 0;
                } else {
                    cpb[sizeof(cpb)-1] = 0;
                }

                draw_text(&g, left_x + 8 + 22*8*scale, ry + 2, cpb, scale, fg, row_bg);
            }

            draw_text(&g, left_x + 8 + 28*8*scale, ry + 2, pi.name, scale, fg, row_bg);
        }

        // Details pane for selected process
        if (pc > 0 && sel < pc && md64api_get_pid_info(pids[sel], &pi) == 0) {
            int dx = right_x + 8;
            int dy = right_y + 6 + line_h;

            char buf[64];
            draw_text(&g, dx, dy, "Name:", scale, dim, rgb(&g, 18, 20, 26));
            draw_text(&g, dx + 8*scale*6, dy, pi.name, scale, fg, rgb(&g, 18, 20, 26));
            dy += line_h;

            draw_text(&g, dx, dy, "PID:", scale, dim, rgb(&g, 18, 20, 26));
            uitoa(buf, pi.pid);
            draw_text(&g, dx + 8*scale*5, dy, buf, scale, fg, rgb(&g, 18, 20, 26));
            dy += line_h;

            draw_text(&g, dx, dy, "PPID:", scale, dim, rgb(&g, 18, 20, 26));
            uitoa(buf, pi.ppid);
            draw_text(&g, dx + 8*scale*5, dy, buf, scale, fg, rgb(&g, 18, 20, 26));
            dy += line_h;

            draw_text(&g, dx, dy, "State:", scale, dim, rgb(&g, 18, 20, 26));
            draw_text(&g, dx + 8*scale*6, dy, state_name(pi.state), scale, fg, rgb(&g, 18, 20, 26));
            dy += line_h;

            draw_text(&g, dx, dy, "CWD:", scale, dim, rgb(&g, 18, 20, 26));
            dy += line_h;
            draw_text(&g, dx, dy, pi.cwd, 1, fg, rgb(&g, 18, 20, 26));
            dy += line_h + 4;

            // RAM usage
            {
                char mbuf[64];
                uint64_t mib = pi.mem_total_bytes / (1024u * 1024u);
                // "RAM: <MiB> MiB"
                strcpy(mbuf, "RAM: ");
                char nb[16];
                uitoa(nb, (unsigned)mib);
                strcat(mbuf, nb);
                strcat(mbuf, " MiB");
                draw_text(&g, dx, dy, mbuf, scale, fg, rgb(&g, 18, 20, 26));
                dy += line_h;

                // Memory bar: process mem vs total system RAM
                {
                    uint64_t total_bytes = (uint64_t)si.sys_total_ram * 1024ULL * 1024ULL;
                    int bx = dx;
                    int by = dy;
                    int bw = right_w - 16;
                    int bh = 10;
                    fill_rect(&g, bx, by, bw, bh, rgb(&g, 40, 45, 55));
                    int fillw = 0;
                    if (total_bytes) {
                        fillw = (int)((pi.mem_total_bytes * (uint64_t)bw) / total_bytes);
                        if (fillw < 0) fillw = 0;
                        if (fillw > bw) fillw = bw;
                    }
                    fill_rect(&g, bx, by, fillw, bh, rgb(&g, 80, 160, 255));
                    dy += bh + 6;
                }

                // breakdown (smaller)
                uint64_t img = pi.mem_image_bytes / (1024u * 1024u);
                uint64_t heap = pi.mem_heap_bytes / (1024u * 1024u);
                uint64_t mm = pi.mem_mmap_bytes / (1024u * 1024u);
                uint64_t st = pi.mem_stack_bytes / (1024u * 1024u);

                strcpy(mbuf, "img "); uitoa(nb, (unsigned)img); strcat(mbuf, nb); strcat(mbuf, "M");
                strcat(mbuf, "  heap "); uitoa(nb, (unsigned)heap); strcat(mbuf, nb); strcat(mbuf, "M");
                draw_text(&g, dx, dy, mbuf, 1, dim, rgb(&g, 18, 20, 26));
                dy += 10;

                strcpy(mbuf, "mmap "); uitoa(nb, (unsigned)mm); strcat(mbuf, nb); strcat(mbuf, "M");
                strcat(mbuf, "  stack "); uitoa(nb, (unsigned)st); strcat(mbuf, nb); strcat(mbuf, "M");
                draw_text(&g, dx, dy, mbuf, 1, dim, rgb(&g, 18, 20, 26));
            }
        }

        present(&g);
        yield();
    }
}
