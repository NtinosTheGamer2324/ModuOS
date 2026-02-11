// tia_pf.c
#include "libc.h"
#include "tia_pf.h"

/* TIA color conversion (approximate)
 * The 2600 uses a 7-bit color value: 4-bit hue (high nibble) and 3-bit luma (bits 3..1).
 * A real implementation would use an NTSC/PAL colorburst-based palette; for now we approximate
 * using an HSV-like mapping.
 */
static inline uint32_t tia_color_to_xrgb(uint8_t c) {
    /* Hue: 0..15 */
    uint32_t hue = (uint32_t)(c >> 4) & 0x0Fu;
    /* Luma: 0..7 */
    uint32_t luma = ((uint32_t)c >> 1) & 0x07u;

    /* Map to approximate RGB.
     * - value ramps with luma
     * - saturation is fixed but reduced for very dark colors
     */
    uint32_t v = 32u + luma * 28u;      /* 32..228 */
    uint32_t s = (luma <= 1) ? 80u : 200u; /* 0..255-ish */

    /* Convert HSV(hue*22.5deg, s, v) to RGB using integer math. */
    uint32_t region = hue; /* 0..15 */
    /* Each region represents 360/16 degrees; we treat it like 6-region HSV but with 16 steps.
     * Use a simple wheel: blend primary/secondary colors.
     */
    uint32_t x = (v * s) / 255u;

    uint32_t r=0,g=0,b=0;
    switch (region) {
        case 0:  r=v; g=x; b=0; break;      /* red -> yellow */
        case 1:  r=v; g=v/2; b=0; break;
        case 2:  r=v; g=v; b=0; break;      /* yellow */
        case 3:  r=x; g=v; b=0; break;      /* yellow -> green */
        case 4:  r=0; g=v; b=0; break;      /* green */
        case 5:  r=0; g=v; b=v/2; break;
        case 6:  r=0; g=v; b=v; break;      /* cyan */
        case 7:  r=0; g=x; b=v; break;      /* cyan -> blue */
        case 8:  r=0; g=0; b=v; break;      /* blue */
        case 9:  r=v/2; g=0; b=v; break;
        case 10: r=v; g=0; b=v; break;      /* magenta */
        case 11: r=v; g=0; b=x; break;      /* magenta -> red */
        case 12: r=v; g=0; b=0; break;      /* red */
        case 13: r=v; g=v/4; b=v/8; break;
        case 14: r=v; g=v/8; b=v/16; break;
        default: r=v; g=x/2; b=0; break;
    }

    /* Clamp to 0..255 */
    if (r > 255u) r = 255u;
    if (g > 255u) g = 255u;
    if (b > 255u) b = 255u;

    return ((r & 0xFFu) << 16) | ((g & 0xFFu) << 8) | (b & 0xFFu);
}

uint32_t tia_color_to_xrgb_public(uint8_t c) {
    return tia_color_to_xrgb(c);
}

void tia_pf_state_from_regs(tia_pf_state_t *s, const uint8_t r[0x40]) {
    memset(s, 0, sizeof(*s));
    s->vsync  = r[0x00];
    s->vblank = r[0x01];
    s->colubk = r[0x09];
    s->colupf = r[0x08];
    s->ctrlpf = r[0x0A];
    s->pf0    = r[0x0D];
    s->pf1    = r[0x0E];
    s->pf2    = r[0x0F];
}

static inline int pf_bit(const tia_pf_state_t *s, int x_pf /*0..19*/) {
    /* PF is 20 bits: PF0 high nybble (bits 4-7) reversed, PF1 8 bits, PF2 8 bits reversed.
     * This is simplified but matches common documentation.
     */
    if (x_pf < 4) {
        /* PF0 bits 4..7 correspond to leftmost 4 bits, but reversed */
        int b = 7 - x_pf; /* maps 0->7 .. 3->4 */
        return (s->pf0 >> b) & 1;
    }
    if (x_pf < 12) {
        int b = 11 - x_pf; /* 4->7 ... 11->0 */
        return (s->pf1 >> b) & 1;
    }
    /* PF2 reversed */
    int b = x_pf - 12; /* 12->0 ... 19->7 */
    return (s->pf2 >> b) & 1;
}

void tia_pf_render_160x192(const tia_pf_state_t *s, uint32_t *dst, uint32_t pitch_bytes) {
    if (!s || !dst) return;

    uint32_t bg = tia_color_to_xrgb(s->colubk);
    uint32_t fg = tia_color_to_xrgb(s->colupf);

    /* Each PF bit covers 4 color clocks; we render 160px => 40 PF bits across.
     * We use 20-bit PF pattern mirrored or repeated for right half.
     */
    int reflect = (s->ctrlpf & 0x01) ? 1 : 0;

    for (uint32_t y = 0; y < 192; y++) {
        uint32_t *row = (uint32_t*)((uint8_t*)dst + (uint64_t)y * pitch_bytes);
        for (uint32_t x = 0; x < 160; x++) {
            int half = (x >= 80);
            int x_in_half = (int)(x % 80);
            int bit = x_in_half / 4; /* 0..19 */

            int pf_idx = bit;
            if (half) {
                pf_idx = reflect ? (19 - bit) : bit;
            }

            row[x] = pf_bit(s, pf_idx) ? fg : bg;
        }
    }
}
