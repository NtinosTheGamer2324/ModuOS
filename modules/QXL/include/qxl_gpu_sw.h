#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// Software (CPU-side) rendering fallbacks for the QXL GPU driver.
// These operate directly on the mapped QXL primary surface memory.
// ---------------------------------------------------------------------------

int sw_fill_rect32_native(const framebuffer_t *fb,
                           uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h,
                           uint32_t native_pixel);

int sw_blit_rect32(const framebuffer_t *fb,
                   uint32_t src_x, uint32_t src_y,
                   uint32_t dst_x, uint32_t dst_y,
                   uint32_t w, uint32_t h);

int sw_cursor_set_argb32(uint32_t w, uint32_t h,
                          int32_t hot_x, int32_t hot_y,
                          const uint32_t *pixels_argb);

int sw_cursor_move(int32_t x, int32_t y);

int sw_cursor_show(int visible);

int sw_set_mode(uint32_t width, uint32_t height, uint32_t bpp);