#include "libc.h"
#include "string.h"

/*
 * bmpview.sqr
 *
 * Minimal BMP viewer for ModuOS userland.
 *
 * Usage:
 *   bmpview /path/to/image.bmp [x] [y]
 *
 * Supports:
 *   - 24bpp BI_RGB
 *   - 32bpp BI_RGB
 *   - 32bpp BI_BITFIELDS (common VBE-friendly BMP)
 *
 * Draws directly into framebuffer from $/dev/graphics/video0.
 */

static int to_int(const char *s) {
    if (!s) return 0;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v*10 + (*s - '0'); s++; }
    return v * sign;
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t rr = (uint16_t)((r * 31u) / 255u);
    uint16_t gg = (uint16_t)((g * 63u) / 255u);
    uint16_t bb = (uint16_t)((b * 31u) / 255u);
    return (uint16_t)((rr << 11) | (gg << 5) | bb);
}

static uint32_t xrgb8888(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static int32_t rds32(const uint8_t *p) { return (int32_t)rd32(p); }

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

static int draw_bmp_to_fb(const uint8_t *buf, uint32_t size, int dstx, int dsty,
                          int key_enabled, uint8_t key_r, uint8_t key_g, uint8_t key_b) {
    if (!buf || size < 54) return -1;
    if (buf[0] != 'B' || buf[1] != 'M') return -2;

    uint32_t pixel_off = rd32(buf + 10);
    uint32_t dib_size = rd32(buf + 14);
    if (dib_size < 40) return -3;
    if (14u + dib_size > size) return -3;

    int32_t w = rds32(buf + 18);
    int32_t h = rds32(buf + 22);
    uint16_t planes = rd16(buf + 26);
    uint16_t bpp = rd16(buf + 28);
    uint32_t comp = rd32(buf + 30);

    if (planes != 1 || w <= 0 || h == 0) return -4;
    uint32_t width = (uint32_t)w;
    uint32_t height = (uint32_t)(h < 0 ? -h : h);
    int top_down = (h < 0);

    if (pixel_off >= size) return -5;

    /* framebuffer info */
    md64api_grp_video_info_t vi;
    if (md64api_grp_get_video0_info(&vi) != 0) return -10;
    if (vi.mode != MD64API_GRP_MODE_GRAPHICS || vi.fb_addr == 0) return -11;

    uint8_t fmt = vi.fmt;
    if (fmt == MD64API_GRP_FMT_UNKNOWN) {
        if (vi.bpp == 32) fmt = MD64API_GRP_FMT_XRGB8888;
        else if (vi.bpp == 16) fmt = MD64API_GRP_FMT_RGB565;
    }

    uint8_t *fb = (uint8_t*)(uintptr_t)vi.fb_addr;

    uint32_t rmask=0, gmask=0, bmask=0, amask=0;
    if (bpp == 32 && (comp == 3 || comp == 6)) {
        if (dib_size >= 108) {
            rmask = rd32(buf + 54);
            gmask = rd32(buf + 58);
            bmask = rd32(buf + 62);
            amask = rd32(buf + 66);
        } else {
            rmask = rd32(buf + 54);
            gmask = rd32(buf + 58);
            bmask = rd32(buf + 62);
            amask = 0;
        }
    }

    /* Row stride: BMP rows are padded to 4 bytes */
    uint32_t src_row_stride;
    if (bpp == 24) src_row_stride = ((width * 3u + 3u) / 4u) * 4u;
    else if (bpp == 32) src_row_stride = width * 4u;
    else return -6;

    if (pixel_off + (uint64_t)src_row_stride * height > size) return -7;

    for (uint32_t y = 0; y < height; y++) {
        uint32_t sy = top_down ? y : (height - 1u - y);
        const uint8_t *row = buf + pixel_off + (uint64_t)sy * src_row_stride;

        int fy = dsty + (int)y;
        if (fy < 0 || (uint32_t)fy >= vi.height) continue;

        if (fmt == MD64API_GRP_FMT_XRGB8888 && vi.bpp == 32) {
            uint32_t *dst = (uint32_t*)(fb + (uint64_t)fy * vi.pitch);
            for (uint32_t x = 0; x < width; x++) {
                int fx = dstx + (int)x;
                if (fx < 0 || (uint32_t)fx >= vi.width) continue;

                uint8_t r,g,b;
                if (bpp == 24) {
                    b = row[x*3u + 0];
                    g = row[x*3u + 1];
                    r = row[x*3u + 2];
                } else {
                    uint32_t px = rd32(row + x*4u);
                    if (comp == 3 || comp == 6) {
                        r = scale_masked(px, rmask);
                        g = scale_masked(px, gmask);
                        b = scale_masked(px, bmask);
                    } else {
                        /* BI_RGB 32bpp is usually B,G,R,X */
                        b = (uint8_t)(px & 0xFF);
                        g = (uint8_t)((px >> 8) & 0xFF);
                        r = (uint8_t)((px >> 16) & 0xFF);
                    }
                }
                if (key_enabled && r == key_r && g == key_g && b == key_b) continue;
                dst[fx] = xrgb8888(r,g,b);
            }
        } else if (fmt == MD64API_GRP_FMT_RGB565 && vi.bpp == 16) {
            uint16_t *dst = (uint16_t*)(fb + (uint64_t)fy * vi.pitch);
            for (uint32_t x = 0; x < width; x++) {
                int fx = dstx + (int)x;
                if (fx < 0 || (uint32_t)fx >= vi.width) continue;

                uint8_t r,g,b;
                if (bpp == 24) {
                    b = row[x*3u + 0];
                    g = row[x*3u + 1];
                    r = row[x*3u + 2];
                } else {
                    uint32_t px = rd32(row + x*4u);
                    if (comp == 3 || comp == 6) {
                        r = scale_masked(px, rmask);
                        g = scale_masked(px, gmask);
                        b = scale_masked(px, bmask);
                    } else {
                        b = (uint8_t)(px & 0xFF);
                        g = (uint8_t)((px >> 8) & 0xFF);
                        r = (uint8_t)((px >> 16) & 0xFF);
                    }
                }
                if (key_enabled && r == key_r && g == key_g && b == key_b) continue;
                dst[fx] = rgb565(r,g,b);
            }
        } else {
            return -12;
        }
    }

    return 0;
}

int md_main(long argc, char **argv) {
    if (argc < 2) {
        puts_raw("Usage: bmpview /path/to/file.bmp [x] [y] [--key R G B]\n");
        return 1;
    }

    const char *path = argv[1];
    int x = 0;
    int y = 0;

    int key_enabled = 0;
    uint8_t key_r = 255, key_g = 0, key_b = 255; /* default magenta */

    if (argc >= 3) x = to_int(argv[2]);
    if (argc >= 4) y = to_int(argv[3]);

    /* Optional: --key R G B */
    for (long i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--key") == 0 && i + 3 < argc) {
            key_enabled = 1;
            key_r = (uint8_t)to_int(argv[i + 1]);
            key_g = (uint8_t)to_int(argv[i + 2]);
            key_b = (uint8_t)to_int(argv[i + 3]);
            i += 3;
        }
    }

    fs_file_info_t st;
    if (stat(path, &st) != 0) {
        puts_raw("bmpview: stat failed\n");
        return 2;
    }
    if (st.is_directory) {
        puts_raw("bmpview: is a directory\n");
        return 3;
    }

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        puts_raw("bmpview: open failed\n");
        return 4;
    }

    uint32_t size = st.size;
    uint8_t *buf = (uint8_t*)malloc(size);
    if (!buf) {
        close(fd);
        puts_raw("bmpview: out of memory\n");
        return 5;
    }

    uint32_t got = 0;
    while (got < size) {
        ssize_t n = read(fd, buf + got, size - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    close(fd);

    if (got != size) {
        free(buf);
        puts_raw("bmpview: short read\n");
        return 6;
    }

    int rc = draw_bmp_to_fb(buf, size, x, y, key_enabled, key_r, key_g, key_b);
    free(buf);

    if (rc != 0) {
        printf("bmpview: decode/draw failed (%d)\n", rc);
        return 7;
    }

    return 0;
}
