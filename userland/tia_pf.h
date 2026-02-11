// tia_pf.h
#pragma once
#include <stdint.h>

/* Minimal TIA playfield renderer (WIP)
 * Uses PF0/PF1/PF2 + CTRLPF + COLUBK/COLUPF.
 */

typedef struct {
    uint8_t vsync;
    uint8_t vblank;
    uint8_t colubk;
    uint8_t colupf;
    uint8_t ctrlpf;
    uint8_t pf0, pf1, pf2;
} tia_pf_state_t;

void tia_pf_state_from_regs(tia_pf_state_t *s, const uint8_t tia_regs[0x40]);

/* Convert a raw TIA color byte to XRGB8888 (approximate palette). */
uint32_t tia_color_to_xrgb_public(uint8_t c);

/* Render playfield-only into a 160x192 XRGB8888 buffer.
 * This is a simplified approximation; timing will be refined later.
 */
void tia_pf_render_160x192(const tia_pf_state_t *s, uint32_t *dst, uint32_t pitch_bytes);
