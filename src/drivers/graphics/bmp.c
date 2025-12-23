#include "moduos/drivers/graphics/bmp.h"
#include "moduos/kernel/memory/string.h"

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int32_t rds32(const uint8_t *p) {
    return (int32_t)rd32(p);
}

static uint8_t scale_masked(uint32_t v, uint32_t mask) {
    if (mask == 0) return 0;
    /* normalize to 0..255 */
    uint32_t m = mask;
    uint32_t shift = 0;
    while ((m & 1u) == 0u) { m >>= 1; shift++; }
    uint32_t bits = 0;
    uint32_t tmp = m;
    while (tmp) { bits++; tmp >>= 1; }
    uint32_t val = (v & mask) >> shift;
    uint32_t max = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    return (uint8_t)((val * 255u) / (max ? max : 1u));
}

int bmp_parse(bmp_image_t *out, const void *bufv, size_t size) {
    if (!out || !bufv || size < 54) return -1;
    memset(out, 0, sizeof(*out));

    const uint8_t *buf = (const uint8_t*)bufv;
    if (buf[0] != 'B' || buf[1] != 'M') return -2;

    uint32_t file_size = rd32(buf + 2);
    (void)file_size;

    uint32_t pixel_off = rd32(buf + 10);
    uint32_t dib_size = rd32(buf + 14);
    if (dib_size < 40) return -3;
    if (14u + dib_size > size) return -3;

    int32_t w = rds32(buf + 18);
    int32_t h = rds32(buf + 22);
    uint16_t planes = rd16(buf + 26);
    uint16_t bpp = rd16(buf + 28);
    uint32_t comp = rd32(buf + 30);

    if (planes != 1) return -4;
    if (w <= 0 || h == 0) return -4;

    out->width = (uint32_t)w;
    out->height = (uint32_t)(h < 0 ? -h : h);
    out->top_down = (h < 0) ? 1 : 0;
    out->bpp = bpp;
    out->comp = comp;

    /* Supported:
     *  - 8bpp BI_RGB (0) with palette
     *  - 16bpp BI_RGB (0) (assume RGB555) and BI_BITFIELDS (3)
     *  - 24bpp BI_RGB (0)
     *  - 32bpp BI_RGB (0)
     *  - 32bpp BI_BITFIELDS (3) / BI_ALPHABITFIELDS (6)
     */
    if (bpp == 8) {
        if (comp != 0) return -5;
        // palette: default 256 entries unless clr_used says otherwise
        uint32_t clr_used = 0;
        if (dib_size >= 40) {
            clr_used = rd32(buf + 46);
        }
        if (clr_used == 0) clr_used = 256;
        // palette starts immediately after DIB header
        size_t pal_off = 14u + (size_t)dib_size;
        size_t pal_sz = (size_t)clr_used * 4u;
        if (pal_off + pal_sz > size) return -7;
        out->palette = buf + pal_off;
        out->palette_colors = clr_used;
        out->rmask = out->gmask = out->bmask = out->amask = 0;
        out->row_stride = ((out->width + 3u) / 4u) * 4u;
    } else if (bpp == 16) {
        if (comp == 0) {
            // Common default for 16bpp BI_RGB is RGB555
            out->rmask = 0x7C00u;
            out->gmask = 0x03E0u;
            out->bmask = 0x001Fu;
            out->amask = 0;
        } else if (comp == 3) {
            // BITFIELDS masks follow BITMAPINFOHEADER (40 bytes) or are in V4+
            if (dib_size >= 108) {
                out->rmask = rd32(buf + 54);
                out->gmask = rd32(buf + 58);
                out->bmask = rd32(buf + 62);
                out->amask = rd32(buf + 66);
            } else {
                if (size < 14 + 40 + 12) return -7;
                out->rmask = rd32(buf + 54);
                out->gmask = rd32(buf + 58);
                out->bmask = rd32(buf + 62);
                out->amask = 0;
            }
        } else {
            return -6;
        }
        // 16bpp rows are padded to 4 bytes
        out->row_stride = ((out->width * 2u + 3u) / 4u) * 4u;
    } else if (bpp == 24) {
        if (comp != 0) return -5;
        /* rows padded to 4 bytes */
        out->row_stride = ((out->width * 3u + 3u) / 4u) * 4u;
        out->rmask = out->gmask = out->bmask = out->amask = 0;
    } else if (bpp == 32) {
        if (comp == 0) {
            out->row_stride = out->width * 4u;
            out->rmask = out->gmask = out->bmask = out->amask = 0;
        } else if (comp == 3 || comp == 6) {
            /* BITMAPV4HEADER(108) or V5(124) contains masks at fixed offsets */
            if (dib_size >= 108) {
                out->rmask = rd32(buf + 54);
                out->gmask = rd32(buf + 58);
                out->bmask = rd32(buf + 62);
                out->amask = rd32(buf + 66);
            } else {
                /* BITFIELDS masks follow BITMAPINFOHEADER (40 bytes) */
                if (size < 14 + 40 + 12) return -7;
                out->rmask = rd32(buf + 54);
                out->gmask = rd32(buf + 58);
                out->bmask = rd32(buf + 62);
                out->amask = 0;
            }
            out->row_stride = out->width * 4u;
        } else {
            return -6;
        }
    } else {
        return -5;
    }

    if (pixel_off >= size) return -8;
    // Ensure palette (if any) does not overlap pixel data area
    if (out->palette && (size_t)(out->palette - buf) >= pixel_off) {
        // palette must be before pixel data
        return -7;
    }
    out->pixel_data = buf + pixel_off;
    out->pixel_data_size = size - pixel_off;

    /* Must at least contain one row */
    if (out->pixel_data_size < out->row_stride) return -9;

    return 0;
}

int bmp_get_pixel_rgba(const bmp_image_t *img, uint32_t x, uint32_t y,
                       uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a) {
    if (!img || !img->pixel_data) return 0;
    if (x >= img->width || y >= img->height) return 0;

    uint32_t yy = img->top_down ? y : (img->height - 1u - y);
    if (img->bpp == 24) {
        size_t off = (size_t)yy * img->row_stride + (size_t)x * 3u;
        if (off + 3 > img->pixel_data_size) return 0;
        /* BMP 24bpp is B,G,R */
        uint8_t bb = img->pixel_data[off + 0];
        uint8_t gg = img->pixel_data[off + 1];
        uint8_t rr = img->pixel_data[off + 2];
        if (r) *r = rr;
        if (g) *g = gg;
        if (b) *b = bb;
        if (a) *a = 255;
        return 1;
    }

    if (img->bpp == 8) {
        size_t off = (size_t)yy * img->row_stride + (size_t)x;
        if (off + 1 > img->pixel_data_size) return 0;
        uint8_t idx = img->pixel_data[off];
        if (!img->palette || idx >= img->palette_colors) return 0;
        const uint8_t *pe = img->palette + (size_t)idx * 4u;
        // palette entries are B,G,R,0
        if (b) *b = pe[0];
        if (g) *g = pe[1];
        if (r) *r = pe[2];
        if (a) *a = 255;
        return 1;
    }

    if (img->bpp == 16) {
        size_t off = (size_t)yy * img->row_stride + (size_t)x * 2u;
        if (off + 2 > img->pixel_data_size) return 0;
        uint16_t px16 = rd16(img->pixel_data + off);
        uint32_t px = (uint32_t)px16;
        if (r) *r = scale_masked(px, img->rmask);
        if (g) *g = scale_masked(px, img->gmask);
        if (b) *b = scale_masked(px, img->bmask);
        if (a) *a = img->amask ? scale_masked(px, img->amask) : 255;
        return 1;
    }

    /* 32bpp */
    size_t off = (size_t)yy * img->row_stride + (size_t)x * 4u;
    if (off + 4 > img->pixel_data_size) return 0;
    uint32_t px = rd32(img->pixel_data + off);

    if (img->comp == 3 || img->comp == 6) {
        if (r) *r = scale_masked(px, img->rmask);
        if (g) *g = scale_masked(px, img->gmask);
        if (b) *b = scale_masked(px, img->bmask);
        if (a) *a = img->amask ? scale_masked(px, img->amask) : 255;
    } else {
        /* BI_RGB: usually B,G,R,X */
        if (b) *b = (uint8_t)(px & 0xFF);
        if (g) *g = (uint8_t)((px >> 8) & 0xFF);
        if (r) *r = (uint8_t)((px >> 16) & 0xFF);
        if (a) *a = 255;
    }
    return 1;
}
