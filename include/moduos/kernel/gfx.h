#pragma once

#include <stdint.h>
#include "moduos/drivers/graphics/framebuffer.h"
#include "moduos/drivers/graphics/gfx_mode.h"

/* Forward declaration to avoid include cycles (sqrm.h includes gfx.h). */
typedef struct sqrm_gpu_device sqrm_gpu_device_t;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Kernel callback to update the active framebuffer after a mode change.
 *
 * This should be called whenever the underlying framebuffer address/geometry changes.
 * It updates the VGA graphics backend and cleanly reinitializes the framebuffer console
 * (fb_console) so text rendering uses the new mode.
 */
int gfx_update_framebuffer(const framebuffer_t *new_fb);

// Enumerate available video modes from the active GPU driver.
// Returns number of modes written to out_modes, or negative on error.
int gfx_enumerate_modes(gfx_mode_t *out_modes, uint32_t max_modes);

// Returns the active SQRM GPU module name (e.g. "qxl_gpu", "vmsvga"), or empty string.
const char *gfx_get_sqrm_gpu_driver_name(void);

// Returns the active SQRM GPU device descriptor (includes optional accel hooks), or NULL.
const sqrm_gpu_device_t *gfx_get_sqrm_gpu_device(void);

// Query GPU capabilities (2D/3D acceleration support)
uint32_t gfx_get_gpu_caps(void);

/* ------------------------------------------------------------
 * GPU acceleration types (32bpp destination)
 * ------------------------------------------------------------
 * These types are used by the kernel to describe sources for accelerated blits.
 * They are GPU-agnostic and do not depend on DEVFS.
 */

typedef enum {
    GFX_SRC_FMT_INVALID  = 0,
    GFX_SRC_FMT_XRGB8888 = 1,
    GFX_SRC_FMT_RGB565   = 2,
} gfx_src_fmt_t;

/* Scatter/gather-backed source image.
 * phys_pages is an array of 4KiB physical page base addresses.
 */
typedef struct {
    gfx_src_fmt_t fmt;
    uint32_t pitch_bytes;
    uint32_t width;
    uint32_t height;

    const uint64_t *phys_pages;
    uint32_t page_count;

    /* Byte offset from the start of the image to the top-left pixel (src_x,src_y).
     * Usually 0.
     */
    uint64_t base_offset;
} gfx_src_sg_t;

#ifdef __cplusplus
}
#endif
