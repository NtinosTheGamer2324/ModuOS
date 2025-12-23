#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Simple built-in 8x16 1bpp bitmap font (ASCII 0..127). */
#define BITMAP_FONT_W 8
#define BITMAP_FONT_H 16

/* Returns pointer to 16 bytes, each byte is one row (bit 7 = leftmost pixel). */
const uint8_t *bitmap_font_glyph8x16(uint8_t ch);

#ifdef __cplusplus
}
#endif
