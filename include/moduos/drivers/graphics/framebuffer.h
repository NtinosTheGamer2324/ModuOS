#ifndef MODUOS_DRIVERS_GRAPHICS_FRAMEBUFFER_H
#define MODUOS_DRIVERS_GRAPHICS_FRAMEBUFFER_H

#include <stdint.h>
#include <stddef.h>

// Generic framebuffer descriptor (suitable for VGA/VBE and future drivers)

typedef enum {
    FB_MODE_TEXT = 0,
    FB_MODE_GRAPHICS = 1,
} framebuffer_mode_t;

typedef enum {
    FB_FMT_UNKNOWN = 0,
    FB_FMT_RGB565 = 1,
    FB_FMT_XRGB8888 = 2,
} framebuffer_format_t;

typedef struct {
    void *addr;             // linear framebuffer virtual address (kernel)
    uint64_t phys_addr;     // physical base address of the framebuffer
    uint64_t size_bytes;    // mapped size in bytes

    uint32_t width;
    uint32_t height;
    uint32_t pitch;         // bytes per scanline
    uint8_t bpp;            // bits per pixel
    framebuffer_format_t fmt;

    // For Multiboot2 RGB framebuffers (fb_type=1): channel field positions.
    uint8_t red_pos, red_mask_size;
    uint8_t green_pos, green_mask_size;
    uint8_t blue_pos, blue_mask_size;
} framebuffer_t;

#endif
