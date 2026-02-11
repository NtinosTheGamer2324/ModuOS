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
    
    /* Verify magic */
    if (ptr[0] != FNT_MAGIC_0 || ptr[1] != FNT_MAGIC_1 ||
        ptr[2] != FNT_MAGIC_2 || ptr[3] != FNT_MAGIC_3) {
        com_write_string(COM1_PORT, "[FNT] Invalid magic number\n");
        return NULL;
    }
    ptr += 4;
    
    /* Check version */
    uint16_t version = read_le16(ptr);
    ptr += 2;
    
    if (version != FNT_VERSION) {
        com_printf(COM1_PORT, "[FNT] Unsupported version %u\n", version);
        return NULL;
    }
    
    /* Allocate font structure */
    fnt_font_t *font = (fnt_font_t *)kmalloc(sizeof(fnt_font_t));
    if (!font) {
        com_write_string(COM1_PORT, "[FNT] Failed to allocate font structure\n");
        return NULL;
    }
    
    memset(font, 0, sizeof(fnt_font_t));
    
    /* Read font name */
    if (ptr + 2 > end) goto error;
    uint16_t name_len = read_le16(ptr);
    ptr += 2;
    
    if (ptr + name_len > end) goto error;
    
    font->header.name = (char *)kmalloc(name_len + 1);
    if (!font->header.name) goto error;
    
    memcpy(font->header.name, ptr, name_len);
    font->header.name[name_len] = '\0';
    ptr += name_len;
    
    /* Read font metrics */
    if (ptr + 6 > end) goto error;
    font->header.glyph_width = read_le16(ptr);
    ptr += 2;
    font->header.glyph_height = read_le16(ptr);
    ptr += 2;
    font->header.baseline = read_le16(ptr);
    ptr += 2;
    
    com_printf(COM1_PORT, "[FNT] Loading font '%s' (%ux%u, baseline %u)\n",
              font->header.name, font->header.glyph_width,
              font->header.glyph_height, font->header.baseline);
    
    /* Read glyph count */
    if (ptr + 4 > end) goto error;
    font->header.glyph_count = read_le32(ptr);
    ptr += 4;
    
    com_printf(COM1_PORT, "[FNT] Font contains %u glyphs\n", font->header.glyph_count);
    
    if (font->header.glyph_count == 0) {
        com_write_string(COM1_PORT, "[FNT] Warning - font has no glyphs\n");
        return font;
    }
    
    /* Allocate glyph array */
    font->glyphs = (fnt_glyph_t *)kmalloc(sizeof(fnt_glyph_t) * font->header.glyph_count);
    if (!font->glyphs) goto error;
    
    memset(font->glyphs, 0, sizeof(fnt_glyph_t) * font->header.glyph_count);
    
    /* Load each glyph */
    for (uint32_t i = 0; i < font->header.glyph_count; i++) {
        if (ptr + 12 > end) goto error;
        
        fnt_glyph_t *glyph = &font->glyphs[i];
        
        /* Read glyph metadata */
        glyph->codepoint = read_le32(ptr);
        ptr += 4;
        glyph->width = read_le16(ptr);
        ptr += 2;
        glyph->bitmap_width = read_le16(ptr);
        ptr += 2;
        glyph->bitmap_height = read_le16(ptr);
        ptr += 2;
        
        /* Calculate bitmap size in bytes */
        uint32_t bytes_per_row = (glyph->bitmap_width + 7) / 8;
        uint32_t bitmap_size = bytes_per_row * glyph->bitmap_height;
        
        if (ptr + bitmap_size > end) goto error;
        
        /* Allocate and copy bitmap */
        glyph->bitmap = (uint8_t *)kmalloc(bitmap_size);
        if (!glyph->bitmap) goto error;
        
        memcpy(glyph->bitmap, ptr, bitmap_size);
        ptr += bitmap_size;
        
        /* Cache ASCII glyphs for fast lookup */
        if (glyph->codepoint < 128) {
            font->ascii_cache[glyph->codepoint] = glyph;
        }
    }
    
    com_printf(COM1_PORT, "[FNT] Successfully loaded %u glyphs\n", font->header.glyph_count);
    
    /* Store header info */
    font->header.magic[0] = FNT_MAGIC_0;
    font->header.magic[1] = FNT_MAGIC_1;
    font->header.magic[2] = FNT_MAGIC_2;
    font->header.magic[3] = FNT_MAGIC_3;
    font->header.version = version;
    
    return font;
    
error:
    com_write_string(COM1_PORT, "[FNT] Error loading font\n");
    fnt_free_font(font);
    return NULL;
}

void fnt_free_font(fnt_font_t *font) {
    if (!font) return;
    
    if (font->header.name) {
        kfree(font->header.name);
    }
    
    if (font->glyphs) {
        for (uint32_t i = 0; i < font->header.glyph_count; i++) {
            if (font->glyphs[i].bitmap) {
                kfree(font->glyphs[i].bitmap);
            }
        }
        kfree(font->glyphs);
    }
    
    kfree(font);
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

int fnt_string_width(fnt_font_t *font, const char *text) {
    if (!font || !text) return 0;
    
    int width = 0;
    
    while (*text) {
        /* Simple ASCII for now (TODO: proper UTF-8 decoding) */
        uint32_t codepoint = (uint8_t)*text;
        text++;
        
        fnt_glyph_t *glyph = fnt_get_glyph(font, codepoint);
        if (glyph) {
            width += glyph->width;
        } else {
            /* Use default width for missing glyphs */
            width += font->header.glyph_width / 2;
        }
    }
    
    return width;
}
