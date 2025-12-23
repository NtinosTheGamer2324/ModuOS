#pragma once

#include "moduos/drivers/graphics/framebuffer.h"
#include <stdint.h>

// Bounds-checked framebuffer helpers for kernel tasks.
// Intended for debugging early graphics code that may overrun pitch/height.

void fb_checked_putpixel32(const framebuffer_t *fb, uint32_t x, uint32_t y, uint32_t px);
void fb_checked_fill_rect32(const framebuffer_t *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t px);
