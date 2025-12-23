#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal PF2 (GRUB font) support.
 * We only need enough to render ASCII text to the linear framebuffer.
 */

typedef struct {
    uint16_t width;
    uint16_t height;
    int16_t xoff;
    int16_t yoff;
    uint16_t advance;
    const uint8_t *bitmap;   /* packed bits, row-major */
    size_t bitmap_size;      /* bytes */
} pf2_glyph_t;

typedef struct {
    /* Raw file image (owned) */
    void *file_buf;
    size_t file_size;

    /* Metrics */
    uint16_t maxw;
    uint16_t maxh;
    uint16_t asce;
    uint16_t desc;

    /* ASCII fast-path: codepoint -> glyph offset in file (0 means missing) */
    uint32_t ascii_offset[256];
} pf2_font_t;

/* Parse PF2 from an in-memory buffer (takes ownership of file_buf). */
int pf2_font_from_buffer(pf2_font_t *out, void *file_buf, size_t file_size);

/* Load PF2 from filesystem (uses hvfs_read under the hood). */
int pf2_font_load_from_mount_slot(pf2_font_t *out, int mount_slot, const char *path);

void pf2_font_destroy(pf2_font_t *font);

/* Parse a glyph at codepoint into out_g.
 * Note: out_g->bitmap points into the font's file buffer.
 */
int pf2_get_glyph(const pf2_font_t *font, uint32_t codepoint, pf2_glyph_t *out_g);

#ifdef __cplusplus
}
#endif
