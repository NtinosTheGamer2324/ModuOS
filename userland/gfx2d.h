#pragma once
/*
 * gfx2d.h — ModuOS low-level graphics wrapper (MVC3 backend)
 *
 * Public API is identical to the old videoctl-based gfx2d.h so that
 * existing callers need zero changes.  The implementation now targets
 * $/dev/mvc/mvi0 and speaks the MVC3 wire protocol (mvc3.h).
 *
 * Copyright © 2025-2026 ModuOS Project Contributors — GPL v2.0
 */

#include <stdint.h>
#include <stddef.h>

/* ── Info struct (returned by gfx2d_get_info) ──────────────────────── */

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t fmt;
    uint32_t caps;
    char     driver[32];
} gfx2d_info_t;

/* ── Device handle ─────────────────────────────────────────────────── */

typedef struct {
    int fd;   /* file descriptor for $/dev/mvc/mvi0 */

    /* -------------------------------------------------------------------
     * Zero-copy ring path (MVC3_CMD_MAP_RING)
     *
     * The kernel allocates the ring and maps it into this process's VA.
     * Draw commands are written directly into ring slots — no syscall
     * until gfx2d_flush() sends a single MVC3_CMD_SUBMIT packet.
     * ------------------------------------------------------------------ */
    int      use_mapped_cmdbuf;   /* 1 if ring path is active            */
    void    *mapped_cmdbuf;       /* base of the kernel-owned ring        */
    uint32_t mapped_cmdbuf_size;  /* total ring size in bytes             */
    uint32_t mapped_cmdbuf_used;  /* bytes written this frame             */
    uint32_t mapped_cmd_count;    /* slots written this frame             */

    /* -------------------------------------------------------------------
     * Copy-batch fallback path
     *
     * MVC3_CMD_ENQUEUE packets are memcpy'd into a malloc'd buffer and
     * submitted in one write() call at flush time.
     * ------------------------------------------------------------------ */
    void    *cmdbuf;        /* malloc'd accumulation buffer (or NULL)    */
    uint32_t cmdbuf_size;   /* allocated size in bytes                   */
    uint32_t cmdbuf_used;   /* bytes written this frame                  */
    uint32_t cmd_count;     /* commands written this frame               */
} gfx2d_t;

/* ── Public API ────────────────────────────────────────────────────── */

/* Open $/dev/mvc/mvi0.  Tries the zero-copy ring path first;
 * falls back to copy-batch if MAP_RING fails. */
int  gfx2d_open(gfx2d_t *g);

/* Flush any pending work and close the device. */
int  gfx2d_close(gfx2d_t *g);

/* Query framebuffer geometry, format, and GPU capability flags. */
int  gfx2d_get_info(gfx2d_t *g, gfx2d_info_t *out);

/* Fill a solid rectangle with an ARGB8888 colour. */
int  gfx2d_fill_rect(gfx2d_t *g, uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h, uint32_t argb);

/* Screen-to-screen pixel copy. */
int  gfx2d_blit_rect(gfx2d_t *g,
                     uint32_t src_x, uint32_t src_y,
                     uint32_t dst_x, uint32_t dst_y,
                     uint32_t w, uint32_t h);

/* Blit from an off-screen buffer (allocated with gfx2d_alloc_buf). */
int  gfx2d_blit_buf(gfx2d_t *g, uint32_t handle,
                    uint32_t src_x, uint32_t src_y,
                    uint32_t dst_x, uint32_t dst_y,
                    uint32_t w,     uint32_t h,
                    uint32_t src_pitch, uint32_t src_fmt);

/* Allocate an off-screen pixel buffer. */
int  gfx2d_alloc_buf(gfx2d_t *g, uint32_t size_bytes, uint32_t fmt,
                     uint32_t *out_handle, uint32_t *out_pitch);

/* Map an allocated buffer into userspace. */
int  gfx2d_map_buf(gfx2d_t *g, uint32_t handle,
                   void **out_addr, uint32_t *out_size,
                   uint32_t *out_pitch, uint32_t *out_fmt);

/* Submit all pending draw commands and present a screen region.
 * Pass (0,0,0,0) to present the entire screen. */
int  gfx2d_flush(gfx2d_t *g, uint32_t x, uint32_t y,
                 uint32_t w, uint32_t h);

/* Upload a hardware cursor bitmap (ARGB8888). */
int  gfx2d_cursor_set(gfx2d_t *g, uint32_t w, uint32_t h,
                      int32_t hot_x, int32_t hot_y,
                      const uint32_t *argb_pixels);

/* Move the hardware cursor to (x, y). */
int  gfx2d_cursor_move(gfx2d_t *g, int32_t x, int32_t y);

/* Show (visible=1) or hide (visible=0) the hardware cursor. */
int  gfx2d_cursor_show(gfx2d_t *g, int visible);