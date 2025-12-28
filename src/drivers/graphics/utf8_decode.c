#include <stdint.h>
#include <stddef.h>

/*
 * Minimal UTF-8 decoder.
 * - Validates continuation bytes.
 * - Rejects overlong encodings.
 * - Rejects surrogate range U+D800..U+DFFF.
 * - Rejects > U+10FFFF.
 * Returns number of bytes consumed (>0) on success, 0 on invalid/incomplete.
 */
size_t utf8_decode_one(const char *s, size_t n, uint32_t *out_cp) {
    if (!s || n == 0 || !out_cp) return 0;

    uint8_t b0 = (uint8_t)s[0];
    if (b0 < 0x80) {
        *out_cp = b0;
        return 1;
    }

    /* 2-byte */
    if ((b0 & 0xE0) == 0xC0) {
        if (n < 2) return 0;
        uint8_t b1 = (uint8_t)s[1];
        if ((b1 & 0xC0) != 0x80) return 0;
        uint32_t cp = ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(b1 & 0x3F);
        if (cp < 0x80) return 0; /* overlong */
        *out_cp = cp;
        return 2;
    }

    /* 3-byte */
    if ((b0 & 0xF0) == 0xE0) {
        if (n < 3) return 0;
        uint8_t b1 = (uint8_t)s[1];
        uint8_t b2 = (uint8_t)s[2];
        if (((b1 & 0xC0) != 0x80) || ((b2 & 0xC0) != 0x80)) return 0;
        uint32_t cp = ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(b1 & 0x3F) << 6) | (uint32_t)(b2 & 0x3F);
        if (cp < 0x800) return 0; /* overlong */
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0; /* surrogate */
        *out_cp = cp;
        return 3;
    }

    /* 4-byte */
    if ((b0 & 0xF8) == 0xF0) {
        if (n < 4) return 0;
        uint8_t b1 = (uint8_t)s[1];
        uint8_t b2 = (uint8_t)s[2];
        uint8_t b3 = (uint8_t)s[3];
        if (((b1 & 0xC0) != 0x80) || ((b2 & 0xC0) != 0x80) || ((b3 & 0xC0) != 0x80)) return 0;
        uint32_t cp = ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(b1 & 0x3F) << 12) | ((uint32_t)(b2 & 0x3F) << 6) | (uint32_t)(b3 & 0x3F);
        if (cp < 0x10000) return 0; /* overlong */
        if (cp > 0x10FFFF) return 0;
        *out_cp = cp;
        return 4;
    }

    return 0;
}
