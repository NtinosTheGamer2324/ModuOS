#include "libc.h"
#include "string.h"

/*
 * gfxtest.sqr
 *
 * Minimal userland graphics test for the MD64API GRP device:
 *   $/dev/graphics/video0
 *
 * This app:
 *  1) reads md64api_grp_video_info_t
 *  2) prints mode/format/geometry
 *  3) if GRAPHICS + framebuffer address present, draws a few patterns
 */

static uint32_t pack_xrgb8888(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static uint16_t pack_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t rr = (uint16_t)((r * 31u) / 255u);
    uint16_t gg = (uint16_t)((g * 63u) / 255u);
    uint16_t bb = (uint16_t)((b * 31u) / 255u);
    return (uint16_t)((rr << 11) | (gg << 5) | (bb));
}

static void draw_rect_xrgb8888(uint8_t *fb, uint32_t pitch, uint32_t w, uint32_t h,
                               uint32_t x, uint32_t y, uint32_t rw, uint32_t rh,
                               uint32_t color) {
    if (!fb) return;
    if (x >= w || y >= h) return;
    if (x + rw > w) rw = w - x;
    if (y + rh > h) rh = h - y;

    for (uint32_t yy = 0; yy < rh; yy++) {
        uint32_t *row = (uint32_t *)(fb + (uint64_t)(y + yy) * pitch);
        for (uint32_t xx = 0; xx < rw; xx++) {
            row[x + xx] = color;
        }
    }
}

static void draw_rect_rgb565(uint8_t *fb, uint32_t pitch, uint32_t w, uint32_t h,
                             uint32_t x, uint32_t y, uint32_t rw, uint32_t rh,
                             uint16_t color) {
    if (!fb) return;
    if (x >= w || y >= h) return;
    if (x + rw > w) rw = w - x;
    if (y + rh > h) rh = h - y;

    for (uint32_t yy = 0; yy < rh; yy++) {
        uint16_t *row = (uint16_t *)(fb + (uint64_t)(y + yy) * pitch);
        for (uint32_t xx = 0; xx < rw; xx++) {
            row[x + xx] = color;
        }
    }
}

static void draw_gradient_xrgb8888(uint8_t *fb, uint32_t pitch, uint32_t w, uint32_t h) {
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(fb + (uint64_t)y * pitch);
        for (uint32_t x = 0; x < w; x++) {
            uint8_t r = (uint8_t)((x * 255u) / (w ? w : 1));
            uint8_t g = (uint8_t)((y * 255u) / (h ? h : 1));
            uint8_t b = (uint8_t)(((x ^ y) & 0xFF));
            row[x] = pack_xrgb8888(r, g, b);
        }
    }
}

static void draw_gradient_rgb565(uint8_t *fb, uint32_t pitch, uint32_t w, uint32_t h) {
    for (uint32_t y = 0; y < h; y++) {
        uint16_t *row = (uint16_t *)(fb + (uint64_t)y * pitch);
        for (uint32_t x = 0; x < w; x++) {
            uint8_t r = (uint8_t)((x * 255u) / (w ? w : 1));
            uint8_t g = (uint8_t)((y * 255u) / (h ? h : 1));
            uint8_t b = (uint8_t)(((x ^ y) & 0xFF));
            row[x] = pack_rgb565(r, g, b);
        }
    }
}

int md_main(long argc, char **argv) {
    (void)argc;
    (void)argv;

    puts_raw("gfxtest - MD64API GRP graphics test (rev2)\n");

    int fd = open(MD64API_GRP_DEFAULT_DEVICE, O_RDONLY, 0);
    if (fd < 0) {
        printf("gfxtest: cannot open %s\n", MD64API_GRP_DEFAULT_DEVICE);
        return 1;
    }

    md64api_grp_video_info_t info;
    memset(&info, 0, sizeof(info));

    ssize_t n = read(fd, &info, sizeof(info));
    close(fd);

    if (n < (ssize_t)sizeof(info)) {
        printf("gfxtest: read video info failed (n=%ld)\n", (long)n);
        return 1;
    }

    printf("mode=%u fmt=%u bpp=%u\n", (unsigned)info.mode, (unsigned)info.fmt, (unsigned)info.bpp);
    printf("w=%u h=%u pitch=%u\n", (unsigned)info.width, (unsigned)info.height, (unsigned)info.pitch);
    printf("fb_addr=0x%llx\n", (unsigned long long)info.fb_addr);

    if (info.mode != MD64API_GRP_MODE_GRAPHICS || info.width == 0 || info.height == 0) {
        puts_raw("gfxtest: not in graphics mode (boot with gfx enabled), nothing to draw.\n");
        return 0;
    }

    puts_raw("Drawing test pattern (small backbuffer + gfx_blit region)...\n");

    /*
     * Be tolerant: some kernels may report fmt=UNKNOWN (0) even though bpp is known.
     * In that case, infer from bpp.
     */
    uint8_t fmt = info.fmt;
    if (fmt == MD64API_GRP_FMT_UNKNOWN) {
        if (info.bpp == 32) fmt = MD64API_GRP_FMT_XRGB8888;
        else if (info.bpp == 16) fmt = MD64API_GRP_FMT_RGB565;
    }

    uint32_t bpp_bytes = (fmt == MD64API_GRP_FMT_RGB565) ? 2u : 4u;

    // Use a small test surface so this app is fast even in VMs.
    uint32_t test_w = 256;
    uint32_t test_h = 256;
    if (test_w > info.width) test_w = info.width;
    if (test_h > info.height) test_h = info.height;

    uint32_t pitch = test_w * bpp_bytes;
    uint32_t buf_size = pitch * test_h;

    uint8_t *bb = (uint8_t*)malloc(buf_size);
    if (!bb) {
        puts_raw("gfxtest: out of memory\n");
        return 3;
    }

    if (fmt == MD64API_GRP_FMT_XRGB8888 && info.bpp == 32) {
        draw_gradient_xrgb8888(bb, pitch, test_w, test_h);
        draw_rect_xrgb8888(bb, pitch, test_w, test_h, 10, 10, 80, 60, pack_xrgb8888(255, 0, 0));
        draw_rect_xrgb8888(bb, pitch, test_w, test_h, 100, 10, 80, 60, pack_xrgb8888(0, 255, 0));
        draw_rect_xrgb8888(bb, pitch, test_w, test_h, 190, 10, 60, 60, pack_xrgb8888(0, 0, 255));
    } else if (fmt == MD64API_GRP_FMT_RGB565 && info.bpp == 16) {
        draw_gradient_rgb565(bb, pitch, test_w, test_h);
        draw_rect_rgb565(bb, pitch, test_w, test_h, 10, 10, 80, 60, pack_rgb565(255, 0, 0));
        draw_rect_rgb565(bb, pitch, test_w, test_h, 100, 10, 80, 60, pack_rgb565(0, 255, 0));
        draw_rect_rgb565(bb, pitch, test_w, test_h, 190, 10, 60, 60, pack_rgb565(0, 0, 255));
    } else {
        free(bb);
        puts_raw("gfxtest: unsupported framebuffer format; expected RGB565(16bpp) or XRGB8888(32bpp).\n");
        return 2;
    }

    // Blit the small surface to the top-left corner.
    int rc = gfx_blit(bb, (uint16_t)test_w, (uint16_t)test_h, 0, 0, (uint16_t)pitch, (uint16_t)fmt);
    free(bb);

    if (rc != 0) {
        printf("gfxtest: gfx_blit failed (%d)\n", rc);
        return 4;
    }

    puts_raw("gfxtest finished.\n");
    return 0;
}
