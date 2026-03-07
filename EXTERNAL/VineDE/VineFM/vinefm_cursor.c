#include "libc.h"
#include "string.h"
#include "../include/moduos/kernel/events/events.h"

#define CURSOR_W 16
#define CURSOR_H 16

typedef struct {
    int width;
    int height;
    uint32_t *pixels;
} cursor_img_t;

#ifndef SEEK_SET
#define SEEK_SET 0
#endif

static int load_cursor_bmp_rgba8888(const char *path, cursor_img_t *out) {
    if (!out) return -1;
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -2;
    uint8_t header[54];
    if (read(fd, header, sizeof(header)) != (ssize_t)sizeof(header)) {
        close(fd);
        return -3;
    }
    if (header[0] != 'B' || header[1] != 'M') {
        close(fd);
        return -4;
    }
    uint32_t off = *(uint32_t *)&header[10];
    uint32_t width = *(uint32_t *)&header[18];
    uint32_t height = *(uint32_t *)&header[22];
    uint16_t bpp = *(uint16_t *)&header[28];
    if (bpp != 32) {
        close(fd);
        return -5;
    }

    size_t size = (size_t)width * height * 4;
    uint32_t *pixels = (uint32_t *)malloc(size);
    if (!pixels) {
        close(fd);
        return -6;
    }

    lseek(fd, (int)off, SEEK_SET);
    if (read(fd, pixels, size) != (ssize_t)size) {
        free(pixels);
        close(fd);
        return -7;
    }
    close(fd);

    out->width = (int)width;
    out->height = (int)height;
    out->pixels = pixels;
    return 0;
}

static void alpha_blit_cursor_xrgb8888(uint8_t *dst, uint32_t pitch, uint32_t w, uint32_t h,
                                       const cursor_img_t *cur, int32_t x, int32_t y) {
    if (!dst || !cur || !cur->pixels) return;
    for (int yy = 0; yy < cur->height; yy++) {
        int32_t dy = y + yy;
        if (dy < 0 || dy >= (int32_t)h) continue;
        uint32_t *row = (uint32_t *)(dst + (uint64_t)dy * pitch);
        for (int xx = 0; xx < cur->width; xx++) {
            int32_t dx = x + xx;
            if (dx < 0 || dx >= (int32_t)w) continue;
            uint32_t pix = cur->pixels[(cur->height - 1 - yy) * cur->width + xx];
            uint8_t a = (uint8_t)(pix >> 24);
            if (a == 0) continue;
            row[dx] = pix & 0x00FFFFFFu;
        }
    }
}

static void alpha_blit_cursor_rgb565(uint8_t *dst, uint32_t pitch, uint32_t w, uint32_t h,
                                     const cursor_img_t *cur, int32_t x, int32_t y) {
    if (!dst || !cur || !cur->pixels) return;
    for (int yy = 0; yy < cur->height; yy++) {
        int32_t dy = y + yy;
        if (dy < 0 || dy >= (int32_t)h) continue;
        uint16_t *row = (uint16_t *)(dst + (uint64_t)dy * pitch);
        for (int xx = 0; xx < cur->width; xx++) {
            int32_t dx = x + xx;
            if (dx < 0 || dx >= (int32_t)w) continue;
            uint32_t pix = cur->pixels[(cur->height - 1 - yy) * cur->width + xx];
            uint8_t a = (uint8_t)(pix >> 24);
            if (a == 0) continue;
            uint8_t r = (uint8_t)(pix >> 16);
            uint8_t g = (uint8_t)(pix >> 8);
            uint8_t b = (uint8_t)pix;
            uint16_t rgb565 = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
            row[dx] = rgb565;
        }
    }
}
