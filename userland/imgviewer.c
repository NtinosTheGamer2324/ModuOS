#include "libc.h"
#include "string.h"
#include "../include/moduos/kernel/events/events.h"

/*
 * imgviewer.sqr
 *
 * Classic-style BMP image viewer:
 *  - BMP only (24bpp, 32bpp)
 *  - zoom + pan
 *
 * Controls:
 *  - Drag with left mouse: pan
 *  - '+' / '-': zoom in/out
 *  - 'f': toggle fit-to-screen
 *  - ESC: exit
 */

static inline uint32_t pack_xrgb8888(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

#define CURSOR_W 16u
#define CURSOR_H 16u

/* helpers used by both cursor bmp loader and main bmp loader */
static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline int32_t rds32(const uint8_t *p) { return (int32_t)rd32(p); }
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

typedef struct {
    uint32_t w;
    uint32_t h;
    uint8_t rgba[CURSOR_W * CURSOR_H * 4u];
} cursor_img_t;

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
    uint16_t planes = rd16(buf + 26);
    uint16_t bpp = rd16(buf + 28);

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

/* Minimal BMP loader into XRGB8888 */
typedef struct {
    uint32_t w;
    uint32_t h;
    uint16_t bpp;
    uint32_t comp;
    int top_down;
    uint32_t pixel_off;
    uint32_t src_row_stride;
    uint8_t *buf;
    uint32_t size;
} bmp_img_t;

static uint8_t scale_masked(uint32_t v, uint32_t mask) {
    if (mask == 0) return 0;
    uint32_t m = mask;
    uint32_t shift = 0;
    while ((m & 1u) == 0u) { m >>= 1; shift++; }
    uint32_t bits = 0;
    uint32_t tmp = m;
    while (tmp) { bits++; tmp >>= 1; }
    uint32_t val = (v & mask) >> shift;
    uint32_t max = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    return (uint8_t)((val * 255u) / (max ? max : 1u));
}

static int load_bmp(const char *path, bmp_img_t *out) {
    memset(out, 0, sizeof(*out));

    fs_file_info_t st;
    if (stat(path, &st) != 0) return -1;
    if (st.is_directory) return -2;

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -3;

    uint32_t size = st.size;
    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) { close(fd); return -4; }

    uint32_t got = 0;
    while (got < size) {
        ssize_t n = read(fd, buf + got, size - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    close(fd);

    if (got != size || size < 54) { free(buf); return -5; }
    if (buf[0] != 'B' || buf[1] != 'M') { free(buf); return -6; }

    uint32_t pixel_off = rd32(buf + 10);
    uint32_t dib_size = rd32(buf + 14);
    if (dib_size < 40 || 14u + dib_size > size) { free(buf); return -7; }

    int32_t w = rds32(buf + 18);
    int32_t h = rds32(buf + 22);
    uint16_t planes = rd16(buf + 26);
    uint16_t bpp = rd16(buf + 28);
    uint32_t comp = rd32(buf + 30);

    if (planes != 1 || w <= 0 || h == 0) { free(buf); return -8; }

    uint32_t width = (uint32_t)w;
    uint32_t height = (uint32_t)(h < 0 ? -h : h);
    int top_down = (h < 0);

    uint32_t src_row_stride;
    if (bpp == 24) src_row_stride = ((width * 3u + 3u) / 4u) * 4u;
    else if (bpp == 32) src_row_stride = width * 4u;
    else { free(buf); return -9; }

    if (pixel_off + (uint64_t)src_row_stride * height > size) { free(buf); return -10; }

    out->w = width;
    out->h = height;
    out->bpp = bpp;
    out->comp = comp;
    out->top_down = top_down;
    out->pixel_off = pixel_off;
    out->src_row_stride = src_row_stride;
    out->buf = buf;
    out->size = size;
    return 0;
}

static void free_bmp(bmp_img_t *img) {
    if (img && img->buf) free(img->buf);
    if (img) memset(img, 0, sizeof(*img));
}

static uint32_t bmp_get_pixel_xrgb(const bmp_img_t *bmp, uint32_t x, uint32_t y) {
    uint32_t rmask=0, gmask=0, bmask=0;
    if (bmp->bpp == 32 && (bmp->comp == 3 || bmp->comp == 6)) {
        rmask = rd32(bmp->buf + 54);
        gmask = rd32(bmp->buf + 58);
        bmask = rd32(bmp->buf + 62);
    }

    uint32_t sy = bmp->top_down ? y : (bmp->h - 1u - y);
    const uint8_t *row = bmp->buf + bmp->pixel_off + (uint64_t)sy * bmp->src_row_stride;

    uint8_t r,g,b;
    if (bmp->bpp == 24) {
        b = row[x*3u + 0];
        g = row[x*3u + 1];
        r = row[x*3u + 2];
    } else {
        uint32_t px = rd32(row + x*4u);
        if (bmp->comp == 3 || bmp->comp == 6) {
            r = scale_masked(px, rmask);
            g = scale_masked(px, gmask);
            b = scale_masked(px, bmask);
        } else {
            b = (uint8_t)(px & 0xFF);
            g = (uint8_t)((px >> 8) & 0xFF);
            r = (uint8_t)((px >> 16) & 0xFF);
        }
    }

    return pack_xrgb8888(r,g,b);
}

int md_main(long argc, char **argv) {
    if (argc < 2) {
        puts_raw("imgviewer: usage: imgviewer <file.bmp>\n");
        return 1;
    }

    md64api_grp_video_info_t vi;
    memset(&vi, 0, sizeof(vi));
    if (md64api_grp_get_video0_info(&vi) != 0) {
        puts_raw("imgviewer: cannot read video info\n");
        return 2;
    }
    if (vi.mode != MD64API_GRP_MODE_GRAPHICS || vi.bpp != 32) {
        puts_raw("imgviewer: requires 32bpp graphics\n");
        return 0;
    }

    bmp_img_t bmp;
    int rc = load_bmp(argv[1], &bmp);
    if (rc != 0) {
        puts_raw("imgviewer: failed to load bmp\n");
        return 3;
    }

    uint32_t pitch = vi.width * 4u;
    uint32_t buf_size = pitch * vi.height;
    uint8_t *bb = (uint8_t*)malloc(buf_size);
    if (!bb) {
        free_bmp(&bmp);
        return 4;
    }

    int efd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    if (efd < 0) {
        free(bb);
        free_bmp(&bmp);
        return 5;
    }

    /* view state */
    int fit = 1;
    int zoom = 100; /* percent */
    int32_t pan_x = 0;
    int32_t pan_y = 0;

    int32_t mx = (int32_t)(vi.width / 2u);
    int32_t my = (int32_t)(vi.height / 2u);
    uint8_t buttons = 0, prev_buttons = 0;
    int dragging = 0;
    int32_t drag_start_mx = 0, drag_start_my = 0;
    int32_t drag_start_panx = 0, drag_start_pany = 0;

    for (;;) {
        for (;;) {
            Event e;
            ssize_t n = read(efd, &e, sizeof(e));
            if (n != (ssize_t)sizeof(e)) break;

            if (e.type == EVENT_KEY_PRESSED && e.data.keyboard.keycode == KEY_ESCAPE) {
                close(efd);
                free(bb);
                free_bmp(&bmp);
                return 0;
            }

            if (e.type == EVENT_CHAR_INPUT) {
                char c = e.data.keyboard.ascii;
                if (c == '+' || c == '=') { zoom += 10; if (zoom > 800) zoom = 800; fit = 0; }
                else if (c == '-') { zoom -= 10; if (zoom < 10) zoom = 10; fit = 0; }
                else if (c == 'f' || c == 'F') { fit = !fit; }
                else if (c == '0') { zoom = 100; pan_x = pan_y = 0; fit = 0; }
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
        int left_prev = (prev_buttons & 1) != 0;
        int left_pressed = left_now && !left_prev;
        int left_released = !left_now && left_prev;

        if (left_pressed) {
            dragging = 1;
            drag_start_mx = mx;
            drag_start_my = my;
            drag_start_panx = pan_x;
            drag_start_pany = pan_y;
            fit = 0;
        }
        if (dragging && left_now) {
            pan_x = drag_start_panx + (mx - drag_start_mx);
            pan_y = drag_start_pany + (my - drag_start_my);
        }
        if (left_released) {
            dragging = 0;
        }

        prev_buttons = buttons;

        /* background frame (classic) */
        fill_rect(bb, pitch, vi.width, vi.height, 0, 0, (int32_t)vi.width, (int32_t)vi.height, pack_xrgb8888(192, 192, 192));
        fill_rect(bb, pitch, vi.width, vi.height, 0, 0, (int32_t)vi.width, 26, pack_xrgb8888(160, 160, 160));

        /* compute zoom */
        int32_t draw_w, draw_h;
        if (fit) {
            /* fit to screen (keep aspect) */
            uint32_t aw = vi.width;
            uint32_t ah = vi.height - 26;
            uint64_t sx = (uint64_t)aw * 1000ULL / (bmp.w ? bmp.w : 1u);
            uint64_t sy = (uint64_t)ah * 1000ULL / (bmp.h ? bmp.h : 1u);
            uint64_t s = sx < sy ? sx : sy;
            zoom = (int)(s / 10ULL);
            if (zoom < 1) zoom = 1;
        }

        draw_w = (int32_t)((bmp.w * (uint32_t)zoom) / 100u);
        draw_h = (int32_t)((bmp.h * (uint32_t)zoom) / 100u);
        if (draw_w <= 0) draw_w = 1;
        if (draw_h <= 0) draw_h = 1;

        int32_t ox = (int32_t)(vi.width/2u) - draw_w/2 + pan_x;
        int32_t oy = 26 + (int32_t)((vi.height - 26)/2u) - draw_h/2 + pan_y;

        /* draw image nearest-neighbor */
        for (int32_t dy = 0; dy < draw_h; dy++) {
            int32_t sy = (int32_t)(((int64_t)dy * (int64_t)bmp.h) / (draw_h ? draw_h : 1));
            if (sy < 0) sy = 0;
            if ((uint32_t)sy >= bmp.h) sy = (int32_t)bmp.h - 1;

            int32_t py = oy + dy;
            if ((uint32_t)py >= vi.height) continue;
            if (py < 26) continue;

            uint32_t *row = (uint32_t *)(bb + (uint64_t)py * pitch);
            for (int32_t dx = 0; dx < draw_w; dx++) {
                int32_t px = ox + dx;
                if ((uint32_t)px >= vi.width) continue;

                int32_t sx = (int32_t)(((int64_t)dx * (int64_t)bmp.w) / (draw_w ? draw_w : 1));
                if (sx < 0) sx = 0;
                if ((uint32_t)sx >= bmp.w) sx = (int32_t)bmp.w - 1;

                row[px] = bmp_get_pixel_xrgb(&bmp, (uint32_t)sx, (uint32_t)sy);
            }
        }

            /* cursor (arrow only) */
        static cursor_img_t cur_arrow;
        static int cur_loaded = 0;
        if (!cur_loaded) {
            cur_loaded = 1;
            (void)load_cursor_bmp_rgba8888("/ModuOS/shared/usr/assets/mouse/arrow.bmp", &cur_arrow);
        }
        alpha_blit_cursor_xrgb8888(bb, pitch, vi.width, vi.height, &cur_arrow, mx, my);

        (void)gfx_blit(bb, (uint16_t)vi.width, (uint16_t)vi.height, 0, 0, (uint16_t)pitch, (uint16_t)MD64API_GRP_FMT_XRGB8888);
        yield();
    }
}
