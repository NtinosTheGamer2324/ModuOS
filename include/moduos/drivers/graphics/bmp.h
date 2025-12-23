#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t width;
    uint32_t height;
    uint16_t bpp;
    uint32_t comp; /* BMP compression field (BI_RGB=0, BI_BITFIELDS=3, BI_ALPHABITFIELDS=6) */

    /* Pixel masks (for BI_BITFIELDS / v4 headers) */
    uint32_t rmask;
    uint32_t gmask;
    uint32_t bmask;
    uint32_t amask;

    /* Palette (for 8bpp indexed BMPs). Points into original file buffer. */
    const uint8_t *palette;
    uint32_t palette_colors;

    /* Points into the original file buffer */
    const uint8_t *pixel_data;
    size_t pixel_data_size;
    uint32_t row_stride;

    int top_down; /* 1 if stored top-down */
} bmp_image_t;

/* Parse a BMP from an in-memory buffer. Supports 24bpp BI_RGB and 32bpp BI_RGB/BI_BITFIELDS. */
int bmp_parse(bmp_image_t *out, const void *buf, size_t size);

/* Read one pixel as 0..255 RGBA (independent of masks). */
int bmp_get_pixel_rgba(const bmp_image_t *img, uint32_t x, uint32_t y,
                       uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a);

#ifdef __cplusplus
}
#endif
