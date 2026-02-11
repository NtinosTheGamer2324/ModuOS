//lib_gfx2d.c
#include "libc.h"
#include "gfx2d.h"
#include "../include/moduos/drivers/graphics/videoctl.h"

#define VIDEO0_PATH "$/dev/graphics/video0"

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

int gfx2d_open(gfx2d_t *g) {
    if (!g) return -EINVAL;
    int fd = open(VIDEO0_PATH, O_RDWR, 0);
    if (fd < 0) return -ENOENT;
    g->fd = fd;
    return 0;
}

int gfx2d_close(gfx2d_t *g) {
    if (!g) return -EINVAL;
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
    req.hdr.magic = VIDEOCTL_MAGIC2;
    req.hdr.abi_version = VIDEOCTL_ABI_VERSION;
    req.hdr.cmd = VIDEOCTL_CMD2_GET_INFO;
    req.hdr.size_bytes = sizeof(req);

    int rc = write_full(g->fd, &req, sizeof(req));
    if (rc != 0) return rc;
    rc = read_full(g->fd, &req, sizeof(req));
    if (rc != 0) return rc;

    out->width = req.width;
    out->height = req.height;
    out->pitch = req.pitch;
    out->bpp = req.bpp;
    out->fmt = req.fmt;
    out->caps = req.caps;
    memcpy(out->driver, req.driver, sizeof(out->driver));
    out->driver[sizeof(out->driver)-1] = 0;

    return 0;
}

int gfx2d_fill_rect(gfx2d_t *g, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t argb) {
    if (!g) return -EINVAL;
    videoctl2_enqueue_t req;
    memset(&req, 0, sizeof(req));
    req.hdr.magic = VIDEOCTL_MAGIC2;
    req.hdr.abi_version = VIDEOCTL_ABI_VERSION;
    req.hdr.cmd = VIDEOCTL_CMD2_ENQUEUE;
    req.hdr.size_bytes = sizeof(req);

    req.u.fill.op = VIDEOCTL2_OP_FILL_RECT;
    req.u.fill.x = x;
    req.u.fill.y = y;
    req.u.fill.w = w;
    req.u.fill.h = h;
    req.u.fill.argb = argb;

    return write_full(g->fd, &req, sizeof(req));
}

int gfx2d_blit_rect(gfx2d_t *g, uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t w, uint32_t h) {
    if (!g) return -EINVAL;
    videoctl2_enqueue_t req;
    memset(&req, 0, sizeof(req));
    req.hdr.magic = VIDEOCTL_MAGIC2;
    req.hdr.abi_version = VIDEOCTL_ABI_VERSION;
    req.hdr.cmd = VIDEOCTL_CMD2_ENQUEUE;
    req.hdr.size_bytes = sizeof(req);

    req.u.blit.op = VIDEOCTL2_OP_BLIT;
    req.u.blit.src_x = src_x;
    req.u.blit.src_y = src_y;
    req.u.blit.dst_x = dst_x;
    req.u.blit.dst_y = dst_y;
    req.u.blit.w = w;
    req.u.blit.h = h;

    return write_full(g->fd, &req, sizeof(req));
}

int gfx2d_alloc_buf(gfx2d_t *g, uint32_t size_bytes, uint32_t fmt, uint32_t *out_handle, uint32_t *out_pitch) {
    if (!g || !out_handle || !out_pitch) return -EINVAL;

    videoctl2_alloc_buf_t req;
    memset(&req, 0, sizeof(req));
    req.hdr.magic = VIDEOCTL_MAGIC2;
    req.hdr.abi_version = VIDEOCTL_ABI_VERSION;
    req.hdr.cmd = VIDEOCTL_CMD2_ALLOC_BUF;
    req.hdr.size_bytes = sizeof(req);
    req.size_bytes = size_bytes;
    req.fmt = fmt;

    int rc = write_full(g->fd, &req, sizeof(req));
    if (rc != 0) return rc;
    rc = read_full(g->fd, &req, sizeof(req));
    if (rc != 0) return rc;

    *out_handle = req.handle;
    *out_pitch = req.pitch;
    return 0;
}

int gfx2d_map_buf(gfx2d_t *g, uint32_t handle, void **out_addr, uint32_t *out_size, uint32_t *out_pitch, uint32_t *out_fmt) {
    if (!g || !out_addr || !out_size || !out_pitch || !out_fmt) return -EINVAL;

    videoctl2_map_buf_t req;
    memset(&req, 0, sizeof(req));
    req.hdr.magic = VIDEOCTL_MAGIC2;
    req.hdr.abi_version = VIDEOCTL_ABI_VERSION;
    req.hdr.cmd = VIDEOCTL_CMD2_MAP_BUF;
    req.hdr.size_bytes = sizeof(req);
    req.handle = handle;

    int rc = write_full(g->fd, &req, sizeof(req));
    if (rc != 0) return rc;
    rc = read_full(g->fd, &req, sizeof(req));
    if (rc != 0) return rc;

    *out_addr = (void*)(uintptr_t)req.user_addr;
    *out_size = req.size_bytes;
    *out_pitch = req.pitch;
    *out_fmt = req.fmt;
    return 0;
}

int gfx2d_blit_buf(gfx2d_t *g, uint32_t handle,
                   uint32_t src_x, uint32_t src_y,
                   uint32_t dst_x, uint32_t dst_y,
                   uint32_t w, uint32_t h,
                   uint32_t src_pitch, uint32_t src_fmt) {
    if (!g) return -EINVAL;

    videoctl2_enqueue_t req;
    memset(&req, 0, sizeof(req));
    req.hdr.magic = VIDEOCTL_MAGIC2;
    req.hdr.abi_version = VIDEOCTL_ABI_VERSION;
    req.hdr.cmd = VIDEOCTL_CMD2_ENQUEUE;
    req.hdr.size_bytes = sizeof(req);

    req.u.blit_buf.op = VIDEOCTL2_OP_BLIT_BUF;
    req.u.blit_buf.handle = handle;
    req.u.blit_buf.src_x = src_x;
    req.u.blit_buf.src_y = src_y;
    req.u.blit_buf.dst_x = dst_x;
    req.u.blit_buf.dst_y = dst_y;
    req.u.blit_buf.w = w;
    req.u.blit_buf.h = h;
    req.u.blit_buf.src_pitch = src_pitch;
    req.u.blit_buf.src_fmt = src_fmt;

    return write_full(g->fd, &req, sizeof(req));
}

int gfx2d_flush(gfx2d_t *g, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!g) return -EINVAL;

    videoctl2_flush_t req;
    memset(&req, 0, sizeof(req));
    req.hdr.magic = VIDEOCTL_MAGIC2;
    req.hdr.abi_version = VIDEOCTL_ABI_VERSION;
    req.hdr.cmd = VIDEOCTL_CMD2_FLUSH;
    req.hdr.size_bytes = sizeof(req);

    req.x = x;
    req.y = y;
    req.w = w;
    req.h = h;

    return write_full(g->fd, &req, sizeof(req));
}

int gfx2d_cursor_set(gfx2d_t *g, uint32_t w, uint32_t h, int32_t hot_x, int32_t hot_y, const uint32_t *argb_pixels) {
    if (!g || !argb_pixels) return -EINVAL;

    videoctl2_cursor_set_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.hdr.magic = VIDEOCTL_MAGIC2;
    hdr.hdr.abi_version = VIDEOCTL_ABI_VERSION;
    hdr.hdr.cmd = VIDEOCTL_CMD2_CURSOR_SET;
    hdr.hdr.size_bytes = (uint32_t)(sizeof(hdr) + (uint64_t)w * (uint64_t)h * 4ULL);
    hdr.w = w;
    hdr.h = h;
    hdr.hot_x = hot_x;
    hdr.hot_y = hot_y;

    /* Write header + pixels in one go via a temporary buffer.
     * (video0 expects a single write() containing both.)
     */
    uint64_t total = (uint64_t)hdr.hdr.size_bytes;
    if (total == 0 || total > (256ULL * 1024ULL)) {
        return -EINVAL;
    }

    uint8_t *tmp = (uint8_t*)malloc((size_t)total);
    if (!tmp) return -ENOMEM;
    memcpy(tmp, &hdr, sizeof(hdr));
    memcpy(tmp + sizeof(hdr), argb_pixels, (size_t)w * (size_t)h * 4u);
    int rc = write_full(g->fd, tmp, (size_t)total);
    free(tmp);
    return rc;
}

int gfx2d_cursor_move(gfx2d_t *g, int32_t x, int32_t y) {
    if (!g) return -EINVAL;
    videoctl2_cursor_move_t req;
    memset(&req, 0, sizeof(req));
    req.hdr.magic = VIDEOCTL_MAGIC2;
    req.hdr.abi_version = VIDEOCTL_ABI_VERSION;
    req.hdr.cmd = VIDEOCTL_CMD2_CURSOR_MOVE;
    req.hdr.size_bytes = sizeof(req);
    req.x = x;
    req.y = y;
    return write_full(g->fd, &req, sizeof(req));
}

int gfx2d_cursor_show(gfx2d_t *g, int visible) {
    if (!g) return -EINVAL;
    videoctl2_cursor_show_t req;
    memset(&req, 0, sizeof(req));
    req.hdr.magic = VIDEOCTL_MAGIC2;
    req.hdr.abi_version = VIDEOCTL_ABI_VERSION;
    req.hdr.cmd = VIDEOCTL_CMD2_CURSOR_SHOW;
    req.hdr.size_bytes = sizeof(req);
    req.visible = visible ? 1u : 0u;
    return write_full(g->fd, &req, sizeof(req));
}
