//lib_gfx2d.c
//
// Uses VIDEOCTL_CMD2_MAP_CMDBUF / SUBMIT_CMDBUF when available.
//
// With the mapped-buffer path the game loop writes commands directly into a
// page that is shared with the kernel — no copy, no per-command syscall.
// A single ioctl-style write (SUBMIT_CMDBUF) at present-time tells the kernel
// "process N commands from the shared buffer", then returns immediately while
// the GPU/driver works asynchronously.
//
// Falls back to the legacy batched-write path when MAP_CMDBUF is unavailable.

#include "libc.h"
#include "gfx2d.h"
#include "../include/moduos/drivers/graphics/videoctl.h"

#define VIDEO0_PATH "$/dev/graphics/video0"

/* Default mapped command buffer size: 1 MB.
 * At 56 bytes per command that's ~18,000 commands — more than enough for
 * a full Teseraris frame including pixel-by-pixel text (which is now span-
 * merged anyway, but belt-and-suspenders). */
#define MAPPED_CMDBUF_SIZE (1024 * 1024)

/* Legacy fallback userspace copy buffer: 256 KB */
#define COPY_CMDBUF_SIZE (256 * 1024)

static int write_full(int fd, void *buf, size_t sz) {
    ssize_t r = write(fd, buf, sz);
    if (r < 0) return (int)r;
    if ((size_t)r != sz) return -EIO;
    return 0;
}

static int read_full(int fd, void *buf, size_t sz) {
    ssize_t r = read(fd, buf, sz);
    if (r < 0) return (int)r;
    if ((size_t)r != sz) return -EIO;
    return 0;
}

/* -----------------------------------------------------------------------
 * Try to establish the zero-copy mapped command buffer path.
 * Sets g->mapped_cmdbuf / g->mapped_cmdbuf_size on success.
 * Returns 0 on success, -1 if the kernel doesn't support it (fall back).
 * ----------------------------------------------------------------------- */
static int gfx2d_try_map_cmdbuf(gfx2d_t *g) {
    videoctl2_map_cmdbuf_t req;
    memset(&req, 0, sizeof(req));
    req.hdr.magic        = VIDEOCTL_MAGIC2;
    req.hdr.abi_version  = VIDEOCTL_ABI_VERSION;
    req.hdr.cmd          = VIDEOCTL_CMD2_MAP_CMDBUF;
    req.hdr.size_bytes   = sizeof(req);
    req.size_bytes        = MAPPED_CMDBUF_SIZE;

    if (write_full(g->fd, &req, sizeof(req)) != 0) return -1;
    if (read_full(g->fd, &req, sizeof(req))  != 0) return -1;

    if (req.user_addr == 0 || req.actual_size == 0) return -1;

    g->mapped_cmdbuf      = (void *)(uintptr_t)req.user_addr;
    g->mapped_cmdbuf_size = req.actual_size;
    g->mapped_cmdbuf_used = 0;
    g->mapped_cmd_count   = 0;
    return 0;
}

/* -----------------------------------------------------------------------
 * Open the graphics device and set up the best available command path.
 * Priority:
 *   1. Kernel-mapped shared buffer (VIDEOCTL_CMD2_MAP_CMDBUF)  — zero-copy
 *   2. Userspace copy buffer + single write() per flush         — one-copy
 * ----------------------------------------------------------------------- */
int gfx2d_open(gfx2d_t *g) {
    if (!g) return -EINVAL;
    memset(g, 0, sizeof(*g));

    int fd = open(VIDEO0_PATH, O_RDWR, 0);
    if (fd < 0) return -ENOENT;
    g->fd = fd;

    /* Try zero-copy mapped buffer first */
    if (gfx2d_try_map_cmdbuf(g) == 0) {
        g->use_mapped_cmdbuf = 1;
        printf("[GFX2D] Using zero-copy mapped command buffer (%u bytes)\n",
               g->mapped_cmdbuf_size);
        return 0;
    }

    /* Fall back: allocate a userspace copy buffer */
    g->cmdbuf_size = COPY_CMDBUF_SIZE;
    g->cmdbuf      = malloc(g->cmdbuf_size);
    if (g->cmdbuf) {
        g->cmdbuf_used = 0;
        g->cmd_count   = 0;
        printf("[GFX2D] Using copy-batch command buffer (%u bytes)\n",
               g->cmdbuf_size);
    } else {
        g->cmdbuf_size = 0;
        printf("[GFX2D] Warning: command buffer alloc failed, using direct writes\n");
    }

    return 0;
}

int gfx2d_close(gfx2d_t *g) {
    if (!g) return -EINVAL;
    /* mapped_cmdbuf is kernel-owned memory — do NOT free() it */
    if (g->cmdbuf) {
        free(g->cmdbuf);
        g->cmdbuf = NULL;
    }
    if (g->fd >= 0) {
        close(g->fd);
        g->fd = -1;
    }
    return 0;
}

int gfx2d_get_info(gfx2d_t *g, gfx2d_info_t *out) {
    if (!g || !out) return -EINVAL;

    videoctl2_info_t req;
    memset(&req, 0, sizeof(req));
    req.hdr.magic       = VIDEOCTL_MAGIC2;
    req.hdr.abi_version = VIDEOCTL_ABI_VERSION;
    req.hdr.cmd         = VIDEOCTL_CMD2_GET_INFO;
    req.hdr.size_bytes  = sizeof(req);

    int rc = write_full(g->fd, &req, sizeof(req));
    if (rc != 0) return rc;
    rc = read_full(g->fd, &req, sizeof(req));
    if (rc != 0) return rc;

    out->width  = req.width;
    out->height = req.height;
    out->pitch  = req.pitch;
    out->bpp    = req.bpp;
    out->fmt    = req.fmt;
    out->caps   = req.caps;
    memcpy(out->driver, req.driver, sizeof(out->driver));
    out->driver[sizeof(out->driver)-1] = 0;
    return 0;
}

/* -----------------------------------------------------------------------
 * Internal: enqueue one videoctl2_enqueue_t into whichever buffer is active.
 * Returns 0 on success.
 * ----------------------------------------------------------------------- */
static int enqueue_cmd(gfx2d_t *g, const videoctl2_enqueue_t *cmd) {
    size_t sz = sizeof(*cmd);

    /* --- Zero-copy path: write directly into the mapped kernel buffer --- */
    if (g->use_mapped_cmdbuf && g->mapped_cmdbuf) {
        if (g->mapped_cmdbuf_used + sz <= g->mapped_cmdbuf_size) {
            memcpy((uint8_t *)g->mapped_cmdbuf + g->mapped_cmdbuf_used, cmd, sz);
            g->mapped_cmdbuf_used += (uint32_t)sz;
            g->mapped_cmd_count++;
            return 0;
        }
        /* Mapped buffer full — flush it mid-frame and carry on.
         * This should be extremely rare given the 1 MB buffer. */
        videoctl2_submit_cmdbuf_t sub;
        memset(&sub, 0, sizeof(sub));
        sub.hdr.magic       = VIDEOCTL_MAGIC2;
        sub.hdr.abi_version = VIDEOCTL_ABI_VERSION;
        sub.hdr.cmd         = VIDEOCTL_CMD2_SUBMIT_CMDBUF;
        sub.hdr.size_bytes  = sizeof(sub);
        sub.count           = g->mapped_cmd_count;
        write_full(g->fd, &sub, sizeof(sub));
        g->mapped_cmdbuf_used = 0;
        g->mapped_cmd_count   = 0;

        /* Now retry */
        memcpy((uint8_t *)g->mapped_cmdbuf, cmd, sz);
        g->mapped_cmdbuf_used = (uint32_t)sz;
        g->mapped_cmd_count   = 1;
        return 0;
    }

    /* --- Copy-batch path: accumulate in userspace malloc buffer --- */
    if (g->cmdbuf && g->cmdbuf_used + sz <= g->cmdbuf_size) {
        memcpy((uint8_t *)g->cmdbuf + g->cmdbuf_used, cmd, sz);
        g->cmdbuf_used += (uint32_t)sz;
        g->cmd_count++;
        return 0;
    }

    /* --- Last resort: direct synchronous write (slowest) --- */
    return write_full(g->fd, (void *)cmd, sz);
}

/* -----------------------------------------------------------------------
 * Public drawing API — all go through enqueue_cmd()
 * ----------------------------------------------------------------------- */

int gfx2d_fill_rect(gfx2d_t *g, uint32_t x, uint32_t y,
                    uint32_t w, uint32_t h, uint32_t argb) {
    if (!g) return -EINVAL;

    videoctl2_enqueue_t req;
    memset(&req, 0, sizeof(req));
    req.hdr.magic       = VIDEOCTL_MAGIC2;
    req.hdr.abi_version = VIDEOCTL_ABI_VERSION;
    req.hdr.cmd         = VIDEOCTL_CMD2_ENQUEUE;
    req.hdr.size_bytes  = sizeof(req);
    req.u.fill.op       = VIDEOCTL2_OP_FILL_RECT;
    req.u.fill.x        = x;
    req.u.fill.y        = y;
    req.u.fill.w        = w;
    req.u.fill.h        = h;
    req.u.fill.argb     = argb;

    return enqueue_cmd(g, &req);
}

int gfx2d_blit_rect(gfx2d_t *g, uint32_t src_x, uint32_t src_y,
                    uint32_t dst_x, uint32_t dst_y, uint32_t w, uint32_t h) {
    if (!g) return -EINVAL;

    videoctl2_enqueue_t req;
    memset(&req, 0, sizeof(req));
    req.hdr.magic       = VIDEOCTL_MAGIC2;
    req.hdr.abi_version = VIDEOCTL_ABI_VERSION;
    req.hdr.cmd         = VIDEOCTL_CMD2_ENQUEUE;
    req.hdr.size_bytes  = sizeof(req);
    req.u.blit.op       = VIDEOCTL2_OP_BLIT;
    req.u.blit.src_x    = src_x;
    req.u.blit.src_y    = src_y;
    req.u.blit.dst_x    = dst_x;
    req.u.blit.dst_y    = dst_y;
    req.u.blit.w        = w;
    req.u.blit.h        = h;

    return enqueue_cmd(g, &req);
}

int gfx2d_blit_buf(gfx2d_t *g, uint32_t handle,
                   uint32_t src_x, uint32_t src_y,
                   uint32_t dst_x, uint32_t dst_y,
                   uint32_t w,     uint32_t h,
                   uint32_t src_pitch, uint32_t src_fmt) {
    if (!g) return -EINVAL;

    videoctl2_enqueue_t req;
    memset(&req, 0, sizeof(req));
    req.hdr.magic              = VIDEOCTL_MAGIC2;
    req.hdr.abi_version        = VIDEOCTL_ABI_VERSION;
    req.hdr.cmd                = VIDEOCTL_CMD2_ENQUEUE;
    req.hdr.size_bytes         = sizeof(req);
    req.u.blit_buf.op          = VIDEOCTL2_OP_BLIT_BUF;
    req.u.blit_buf.handle      = handle;
    req.u.blit_buf.src_x       = src_x;
    req.u.blit_buf.src_y       = src_y;
    req.u.blit_buf.dst_x       = dst_x;
    req.u.blit_buf.dst_y       = dst_y;
    req.u.blit_buf.w           = w;
    req.u.blit_buf.h           = h;
    req.u.blit_buf.src_pitch   = src_pitch;
    req.u.blit_buf.src_fmt     = src_fmt;

    return enqueue_cmd(g, &req);
}

/* -----------------------------------------------------------------------
 * Flush: submit all pending commands then send the FLUSH packet.
 *
 * Zero-copy path: sends SUBMIT_CMDBUF (one small write containing just the
 *   count) — the kernel reads commands directly from the mapped buffer, no
 *   data copy through the syscall boundary.
 *
 * Copy-batch path: writes the whole command batch in one write(), then the
 *   FLUSH packet in a second write().
 * ----------------------------------------------------------------------- */
int gfx2d_flush(gfx2d_t *g, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!g) return -EINVAL;

    /* Submit pending commands */
    if (g->use_mapped_cmdbuf && g->mapped_cmdbuf) {
        if (g->mapped_cmd_count > 0) {
            videoctl2_submit_cmdbuf_t sub;
            memset(&sub, 0, sizeof(sub));
            sub.hdr.magic       = VIDEOCTL_MAGIC2;
            sub.hdr.abi_version = VIDEOCTL_ABI_VERSION;
            sub.hdr.cmd         = VIDEOCTL_CMD2_SUBMIT_CMDBUF;
            sub.hdr.size_bytes  = sizeof(sub);
            sub.count           = g->mapped_cmd_count;

            /* This is the ONLY syscall per frame on the fast path.
             * The kernel reads command data from the shared mapped page —
             * zero bytes are copied through the write() call itself. */
            int rc = write_full(g->fd, &sub, sizeof(sub));
            if (rc != 0) return rc;

            g->mapped_cmdbuf_used = 0;
            g->mapped_cmd_count   = 0;
        }
    } else if (g->cmdbuf && g->cmdbuf_used > 0) {
        /* Copy-batch fallback: one write for all commands */
        ssize_t written = write(g->fd, g->cmdbuf, g->cmdbuf_used);
        if (written != (ssize_t)g->cmdbuf_used) return -EIO;
        g->cmdbuf_used = 0;
        g->cmd_count   = 0;
    }

    /* Send the FLUSH / present packet */
    videoctl2_flush_t req;
    memset(&req, 0, sizeof(req));
    req.hdr.magic       = VIDEOCTL_MAGIC2;
    req.hdr.abi_version = VIDEOCTL_ABI_VERSION;
    req.hdr.cmd         = VIDEOCTL_CMD2_FLUSH;
    req.hdr.size_bytes  = sizeof(req);
    req.x = x;
    req.y = y;
    req.w = w;
    req.h = h;

    return write_full(g->fd, &req, sizeof(req));
}

/* -----------------------------------------------------------------------
 * Synchronous control commands (one-time setup, not in the hot path)
 * ----------------------------------------------------------------------- */

int gfx2d_alloc_buf(gfx2d_t *g, uint32_t size_bytes, uint32_t fmt,
                    uint32_t *out_handle, uint32_t *out_pitch) {
    if (!g || !out_handle || !out_pitch) return -EINVAL;

    videoctl2_alloc_buf_t req;
    memset(&req, 0, sizeof(req));
    req.hdr.magic       = VIDEOCTL_MAGIC2;
    req.hdr.abi_version = VIDEOCTL_ABI_VERSION;
    req.hdr.cmd         = VIDEOCTL_CMD2_ALLOC_BUF;
    req.hdr.size_bytes  = sizeof(req);
    req.size_bytes       = size_bytes;
    req.fmt              = fmt;

    int rc = write_full(g->fd, &req, sizeof(req));
    if (rc != 0) return rc;
    rc = read_full(g->fd, &req, sizeof(req));
    if (rc != 0) return rc;

    *out_handle = req.handle;
    *out_pitch  = req.pitch;
    return 0;
}

int gfx2d_map_buf(gfx2d_t *g, uint32_t handle,
                  void **out_addr, uint32_t *out_size,
                  uint32_t *out_pitch, uint32_t *out_fmt) {
    if (!g || !out_addr || !out_size || !out_pitch || !out_fmt) return -EINVAL;

    videoctl2_map_buf_t req;
    memset(&req, 0, sizeof(req));
    req.hdr.magic       = VIDEOCTL_MAGIC2;
    req.hdr.abi_version = VIDEOCTL_ABI_VERSION;
    req.hdr.cmd         = VIDEOCTL_CMD2_MAP_BUF;
    req.hdr.size_bytes  = sizeof(req);
    req.handle           = handle;

    int rc = write_full(g->fd, &req, sizeof(req));
    if (rc != 0) return rc;
    rc = read_full(g->fd, &req, sizeof(req));
    if (rc != 0) return rc;

    *out_addr  = (void *)(uintptr_t)req.user_addr;
    *out_size  = req.size_bytes;
    *out_pitch = req.pitch;
    *out_fmt   = req.fmt;
    return 0;
}

/* -----------------------------------------------------------------------
 * Cursor API (synchronous control, not in the render hot path)
 * ----------------------------------------------------------------------- */

int gfx2d_cursor_set(gfx2d_t *g, uint32_t w, uint32_t h,
                     int32_t hot_x, int32_t hot_y,
                     const uint32_t *argb_pixels) {
    if (!g || !argb_pixels) return -EINVAL;

    videoctl2_cursor_set_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.hdr.magic       = VIDEOCTL_MAGIC2;
    hdr.hdr.abi_version = VIDEOCTL_ABI_VERSION;
    hdr.hdr.cmd         = VIDEOCTL_CMD2_CURSOR_SET;
    hdr.hdr.size_bytes  = (uint32_t)(sizeof(hdr) + (uint64_t)w * h * 4ULL);
    hdr.w     = w;
    hdr.h     = h;
    hdr.hot_x = hot_x;
    hdr.hot_y = hot_y;

    uint64_t total = hdr.hdr.size_bytes;
    if (total == 0 || total > 256ULL * 1024ULL) return -EINVAL;

    uint8_t *tmp = (uint8_t *)malloc((size_t)total);
    if (!tmp) return -ENOMEM;
    memcpy(tmp, &hdr, sizeof(hdr));
    memcpy(tmp + sizeof(hdr), argb_pixels, (size_t)w * h * 4u);
    int rc = write_full(g->fd, tmp, (size_t)total);
    free(tmp);
    return rc;
}

int gfx2d_cursor_move(gfx2d_t *g, int32_t x, int32_t y) {
    if (!g) return -EINVAL;
    videoctl2_cursor_move_t req;
    memset(&req, 0, sizeof(req));
    req.hdr.magic       = VIDEOCTL_MAGIC2;
    req.hdr.abi_version = VIDEOCTL_ABI_VERSION;
    req.hdr.cmd         = VIDEOCTL_CMD2_CURSOR_MOVE;
    req.hdr.size_bytes  = sizeof(req);
    req.x = x;
    req.y = y;
    return write_full(g->fd, &req, sizeof(req));
}

int gfx2d_cursor_show(gfx2d_t *g, int visible) {
    if (!g) return -EINVAL;
    videoctl2_cursor_show_t req;
    memset(&req, 0, sizeof(req));
    req.hdr.magic       = VIDEOCTL_MAGIC2;
    req.hdr.abi_version = VIDEOCTL_ABI_VERSION;
    req.hdr.cmd         = VIDEOCTL_CMD2_CURSOR_SHOW;
    req.hdr.size_bytes  = sizeof(req);
    req.visible = visible ? 1u : 0u;
    return write_full(g->fd, &req, sizeof(req));
}