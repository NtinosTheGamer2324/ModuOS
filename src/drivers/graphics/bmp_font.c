#include "moduos/drivers/graphics/bmp_font.h"
#include "moduos/kernel/memory/string.h"

static uint8_t lum(uint8_t r, uint8_t g, uint8_t b) {
    /* cheap luma approximation */
    return (uint8_t)((uint16_t)r/4u + (uint16_t)g/2u + (uint16_t)b/4u);
}

int bmp_font_init_moduosdef(bmp_font_t *out, const void *bmp_buf, size_t bmp_size) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    int r = bmp_parse(&out->img, bmp_buf, bmp_size);
    if (r != 0) return r;

    /* From file header observation: 570x150.
     * Assume 19 columns x 5 rows => 95 glyphs.
     * Each cell 30x30.
     */
    out->cell_w = 30;
    out->cell_h = 30;
    out->cols = 19;
    out->rows = 5;
    out->first_char = 0x20;
    out->char_count = 95;
    out->threshold = 32;
    out->invert = 0;

    if (out->img.width < (uint32_t)(out->cols * out->cell_w) ||
        out->img.height < (uint32_t)(out->rows * out->cell_h)) {
        return -20;
    }

    /* Auto-detect inversion by sampling the background of the first cell.
     * If the background is bright (luma high), we assume glyphs are dark and invert.
     */
    {
        uint32_t x0 = 0;
        uint32_t y0 = 0;
        uint32_t bright = 0, total = 0;

        /* If alpha is present, inversion is not meaningful for transparent backgrounds. */
        if (out->img.amask != 0) {
            out->invert = 0;
            return 0;
        }
        for (uint32_t sy = 0; sy < 8; sy++) {
            for (uint32_t sx = 0; sx < 8; sx++) {
                uint8_t rr, gg, bb, aa;
                if (!bmp_get_pixel_rgba(&out->img, x0 + sx, y0 + sy, &rr, &gg, &bb, &aa)) continue;
                uint8_t l = lum(rr, gg, bb);
                if (l >= 128) bright++;
                total++;
            }
        }
        if (total > 0 && bright > (total * 3u) / 4u) {
            out->invert = 1;
        }
    }

    return 0;
}

int bmp_font_glyph_pixel_on(const bmp_font_t *f, uint8_t ch, uint16_t gx, uint16_t gy) {
    if (!f) return 0;
    if (ch < f->first_char) return 0;
    uint32_t idx = (uint32_t)(ch - f->first_char);
    if (idx >= f->char_count) return 0;

    if (gx >= f->cell_w || gy >= f->cell_h) return 0;

    /* Hard safety: avoid divide-by-zero if font struct is corrupted */
    if (f->cols == 0 || f->rows == 0) return 0;

    uint32_t col = idx % f->cols;
    uint32_t row = idx / f->cols;
    if (row >= f->rows) return 0;

    uint32_t x = col * f->cell_w + gx;
    uint32_t y = row * f->cell_h + gy;

    uint8_t r,g,b,a;
    if (!bmp_get_pixel_rgba(&f->img, x, y, &r, &g, &b, &a)) return 0;

    /* If the BMP provides alpha, treat alpha as authoritative.
     * This avoids RGB garbage in fully transparent pixels.
     */
    if (f->img.amask != 0) {
        return (a >= f->threshold) ? 1 : 0;
    }

    int on = (lum(r,g,b) >= f->threshold);
    return f->invert ? !on : on;
}
