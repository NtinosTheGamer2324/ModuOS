// lib_8bit.c
#pragma once
#include <stdint.h>
#include <stddef.h>

/* Lib8bit: shared low-level helpers for 8-bit console emulators.
 * Keep this OS-agnostic where possible.
 */

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch_bytes;
    uint32_t fmt; /* MD64API_GRP_FMT_* */
    void *pixels; /* mapped linear buffer (owned by caller) */
} emu_frame_t;

/* Simple clamp helper */
static inline uint32_t emu_u32_min(uint32_t a, uint32_t b) { return a < b ? a : b; }

/* Time/tick abstraction (placeholder).
 * TODO: replace with a proper kernel/userland timing API.
 */
uint64_t emu_get_ticks_ms(void);
