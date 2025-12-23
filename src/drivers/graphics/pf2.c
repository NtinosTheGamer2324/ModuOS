#include "moduos/drivers/graphics/pf2.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/fs/hvfs.h"

static void com_print_dec64(uint64_t v) {
    char tmp[32];
    int pos = 0;
    if (v == 0) { 
        com_write_string(COM1_PORT, "0"); 
        return; 
    }
    while (v > 0 && pos < 31) {
        tmp[pos++] = '0' + (v % 10);
        v /= 10;
    }
    for (int i = pos - 1; i >= 0; i--) {
        char c[2] = { tmp[i], 0 };
        com_write_string(COM1_PORT, c);
    }
}

/* PF2 uses big-endian fields in chunks and glyph records. */
static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8) | (uint16_t)p[1];
}
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static int16_t be16s(const uint8_t *p) {
    return (int16_t)be16(p);
}

static const uint8_t *find_tag(const uint8_t *buf, size_t size, const char tag4[4]) {
    if (size < 8) return NULL;
    for (size_t i = 0; i + 8 <= size; i++) {
        if (buf[i + 0] == (uint8_t)tag4[0] &&
            buf[i + 1] == (uint8_t)tag4[1] &&
            buf[i + 2] == (uint8_t)tag4[2] &&
            buf[i + 3] == (uint8_t)tag4[3]) {
            return buf + i;
        }
    }
    return NULL;
}

/* Try to locate the real CHIX table.
 * PF2 files may contain CHIX-like byte sequences in glyph data, so we validate
 * by checking that common ASCII glyph offsets exist and point to plausible glyph records.
 */
static const uint8_t *find_chix_best(const uint8_t *buf, size_t size, uint32_t *out_len) {
    const uint8_t *best = NULL;
    uint32_t best_score = 0;
    uint32_t best_len = 0;

    for (size_t i = 0; i + 8 <= size; i++) {
        if (buf[i] != 'C' || buf[i+1] != 'H' || buf[i+2] != 'I' || buf[i+3] != 'X') continue;

        const uint8_t *p = buf + i;
        uint32_t len = be32(p + 4);
        size_t data_off = (size_t)(p - buf) + 8;
        if (len < 16) continue;
        if (data_off + (size_t)len > size) continue;

        uint32_t pairs = (uint32_t)(len / 8u);
        if (pairs < 64) continue;

        /* Build small score based on a few ASCII codepoints being present with sane offsets */
        uint32_t score = 0;
        uint32_t want[] = { 32, 33, 48, 65, 97 };

        for (uint32_t wi = 0; wi < (uint32_t)(sizeof(want)/sizeof(want[0])); wi++) {
            uint32_t target = want[wi];
            for (uint32_t j = 0; j < pairs; j++) {
                const uint8_t *e = buf + data_off + (size_t)j * 8u;
                uint32_t cp = be32(e + 0);
                uint32_t of = be32(e + 4);
                if (cp != target) continue;
                if (of < 0x100 || of >= (uint32_t)size) break;

                /* Check glyph header plausibility at of */
                if ((size_t)of + 10 > size) break;
                uint16_t gw = be16(buf + of + 0);
                uint16_t gh = be16(buf + of + 2);
                uint16_t ga = be16(buf + of + 8);
                if (gw > 0 && gw <= 128 && gh > 0 && gh <= 128 && ga <= 256) {
                    score++;
                }
                break;
            }
        }

        if (score > best_score) {
            best_score = score;
            best = p;
            best_len = len;
            if (best_score == 5) break; /* perfect */
        }
    }

    if (out_len) *out_len = best_len;
    return best;
}

static int pf2_parse_chunks(pf2_font_t *out, const uint8_t *buf, size_t size) {
    if (size < 16) return -1;

    /* PF2 starts with chunk "FILE" then 4-byte magic "PFF2" */
    if (memcmp(buf, "FILE", 4) != 0) return -2;
    if (be32(buf + 4) != 4 || memcmp(buf + 8, "PFF2", 4) != 0) {
        com_write_string(COM1_PORT, "[PF2] ERROR: missing PFF2 magic\n");
        return -2;
    }

    memset(out->ascii_offset, 0, sizeof(out->ascii_offset));

    /* This PF2 in the repo does not follow a perfectly self-describing chunk stream
     * (padding/packing varies). Instead of strict walking, locate tags by scanning.
     */
    const uint8_t *p;

    p = find_tag(buf, size, "MAXW");
    if (p && (size_t)(p - buf) + 8 + 2 <= size) out->maxw = be16(p + 8);

    p = find_tag(buf, size, "MAXH");
    if (p && (size_t)(p - buf) + 8 + 2 <= size) out->maxh = be16(p + 8);

    p = find_tag(buf, size, "ASCE");
    if (p && (size_t)(p - buf) + 8 + 2 <= size) out->asce = be16(p + 8);

    p = find_tag(buf, size, "DESC");
    if (p && (size_t)(p - buf) + 8 + 2 <= size) out->desc = be16(p + 8);

    /* CHIX: locate best candidate by scoring */
    uint32_t chix_len = 0;
    p = find_chix_best(buf, size, &chix_len);
    if (!p) {
        com_write_string(COM1_PORT, "[PF2] ERROR: CHIX tag not found\n");
        return -3;
    }

    size_t chix_off = (size_t)(p - buf) + 8;
    if (chix_off + chix_len > size) {
        com_write_string(COM1_PORT, "[PF2] ERROR: CHIX chunk past EOF\n");
        return -3;
    }

    if ((chix_len % 8) != 0) {
        com_write_string(COM1_PORT, "[PF2] WARN: CHIX len not multiple of 8, len=");
        com_print_dec64((uint64_t)chix_len);
        com_write_string(COM1_PORT, " (ignoring tail bytes)\n");
    }

    const uint8_t *data = buf + chix_off;
    uint32_t pairs = (uint32_t)(chix_len / 8u);
    for (uint32_t i = 0; i < pairs * 8u; i += 8u) {
        uint32_t cp = be32(data + i);
        uint32_t goff = be32(data + i + 4);
        if (cp < 256 && goff < (uint32_t)size) out->ascii_offset[cp] = goff;
    }

    if (out->maxw == 0 || out->maxh == 0) {
        com_write_string(COM1_PORT, "[PF2] WARN: MAXW/MAXH missing or zero\n");
    }

    return 0;
}

int pf2_font_from_buffer(pf2_font_t *out, void *file_buf, size_t file_size) {
    if (!out || !file_buf || file_size < 8) return -1;
    memset(out, 0, sizeof(*out));
    out->file_buf = file_buf;
    out->file_size = file_size;

    int r = pf2_parse_chunks(out, (const uint8_t*)file_buf, file_size);
    if (r != 0) {
        pf2_font_destroy(out);
        return r;
    }

    return 0;
}

int pf2_font_load_from_mount_slot(pf2_font_t *out, int mount_slot, const char *path) {
    if (!out || !path) return -1;
    void *buf = NULL;
    size_t sz = 0;
    int r = hvfs_read(mount_slot, path, &buf, &sz);
    if (r != 0) {
        com_write_string(COM1_PORT, "[PF2] hvfs_read failed for ");
        com_write_string(COM1_PORT, path);
        com_write_string(COM1_PORT, " (hvfs_err=");
        com_print_dec64((uint64_t)r);
        com_write_string(COM1_PORT, ")\n");
        return -100 - r; /* preserve info */
    }

    int pr = pf2_font_from_buffer(out, buf, sz);
    if (pr != 0) {
        com_write_string(COM1_PORT, "[PF2] parse failed for ");
        com_write_string(COM1_PORT, path);
        com_printf(COM1_PORT, " (err=%d)\n", pr);
        /* pf2_font_from_buffer frees on failure */
        return pr;
    }

    com_write_string(COM1_PORT, "[PF2] Loaded font bytes=");
    com_print_dec64((uint64_t)sz);
    com_write_string(COM1_PORT, " maxw=");
    com_print_dec64((uint64_t)out->maxw);
    com_write_string(COM1_PORT, " maxh=");
    com_print_dec64((uint64_t)out->maxh);
    com_write_string(COM1_PORT, "\n");

    return 0;
}

void pf2_font_destroy(pf2_font_t *font) {
    if (!font) return;
    if (font->file_buf) {
        kfree(font->file_buf);
    }
    memset(font, 0, sizeof(*font));
}

int pf2_get_glyph(const pf2_font_t *font, uint32_t codepoint, pf2_glyph_t *out_g) {
    if (!font || !font->file_buf || !out_g) return 0;

    uint32_t goff = 0;
    if (codepoint < 256) goff = font->ascii_offset[codepoint];
    if (goff == 0) return 0;
    if ((size_t)goff + 10 > font->file_size) return 0;

    const uint8_t *p = (const uint8_t*)font->file_buf + goff;

    /* Observed glyph record format in this PF2:
     *   u16 width
     *   u16 height
     *   s16 xoff
     *   s16 yoff
     *   u16 advance
     *   bitmap bytes follow (packed bits), commonly padded to 2-byte boundary.
     */
    uint16_t w = be16(p + 0);
    uint16_t h = be16(p + 2);
    int16_t xo = be16s(p + 4);
    int16_t yo = be16s(p + 6);
    uint16_t adv = be16(p + 8);

    /* Sanity checks: avoid drawing garbage if offsets/format are wrong */
    if (w > 128 || h > 128 || adv > 256) {
        static int logged = 0;
        if (!logged) {
            com_printf(COM1_PORT, "[PF2] WARN: suspicious glyph cp=%u off=0x%x w=%u h=%u adv=%u\n",
                       codepoint, goff, (unsigned)w, (unsigned)h, (unsigned)adv);
            logged = 1;
        }
        return 0;
    }

    size_t row_bytes = (w + 7u) / 8u;
    /* Some PF2 glyph bitmaps pad rows to 16 bits. If so, row_bytes becomes even. */
    if ((row_bytes & 1u) && w > 8) {
        /* keep as-is */
    }
    size_t bm = row_bytes * h;

    if (row_bytes == 0 || bm > 8192) return 0;

    if ((size_t)goff + 10 + bm > font->file_size) {
        static int logged2 = 0;
        if (!logged2) {
            com_printf(COM1_PORT, "[PF2] WARN: glyph bitmap past EOF cp=%u off=0x%x w=%u h=%u bm=%u\n",
                       codepoint, goff, (unsigned)w, (unsigned)h, (unsigned)bm);
            logged2 = 1;
        }
        return 0;
    }

    out_g->width = w;
    out_g->height = h;
    out_g->xoff = xo;
    out_g->yoff = yo;
    out_g->advance = adv;
    out_g->bitmap = p + 10;
    out_g->bitmap_size = bm;
    return 1;
}
