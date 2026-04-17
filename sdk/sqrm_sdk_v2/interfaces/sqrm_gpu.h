#pragma once
/*
 * sqrm_gpu.h — SQRM GPU / framebuffer driver ABI.
 *
 * Available to: GPU modules only.
 * gfx_register_framebuffer and gfx_update_framebuffer in sqrm_kernel_api_t
 * are NULL for all other module types.
 *
 * A GPU module must call api->gfx_register_framebuffer() from sqrm_module_init()
 * with a fully populated sqrm_gpu_device_t.
 *
 * All optional hook pointers (flush, cursor_*, fill_rect32_native, blit_*,
 * draw_*, set_mode, enumerate_modes, shutdown) may be NULL; the kernel will
 * fall back to software implementations where possible.
 *
 * Colors / pixels are ARGB8888 (0xAARRGGBB) unless noted otherwise.
 * All 2D/3D hooks are called from thread context only (never from IRQ).
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Mode descriptor                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
} gfx_mode_t;

/* ------------------------------------------------------------------ */
/*  Framebuffer descriptor                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    void    *addr;          /* kernel virtual address of the framebuffer */
    uint64_t phys_addr;     /* physical address (for DMA / hardware scanout) */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;         /* bytes per scanline */
    uint32_t bpp;           /* bits per pixel */
} framebuffer_t;

/* ------------------------------------------------------------------ */
/*  Scatter-gather source descriptor (for blit_from_sg32)             */
/* ------------------------------------------------------------------ */

typedef struct {
    const void *addr;
    uint32_t    width;
    uint32_t    height;
    uint32_t    pitch;
} gfx_src_sg_t;

/* ------------------------------------------------------------------ */
/*  GPU device (passed to gfx_register_framebuffer)                   */
/* ------------------------------------------------------------------ */

typedef struct sqrm_gpu_device {
    framebuffer_t fb;

    /* -------------------------------------------------------------- */
    /*  Flush hook (optional)                                          */
    /*                                                                 */
    /*  Called after the kernel has drawn into fb.addr to push the    */
    /*  updated region to the display hardware.                        */
    /*  If NULL, fb.addr is assumed to be directly scanned out.       */
    /* -------------------------------------------------------------- */
    void (*flush)(const framebuffer_t *fb,
                  uint32_t x, uint32_t y, uint32_t w, uint32_t h);

    /* -------------------------------------------------------------- */
    /*  Hardware cursor hooks (optional)                               */
    /*                                                                 */
    /*  When provided the kernel can reposition / toggle the cursor   */
    /*  without repainting the framebuffer.                            */
    /*  Pixels are ARGB8888.  Return 0 on success.                    */
    /* -------------------------------------------------------------- */
    int (*cursor_set_argb32)(uint32_t w, uint32_t h,
                             int32_t hot_x, int32_t hot_y,
                             const uint32_t *pixels_argb);
    int (*cursor_move)(int32_t x, int32_t y);
    int (*cursor_show)(int visible);

    /* -------------------------------------------------------------- */
    /*  2D acceleration hooks (optional, thread-context only)         */
    /*                                                                 */
    /*  Enabled automatically when provided and fb.bpp == 32.         */
    /*  Colors are native pixels for the current fb format.           */
    /*  Return 0 on success, negative errno on failure.               */
    /* -------------------------------------------------------------- */
    int (*fill_rect32_native)(const framebuffer_t *fb,
                              uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h,
                              uint32_t native_pixel);

    int (*blit_rect32)(const framebuffer_t *fb,
                       uint32_t src_x, uint32_t src_y,
                       uint32_t dst_x, uint32_t dst_y,
                       uint32_t w,     uint32_t h);

    int (*blit_from_sg32)(const framebuffer_t *fb, const gfx_src_sg_t *src,
                          uint32_t src_x, uint32_t src_y,
                          uint32_t dst_x, uint32_t dst_y,
                          uint32_t w,     uint32_t h);

    /* -------------------------------------------------------------- */
    /*  3D acceleration hooks (optional, future)                      */
    /*                                                                 */
    /*  Basic triangle rasterization / texture mapping.               */
    /*  Return 0 on success, negative errno on failure.               */
    /* -------------------------------------------------------------- */
    int (*draw_triangle)(const framebuffer_t *fb,
                         int32_t x0, int32_t y0, uint32_t color0,
                         int32_t x1, int32_t y1, uint32_t color1,
                         int32_t x2, int32_t y2, uint32_t color2);

    int (*draw_textured_triangle)(const framebuffer_t *fb,
                                  int32_t x0, int32_t y0, float u0, float v0,
                                  int32_t x1, int32_t y1, float u1, float v1,
                                  int32_t x2, int32_t y2, float u2, float v2,
                                  uint32_t texture_id);

    /* Future: vertex buffer submission, transform matrices, etc. */

    /* -------------------------------------------------------------- */
    /*  Mode control (optional)                                        */
    /* -------------------------------------------------------------- */

    /* set_mode() — request a resolution / depth change; return 0 on success */
    int (*set_mode)(uint32_t width, uint32_t height, uint32_t bpp);

    /*
     * enumerate_modes() — write up to max_modes entries into out_modes.
     * Returns the number of modes written, or negative errno on error.
     */
    int (*enumerate_modes)(gfx_mode_t *out_modes, uint32_t max_modes);

    /* -------------------------------------------------------------- */
    /*  Capability flags                                               */
    /* -------------------------------------------------------------- */
    uint32_t caps;
#define SQRM_GPU_CAP_2D_ACCEL      (1u << 0)  /* fill_rect32_native / blit_rect32 */
#define SQRM_GPU_CAP_3D_TRIANGLES  (1u << 1)  /* draw_triangle                    */
#define SQRM_GPU_CAP_3D_TEXTURES   (1u << 2)  /* draw_textured_triangle           */
#define SQRM_GPU_CAP_HW_CURSOR     (1u << 3)  /* cursor_set_argb32 / move / show  */
#define SQRM_GPU_CAP_VSYNC         (1u << 4)  /* vsync supported                  */

    /* -------------------------------------------------------------- */
    /*  Lifecycle (optional)                                           */
    /* -------------------------------------------------------------- */

    /* shutdown() — called on module unload (not yet implemented by kernel) */
    void (*shutdown)(void);
} sqrm_gpu_device_t;

#ifdef __cplusplus
}
#endif