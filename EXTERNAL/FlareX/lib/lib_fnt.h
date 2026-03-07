#pragma once
#include <stdint.h>
#include <stddef.h>

#define FNT_MAGIC_0 'F'
#define FNT_MAGIC_1 'N'
#define FNT_MAGIC_2 'T'
#define FNT_MAGIC_3 '1'
#define FNT_VERSION 1

typedef struct {
    char magic[4];
    uint16_t version;
    char *name;
    uint16_t glyph_width;
    uint16_t glyph_height;
    uint16_t baseline;
    uint32_t glyph_count;
} fnt_header_t;

typedef struct {
    uint32_t codepoint;
    uint16_t width;
    uint16_t bitmap_width;
    uint16_t bitmap_height;
    uint8_t *bitmap;
} fnt_glyph_t;

typedef struct {
    fnt_header_t header;
    fnt_glyph_t *glyphs;
    fnt_glyph_t *ascii_cache[128];
} fnt_font_t;

fnt_font_t *fnt_load_font(const void *data, size_t size);
void fnt_free_font(fnt_font_t *font);
fnt_glyph_t *fnt_get_glyph(fnt_font_t *font, uint32_t codepoint);

static inline int fnt_get_pixel(const fnt_glyph_t *glyph, int x, int y) {
    if (!glyph || !glyph->bitmap) return 0;
    if (x < 0 || x >= glyph->bitmap_width) return 0;
    if (y < 0 || y >= glyph->bitmap_height) return 0;

    /* Row-padded: ceil(bitmap_width/8) bytes per row. */
    int bytes_per_row = (glyph->bitmap_width + 7) / 8;
    int byte_index = y * bytes_per_row + x / 8;
    int bit_offset = 7 - (x % 8);

    return (glyph->bitmap[byte_index] >> bit_offset) & 1;
}

int fnt_string_width(fnt_font_t *font, const char *text);
int fnt_string_width_scaled(fnt_font_t *font, const char *text, int scale);
