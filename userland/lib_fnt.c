#include "lib_fnt.h"
#include "libc.h"
#include "string.h"

/* -----------------------------------------------------------------------
 * Fast glyph lookup
 *
 * ASCII (codepoints 0-127) is handled by the direct ascii_cache[] array
 * already present in fnt_font_t — O(1), no change needed.
 *
 * Non-ASCII was previously a linear scan through all glyphs on every call.
 * We replace it with a simple open-addressing hash table stored inside
 * fnt_font_t.  Lookup becomes O(1) average instead of O(n).
 *
 * Hash table lives in font->hash_buckets / font->hash_size.
 * Each bucket stores a pointer to the fnt_glyph_t (NULL = empty).
 * Collision resolution: linear probing.
 * Load factor cap: 0.65  (table is sized to 2× glyph_count, rounded up
 * to the next power of two, so worst-case load ≈ 0.5).
 * ----------------------------------------------------------------------- */

#define HASH_LOAD_NUM  2   /* table_size = glyph_count * HASH_LOAD_NUM */

static uint32_t read_u16_le(const uint8_t *p) {
    return (uint32_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Next power of two >= v (v must be > 0) */
static uint32_t next_pow2(uint32_t v) {
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4;
    v |= v >> 8; v |= v >> 16;
    return v + 1;
}

/* Build the hash table after all glyphs are loaded */
static int build_hash(fnt_font_t *font) {
    uint32_t sz = next_pow2((uint32_t)(font->header.glyph_count * HASH_LOAD_NUM));
    if (sz < 16) sz = 16;

    font->hash_buckets = (fnt_glyph_t **)malloc(sz * sizeof(fnt_glyph_t *));
    if (!font->hash_buckets) return -1;
    memset(font->hash_buckets, 0, sz * sizeof(fnt_glyph_t *));
    font->hash_size = sz;

    uint32_t mask = sz - 1;
    for (uint32_t i = 0; i < font->header.glyph_count; i++) {
        /* Skip ASCII — already in ascii_cache, no need to hash */
        if (font->glyphs[i].codepoint < 128) continue;

        uint32_t slot = font->glyphs[i].codepoint & mask;
        while (font->hash_buckets[slot] != NULL)
            slot = (slot + 1) & mask;
        font->hash_buckets[slot] = &font->glyphs[i];
    }
    return 0;
}

fnt_font_t *fnt_load_font(const void *data, size_t size) {
    if (!data || size < 16) {
        printf("[FNT] Error: NULL data or size too small (%lu)\n",
               (unsigned long)size);
        return NULL;
    }

    const uint8_t *p   = (const uint8_t *)data;
    const uint8_t *end = p + size;

    if (p[0] != FNT_MAGIC_0 || p[1] != FNT_MAGIC_1 ||
        p[2] != FNT_MAGIC_2 || p[3] != FNT_MAGIC_3) {
        printf("[FNT] Error: Invalid magic bytes: %c%c%c%c\n",
               p[0], p[1], p[2], p[3]);
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
    font->header.version = (uint16_t)read_u16_le(p);
    p += 2;

    if (p + 2 > end) goto error;
    uint16_t name_len = (uint16_t)read_u16_le(p);
    p += 2;

    if (p + name_len > end) goto error;
    font->header.name = (char *)malloc(name_len + 1);
    if (!font->header.name) goto error;
    memcpy(font->header.name, p, name_len);
    font->header.name[name_len] = '\0';
    p += name_len;

    if (p + 6 > end) goto error;
    font->header.glyph_width  = (uint16_t)read_u16_le(p); p += 2;
    font->header.glyph_height = (uint16_t)read_u16_le(p); p += 2;
    font->header.baseline     = (uint16_t)read_u16_le(p); p += 2;

    printf("[FNT] Font metrics: %ux%u, baseline %u\n",
           font->header.glyph_width,
           font->header.glyph_height,
           font->header.baseline);

    if (p + 4 > end) goto error;
    font->header.glyph_count = read_u32_le(p);
    p += 4;

    printf("[FNT] Glyph count: %u\n", font->header.glyph_count);

    if (font->header.glyph_count == 0 || font->header.glyph_count > 10000) {
        printf("[FNT] Error: Invalid glyph count\n");
        goto error;
    }

    font->glyphs = (fnt_glyph_t *)malloc(
                       sizeof(fnt_glyph_t) * font->header.glyph_count);
    if (!font->glyphs) goto error;
    memset(font->glyphs, 0, sizeof(fnt_glyph_t) * font->header.glyph_count);

    for (uint32_t i = 0; i < font->header.glyph_count; i++) {
        if (p + 10 > end) goto error;

        font->glyphs[i].codepoint     = read_u32_le(p); p += 4;
        font->glyphs[i].width         = (uint16_t)read_u16_le(p); p += 2;
        font->glyphs[i].bitmap_width  = (uint16_t)read_u16_le(p); p += 2;
        font->glyphs[i].bitmap_height = (uint16_t)read_u16_le(p); p += 2;

        size_t bitmap_bytes = ((size_t)font->glyphs[i].bitmap_width *
                               (size_t)font->glyphs[i].bitmap_height + 7) / 8;

        if (p + bitmap_bytes > end) goto error;

        font->glyphs[i].bitmap = (uint8_t *)malloc(bitmap_bytes);
        if (!font->glyphs[i].bitmap) goto error;
        memcpy(font->glyphs[i].bitmap, p, bitmap_bytes);
        p += bitmap_bytes;

        /* ASCII fast cache (unchanged) */
        if (font->glyphs[i].codepoint < 128)
            font->ascii_cache[font->glyphs[i].codepoint] = &font->glyphs[i];
    }

    /* Build O(1) hash table for non-ASCII codepoints */
    if (build_hash(font) != 0) {
        /* Non-fatal: hash build failed (OOM), fall back to linear scan */
        font->hash_buckets = NULL;
        font->hash_size    = 0;
        printf("[FNT] Warning: hash table alloc failed, non-ASCII lookup will be slow\n");
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

    if (font->header.name)
        free(font->header.name);

    if (font->glyphs) {
        for (uint32_t i = 0; i < font->header.glyph_count; i++)
            if (font->glyphs[i].bitmap)
                free(font->glyphs[i].bitmap);
        free(font->glyphs);
    }

    if (font->hash_buckets)
        free(font->hash_buckets);

    free(font);
}

fnt_glyph_t *fnt_get_glyph(fnt_font_t *font, uint32_t codepoint) {
    if (!font) return NULL;

    /* ASCII: direct array lookup — O(1) */
    if (codepoint < 128)
        return font->ascii_cache[codepoint];

    /* Non-ASCII: hash table lookup — O(1) average */
    if (font->hash_buckets && font->hash_size > 0) {
        uint32_t mask = font->hash_size - 1;
        uint32_t slot = codepoint & mask;
        while (font->hash_buckets[slot] != NULL) {
            if (font->hash_buckets[slot]->codepoint == codepoint)
                return font->hash_buckets[slot];
            slot = (slot + 1) & mask;
        }
        return NULL; /* not found */
    }

    /* Fallback: linear scan (only if hash table failed to build) */
    for (uint32_t i = 0; i < font->header.glyph_count; i++)
        if (font->glyphs[i].codepoint == codepoint)
            return &font->glyphs[i];

    return NULL;
}

int fnt_string_width(fnt_font_t *font, const char *text) {
    if (!font || !text) return 0;
    int width = 0;
    while (*text) {
        fnt_glyph_t *g = fnt_get_glyph(font, (uint32_t)(unsigned char)*text);
        if (g) width += g->width;
        text++;
    }
    return width;
}

int fnt_string_width_scaled(fnt_font_t *font, const char *text, int scale) {
    if (scale <= 0) scale = 1;
    return fnt_string_width(font, text) * scale;
}