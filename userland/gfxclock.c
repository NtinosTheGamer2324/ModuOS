/*
 * gfxclock.c — ModuOS graphical clock
 *
 * Analog clock face (smooth-sweeping hands, glowing rim, comet trail on the
 * second hand) plus a digital date/time readout pill, driven by the RTC
 * devfs node exposed by the RTC kernel module.
 *
 * Controls:
 *   ESC / Q     — quit
 *   T           — toggle 12h / 24h digital display
 *
 * Copyright © 2026 New Technologies Software — GPL v2.0
 */

#include "libc.h"
#include "NodGL.h"
#include "lib_fnt.h"
#include "../include/moduos/kernel/events/events.h"

/* ============================================================
   RTC device
   ============================================================
   Must byte-for-byte match the rtc_time_t layout defined in the RTC
   kernel module. No shared header between kernel module and userland
   exists yet, so this is kept in sync manually — see rtc.c / rtc_client.c.
*/
typedef struct {
    uint8_t  second;
    uint8_t  minute;
    uint8_t  hour;
    uint8_t  day;
    uint8_t  month;
    uint32_t year;
} rtc_time_t;

#define RTC_DEV_PATH "$/dev/rtc"

/* ============================================================
   Fonts — Terminus is the preferred UI font; Unicode.fnt (older, but
   covers Greek and other non-ASCII ranges) is the fallback.
   ============================================================ */
#define FONT_PRIMARY  "/ModuOS/shared/assets/fonts/Terminus.fnt"
#define FONT_FALLBACK "/ModuOS/shared/assets/fonts/Unicode.fnt"

/* ============================================================
   Fixed-point sine table (0..255 -> 0..360deg), matches the one used
   throughout the rest of the userland demo/effect code so hand angles
   behave identically to everything else that draws circles on ModuOS.
   ============================================================ */
static const int8_t sin_tbl[256] = {
     0,  3,  6,  9, 12, 15, 18, 21, 24,27, 30, 33, 36, 39, 42, 45,
    48, 51, 54, 57, 59, 62, 65, 67, 70, 73, 75, 78, 80, 82, 85, 87,
    89, 91, 94, 96, 98,100,102,103,105,107,108,110,112,113,114,116,
   117,118,119,120,121,122,123,123,124,125,125,126,126,126,127,127,
   127,127,127,126,126,126,125,125,124,123,123,122,121,120,119,118,
   117,116,114,113,112,110,108,107,105,103,102,100, 98, 96, 94, 91,
    89, 87, 85, 82, 80, 78, 75, 73, 70, 67, 65, 62, 59, 57, 54, 51,
    48, 45, 42, 39, 36, 33, 30, 27, 24, 21, 18, 15, 12,  9,  6,  3,
     0, -3, -6, -9,-12,-15,-18,-21,-24,-27,-30,-33,-36,-39,-42,-45,
   -48,-51,-54,-57,-59,-62,-65,-67,-70,-73,-75,-78,-80,-82,-85,-87,
   -89,-91,-94,-96,-98,-100,-102,-103,-105,-107,-108,-110,-112,-113,
  -114,-116,-117,-118,-119,-120,-121,-122,-123,-123,-124,-125,-125,
  -126,-126,-126,-127,-127,-127,-127,-127,-126,-126,-126,-125,-125,
  -124,-123,-123,-122,-121,-120,-119,-118,-117,-116,-114,-113,-112,
  -110,-108,-107,-105,-103,-102,-100,-98,-96,-94,-91,-89,-87,-85,
   -82,-80,-78,-75,-73,-70,-67,-65,-62,-59,-57,-54,-51,-48,-45,-42,
   -39,-36,-33,-30,-27,-24,-21,-18,-15,-12, -9, -6, -3
};

static inline int isin(uint8_t angle) { return sin_tbl[angle]; }
static inline int icos(uint8_t angle) { return sin_tbl[(uint8_t)(angle + 64)]; }

static inline float sqrtf3_fast(float v) {
    if (v <= 0.0f) return 0.0f;
    float x = v, g = v * 0.5f + 0.5f;
    for (int i = 0; i < 4; i++) g = 0.5f * (g + x / g);
    return g;
}

static inline float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ============================================================
   App globals
   ============================================================ */

static struct {
    NodGL_Device  device;
    NodGL_Context ctx;
    NodGL_Texture tex;
    uint8_t      *bb;
    uint32_t      bb_pitch;
    uint32_t      sw, sh;
    fnt_font_t   *font;

    int      rtc_fd;
    int      quit;
    int      fmt24;          /* 1 = 24h, 0 = 12h */

    /* Latest RTC snapshot + smoothing state for the sweeping second hand */
    rtc_time_t now;
    int        last_raw_sec;
    uint64_t   sec_baseline_ms;

    /* Comet trail behind the second hand tip, in rim-angle bytes */
    uint8_t  trail[24];
    int      trail_len;
} g;

/* ============================================================
   Pixel helpers (same conventions as the rest of ModuOS userland demos)
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

static void clear(uint32_t col) { fill(0, 0, (int)g.sw, (int)g.sh, col); }

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

/* ============================================================
   Text
   ============================================================ */

static int text_width(const char *s) {
    if (!g.font || !s) return 0;
    return fnt_string_width(g.font, s);
}

static void draw_text(int x, int y, const char *s, uint32_t col) {
    if (!g.font || !s) return;
    int cx = x;
    uint8_t r = (col >> 16) & 0xFF, gv = (col >> 8) & 0xFF, b = col & 0xFF;
    while (*s) {
        fnt_glyph_t *gl = fnt_get_glyph(g.font, (uint32_t)(unsigned char)*s);
        if (gl) {
            for (int dy = 0; dy < gl->bitmap_height; dy++)
                for (int dx = 0; dx < gl->bitmap_width; dx++)
                    if (fnt_get_pixel(gl, dx, dy))
                        blend(cx + dx, y + dy, r, gv, b, 255);
            cx += gl->width;
        }
        s++;
    }
}

static void draw_text_centered(int cx, int y, const char *s, uint32_t col) {
    draw_text(cx - text_width(s) / 2, y, s, col);
}

/* ============================================================
   Geometry primitives
   ============================================================ */

/* Anti-aliased filled disc (used for the rim glow, hub, and tick dots). */
static void draw_disc(float cx, float cy, float radius, uint8_t r, uint8_t gv, uint8_t b, uint8_t a) {
    int x0 = (int)(cx - radius - 1), x1 = (int)(cx + radius + 1);
    int y0 = (int)(cy - radius - 1), y1 = (int)(cy + radius + 1);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float dx = (x + 0.5f) - cx, dy = (y + 0.5f) - cy;
            float d = sqrtf3_fast(dx * dx + dy * dy);
            if (d > radius + 1.0f) continue;
            float edge = clampf(radius + 0.5f - d, 0.0f, 1.0f);
            uint8_t aa = (uint8_t)((float)a * edge);
            if (aa) blend(x, y, r, gv, b, aa);
        }
    }
}

/* Anti-aliased ring outline. */
static void draw_ring(float cx, float cy, float radius, float thickness, uint8_t r, uint8_t gv, uint8_t b, uint8_t a) {
    int x0 = (int)(cx - radius - thickness - 1), x1 = (int)(cx + radius + thickness + 1);
    int y0 = (int)(cy - radius - thickness - 1), y1 = (int)(cy + radius + thickness + 1);
    float inner = radius - thickness * 0.5f, outer = radius + thickness * 0.5f;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float dx = (x + 0.5f) - cx, dy = (y + 0.5f) - cy;
            float d = sqrtf3_fast(dx * dx + dy * dy);
            if (d < inner - 1.0f || d > outer + 1.0f) continue;
            float edge_out = clampf(outer - d + 0.5f, 0.0f, 1.0f);
            float edge_in  = clampf(d - inner + 0.5f, 0.0f, 1.0f);
            float k = edge_out < edge_in ? edge_out : edge_in;
            uint8_t aa = (uint8_t)((float)a * k);
            if (aa) blend(x, y, r, gv, b, aa);
        }
    }
}

/* Tapered, anti-aliased hand: wide at the base, narrow at the tip. */
static void draw_hand(float cx, float cy, uint8_t angle_byte, float length, float back_len,
                       float base_half_w, float tip_half_w,
                       uint8_t r, uint8_t gv, uint8_t b, uint8_t a) {
    float dx = (float)isin(angle_byte);
    float dy = -(float)icos(angle_byte);
    float norm = sqrtf3_fast(dx * dx + dy * dy);
    if (norm < 0.001f) return;
    dx /= norm; dy /= norm;

    float ex = cx + dx * length, ey = cy + dy * length;
    float bx = cx - dx * back_len, by = cy - dy * back_len;

    int x0 = (int)(cx - length - back_len - 4), x1 = (int)(cx + length + back_len + 4);
    int y0 = (int)(cy - length - back_len - 4), y1 = (int)(cy + length + back_len + 4);
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > (int)g.sw) x1 = (int)g.sw; if (y1 > (int)g.sh) y1 = (int)g.sh;

    float sdx = ex - bx, sdy = ey - by;
    float seg_len2 = sdx * sdx + sdy * sdy;
    if (seg_len2 < 0.001f) return;

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float px = (x + 0.5f) - bx, py = (y + 0.5f) - by;
            float t = (px * sdx + py * sdy) / seg_len2;
            t = clampf(t, 0.0f, 1.0f);
            float projx = bx + t * sdx, projy = by + t * sdy;
            float ddx = (x + 0.5f) - projx, ddy = (y + 0.5f) - projy;
            float dist = sqrtf3_fast(ddx * ddx + ddy * ddy);

            /* Width tapers only across the forward (tip) portion of the hand. */
            float back_frac = back_len / (back_len + length);
            float taper_t = t <= back_frac ? 0.0f : (t - back_frac) / (1.0f - back_frac);
            float half_w = base_half_w + (tip_half_w - base_half_w) * taper_t;

            if (dist > half_w + 1.0f) continue;
            float edge = clampf(half_w + 0.5f - dist, 0.0f, 1.0f);
            uint8_t aa = (uint8_t)((float)a * edge);
            if (aa) blend(x, y, r, gv, b, aa);
        }
    }
}

/* ============================================================
   RTC read + weekday computation
   ============================================================ */

static void rtc_poll(void) {
    if (g.rtc_fd < 0) return;
    rtc_time_t t;
    ssize_t n = read(g.rtc_fd, &t, sizeof(t));
    if (n != (ssize_t)sizeof(t)) return;

    if (t.second != g.last_raw_sec) {
        g.last_raw_sec = t.second;
        g.sec_baseline_ms = time_ms();

        /* Push the previous second-hand angle into the comet trail. */
        float prev_sec_f = (float)g.now.second;
        uint8_t prev_angle = (uint8_t)(int)(prev_sec_f / 60.0f * 256.0f);
        if (g.trail_len < (int)(sizeof(g.trail) / sizeof(g.trail[0]))) {
            g.trail[g.trail_len++] = prev_angle;
        } else {
            for (int i = 1; i < g.trail_len; i++) g.trail[i - 1] = g.trail[i];
            g.trail[g.trail_len - 1] = prev_angle;
        }
    }
    g.now = t;
}

/* Zeller's congruence (Gregorian), 5*J form to avoid negative mod on
 * integer division. Returns 0=Saturday .. 6=Friday. */
static int day_of_week(int y, int m, int d) {
    if (m < 3) { m += 12; y -= 1; }
    int K = y % 100, J = y / 100;
    int h = (d + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
    return h;
}

static const char *weekday_name(int h) {
    static const char *names[7] = {
        "Saturday", "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday"
    };
    return names[h % 7];
}

static const char *month_name(uint8_t m) {
    static const char *names[] = {
        "???", "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };
    if (m < 1 || m > 12) return names[0];
    return names[m];
}

/* ============================================================
   Background
   ============================================================ */

static void draw_background(float cx, float cy, float max_r) {
    /* Deep radial gradient, navy center fading to near-black edges. */
    uint32_t row_cache_y = 0xFFFFFFFF;
    for (int y = 0; y < (int)g.sh; y++) {
        for (int x = 0; x < (int)g.sw; x++) {
            float dx = x - cx, dy = y - cy;
            float d = sqrtf3_fast(dx * dx + dy * dy);
            float t = clampf(d / (max_r * 2.4f), 0.0f, 1.0f);
            /* lerp between two navy tones */
            uint8_t r  = (uint8_t)(16  + (6  - 16)  * t + 16 * t * t);
            uint8_t gv = (uint8_t)(22  + (8  - 22)  * t + 10 * t * t);
            uint8_t b  = (uint8_t)(46  + (18 - 46)  * t + 4  * t * t);
            ((uint32_t *)(g.bb + (uint64_t)y * g.bb_pitch))[x] =
                0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)gv << 8) | b;
        }
    }
    (void)row_cache_y;
}

/* ============================================================
   Clock face
   ============================================================ */

static void draw_clock(void) {
    float cx = g.sw * 0.5f;
    float cy = g.sh * 0.5f - 20.0f; /* leave room for the digital pill below */
    float radius = ((g.sw < g.sh) ? (float)g.sw : (float)g.sh) * 0.34f;

    draw_background(cx, cy, radius);

    /* Outer soft glow (a few translucent rings, largest/faintest first). */
    draw_ring(cx, cy, radius + 10, 18, 60, 200, 255, 18);
    draw_ring(cx, cy, radius + 4,  8,  80, 220, 255, 30);

    /* Face + rim */
    draw_disc(cx, cy, radius, 14, 18, 30, 235);
    draw_ring(cx, cy, radius, 3, 120, 235, 255, 220);

    /* Ticks: 60 minor, 12 major, cardinal points accented gold. */
    for (int i = 0; i < 60; i++) {
        uint8_t ang = (uint8_t)(i * 256 / 60);
        float dx = isin(ang) / 127.0f, dy = -icos(ang) / 127.0f;
        int major = (i % 5 == 0);
        int cardinal = (i % 15 == 0);
        float r0 = radius - (major ? 16.0f : 8.0f);
        float r1 = radius - 4.0f;
        float half_w = major ? 2.0f : 1.0f;
        uint8_t cr, cg, cb, ca;
        if (cardinal)      { cr = 255; cg = 205; cb = 90;  ca = 255; }
        else if (major)    { cr = 230; cg = 235; cb = 245; ca = 220; }
        else               { cr = 140; cg = 150; cb = 165; ca = 140; }

        int steps = (int)(r1 - r0);
        if (steps < 1) steps = 1;
        for (int s = 0; s <= steps; s++) {
            float rr = r0 + (r1 - r0) * ((float)s / (float)steps);
            float px = cx + dx * rr, py = cy + dy * rr;
            draw_disc(px, py, half_w, cr, cg, cb, ca);
        }
    }

    /* Hour numerals (skipped gracefully if no font could be loaded). */
    if (g.font) {
        for (int h = 1; h <= 12; h++) {
            uint8_t ang = (uint8_t)(h * 256 / 12);
            float dx = isin(ang) / 127.0f, dy = -icos(ang) / 127.0f;
            float rr = radius - 34.0f;
            float px = cx + dx * rr, py = cy + dy * rr;
            char buf[4];
            int n = 0;
            if (h >= 10) buf[n++] = '0' + (h / 10);
            buf[n++] = '0' + (h % 10);
            buf[n] = 0;
            int tw = text_width(buf);
            draw_text((int)px - tw / 2, (int)py - 6, buf, 0xFFE8ECF5);
        }
    }

    /* Comet trail behind the second hand — faint, fading dots along the rim. */
    for (int i = 0; i < g.trail_len; i++) {
        uint8_t ang = g.trail[g.trail_len - 1 - i];
        float dx = isin(ang) / 127.0f, dy = -icos(ang) / 127.0f;
        float rr = radius - 20.0f;
        float px = cx + dx * rr, py = cy + dy * rr;
        uint8_t a = (uint8_t)(90 - i * (90 / (int)(sizeof(g.trail) / sizeof(g.trail[0]))));
        draw_disc(px, py, 3.0f, 255, 90, 90, a);
    }

    /* Smooth fractional time for sweeping hands. */
    float frac = clampf((float)(time_ms() - g.sec_baseline_ms) / 1000.0f, 0.0f, 1.0f);
    float sec_f  = (float)g.now.second + frac;
    float min_f  = (float)g.now.minute + sec_f / 60.0f;
    float hour_f = (float)(g.now.hour % 12) + min_f / 60.0f;

    uint8_t a_hour = (uint8_t)(int)(hour_f / 12.0f * 256.0f);
    uint8_t a_min  = (uint8_t)(int)(min_f  / 60.0f * 256.0f);
    uint8_t a_sec  = (uint8_t)(int)(sec_f  / 60.0f * 256.0f);

    /* Hour hand */
    draw_hand(cx, cy, a_hour, radius * 0.50f, radius * 0.12f, 5.5f, 2.0f, 235, 235, 245, 255);
    /* Minute hand */
    draw_hand(cx, cy, a_min,  radius * 0.74f, radius * 0.14f, 4.0f, 1.5f, 235, 235, 245, 255);
    /* Second hand: soft glow pass, then sharp accent-coloured pass */
    draw_hand(cx, cy, a_sec, radius * 0.82f, radius * 0.18f, 4.0f, 2.5f, 255, 90, 90, 40);
    draw_hand(cx, cy, a_sec, radius * 0.82f, radius * 0.18f, 1.6f, 0.8f, 255, 110, 110, 255);

    /* Hub */
    draw_disc(cx, cy, 7.0f, 20, 24, 34, 255);
    draw_ring(cx, cy, 7.0f, 2.0f, 255, 110, 110, 255);
    draw_disc(cx, cy, 2.5f, 255, 230, 230, 255);
}

/* ============================================================
   Digital readout pill + hint
   ============================================================ */

static void draw_digital_pill(void) {
    if (!g.font) return;

    char timebuf[16];
    int hh = g.now.hour;
    const char *suffix = "";
    if (!g.fmt24) {
        suffix = (hh >= 12) ? " PM" : " AM";
        hh = hh % 12; if (hh == 0) hh = 12;
    }
    /* HH:MM:SS (+AM/PM) */
    int n = 0;
    timebuf[n++] = '0' + (hh / 10);
    timebuf[n++] = '0' + (hh % 10);
    timebuf[n++] = ':';
    timebuf[n++] = '0' + (g.now.minute / 10);
    timebuf[n++] = '0' + (g.now.minute % 10);
    timebuf[n++] = ':';
    timebuf[n++] = '0' + (g.now.second / 10);
    timebuf[n++] = '0' + (g.now.second % 10);
    timebuf[n] = 0;

    char linebuf[24];
    {
        int i = 0;
        while (timebuf[i]) { linebuf[i] = timebuf[i]; i++; }
        const char *s = suffix;
        while (*s) linebuf[i++] = *s++;
        linebuf[i] = 0;
    }

    char datebuf[64];
    {
        const char *wd = weekday_name(day_of_week((int)g.now.year, g.now.month, g.now.day));
        const char *mo = month_name(g.now.month);
        int i = 0;
        const char *s = wd; while (*s) datebuf[i++] = *s++;
        datebuf[i++] = ','; datebuf[i++] = ' ';
        s = mo; while (*s) datebuf[i++] = *s++;
        datebuf[i++] = ' ';
        datebuf[i++] = '0' + (g.now.day / 10);
        datebuf[i++] = '0' + (g.now.day % 10);
        datebuf[i++] = ',';
        datebuf[i++] = ' ';
        uint32_t y = g.now.year;
        char ybuf[6]; int yn = 0;
        if (y == 0) { ybuf[yn++] = '0'; }
        while (y) { ybuf[yn++] = '0' + (y % 10); y /= 10; }
        while (yn > 0) datebuf[i++] = ybuf[--yn];
        datebuf[i] = 0;
    }

    int tw1 = text_width(linebuf) * 2; /* time drawn at 2x scale below */
    int tw2 = text_width(datebuf);
    int pill_w = (tw1 > tw2 ? tw1 : tw2) + 48;
    int pill_h = 62;
    int px = (int)g.sw / 2 - pill_w / 2;
    int py = (int)g.sh - pill_h - 24;

    /* Rounded-ish translucent pill via a rect + disc caps */
    fill(px + 10, py, pill_w - 20, pill_h, 0x00000000);
    for (int y = py; y < py + pill_h; y++)
        for (int x = px; x < px + pill_w; x++)
            blend(x, y, 8, 10, 18, 165);
    draw_disc(px + 10,           py + pill_h / 2, pill_h / 2.0f, 8, 10, 18, 165);
    draw_disc(px + pill_w - 10,  py + pill_h / 2, pill_h / 2.0f, 8, 10, 18, 165);

    /* Time, drawn scaled up for emphasis */
    {
        int cx = (int)g.sw / 2;
        int cxx = cx - tw1 / 2;
        int cy = py + 8;
        uint8_t r = 235, gv = 240, b = 255;
        const char *s = linebuf;
        int cxp = cxx;
        while (*s) {
            fnt_glyph_t *gl = fnt_get_glyph(g.font, (uint32_t)(unsigned char)*s);
            if (gl) {
                for (int dy = 0; dy < gl->bitmap_height; dy++)
                    for (int dx = 0; dx < gl->bitmap_width; dx++)
                        if (fnt_get_pixel(gl, dx, dy))
                            fill(cxp + dx * 2, cy + dy * 2, 2, 2,
                                 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)gv << 8) | b);
                cxp += gl->width * 2;
            }
            s++;
        }
    }

    draw_text_centered((int)g.sw / 2, py + 40, datebuf, 0xFFAAB3C8);

    draw_text(16, (int)g.sh - 20, "ESC/Q: quit   T: toggle 12h/24h", 0xFF3A4256);
}

/* ============================================================
   Font loading (Terminus preferred, Unicode.fnt fallback)
   ============================================================ */

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
   Entry point
   ============================================================ */

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;
    memset(&g, 0, sizeof(g));
    g.fmt24 = 1;
    g.last_raw_sec = -1;

    int efd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    if (efd < 0) { printf("gfxclock: no event device\n"); sleep(2); return 2; }

    g.rtc_fd = open(RTC_DEV_PATH, O_RDONLY, 0);
    if (g.rtc_fd < 0) {
        printf("gfxclock: failed to open " RTC_DEV_PATH "\n");
        close(efd);
        return 1;
    }

    if (NodGL_CreateDevice(NodGL_FEATURE_LEVEL_1_0, &g.device, &g.ctx, NULL) != NodGL_OK) {
        printf("gfxclock: NodGL failed\n");
        close(g.rtc_fd); close(efd);
        return 1;
    }
    NodGL_GetScreenResolution(g.device, &g.sw, &g.sh);

    NodGL_TextureDesc td; memset(&td, 0, sizeof(td));
    td.width = g.sw; td.height = g.sh;
    td.format = NodGL_FORMAT_R8G8B8A8_UNORM; td.mip_levels = 1;
    if (NodGL_CreateTexture(g.device, &td, &g.tex) != NodGL_OK) {
        NodGL_ReleaseDevice(g.device); close(g.rtc_fd); close(efd);
        return 1;
    }
    if (NodGL_MapResource(g.ctx, g.tex, (void **)&g.bb, &g.bb_pitch) != NodGL_OK) {
        NodGL_ReleaseResource(g.device, g.tex);
        NodGL_ReleaseDevice(g.device); close(g.rtc_fd); close(efd);
        return 1;
    }

    /* Font: Terminus first, Unicode.fnt fallback. HUD/numerals degrade
     * gracefully (hands + ticks still render) if neither is available. */
    g.font = try_load_font(FONT_PRIMARY);
    if (!g.font) g.font = try_load_font(FONT_FALLBACK);

    /* Prime the first RTC reading before the first frame draws. */
    rtc_poll();
    g.sec_baseline_ms = time_ms();

    g.quit = 0;
    while (!g.quit) {
        Event ev;
        while (read(efd, &ev, sizeof(ev)) > 0) {
            if (ev.type == EVENT_KEY_PRESSED) {
                char ch = ev.data.keyboard.ascii;
                if (ch == 'q' || ch == 'Q' || ch == 0x1B) {
                    g.quit = 1; break;
                } else if (ch == 't' || ch == 'T') {
                    g.fmt24 = !g.fmt24;
                }
            }
        }
        if (g.quit) break;

        rtc_poll();
        draw_clock();
        draw_digital_pill();

        NodGL_DrawTexture(g.ctx, g.tex, 0, 0, 0, 0, g.sw, g.sh);
        NodGL_PresentContext(g.ctx, 1); /* vsync */
        yield();
    }

    if (g.font) fnt_free_font(g.font);
    if (g.bb)   NodGL_UnmapResource(g.ctx, g.tex);
    NodGL_ReleaseResource(g.device, g.tex);
    NodGL_ReleaseDevice(g.device);
    close(g.rtc_fd);
    close(efd);
    input_flush();
    return 0;
}