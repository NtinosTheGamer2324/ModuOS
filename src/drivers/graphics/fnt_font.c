#include "moduos/drivers/graphics/fnt_font.h"
#include "moduos/kernel/memory/kheap.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/COM/com.h"

/* Helper to read little-endian uint16_t */
static uint16_t read_le16(const uint8_t *data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

/* Helper to read little-endian uint32_t */
static uint32_t read_le32(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

fnt_font_t *fnt_load_font(const void *data, size_t size) {
    if (!data || size < 16) {
        com_write_string(COM1_PORT, "[FNT] Invalid data or size\n");
        return NULL;
    }
    
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + size;
    
    ptr += 6; 

    /* Read font name length and name */
    if (ptr + 2 > end) return NULL;
    uint16_t name_len = read_le16(ptr);
    ptr += 2;
    if (ptr + name_len > end) return NULL;
    const uint8_t *name_ptr = ptr;
    ptr += name_len;

    /* Read metrics */
    if (ptr + 6 > end) return NULL;
    uint16_t glyph_width = read_le16(ptr); ptr += 2;
    uint16_t glyph_height = read_le16(ptr); ptr += 2u;
    uint16_t baseline = read_le16(ptr); ptr += 2u;

    if (ptr + 4 > end) return NULL;
    uint32_t glyph_count = read_le32(ptr);
    ptr += 4;

    if (glyph_count == 0) return NULL;

    const uint8_t *temp_ptr = ptr;
    size_t total_bitmap_bytes = 0;
    
    for (uint32_t i = 0; i < glyph_count; i++) {
        if (temp_ptr + 12 > end) return NULL;
        temp_ptr += 4; // codepoint
        uint16_t w = read_le16(temp_ptr); temp_ptr += 2;
        uint16_t bw = read_le16(temp_ptr); temp_ptr += 2;
        uint16_t bh = read_le16(temp_ptr); temp_ptr += 2;
        
        uint32_t bytes_per_row = (bw + 7) / 8;
        size_t bmap_size = bytes_per_row * bh;
        total_bitmap_bytes += bmap_size;
        
        if (temp_ptr + bmap_size > end) return NULL;
        temp_ptr += bmap_size;
    }

    /* Allocate font structure, name, glyph array, AND the massive bitmap blob in just a few clean allocations */
    fnt_font_t *font = (fnt_font_t *)kmalloc(sizeof(fnt_font_t));
    if (!font) return NULL;
    memset(font, 0, sizeof(fnt_font_t));

    font->header.glyph_width = glyph_width;
    font->header.glyph_height = glyph_height;
    font->header.baseline = baseline;
    font->header.glyph_count = glyph_count;

    font->header.name = (char *)kmalloc(name_len + 1);
    if (!font->header.name) { fnt_free_font(font); return NULL; }
    memcpy(font->header.name, name_ptr, name_len);
    font->header.name[name_len] = '\0';

    font->glyphs = (fnt_glyph_t *)kmalloc(sizeof(fnt_glyph_t) * glyph_count);
    if (!font->glyphs) { fnt_free_font(font); return NULL; }
    memset(font->glyphs, 0, sizeof(fnt_glyph_t) * glyph_count);

    uint8_t *bitmap_pool = (uint8_t *)kmalloc(total_bitmap_bytes);
    if (!bitmap_pool) { fnt_free_font(font); return NULL; }
    uint8_t *pool_ptr = bitmap_pool;

    for (uint32_t i = 0; i < glyph_count; i++) {
        fnt_glyph_t *glyph = &font->glyphs[i];
        
        glyph->codepoint = read_le32(ptr); ptr += 4;
        glyph->width = read_le16(ptr); ptr += 2;
        glyph->bitmap_width = read_le16(ptr); ptr += 2;
        glyph->bitmap_height = read_le16(ptr); ptr += 2;
        
        uint32_t bytes_per_row = (glyph->bitmap_width + 7) / 8;
        uint32_t bitmap_size = bytes_per_row * glyph->bitmap_height;
        
        /* Point directly into our pre-allocated pool instead of kmallocing */
        glyph->bitmap = pool_ptr;
        memcpy(glyph->bitmap, ptr, bitmap_size);
        pool_ptr += bitmap_size;
        ptr += bitmap_size;
        
        if (glyph->codepoint < 128) {
            font->ascii_cache[glyph->codepoint] = glyph;
        }
    }

    // Keep track of the pool pointer so fnt_free_font can free it safely
    font->_bitmap_pool = bitmap_pool; 

    com_printf(COM1_PORT, "[FNT] Successfully loaded %u glyphs instantly\n", glyph_count);
    return font;
}

void fnt_free_font(fnt_font_t *font) {
    if (!font) return;
    
    if (font->header.name) {
        kfree(font->header.name);
    }
    
    if (font->glyphs) {
        kfree(font->glyphs); // The glyph array itself
    }
    
    if (font->_bitmap_pool) {
        kfree(font->_bitmap_pool); // Frees all bitmaps in one go!
    }
    
    kfree(font); // Frees the font structure
}

fnt_glyph_t *fnt_get_glyph(fnt_font_t *font, uint32_t codepoint) {
    if (!font) return NULL;
    
    /* Fast path for ASCII */
    if (codepoint < 128) {
        return font->ascii_cache[codepoint];
    }
    
    /* Binary search for other codepoints (glyphs are sorted) */
    int left = 0;
    int right = font->header.glyph_count - 1;
    
    while (left <= right) {
        int mid = (left + right) / 2;
        fnt_glyph_t *glyph = &font->glyphs[mid];
        
        if (glyph->codepoint == codepoint) {
            return glyph;
        } else if (glyph->codepoint < codepoint) {
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }
    
    return NULL;
}

void fnt_render_glyph(const fnt_glyph_t *glyph, void *fb,
                      int x, int y, int pitch, int bpp,
                      uint32_t fg_color, uint32_t bg_color) {
    if (!glyph || !fb) return;
    
    for (int gy = 0; gy < glyph->bitmap_height; gy++) {
        for (int gx = 0; gx < glyph->bitmap_width; gx++) {
            int pixel = fnt_get_pixel(glyph, gx, gy);
            uint32_t color = pixel ? fg_color : bg_color;
            
            /* Skip transparent background */
            if (!pixel && bg_color == 0xFFFFFFFF) continue;
            
            int px = x + gx;
            int py = y + gy;
            
            /* Write pixel based on bpp */
            if (bpp == 32) {
                uint32_t *dest = (uint32_t *)((uint8_t *)fb + py * pitch + px * 4);
                *dest = color;
            } else if (bpp == 24) {
                uint8_t *dest = (uint8_t *)fb + py * pitch + px * 3;
                dest[0] = color & 0xFF;
                dest[1] = (color >> 8) & 0xFF;
                dest[2] = (color >> 16) & 0xFF;
            } else if (bpp == 16) {
                /* Convert 32-bit color to RGB565 */
                uint8_t r = (color >> 16) & 0xFF;
                uint8_t g = (color >> 8) & 0xFF;
                uint8_t b = color & 0xFF;
                uint16_t rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
                uint16_t *dest = (uint16_t *)((uint8_t *)fb + py * pitch + px * 2);
                *dest = rgb565;
            }
        }
    }
}

/* UTF-8 decoder — decodes one codepoint from *str, advances the pointer.
 * Returns the codepoint, or 0xFFFD (replacement char) on invalid sequences. */
static uint32_t utf8_next(const char **str) {
    const uint8_t *s = (const uint8_t *)*str;
    uint32_t cp;
    int bytes;

    if (!s || !*s) return 0;

    if (*s < 0x80) {
        /* 0xxxxxxx — single byte ASCII */
        cp    = *s;
        bytes = 1;
    } else if ((*s & 0xE0) == 0xC0) {
        /* 110xxxxx 10xxxxxx — 2 bytes */
        cp    = *s & 0x1F;
        bytes = 2;
    } else if ((*s & 0xF0) == 0xE0) {
        /* 1110xxxx 10xxxxxx 10xxxxxx — 3 bytes */
        cp    = *s & 0x0F;
        bytes = 3;
    } else if ((*s & 0xF8) == 0xF0) {
        /* 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx — 4 bytes */
        cp    = *s & 0x07;
        bytes = 4;
    } else {
        /* Invalid lead byte */
        *str += 1;
        return 0xFFFD;
    }

    /* Validate and consume continuation bytes */
    for (int i = 1; i < bytes; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            /* Invalid continuation byte — skip the lead byte only */
            *str += 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (s[i] & 0x3F);
    }

    /* Reject overlong encodings and surrogate halves */
    if ((bytes == 2 && cp < 0x80)   ||
        (bytes == 3 && cp < 0x800)  ||
        (bytes == 4 && cp < 0x10000)||
        (cp >= 0xD800 && cp <= 0xDFFF) ||
        (cp > 0x10FFFF)) {
        *str += 1;
        return 0xFFFD;
    }

    *str += bytes;
    return cp;
}

int fnt_string_width(fnt_font_t *font, const char *text) {
    if (!font || !text) return 0;

    int width = 0;
    while (*text) {
        uint32_t cp = utf8_next(&text);
        if (cp == 0) break;

        fnt_glyph_t *glyph = fnt_get_glyph(font, cp);
        width += glyph ? glyph->width : font->header.glyph_width / 2;
    }
    return width;
}

void fnt_render_string(fnt_font_t *font, const char *text,
                       void *fb, int x, int y, int pitch, int bpp,
                       uint32_t fg_color, uint32_t bg_color) {
    if (!font || !text || !fb) return;

    int cx = x;
    while (*text) {
        uint32_t cp = utf8_next(&text);
        if (cp == 0) break;

        fnt_glyph_t *glyph = fnt_get_glyph(font, cp);
        if (!glyph) {
            /* Fall back to replacement char U+FFFD, then a box */
            glyph = fnt_get_glyph(font, 0xFFFD);
        }
        if (glyph) {
            fnt_render_glyph(glyph, fb, cx, y, pitch, bpp, fg_color, bg_color);
            cx += glyph->width;
        } else {
            cx += font->header.glyph_width / 2;
        }
    }
}
