#ifndef FNT_FONT_H
#define FNT_FONT_H

#include <stdint.h>
#include <stddef.h>

/*
 * Custom FNT Font Format Support
 * 
 * File Format:
 * - Magic: "FNT1" (4 bytes)
 * - Version: uint16_t (little-endian)
 * - Font name length: uint16_t
 * - Font name: UTF-8 string
 * - Glyph width: uint16_t (max width)
 * - Glyph height: uint16_t
 * - Baseline: uint16_t
 * - Glyph count: uint32_t
 * - Glyphs: array of glyph entries
 * 
 * Each glyph entry:
 * - Codepoint: uint32_t (Unicode)
 * - Width: uint16_t (actual width)
 * - Bitmap width: uint16_t
 * - Bitmap height: uint16_t
 * - Bitmap data: packed bits (1 bit per pixel)
 */

#define FNT_MAGIC_0 'F'
#define FNT_MAGIC_1 'N'
#define FNT_MAGIC_2 'T'
#define FNT_MAGIC_3 '1'
#define FNT_VERSION 1

/* FNT font header (in-memory representation) */
typedef struct {
    char magic[4];
    uint16_t version;
    char *name;
    uint16_t glyph_width;   /* Maximum glyph width */
    uint16_t glyph_height;
    uint16_t baseline;
    uint32_t glyph_count;
} fnt_header_t;

/* FNT glyph data */
typedef struct {
    uint32_t codepoint;
    uint16_t width;         /* Actual character width for spacing */
    uint16_t bitmap_width;
    uint16_t bitmap_height;
    uint8_t *bitmap;        /* Packed bitmap data (1 bit per pixel) */
} fnt_glyph_t;

/* FNT font context */
typedef struct {
    fnt_header_t header;
    fnt_glyph_t *glyphs;
    
    /* Quick lookup cache for ASCII (0-127) */
    fnt_glyph_t *ascii_cache[128];
} fnt_font_t;

/*
 * Load FNT font from memory buffer
 * 
 * @param data: Pointer to FNT file data
 * @param size: Size of data in bytes
 * @return: Pointer to loaded font, or NULL on error
 */
fnt_font_t *fnt_load_font(const void *data, size_t size);

/*
 * Free loaded FNT font
 * 
 * @param font: Font to free
 */
void fnt_free_font(fnt_font_t *font);

/*
 * Get glyph for a Unicode codepoint
 * 
 * @param font: Font context
 * @param codepoint: Unicode codepoint
 * @return: Pointer to glyph, or NULL if not found
 */
fnt_glyph_t *fnt_get_glyph(fnt_font_t *font, uint32_t codepoint);

/*
 * Get pixel value from glyph bitmap
 * 
 * @param glyph: Glyph data
 * @param x: X coordinate
 * @param y: Y coordinate
 * @return: 1 if pixel is set, 0 otherwise
 */
static inline int fnt_get_pixel(const fnt_glyph_t *glyph, int x, int y) {
    if (!glyph || !glyph->bitmap) return 0;
    if (x < 0 || x >= glyph->bitmap_width) return 0;
    if (y < 0 || y >= glyph->bitmap_height) return 0;
    
    int bit_index = y * glyph->bitmap_width + x;
    int byte_index = bit_index / 8;
    int bit_offset = 7 - (bit_index % 8);
    
    return (glyph->bitmap[byte_index] >> bit_offset) & 1;
}

/*
 * Render a single glyph to framebuffer
 * 
 * @param glyph: Glyph to render
 * @param fb: Framebuffer pointer
 * @param x: X position in framebuffer
 * @param y: Y position in framebuffer
 * @param pitch: Framebuffer pitch (bytes per line)
 * @param bpp: Bits per pixel
 * @param fg_color: Foreground color
 * @param bg_color: Background color (0xFFFFFFFF = transparent)
 */
void fnt_render_glyph(const fnt_glyph_t *glyph, void *fb, 
                      int x, int y, int pitch, int bpp,
                      uint32_t fg_color, uint32_t bg_color);

/*
 * Calculate width of string in pixels
 * 
 * @param font: Font context
 * @param text: UTF-8 string
 * @return: Width in pixels
 */
int fnt_string_width(fnt_font_t *font, const char *text);

#endif /* FNT_FONT_H */
