#include "lib_fnt.h"
#include "../server/libc.h"
#include "../server/string.h"

static uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | 
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

fnt_font_t *fnt_load_font(const void *data, size_t size) {
    if (!data || size < 16) {
        printf("[FNT] Error: NULL data or size too small (%lu)\n", (unsigned long)size);
        return NULL;
    }
    
    const uint8_t *p = (const uint8_t *)data;
    const uint8_t *end = p + size;
    
    if (p[0] != FNT_MAGIC_0 || p[1] != FNT_MAGIC_1 || 
        p[2] != FNT_MAGIC_2 || p[3] != FNT_MAGIC_3) {
        printf("[FNT] Error: Invalid magic bytes: %c%c%c%c\n", p[0], p[1], p[2], p[3]);
        return NULL;
    }
    
    printf("[FNT] Valid magic found\n");
    
    fnt_font_t *font = (fnt_font_t *)malloc(sizeof(fnt_font_t));
    if (!font) return NULL;
    
    memset(font, 0, sizeof(fnt_font_t));
    
    font->header.magic[0] = p[0];
    font->header.magic[1] = p[1];
    font->header.magic[2] = p[2];
    font->header.magic[3] = p[3];
    p += 4;
    
    if (p + 2 > end) goto error;
    font->header.version = read_u16_le(p);
    p += 2;
    
    if (p + 2 > end) goto error;
    uint16_t name_len = read_u16_le(p);
    p += 2;
    
    if (p + name_len > end) goto error;
    font->header.name = (char *)malloc(name_len + 1);
    if (!font->header.name) goto error;
    memcpy(font->header.name, p, name_len);
    font->header.name[name_len] = '\0';
    p += name_len;
    
    if (p + 6 > end) goto error;
    font->header.glyph_width = read_u16_le(p);
    p += 2;
    font->header.glyph_height = read_u16_le(p);
    p += 2;
    font->header.baseline = read_u16_le(p);
    p += 2;
    
    printf("[FNT] Font metrics: %ux%u, baseline %u\n",
           font->header.glyph_width, font->header.glyph_height, font->header.baseline);
    
    if (p + 4 > end) goto error;
    font->header.glyph_count = read_u32_le(p);
    p += 4;
    
    printf("[FNT] Glyph count: %u\n", font->header.glyph_count);
    
    if (font->header.glyph_count == 0 || font->header.glyph_count > 10000) {
        printf("[FNT] Error: Invalid glyph count\n");
        goto error;
    }
    
    font->glyphs = (fnt_glyph_t *)malloc(sizeof(fnt_glyph_t) * font->header.glyph_count);
    if (!font->glyphs) goto error;
    memset(font->glyphs, 0, sizeof(fnt_glyph_t) * font->header.glyph_count);
    
    for (uint32_t i = 0; i < font->header.glyph_count; i++) {
        if (p + 10 > end) goto error;
        
        font->glyphs[i].codepoint = read_u32_le(p);
        p += 4;
        font->glyphs[i].width = read_u16_le(p);
        p += 2;
        font->glyphs[i].bitmap_width = read_u16_le(p);
        p += 2;
        font->glyphs[i].bitmap_height = read_u16_le(p);
        p += 2;
        
        size_t bitmap_bytes = ((size_t)font->glyphs[i].bitmap_width * 
                               (size_t)font->glyphs[i].bitmap_height + 7) / 8;
        
        if (p + bitmap_bytes > end) goto error;
        
        font->glyphs[i].bitmap = (uint8_t *)malloc(bitmap_bytes);
        if (!font->glyphs[i].bitmap) goto error;
        memcpy(font->glyphs[i].bitmap, p, bitmap_bytes);
        p += bitmap_bytes;
        
        if (font->glyphs[i].codepoint < 128) {
            font->ascii_cache[font->glyphs[i].codepoint] = &font->glyphs[i];
        }
    }
    
    printf("[FNT] Successfully loaded font\n");
    return font;
    
error:
    printf("[FNT] Error during parsing\n");
    fnt_free_font(font);
    return NULL;
}

void fnt_free_font(fnt_font_t *font) {
    if (!font) return;
    
    if (font->header.name) {
        free(font->header.name);
    }
    
    if (font->glyphs) {
        for (uint32_t i = 0; i < font->header.glyph_count; i++) {
            if (font->glyphs[i].bitmap) {
                free(font->glyphs[i].bitmap);
            }
        }
        free(font->glyphs);
    }
    
    free(font);
}

fnt_glyph_t *fnt_get_glyph(fnt_font_t *font, uint32_t codepoint) {
    if (!font) return NULL;
    
    if (codepoint < 128 && font->ascii_cache[codepoint]) {
        return font->ascii_cache[codepoint];
    }
    
    for (uint32_t i = 0; i < font->header.glyph_count; i++) {
        if (font->glyphs[i].codepoint == codepoint) {
            return &font->glyphs[i];
        }
    }
    
    return NULL;
}

int fnt_string_width(fnt_font_t *font, const char *text) {
    if (!font || !text) return 0;
    
    int width = 0;
    while (*text) {
        fnt_glyph_t *g = fnt_get_glyph(font, (uint32_t)(unsigned char)*text);
        if (g) {
            width += g->width;
        }
        text++;
    }
    
    return width;
}

int fnt_string_width_scaled(fnt_font_t *font, const char *text, int scale) {
    if (scale <= 0) scale = 1;
    return fnt_string_width(font, text) * scale;
}
