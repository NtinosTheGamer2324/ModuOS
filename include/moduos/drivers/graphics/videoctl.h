#pragma once
// Shared control protocol for $/dev/graphics/video0 (DEVFS write-based ioctl)

#include <stdint.h>
#include "gfx_mode.h"

#define VIDEOCTL_MAGIC 0x4D564344u /* 'MVCD' ModuOS Video Control (legacy v1) */

/* New versioned ABI (v2): stable binary ioctl-style protocol via devfs write.
 * We keep the legacy struct for SET_MODE/GET_MODES, but all new accelerated
 * rendering ops use VIDEOCTL_MAGIC2 and the v2 structs below.
 */
#define VIDEOCTL_MAGIC2 0x4D564332u /* 'MVC2' ModuOS Video Control v2 */
#define VIDEOCTL_ABI_VERSION 1u

typedef enum {
    /* legacy */
    VIDEOCTL_CMD_SET_MODE = 1,
    VIDEOCTL_CMD_GET_MODES = 2,

    /* v2 (async) */
    VIDEOCTL_CMD2_GET_INFO    = 100,
    VIDEOCTL_CMD2_ENQUEUE     = 101,
    VIDEOCTL_CMD2_FLUSH       = 102,
    VIDEOCTL_CMD2_ALLOC_BUF   = 103,
    VIDEOCTL_CMD2_MAP_BUF     = 104,

    /* v2 (cursor) */
    VIDEOCTL_CMD2_CURSOR_SET  = 110,
    VIDEOCTL_CMD2_CURSOR_MOVE = 111,
    VIDEOCTL_CMD2_CURSOR_SHOW = 112,
} videoctl_cmd_t;

/* Legacy request (kept for compatibility). */
typedef struct {
    uint32_t magic;
    uint32_t cmd;

    // For SET_MODE
    uint32_t width;
    uint32_t height;
    uint32_t bpp;

    // For GET_MODES
    uint64_t out_modes_user; // user pointer to gfx_mode_t[]
    uint32_t max_modes;      // capacity in entries
} videoctl_req_t;

/* v2 header: every v2 message begins with this. */
typedef struct {
    uint32_t magic;       // VIDEOCTL_MAGIC2
    uint32_t abi_version; // VIDEOCTL_ABI_VERSION
    uint32_t cmd;         // VIDEOCTL_CMD2_*
    uint32_t size_bytes;  // sizeof(full struct)
} videoctl2_hdr_t;

/* Capability bits for GET_INFO. */
#define VIDEOCTL2_CAP_ENQUEUE_FILL_RECT (1u << 0)
#define VIDEOCTL2_CAP_ENQUEUE_BLIT      (1u << 1)
#define VIDEOCTL2_CAP_ENQUEUE_BLIT_BUF  (1u << 2)
#define VIDEOCTL2_CAP_FLUSH             (1u << 3)
#define VIDEOCTL2_CAP_BUF_HANDLES       (1u << 4)
#define VIDEOCTL2_CAP_BUF_SG_PAGES      (1u << 5) /* buffer memory is non-contiguous scatter/gather pages */
#define VIDEOCTL2_CAP_HW_CURSOR         (1u << 6) /* kernel-composited cursor (set/move/show) */

typedef struct {
    videoctl2_hdr_t hdr;

    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;

    /* Stable pixel format enum (reuse MD64API grp fmts) */
    uint32_t fmt;

    /* Capability bitmask (VIDEOCTL2_CAP_*) */
    uint32_t caps;

    /* Current driver name (NUL-terminated, truncated) */
    char driver[32];
} videoctl2_info_t;

/* Command types for ENQUEUE. */
typedef enum {
    VIDEOCTL2_OP_FILL_RECT = 1,
    VIDEOCTL2_OP_BLIT      = 2,
    VIDEOCTL2_OP_BLIT_BUF  = 3,
} videoctl2_op_t;

/* Fill rectangle op: color is 0xAARRGGBB. */
typedef struct {
    uint32_t op; // VIDEOCTL2_OP_FILL_RECT
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
    uint32_t argb;
} videoctl2_op_fill_rect_t;

/* Blit/copy rectangle within the same framebuffer.
 * Equivalent to memmove of a rectangle: copies src->dst and handles overlap.
 */
typedef struct {
    uint32_t op; // VIDEOCTL2_OP_BLIT
    uint32_t src_x;
    uint32_t src_y;
    uint32_t dst_x;
    uint32_t dst_y;
    uint32_t w;
    uint32_t h;
} videoctl2_op_blit_t;

/* Blit from a per-FD buffer handle into the framebuffer. */
typedef struct {
    uint32_t op; // VIDEOCTL2_OP_BLIT_BUF
    uint32_t handle; // buffer handle (starts at 1)
    uint32_t src_x;
    uint32_t src_y;
    uint32_t dst_x;
    uint32_t dst_y;
    uint32_t w;
    uint32_t h;
    uint32_t src_pitch;
    uint32_t src_fmt; // MD64API_GRP_FMT_XRGB8888 or MD64API_GRP_FMT_RGB565
} videoctl2_op_blit_buf_t;

/* ENQUEUE: copy exactly one op into the kernel queue (async).
 * Returns:
 *  - sizeof(req) on success
 *  - -EAGAIN if queue is full
 */
typedef struct {
    videoctl2_hdr_t hdr;
    union {
        videoctl2_op_fill_rect_t   fill;
        videoctl2_op_blit_t        blit;
        videoctl2_op_blit_buf_t    blit_buf;
    } u;
} videoctl2_enqueue_t;

/* FLUSH: submit queued commands and present updates.
 * If w/h are 0, flushes the bounding rect of queued commands.
 */
typedef struct {
    videoctl2_hdr_t hdr;
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
} videoctl2_flush_t;

/* Buffer allocation / mapping commands (v2)
 * These are synchronous control commands (not enqueued).
 */
typedef struct {
    videoctl2_hdr_t hdr;   /* cmd = VIDEOCTL_CMD2_ALLOC_BUF */
    uint32_t size_bytes;   /* requested size (<=64MiB recommended) */
    uint32_t fmt;          /* MD64API_GRP_FMT_XRGB8888 or MD64API_GRP_FMT_RGB565 */

    /* out */
    uint32_t handle;       /* allocated handle (starts at 1, 0=invalid) */
    uint32_t pitch;        /* bytes per row for the buffer (for linear 2D) */
} videoctl2_alloc_buf_t;

typedef struct {
    videoctl2_hdr_t hdr;   /* cmd = VIDEOCTL_CMD2_MAP_BUF */
    uint32_t handle;
    uint32_t reserved;

    /* out */
    uint64_t user_addr;    /* mapped user virtual address */
    uint32_t size_bytes;
    uint32_t pitch;
    uint32_t fmt;
} videoctl2_map_buf_t;

/* ------------------------------------------------------------
 * Cursor API (v2)
 * ------------------------------------------------------------
 * Cursor pixels are always packed ARGB8888 (0xAARRGGBB).
 * Kernel composites over the framebuffer and restores background when moved/hidden.
 */
#define VIDEOCTL2_CURSOR_MAX_W 64u
#define VIDEOCTL2_CURSOR_MAX_H 64u

typedef struct {
    videoctl2_hdr_t hdr; /* cmd = VIDEOCTL_CMD2_CURSOR_SET */
    uint32_t w;
    uint32_t h;
    int32_t  hot_x;
    int32_t  hot_y;
    uint32_t reserved0;
    uint32_t reserved1;
    /* followed by w*h uint32_t pixels (ARGB8888) */
} videoctl2_cursor_set_t;

typedef struct {
    videoctl2_hdr_t hdr; /* cmd = VIDEOCTL_CMD2_CURSOR_MOVE */
    int32_t x;
    int32_t y;
} videoctl2_cursor_move_t;

typedef struct {
    videoctl2_hdr_t hdr; /* cmd = VIDEOCTL_CMD2_CURSOR_SHOW */
    uint32_t visible; /* 0=hide, 1=show */
} videoctl2_cursor_show_t;
