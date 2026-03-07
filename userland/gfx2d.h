#pragma once
#include <stdint.h>
#include <stddef.h>

/* For VIDEOCTL2_CAP_* and cursor structs. */
#include "../include/moduos/drivers/graphics/videoctl.h"

/* Userland wrapper around $/dev/graphics/video0 v2 (VIDEOCTL_MAGIC2).
 * - Async ENQUEUE + FLUSH API
 * - Buffer handles (ALLOC_BUF/MAP_BUF)
 *
 * This is intentionally minimal and "Linux-like": apps enqueue commands and flush once per frame.
 */

typedef struct {
    int fd;                    /* open fd to $/dev/graphics/video0 */
    void *cmdbuf;              /* mapped command buffer */
    uint32_t cmdbuf_size;      /* size of command buffer */
    uint32_t cmdbuf_used;      /* bytes used in current batch */
    uint32_t cmd_count;        /* number of commands in current batch */
} gfx2d_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t fmt;
    uint32_t caps;
    char driver[32];
} gfx2d_info_t;

/* Open/close */
int gfx2d_open(gfx2d_t *g);
int gfx2d_close(gfx2d_t *g);

/* Query */
int gfx2d_get_info(gfx2d_t *g, gfx2d_info_t *out);

/* Enqueue ops (ARGB is 0xAARRGGBB) */
int gfx2d_fill_rect(gfx2d_t *g, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t argb);
int gfx2d_blit_rect(gfx2d_t *g, uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t w, uint32_t h);

/* Buffer handles (per-FD) */
int gfx2d_alloc_buf(gfx2d_t *g, uint32_t size_bytes, uint32_t fmt, uint32_t *out_handle, uint32_t *out_pitch);
int gfx2d_map_buf(gfx2d_t *g, uint32_t handle, void **out_addr, uint32_t *out_size, uint32_t *out_pitch, uint32_t *out_fmt);

/* Enqueue blit from buffer handle */
int gfx2d_blit_buf(gfx2d_t *g, uint32_t handle,
                   uint32_t src_x, uint32_t src_y,
                   uint32_t dst_x, uint32_t dst_y,
                   uint32_t w, uint32_t h,
                   uint32_t src_pitch, uint32_t src_fmt);

/* Present (flush). If w/h are 0, kernel flushes bounding rect of queued ops. */
int gfx2d_flush(gfx2d_t *g, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* Cursor API (kernel-composited cursor). Pixels are ARGB8888 0xAARRGGBB.
 * Note: This is a "hardware cursor" equivalent implemented in-kernel.
 */
int gfx2d_cursor_set(gfx2d_t *g, uint32_t w, uint32_t h, int32_t hot_x, int32_t hot_y, const uint32_t *argb_pixels);
int gfx2d_cursor_move(gfx2d_t *g, int32_t x, int32_t y);
int gfx2d_cursor_show(gfx2d_t *g, int visible);
