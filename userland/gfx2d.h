#pragma once
// gfx2d - low-level graphics device wrapper for ModuOS video0 driver
//
// Copyright © 2025-2026 ModuOS Project Contributors
// Licensed under GPL v2.0

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t fmt;
    uint32_t caps;
    char     driver[32];
} gfx2d_info_t;

typedef struct {
    int fd;

    /* ---------------------------------------------------------------
     * Zero-copy path (VIDEOCTL_CMD2_MAP_CMDBUF)
     *
     * mapped_cmdbuf points to a page shared between userspace and the
     * kernel driver.  Commands are written directly here with no copy
     * through the write() syscall.  A single SUBMIT_CMDBUF write at
     * flush time tells the kernel how many commands to consume.
     * --------------------------------------------------------------- */
    int      use_mapped_cmdbuf;   /* 1 if this path is active          */
    void    *mapped_cmdbuf;       /* kernel-shared buffer base address  */
    uint32_t mapped_cmdbuf_size;  /* total size of the mapped region    */
    uint32_t mapped_cmdbuf_used;  /* bytes written this frame           */
    uint32_t mapped_cmd_count;    /* commands written this frame        */

    /* ---------------------------------------------------------------
     * Copy-batch fallback path
     *
     * Commands are memcpy'd into a malloc'd buffer and submitted in
     * one write() call at flush time.
     * --------------------------------------------------------------- */
    void    *cmdbuf;        /* malloc'd userspace buffer (or NULL)      */
    uint32_t cmdbuf_size;   /* allocated size in bytes                  */
    uint32_t cmdbuf_used;   /* bytes written this frame                 */
    uint32_t cmd_count;     /* commands written this frame              */
} gfx2d_t;

int  gfx2d_open(gfx2d_t *g);
int  gfx2d_close(gfx2d_t *g);
int  gfx2d_get_info(gfx2d_t *g, gfx2d_info_t *out);

int  gfx2d_fill_rect(gfx2d_t *g, uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h, uint32_t argb);

int  gfx2d_blit_rect(gfx2d_t *g, uint32_t src_x, uint32_t src_y,
                     uint32_t dst_x, uint32_t dst_y, uint32_t w, uint32_t h);

int  gfx2d_blit_buf(gfx2d_t *g, uint32_t handle,
                    uint32_t src_x, uint32_t src_y,
                    uint32_t dst_x, uint32_t dst_y,
                    uint32_t w,     uint32_t h,
                    uint32_t src_pitch, uint32_t src_fmt);

int  gfx2d_alloc_buf(gfx2d_t *g, uint32_t size_bytes, uint32_t fmt,
                     uint32_t *out_handle, uint32_t *out_pitch);

int  gfx2d_map_buf(gfx2d_t *g, uint32_t handle,
                   void **out_addr, uint32_t *out_size,
                   uint32_t *out_pitch, uint32_t *out_fmt);

int  gfx2d_flush(gfx2d_t *g, uint32_t x, uint32_t y,
                 uint32_t w, uint32_t h);

int  gfx2d_cursor_set(gfx2d_t *g, uint32_t w, uint32_t h,
                      int32_t hot_x, int32_t hot_y,
                      const uint32_t *argb_pixels);
int  gfx2d_cursor_move(gfx2d_t *g, int32_t x, int32_t y);
int  gfx2d_cursor_show(gfx2d_t *g, int visible);