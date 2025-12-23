#pragma once

#include <stdint.h>
#include <stddef.h>
#include "moduos/drivers/graphics/bmp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bmp_image_t img;

    /* glyph atlas layout */
    uint16_t cell_w;
    uint16_t cell_h;
    uint16_t cols;
    uint16_t rows;
    uint8_t first_char; /* usually 0x20 */
    uint8_t char_count; /* usually 95 or 96 */

    /* Render threshold: any pixel with alpha>=thr or luminance>=thr is considered "on" */
    uint8_t threshold;

    /* If set, invert the pixel-on test (useful for black-on-white atlases). */
    uint8_t invert;
} bmp_font_t;

/* Load bitmap font atlas from a BMP buffer.
 * This assumes a fixed grid. For ModuOSDEF.bmp we assume 19x5 grid of 30x30.
 */
int bmp_font_init_moduosdef(bmp_font_t *out, const void *bmp_buf, size_t bmp_size);

/* Test if a glyph pixel is "on" (foreground) at glyph-local coordinates. */
int bmp_font_glyph_pixel_on(const bmp_font_t *f, uint8_t ch, uint16_t gx, uint16_t gy);

#ifdef __cplusplus
}
#endif
