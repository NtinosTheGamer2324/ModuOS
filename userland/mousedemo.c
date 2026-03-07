#include "libc.h"
#include "NodGL.h"
#include "string.h"

/*
 * mousedemo.sqr
 *
 * Tiny graphics+input demo:
 *  - animated gradient background
 *  - cursor driven by EVENT_MOUSE_MOVE deltas from $/dev/input/event0
 *  - cursor is a 16x16 transparent BMP from /ModuOS/shared/usr/assets/mouse/
 */

#include "../include/moduos/kernel/events/events.h"

#define CURSOR_W 16u
#define CURSOR_H 16u

typedef struct {
    uint32_t w;
    uint32_t h;
    /* Premultiplied alpha not required; stored as straight RGBA8888 */
    uint8_t rgba[CURSOR_W * CURSOR_H * 4u];
} cursor_img_t;

static inline int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline uint32_t pack_xrgb8888(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline uint16_t pack_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t rr = (uint16_t)((r * 31u) / 255u);
    uint16_t gg = (uint16_t)((g * 63u) / 255u);
    uint16_t bb = (uint16_t)((b * 31u) / 255u);
    return (uint16_t)((rr << 11) | (gg << 5) | (bb));
}

static inline uint32_t u32_min(uint32_t a, uint32_t b) { return a < b ? a : b; }
static inline uint32_t u32_max(uint32_t a, uint32_t b) { return a > b ? a : b; }

static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline int32_t rds32(const uint8_t *p) {
    return (int32_t)rd32(p);
}

/* Load a 16x16 32bpp BMP into RGBA8888.
 * Supports BI_RGB and BI_BITFIELDS (same cases as bmpview.c).
 */
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
    uint32_t comp = rd32(buf + 30);

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

    uint32_t rmask = 0, gmask = 0, bmask = 0, amask = 0;
    if (comp == 3 || comp == 6) {
        /* Bitfields start at offset 54 for classic headers. bmpview.c handles this similarly. */
        rmask = rd32(buf + 54);
        gmask = rd32(buf + 58);
        bmask = rd32(buf + 62);
        if (dib_size >= 108) amask = rd32(buf + 66);
        else amask = 0;
    }

    uint32_t src_row_stride = width * 4u;
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
            uint8_t r, g, b, a;

            if (comp == 3 || comp == 6) {
                /* Mask/shift to 0..255. We do a simple normalize (mask assumed contiguous). */
                /* NOTE: cursor assets are expected to be standard BGRA, so this is fallback. */
                uint32_t rv = (rmask ? (px & rmask) : ((px >> 16) & 0xFF));
                uint32_t gv = (gmask ? (px & gmask) : ((px >> 8) & 0xFF));
                uint32_t bv = (bmask ? (px & bmask) : (px & 0xFF));
                r = (uint8_t)(rmask ? (rv ? 255 : 0) : rv);
                g = (uint8_t)(gmask ? (gv ? 255 : 0) : gv);
                b = (uint8_t)(bmask ? (bv ? 255 : 0) : bv);
                if (amask) {
                    uint32_t av = (px & amask);
                    a = (uint8_t)(av ? 255 : 0);
                } else {
                    a = 255;
                }
            } else {
                /* Common BI_RGB 32bpp BMP is B,G,R,A (or B,G,R,X). We'll treat last byte as alpha. */
                b = (uint8_t)(px & 0xFF);
                g = (uint8_t)((px >> 8) & 0xFF);
                r = (uint8_t)((px >> 16) & 0xFF);
                a = (uint8_t)((px >> 24) & 0xFF);
            }

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

            /* out = src*a + dst*(1-a) */
            uint32_t a = sa;
            uint32_t ia = 255u - a;
            uint8_t or = (uint8_t)((sr * a + dr * ia) / 255u);
            uint8_t og = (uint8_t)((sg * a + dg * ia) / 255u);
            uint8_t ob = (uint8_t)((sb * a + db * ia) / 255u);

            row[dx] = pack_xrgb8888(or, og, ob);
        }
    }
}

static void alpha_blit_cursor_rgb565(uint8_t *dst, uint32_t pitch, uint32_t w, uint32_t h,
                                     const cursor_img_t *cur, int32_t x, int32_t y) {
    for (uint32_t cy = 0; cy < cur->h; cy++) {
        int32_t dy = y + (int32_t)cy;
        if ((uint32_t)dy >= h) continue;
        uint16_t *row = (uint16_t *)(dst + (uint64_t)dy * pitch);

        for (uint32_t cx = 0; cx < cur->w; cx++) {
            int32_t dx = x + (int32_t)cx;
            if ((uint32_t)dx >= w) continue;

            uint32_t si = (cy * cur->w + cx) * 4u;
            uint8_t sr = cur->rgba[si + 0];
            uint8_t sg = cur->rgba[si + 1];
            uint8_t sb = cur->rgba[si + 2];
            uint8_t sa = cur->rgba[si + 3];
            if (sa == 0) continue;

            uint16_t dp = row[dx];
            uint8_t dr = (uint8_t)(((dp >> 11) & 31u) * 255u / 31u);
            uint8_t dg = (uint8_t)(((dp >> 5) & 63u) * 255u / 63u);
            uint8_t db = (uint8_t)((dp & 31u) * 255u / 31u);

            uint32_t a = sa;
            uint32_t ia = 255u - a;
            uint8_t or = (uint8_t)((sr * a + dr * ia) / 255u);
            uint8_t og = (uint8_t)((sg * a + dg * ia) / 255u);
            uint8_t ob = (uint8_t)((sb * a + db * ia) / 255u);

            row[dx] = pack_rgb565(or, og, ob);
        }
    }
}

static void draw_gradient_xrgb8888(uint8_t *fb, uint32_t pitch, uint32_t w, uint32_t h, uint32_t t) {
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(fb + (uint64_t)y * pitch);
        for (uint32_t x = 0; x < w; x++) {
            /* Smooth-ish animated gradient */
            uint8_t r = (uint8_t)(((x + (t >> 2)) * 255u) / (w ? w : 1));
            uint8_t g = (uint8_t)(((y + (t >> 3)) * 255u) / (h ? h : 1));
            uint8_t b = (uint8_t)(((x ^ y ^ (t >> 1)) & 0xFF));
            row[x] = pack_xrgb8888(r, g, b);
        }
    }
}

static void draw_gradient_rgb565(uint8_t *fb, uint32_t pitch, uint32_t w, uint32_t h, uint32_t t) {
    for (uint32_t y = 0; y < h; y++) {
        uint16_t *row = (uint16_t *)(fb + (uint64_t)y * pitch);
        for (uint32_t x = 0; x < w; x++) {
            uint8_t r = (uint8_t)(((x + (t >> 2)) * 255u) / (w ? w : 1));
            uint8_t g = (uint8_t)(((y + (t >> 3)) * 255u) / (h ? h : 1));
            uint8_t b = (uint8_t)(((x ^ y ^ (t >> 1)) & 0xFF));
            row[x] = pack_rgb565(r, g, b);
        }
    }
}

static void draw_cursor_xrgb8888(uint8_t *fb, uint32_t pitch, uint32_t w, uint32_t h,
                                 int32_t mx, int32_t my, uint8_t buttons) {
    /* simple crosshair + small box */
    const int r = 6;

    uint32_t col = pack_xrgb8888(255, 255, 255);
    uint32_t col_down = pack_xrgb8888(255, 220, 0);
    if (buttons) col = col_down;

    for (int dy = -r; dy <= r; dy++) {
        int32_t y = my + dy;
        if ((uint32_t)y >= h) continue;
        int32_t x = mx;
        if ((uint32_t)x >= w) continue;
        uint32_t *row = (uint32_t *)(fb + (uint64_t)y * pitch);
        row[x] = col;
    }

    for (int dx = -r; dx <= r; dx++) {
        int32_t x = mx + dx;
        if ((uint32_t)x >= w) continue;
        int32_t y = my;
        if ((uint32_t)y >= h) continue;
        uint32_t *row = (uint32_t *)(fb + (uint64_t)y * pitch);
        row[x] = col;
    }

    /* outline box (helpful on bright backgrounds) */
    for (int oy = -2; oy <= 2; oy++) {
        for (int ox = -2; ox <= 2; ox++) {
            int32_t x = mx + ox;
            int32_t y = my + oy;
            if ((uint32_t)x >= w || (uint32_t)y >= h) continue;
            uint32_t *row = (uint32_t *)(fb + (uint64_t)y * pitch);
            row[x] = pack_xrgb8888(0, 0, 0);
        }
    }
    /* center pixel on top */
    if ((uint32_t)mx < w && (uint32_t)my < h) {
        uint32_t *row = (uint32_t *)(fb + (uint64_t)my * pitch);
        row[mx] = col;
    }
}

static void draw_cursor_rgb565(uint8_t *fb, uint32_t pitch, uint32_t w, uint32_t h,
                               int32_t mx, int32_t my, uint8_t buttons) {
    const int r = 6;
    uint16_t col = pack_rgb565(255, 255, 255);
    uint16_t col_down = pack_rgb565(255, 220, 0);
    if (buttons) col = col_down;

    for (int dy = -r; dy <= r; dy++) {
        int32_t y = my + dy;
        if ((uint32_t)y >= h) continue;
        int32_t x = mx;
        if ((uint32_t)x >= w) continue;
        uint16_t *row = (uint16_t *)(fb + (uint64_t)y * pitch);
        row[x] = col;
    }

    for (int dx = -r; dx <= r; dx++) {
        int32_t x = mx + dx;
        if ((uint32_t)x >= w) continue;
        int32_t y = my;
        if ((uint32_t)y >= h) continue;
        uint16_t *row = (uint16_t *)(fb + (uint64_t)y * pitch);
        row[x] = col;
    }

    for (int oy = -2; oy <= 2; oy++) {
        for (int ox = -2; ox <= 2; ox++) {
            int32_t x = mx + ox;
            int32_t y = my + oy;
            if ((uint32_t)x >= w || (uint32_t)y >= h) continue;
            uint16_t *row = (uint16_t *)(fb + (uint64_t)y * pitch);
            row[x] = pack_rgb565(0, 0, 0);
        }
    }

    if ((uint32_t)mx < w && (uint32_t)my < h) {
        uint16_t *row = (uint16_t *)(fb + (uint64_t)my * pitch);
        row[mx] = col;
    }
}

static int poll_input_events(int efd, int32_t *mx, int32_t *my, uint8_t *buttons,
                             uint32_t sw, uint32_t sh) {
    if (efd < 0) return 0;

    int want_exit = 0;

    for (;;) {
        Event e;
        ssize_t n = read(efd, &e, sizeof(e));
        if (n != (ssize_t)sizeof(e)) break;

        if (e.type == EVENT_KEY_PRESSED && e.data.keyboard.keycode == KEY_ESCAPE) {
            want_exit = 1;
        } else if (e.type == EVENT_MOUSE_MOVE) {
            /* Track our own cursor position from deltas, and clamp every update.
             * This prevents the cursor from ever getting "lost" off-screen.
             */
            *mx += e.data.mouse.delta_x;
            *my += e.data.mouse.delta_y;
            *mx = clamp_i32(*mx, 0, (int32_t)sw - 1);
            *my = clamp_i32(*my, 0, (int32_t)sh - 1);

            /* Some producers also fill absolute x/y. If they do, use it as a resync. */
            if (e.data.mouse.x || e.data.mouse.y) {
                int32_t ax = e.data.mouse.x;
                int32_t ay = e.data.mouse.y;
                *mx = clamp_i32(ax, 0, (int32_t)sw - 1);
                *my = clamp_i32(ay, 0, (int32_t)sh - 1);
            }
        } else if (e.type == EVENT_MOUSE_BUTTON) {
            *buttons = e.data.mouse.buttons;
            if (e.data.mouse.x || e.data.mouse.y) {
                int32_t ax = e.data.mouse.x;
                int32_t ay = e.data.mouse.y;
                *mx = clamp_i32(ax, 0, (int32_t)sw - 1);
                *my = clamp_i32(ay, 0, (int32_t)sh - 1);
            }
        }
    }

    return want_exit;
}

int md_main(long argc, char **argv) {
    (void)argc;
    (void)argv;

    NodGL_Device device;
    NodGL_Context ctx;
    if (NodGL_CreateDevice(NodGL_FEATURE_LEVEL_1_0, &device, &ctx, NULL) != NodGL_OK) {
        puts_raw("mousedemo: NodGL init failed\n");
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

    uint8_t fmt = MD64API_GRP_FMT_XRGB8888;
    md64api_grp_video_info_t info;
    info.width = screen_w;
    info.height = screen_h;
    info.bpp = 32;

    int efd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    if (efd < 0) {
        NodGL_ReleaseResource(device, backbuffer_tex);
        NodGL_ReleaseDevice(device);
        puts_raw("mousedemo: cannot open $/dev/input/event0\n");
        return 3;
    }

    cursor_img_t cursor;
    int have_cursor = (load_cursor_bmp_rgba8888("/ModuOS/shared/usr/assets/mouse/arrow.bmp", &cursor) == 0);

    int32_t mx = (int32_t)(info.width / 2u);
    int32_t my = (int32_t)(info.height / 2u);
    uint8_t buttons = 0;

    puts_raw("mousedemo: running. Press ESC to exit.\n");

    for (;;) {
        if (poll_input_events(efd, &mx, &my, &buttons, info.width, info.height)) break;

        uint32_t t = (uint32_t)time_ms();
        if (fmt == MD64API_GRP_FMT_XRGB8888 && info.bpp == 32) {
            draw_gradient_xrgb8888(bb, pitch, info.width, info.height, t);
            if (have_cursor) {
                alpha_blit_cursor_xrgb8888(bb, pitch, info.width, info.height, &cursor, mx, my);
            } else {
                draw_cursor_xrgb8888(bb, pitch, info.width, info.height, mx, my, buttons);
            }
        } else if (fmt == MD64API_GRP_FMT_RGB565 && info.bpp == 16) {
            draw_gradient_rgb565(bb, pitch, info.width, info.height, t);
            if (have_cursor) {
                alpha_blit_cursor_rgb565(bb, pitch, info.width, info.height, &cursor, mx, my);
            } else {
                draw_cursor_rgb565(bb, pitch, info.width, info.height, mx, my, buttons);
            }
        } else {
            puts_raw("mousedemo: unsupported framebuffer format\n");
            break;
        }

        NodGL_DrawTexture(ctx, backbuffer_tex, 0, 0, 0, 0, info.width, info.height);
        NodGL_PresentContext(ctx, 0);

        /* Keep CPU usage reasonable */
        yield();
    }

    close(efd);
    NodGL_ReleaseResource(device, backbuffer_tex);
    NodGL_ReleaseDevice(device);
    puts_raw("mousedemo: exit\n");
    return 0;
}
