#include "libc.h"
#include "string.h"
#include "../include/moduos/kernel/events/events.h"
#include "xenith26_proto.h"
#include "xenith26_shm.h"

/*
 * xenithd.sqr
 *
 * Xenith26 display server (MVP step 1):
 *  - claims $/dev/gui0 server role
 *  - owns the framebuffer (via gfx_blit to $/dev/graphics/video0)
 *  - reads $/dev/input/event0
 *  - draws an animated desktop background + transparent cursor
 *
 * No client windows yet.
 */

#define CURSOR_W 16u
#define CURSOR_H 16u

typedef struct {
    uint32_t w;
    uint32_t h;
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
            /* BI_RGB assumed: B,G,R,A */
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

typedef struct {
    uint32_t w;
    uint32_t h;
    uint16_t bpp;
    uint32_t comp;
    int top_down;
    uint32_t pixel_off;
    uint32_t src_row_stride;
    /* Pixel data is BMP raw rows (BGR/BGRA) starting at buf[pixel_off]. */
    uint8_t *buf;
    uint32_t size;
} bmp_img_t;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

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

static void blit_wallpaper_xrgb8888(uint8_t *dst, uint32_t dst_pitch, uint32_t sw, uint32_t sh,
                                    const bmp_img_t *bmp) {
    /* Scale-to-fit (nearest neighbor): sample bmp -> screen */
    uint32_t rmask=0, gmask=0, bmask=0;
    if (bmp->bpp == 32 && (bmp->comp == 3 || bmp->comp == 6)) {
        rmask = rd32(bmp->buf + 54);
        gmask = rd32(bmp->buf + 58);
        bmask = rd32(bmp->buf + 62);
    }

    for (uint32_t dy = 0; dy < sh; dy++) {
        uint32_t sy = (uint32_t)(((uint64_t)dy * (uint64_t)bmp->h) / (sh ? sh : 1u));
        if (sy >= bmp->h) sy = bmp->h - 1u;
        uint32_t srcy = bmp->top_down ? sy : (bmp->h - 1u - sy);
        const uint8_t *row = bmp->buf + bmp->pixel_off + (uint64_t)srcy * bmp->src_row_stride;

        uint32_t *drow = (uint32_t *)(dst + (uint64_t)dy * dst_pitch);
        for (uint32_t dx = 0; dx < sw; dx++) {
            uint32_t sx = (uint32_t)(((uint64_t)dx * (uint64_t)bmp->w) / (sw ? sw : 1u));
            if (sx >= bmp->w) sx = bmp->w - 1u;

            uint8_t r,g,b;
            if (bmp->bpp == 24) {
                b = row[sx*3u + 0];
                g = row[sx*3u + 1];
                r = row[sx*3u + 2];
            } else {
                uint32_t px = rd32(row + sx*4u);
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
            drow[dx] = pack_xrgb8888(r,g,b);
        }
    }
}

static void blit_wallpaper_rgb565(uint8_t *dst, uint32_t dst_pitch, uint32_t sw, uint32_t sh,
                                  const bmp_img_t *bmp) {
    /* Scale-to-fit (nearest neighbor) */
    uint32_t rmask=0, gmask=0, bmask=0;
    if (bmp->bpp == 32 && (bmp->comp == 3 || bmp->comp == 6)) {
        rmask = rd32(bmp->buf + 54);
        gmask = rd32(bmp->buf + 58);
        bmask = rd32(bmp->buf + 62);
    }

    for (uint32_t dy = 0; dy < sh; dy++) {
        uint32_t sy = (uint32_t)(((uint64_t)dy * (uint64_t)bmp->h) / (sh ? sh : 1u));
        if (sy >= bmp->h) sy = bmp->h - 1u;
        uint32_t srcy = bmp->top_down ? sy : (bmp->h - 1u - sy);
        const uint8_t *row = bmp->buf + bmp->pixel_off + (uint64_t)srcy * bmp->src_row_stride;

        uint16_t *drow = (uint16_t *)(dst + (uint64_t)dy * dst_pitch);
        for (uint32_t dx = 0; dx < sw; dx++) {
            uint32_t sx = (uint32_t)(((uint64_t)dx * (uint64_t)bmp->w) / (sw ? sw : 1u));
            if (sx >= bmp->w) sx = bmp->w - 1u;

            uint8_t r,g,b;
            if (bmp->bpp == 24) {
                b = row[sx*3u + 0];
                g = row[sx*3u + 1];
                r = row[sx*3u + 2];
            } else {
                uint32_t px = rd32(row + sx*4u);
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
            drow[dx] = pack_rgb565(r,g,b);
        }
    }
}

static void draw_gradient_xrgb8888(uint8_t *fb, uint32_t pitch, uint32_t w, uint32_t h, uint32_t t) {
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(fb + (uint64_t)y * pitch);
        for (uint32_t x = 0; x < w; x++) {
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

static void claim_gui0_server(int gui_fd) {
    x26_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.magic = X26_MAGIC;
    h.type = X26_MSG_CLAIM_SERVER;
    h.flags = O_NONBLOCK; /* make server reads nonblocking */
    h.size = (uint32_t)sizeof(h);
    (void)write(gui_fd, &h, sizeof(h));
}

static int parse_u32(const char *s, uint32_t *out) {
    if (!s || !*s || !out) return -1;
    uint32_t v = 0;
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        if (c < '0' || c > '9') return -1;
        v = v * 10u + (uint32_t)(c - '0');
    }
    *out = v;
    return 0;
}

static void blit_at_xrgb8888(uint8_t *dst, uint32_t dst_pitch, uint32_t sw, uint32_t sh,
                              int32_t ox, int32_t oy,
                              const uint8_t *src, uint32_t src_stride, uint32_t bw, uint32_t bh) {
    for (uint32_t y = 0; y < bh; y++) {
        int32_t dy = oy + (int32_t)y;
        if ((uint32_t)dy >= sh) continue;
        const uint8_t *srow = src + (uint64_t)y * src_stride;
        uint32_t *drow = (uint32_t *)(dst + (uint64_t)dy * dst_pitch);
        for (uint32_t x = 0; x < bw; x++) {
            int32_t dx = ox + (int32_t)x;
            if ((uint32_t)dx >= sw) continue;
            drow[dx] = ((const uint32_t*)srow)[x];
        }
    }
}

int md_main(long argc, char **argv) {

    md64api_grp_video_info_t vi;
    memset(&vi, 0, sizeof(vi));
    if (md64api_grp_get_video0_info(&vi) != 0) {
        puts_raw("xenithd: cannot read video0 info\n");
        return 1;
    }
    if (vi.mode != MD64API_GRP_MODE_GRAPHICS) {
        puts_raw("xenithd: not in graphics mode\n");
        return 0;
    }

    uint8_t fmt = vi.fmt;
    if (fmt == MD64API_GRP_FMT_UNKNOWN) {
        if (vi.bpp == 32) fmt = MD64API_GRP_FMT_XRGB8888;
        else if (vi.bpp == 16) fmt = MD64API_GRP_FMT_RGB565;
    }

    uint32_t bpp_bytes = (fmt == MD64API_GRP_FMT_RGB565) ? 2u : 4u;
    uint32_t pitch = vi.width * bpp_bytes;
    uint32_t buf_size = pitch * vi.height;

    uint8_t *bb = (uint8_t *)malloc(buf_size);
    if (!bb) {
        puts_raw("xenithd: out of memory\n");
        return 2;
    }

    int ev_fd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    if (ev_fd < 0) {
        puts_raw("xenithd: cannot open $/dev/input/event0\n");
        free(bb);
        return 3;
    }

    int gui_fd = open("$/dev/gui0", O_RDWR | O_NONBLOCK, 0);
    if (gui_fd < 0) {
        puts_raw("xenithd: cannot open $/dev/gui0\n");
        close(ev_fd);
        free(bb);
        return 4;
    }
    claim_gui0_server(gui_fd);

    /* Xenith26 window server state (MVP) */
    uint32_t wm_pid = 0;
    struct Win {
        uint32_t id;
        int32_t x, y;
        uint32_t w, h;
        uint32_t buf_id;
        uint8_t *buf_ptr; /* mapped RO */
        uint32_t stride;
        int mapped;
    } wins[32];
    int win_count = 0;
    uint32_t next_win_id = 1;

    /* Backwards-compatible overlay test: xenithd <buf_id>
     * Creates an implicit window for that buffer.
     */
    if (argc >= 2) {
        uint32_t test_buf_id = 0;
        if (parse_u32(argv[1], &test_buf_id) == 0 && test_buf_id != 0) {
            struct Win *w = &wins[win_count++];
            memset(w, 0, sizeof(*w));
            w->id = next_win_id++;
            w->x = (int32_t)(vi.width / 2u) - 128;
            w->y = (int32_t)(vi.height / 2u) - 128;
            w->w = 256;
            w->h = 256;
            w->buf_id = test_buf_id;
            w->stride = 256 * 4;

            x26_shm_map_req_t m;
            memset(&m, 0, sizeof(m));
            m.buf_id = test_buf_id;
            m.flags = 0; /* RO */
            /* server maps at upper half of shm region to avoid colliding with client mappings */
            m.preferred_addr = X26_SHM_BASE + 0x08000000ULL;
            if (x26_shm_map_u(&m) == 0) {
                w->buf_ptr = (uint8_t *)(uintptr_t)m.mapped_addr;
                w->mapped = 1;
                puts_raw("xenithd: mapped test buffer into implicit window\n");
            }
        }
    }

    /* Wallpaper disabled for now (FS/AHCI performance). Use gradient background. */
    uint8_t *wall_cache = NULL;

    cursor_img_t cursor;
    int have_cursor = (load_cursor_bmp_rgba8888("/ModuOS/shared/usr/assets/mouse/arrow.bmp", &cursor) == 0);

    int32_t mx = (int32_t)(vi.width / 2u);
    int32_t my = (int32_t)(vi.height / 2u);
    uint16_t buttons = 0;

    puts_raw("xenithd: Xenith26 server running (desktop+cursor). ESC exits.\n");

    for (;;) {
        /* pump input: update cursor and forward mouse to WM only (MVP) */
        for (;;) {
            Event e;
            ssize_t n = read(ev_fd, &e, sizeof(e));
            if (n != (ssize_t)sizeof(e)) break;

            if (e.type == EVENT_KEY_PRESSED && e.data.keyboard.keycode == KEY_ESCAPE) {
                puts_raw("xenithd: exit\n");
                close(gui_fd);
                close(ev_fd);
                free(bb);
                if (wall_cache) free(wall_cache);
                return 0;
            }

            if (e.type == EVENT_MOUSE_MOVE) {
                mx += e.data.mouse.delta_x;
                my += e.data.mouse.delta_y;
                mx = clamp_i32(mx, 0, (int32_t)vi.width - 1);
                my = clamp_i32(my, 0, (int32_t)vi.height - 1);
                buttons = e.data.mouse.buttons;
            } else if (e.type == EVENT_MOUSE_BUTTON) {
                buttons = e.data.mouse.buttons;
            }

            if (wm_pid && (e.type == EVENT_MOUSE_MOVE || e.type == EVENT_MOUSE_BUTTON)) {
                x26_input_event_t ie;
                memset(&ie, 0, sizeof(ie));
                ie.type = (uint16_t)e.type;
                ie.dx = e.data.mouse.delta_x;
                ie.dy = e.data.mouse.delta_y;
                ie.x = (int16_t)mx;
                ie.y = (int16_t)my;
                ie.buttons = buttons;

                uint8_t msg[sizeof(x26_hdr_t) + sizeof(ie)];
                x26_hdr_t h;
                memset(&h, 0, sizeof(h));
                h.magic = X26_MAGIC;
                h.type = X26_MSG_INPUT_EVENT;
                h.size = (uint32_t)(sizeof(h) + sizeof(ie));
                h.dst_pid = wm_pid;

                memcpy(msg, &h, sizeof(h));
                memcpy(msg + sizeof(h), &ie, sizeof(ie));
                (void)write(gui_fd, msg, h.size);
            }
        }

        /* pump gui0 messages */
        for (;;) {
            uint8_t msg[256];
            ssize_t n = read(gui_fd, msg, sizeof(msg));
            if (n == 0) break;
            if (n < (ssize_t)sizeof(x26_hdr_t)) break;

            x26_hdr_t *h = (x26_hdr_t*)msg;
            if (h->magic != X26_MAGIC) continue;
            uint32_t payload_len = h->size - (uint32_t)sizeof(x26_hdr_t);
            uint8_t *payload = msg + sizeof(x26_hdr_t);

            if (h->type == X26_MSG_REGISTER_WM) {
                wm_pid = h->src_pid;
                puts_raw("xenithd: WM registered\n");
            } else if (h->type == X26_MSG_CREATE_WINDOW && payload_len >= sizeof(x26_create_window_t)) {
                if (win_count >= 32) continue;
                x26_create_window_t *cw = (x26_create_window_t*)payload;
                struct Win *w = &wins[win_count++];
                memset(w, 0, sizeof(*w));
                w->id = next_win_id++;
                w->w = cw->w;
                w->h = cw->h;
                w->x = 50 + (int32_t)(win_count * 20);
                w->y = 50 + (int32_t)(win_count * 20);

                x26_window_created_t wc;
                wc.id = w->id;
                wc.w = (uint16_t)w->w;
                wc.h = (uint16_t)w->h;

                /* Reply to creating client */
                {
                    uint8_t out[sizeof(x26_hdr_t) + sizeof(wc)];
                    x26_hdr_t oh;
                    memset(&oh, 0, sizeof(oh));
                    oh.magic = X26_MAGIC;
                    oh.type = X26_MSG_WINDOW_CREATED;
                    oh.size = (uint32_t)(sizeof(oh) + sizeof(wc));
                    oh.dst_pid = h->src_pid;
                    memcpy(out, &oh, sizeof(oh));
                    memcpy(out + sizeof(oh), &wc, sizeof(wc));
                    (void)write(gui_fd, out, oh.size);
                }

                /* Broadcast window created to WM (and anyone else) */
                {
                    uint8_t out[sizeof(x26_hdr_t) + sizeof(wc)];
                    x26_hdr_t oh;
                    memset(&oh, 0, sizeof(oh));
                    oh.magic = X26_MAGIC;
                    oh.type = X26_MSG_WINDOW_CREATED;
                    oh.size = (uint32_t)(sizeof(oh) + sizeof(wc));
                    oh.dst_pid = X26_BROADCAST_PID;
                    memcpy(out, &oh, sizeof(oh));
                    memcpy(out + sizeof(oh), &wc, sizeof(wc));
                    (void)write(gui_fd, out, oh.size);
                }
            } else if (h->type == X26_MSG_MAP_WINDOW && payload_len >= sizeof(x26_map_window_t)) {
                x26_map_window_t *mw = (x26_map_window_t*)payload;
                for (int i = 0; i < win_count; i++) {
                    if (wins[i].id == mw->id) {
                        wins[i].x = mw->x;
                        wins[i].y = mw->y;
                    }
                }
            } else if (h->type == X26_MSG_ATTACH_BUFFER && payload_len >= sizeof(x26_attach_buffer_t)) {
                x26_attach_buffer_t *ab = (x26_attach_buffer_t*)payload;
                for (int i = 0; i < win_count; i++) {
                    if (wins[i].id == ab->id) {
                        wins[i].buf_id = ab->buf_id;
                        /* map RO for server at unique slot */
                        x26_shm_map_req_t m;
                        memset(&m, 0, sizeof(m));
                        m.buf_id = ab->buf_id;
                        m.flags = 0;
                        /* server maps at upper half of shm region to avoid colliding with client mappings */
                        m.preferred_addr = X26_SHM_BASE + 0x08000000ULL + (uint64_t)i * 0x00200000ULL;
                        if (x26_shm_map_u(&m) == 0) {
                            wins[i].buf_ptr = (uint8_t *)(uintptr_t)m.mapped_addr;
                            wins[i].stride = wins[i].w * 4;
                            wins[i].mapped = 1;
                        }
                    }
                }
            } else if (h->type == X26_MSG_MOVE_WINDOW && payload_len >= sizeof(x26_move_window_t)) {
                x26_move_window_t *mv = (x26_move_window_t*)payload;
                for (int i = 0; i < win_count; i++) {
                    if (wins[i].id == mv->id) {
                        wins[i].x = mv->x;
                        wins[i].y = mv->y;
                        break;
                    }
                }
            } else if (h->type == X26_MSG_RAISE_WINDOW && payload_len >= sizeof(x26_window_id_t)) {
                x26_window_id_t *rid = (x26_window_id_t*)payload;
                for (int i = 0; i < win_count; i++) {
                    if (wins[i].id == rid->id) {
                        /* move to end for topmost */
                        struct Win tmp = wins[i];
                        for (int j = i; j < win_count - 1; j++) wins[j] = wins[j + 1];
                        wins[win_count - 1] = tmp;
                        break;
                    }
                }
            } else if (h->type == X26_MSG_PRESENT) {
                /* MVP: ignore damage; compositor redraws every frame */
            }
        }

        (void)buttons;

        uint32_t t = (uint32_t)time_ms();
        if (fmt == MD64API_GRP_FMT_XRGB8888 && vi.bpp == 32) {
            if (wall_cache) {
                memcpy(bb, wall_cache, buf_size);
            } else {
                draw_gradient_xrgb8888(bb, pitch, vi.width, vi.height, t);
            }
            /* Composite windows in z-order */
            for (int i = 0; i < win_count; i++) {
                struct Win *w = &wins[i];
                if (!w->mapped || !w->buf_ptr) continue;
                /* temporary: use xrgb8888 compositing only */
                blit_at_xrgb8888(bb, pitch, vi.width, vi.height, w->x, w->y, w->buf_ptr, w->stride, w->w, w->h);
            }
            if (have_cursor) alpha_blit_cursor_xrgb8888(bb, pitch, vi.width, vi.height, &cursor, mx, my);
        } else if (fmt == MD64API_GRP_FMT_RGB565 && vi.bpp == 16) {
            if (wall_cache) {
                memcpy(bb, wall_cache, buf_size);
            } else {
                draw_gradient_rgb565(bb, pitch, vi.width, vi.height, t);
            }
            if (have_cursor) alpha_blit_cursor_rgb565(bb, pitch, vi.width, vi.height, &cursor, mx, my);
        }

        (void)gfx_blit(bb, (uint16_t)vi.width, (uint16_t)vi.height, 0, 0, (uint16_t)pitch, (uint16_t)fmt);
        yield();
    }
}
