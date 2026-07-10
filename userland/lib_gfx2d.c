/*
 * lib_gfx2d.c — ModuOS graphics wrapper, MVC3 backend
 *
 * Talks to $/dev/mvc/mvi0 instead of the old $/dev/graphics/video0.
 * Wire protocol: mvc3.h
 *
 * Fast path  — kernel-allocated ring (MAP_RING), one SUBMIT write/frame
 * Slow path  — malloc'd copy buffer, one write() per flush
 *
 * Buffer allocation (gfx2d_alloc_buf):
 *   1. MVC3_CMD_ALLOC_BUF  — kernel kmalloc's a page-aligned buffer and
 *      returns its full 64-bit KVA in resp.handle.
 *   2. MVC3_CMD_MAP_BUF    — kernel walks the session mapping table, finds
 *      the buffer by KVA, and wires its pages into the calling process's
 *      page table via devfs_mmap_region().  Returns the user VA.
 *   3. The full 64-bit user VA is stored in the buffer table and returned
 *      as *out_handle (uint64_t).  It also goes into every BLIT_BUF ring
 *      slot; the kernel reads from it via usercopy during write().
 *   Fallback: if MAP_BUF returns user_addr == 0 (old kernel / no mmap),
 *      malloc a local buffer instead.  The kernel can read userspace pages
 *      during write(), so BLIT_BUF still works on the copy-batch path.
 *
 * Handle width
 * ────────────
 * All handles exposed in the public API are now uint64_t so that addresses
 * above 4 GiB are never silently truncated.  Internally the ring slot
 * blit_buf.handle field is already uint64_t (see mvc3.h).
 *
 * Copyright © 2025-2026 ModuOS Project Contributors — GPL v2.0
 */

#include "libc.h"
#include "gfx2d.h"
#include "mvc3.h"

#define MVI0_PATH "$/dev/mvc/mvi0"

#define GFX2D_RING_SIZE     (1024u * 1024u)
#define GFX2D_COPY_BUF_SIZE (256u * 1024u)

/* ── Internal I/O helpers ──────────────────────────────────────────── */

static int write_full(int fd, const void *buf, size_t sz) {
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

/* ── Header builder ────────────────────────────────────────────────── */

static void hdr_init(mvc3_hdr_t *h, mvc3_cmd_t cmd, uint32_t total_bytes) {
    h->magic       = MVC3_MAGIC;
    h->abi_version = MVC3_ABI_VERSION;
    h->cmd         = (uint32_t)cmd;
    h->size_bytes  = total_bytes;
}

/* ── Zero-copy ring path setup ─────────────────────────────────────── */

static int try_map_ring(gfx2d_t *g) {
    (void)g;
    return -1; /* disabled — use copy-batch */
}

/* ── Open / Close ──────────────────────────────────────────────────── */

int gfx2d_open(gfx2d_t *g) {
    if (!g) return -EINVAL;
    memset(g, 0, sizeof(*g));

    int fd = open(MVI0_PATH, O_RDWR, 0);
    if (fd < 0) return -ENOENT;
    g->fd = fd;

    if (try_map_ring(g) == 0) {
        g->use_mapped_cmdbuf = 1;
        printf("[GFX2D/MVC3] zero-copy ring: %u bytes\n", g->mapped_cmdbuf_size);
        return 0;
    }

    g->cmdbuf_size = GFX2D_COPY_BUF_SIZE;
    g->cmdbuf      = malloc(g->cmdbuf_size);
    if (g->cmdbuf) {
        g->cmdbuf_used = 0;
        g->cmd_count   = 0;
        printf("[GFX2D/MVC3] copy-batch buffer: %u bytes\n", g->cmdbuf_size);
    } else {
        g->cmdbuf_size = 0;
        printf("[GFX2D/MVC3] Warning: copy-batch alloc failed, direct writes\n");
    }

    return 0;
}

/* ── Buffer table cleanup ──────────────────────────────────────────── */

static void free_bufs(gfx2d_t *g) {
    for (uint32_t i = 0; i < g->n_bufs; i++) {
        /*
         * kva_mapped == 1  →  user_va is a kernel-owned mmap.  It will be
         *                     unmapped automatically when the fd is closed.
         *                     Do NOT free() it.
         * kva_mapped == 0  →  user_va is a local malloc fallback.  Free it.
         */
        if (!g->bufs[i].kva_mapped && g->bufs[i].user_va) {
            free(g->bufs[i].user_va);
        }
        g->bufs[i].user_va = NULL;
    }
    g->n_bufs = 0;
}

int gfx2d_close(gfx2d_t *g) {
    if (!g) return -EINVAL;
    free_bufs(g);
    /* mapped_cmdbuf is kernel-owned — do NOT free() it */
    if (g->cmdbuf) { free(g->cmdbuf); g->cmdbuf = NULL; }
    if (g->fd >= 0) { close(g->fd); g->fd = -1; }
    return 0;
}

/* ── GET_INFO ──────────────────────────────────────────────────────── */

int gfx2d_get_info(gfx2d_t *g, gfx2d_info_t *out) {
    if (!g || !out) return -EINVAL;

    mvc3_get_info_req_t req;
    memset(&req, 0, sizeof(req));
    hdr_init(&req.hdr, MVC3_CMD_GET_INFO, sizeof(req));

    int rc = write_full(g->fd, &req, sizeof(req));
    if (rc != 0) return rc;

    mvc3_get_info_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    rc = read_full(g->fd, &resp, sizeof(resp));
    if (rc != 0) return rc;
    if (resp.hdr.magic != MVC3_MAGIC) return -EIO;

    out->width  = resp.width;
    out->height = resp.height;
    out->pitch  = resp.pitch;
    out->bpp    = resp.bpp;
    out->fmt    = resp.fmt;
    out->caps   = resp.caps;
    memcpy(out->driver, resp.driver, sizeof(out->driver));
    out->driver[sizeof(out->driver) - 1] = 0;
    return 0;
}

/* ── Internal: enqueue one draw slot ──────────────────────────────── */

static int enqueue_slot(gfx2d_t *g, const mvc3_ring_slot_t *slot) {

    /* Zero-copy ring */
    if (g->use_mapped_cmdbuf && g->mapped_cmdbuf) {
        uint32_t slot_sz = sizeof(mvc3_ring_slot_t);

        if (g->mapped_cmdbuf_used + slot_sz <= g->mapped_cmdbuf_size) {
            memcpy((uint8_t *)g->mapped_cmdbuf + g->mapped_cmdbuf_used,
                   slot, slot_sz);
            g->mapped_cmdbuf_used += slot_sz;
            g->mapped_cmd_count++;
            return 0;
        }

        /* Ring full — flush and retry */
        mvc3_submit_t sub;
        memset(&sub, 0, sizeof(sub));
        hdr_init(&sub.hdr, MVC3_CMD_SUBMIT, sizeof(sub));
        sub.count = g->mapped_cmd_count;
        write_full(g->fd, &sub, sizeof(sub));
        g->mapped_cmdbuf_used = 0;
        g->mapped_cmd_count   = 0;

        memcpy(g->mapped_cmdbuf, slot, slot_sz);
        g->mapped_cmdbuf_used = slot_sz;
        g->mapped_cmd_count   = 1;
        return 0;
    }

    /* Copy-batch */
    mvc3_enqueue_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    hdr_init(&pkt.hdr, MVC3_CMD_ENQUEUE, sizeof(pkt));
    memcpy(&pkt.slot, slot, sizeof(mvc3_ring_slot_t));

    if (g->cmdbuf && g->cmdbuf_used + sizeof(pkt) <= g->cmdbuf_size) {
        memcpy((uint8_t *)g->cmdbuf + g->cmdbuf_used, &pkt, sizeof(pkt));
        g->cmdbuf_used += (uint32_t)sizeof(pkt);
        g->cmd_count++;
        return 0;
    }

    /* Direct write (last resort) */
    return write_full(g->fd, &pkt, sizeof(pkt));
}

/* ── Drawing API ───────────────────────────────────────────────────── */

int gfx2d_fill_rect(gfx2d_t *g, uint32_t x, uint32_t y,
                    uint32_t w, uint32_t h, uint32_t argb) {
    if (!g) return -EINVAL;
    mvc3_ring_slot_t slot;
    memset(&slot, 0, sizeof(slot));
    slot.op          = MVC3_OP_FILL_RECT;
    slot.u.fill.x    = x;
    slot.u.fill.y    = y;
    slot.u.fill.w    = w;
    slot.u.fill.h    = h;
    slot.u.fill.argb = argb;
    return enqueue_slot(g, &slot);
}

int gfx2d_blit_rect(gfx2d_t *g, uint32_t src_x, uint32_t src_y,
                    uint32_t dst_x, uint32_t dst_y, uint32_t w, uint32_t h) {
    if (!g) return -EINVAL;
    mvc3_ring_slot_t slot;
    memset(&slot, 0, sizeof(slot));
    slot.op           = MVC3_OP_BLIT;
    slot.u.blit.src_x = src_x;
    slot.u.blit.src_y = src_y;
    slot.u.blit.dst_x = dst_x;
    slot.u.blit.dst_y = dst_y;
    slot.u.blit.w     = w;
    slot.u.blit.h     = h;
    return enqueue_slot(g, &slot);
}

int gfx2d_blit_buf(gfx2d_t *g, uint64_t handle,
                   uint32_t src_x, uint32_t src_y,
                   uint32_t dst_x, uint32_t dst_y,
                   uint32_t w, uint32_t h,
                   uint32_t src_pitch, uint32_t src_fmt) {
    if (!g) return -EINVAL;
    mvc3_ring_slot_t slot;
    memset(&slot, 0, sizeof(slot));
    slot.op                   = MVC3_OP_BLIT_BUF;
    slot.u.blit_buf.handle    = handle;   /* full 64-bit user VA */
    slot.u.blit_buf.src_x     = src_x;
    slot.u.blit_buf.src_y     = src_y;
    slot.u.blit_buf.dst_x     = dst_x;
    slot.u.blit_buf.dst_y     = dst_y;
    slot.u.blit_buf.w         = w;
    slot.u.blit_buf.h         = h;
    slot.u.blit_buf.src_pitch = src_pitch;
    slot.u.blit_buf.src_fmt   = src_fmt;
    return enqueue_slot(g, &slot);
}

/* ── Flush / present ───────────────────────────────────────────────── */

int gfx2d_flush(gfx2d_t *g, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!g) return -EINVAL;

    if (g->use_mapped_cmdbuf && g->mapped_cmdbuf) {
        if (g->mapped_cmd_count > 0) {
            mvc3_submit_t sub;
            memset(&sub, 0, sizeof(sub));
            hdr_init(&sub.hdr, MVC3_CMD_SUBMIT, sizeof(sub));
            sub.count = g->mapped_cmd_count;
            int rc = write_full(g->fd, &sub, sizeof(sub));
            if (rc != 0) return rc;
            g->mapped_cmdbuf_used = 0;
            g->mapped_cmd_count   = 0;
        }
    } else if (g->cmdbuf && g->cmdbuf_used > 0) {
        ssize_t written = write(g->fd, g->cmdbuf, g->cmdbuf_used);
        if (written != (ssize_t)g->cmdbuf_used) return -EIO;
        g->cmdbuf_used = 0;
        g->cmd_count   = 0;
    }

    mvc3_flush_t flush;
    memset(&flush, 0, sizeof(flush));
    hdr_init(&flush.hdr, MVC3_CMD_FLUSH, sizeof(flush));
    flush.x = x;
    flush.y = y;
    flush.w = w;
    flush.h = h;
    return write_full(g->fd, &flush, sizeof(flush));
}

/* ── Buffer management ─────────────────────────────────────────────── */

int gfx2d_alloc_buf(gfx2d_t *g, uint32_t size_bytes, uint32_t fmt,
                    uint64_t *out_handle, uint32_t *out_pitch)
{
    if (!g || !out_handle || !out_pitch) return -EINVAL;
    if (g->n_bufs >= GFX2D_MAX_BUFS)    return -ENOMEM;

    /* Send ALLOC_BUF so the kernel knows about this buffer
     * and can accept BLIT_BUF commands referencing it. */
    mvc3_alloc_buf_req_t req;
    memset(&req, 0, sizeof(req));
    hdr_init(&req.hdr, MVC3_CMD_ALLOC_BUF, sizeof(req));
    req.size_bytes = size_bytes;
    req.fmt        = fmt;

    if (write_full(g->fd, &req, sizeof(req)) != 0) return -EIO;

    mvc3_alloc_buf_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    if (read_full(g->fd, &resp, sizeof(resp)) != 0) return -EIO;
    if (resp.handle == 0) return -ENOMEM;

    /*
     * Skip MAP_BUF entirely — devfs_mmap_region is broken.
     * Allocate locally; kernel reads these pages via usercopy
     * during write() syscall, so BLIT_BUF works fine.
     */
    gfx2d_buf_t *b = &g->bufs[g->n_bufs];
    b->kva        = resp.handle;
    b->size_bytes = size_bytes;
    b->fmt        = fmt;
    b->kva_mapped = 0;
    b->pitch      = resp.pitch ? resp.pitch : size_bytes / 4u;

    b->user_va = malloc(size_bytes);
    if (!b->user_va) return -ENOMEM;
    memset(b->user_va, 0, size_bytes);

    g->n_bufs++;

    /*
     * Return the full 64-bit user VA as the handle.
     * Callers must store it as uint64_t and pass it back unchanged to
     * gfx2d_blit_buf / gfx2d_map_buf.  Never truncate to 32 bits.
     */
    *out_handle = (uint64_t)(uintptr_t)b->user_va;
    *out_pitch  = b->pitch;
    return 0;
}

int gfx2d_map_buf(gfx2d_t *g, uint64_t handle,
                  void **out_addr, uint32_t *out_size,
                  uint32_t *out_pitch, uint32_t *out_fmt)
{
    if (!g || !out_addr) return -EINVAL;

    void *va = (void *)(uintptr_t)handle;

    /*
     * Look up by user VA (64-bit) in the buffer table so we can return
     * accurate size/pitch/fmt.  gfx2d_alloc_buf is the only code that
     * adds entries, so anything not in the table is an invalid handle.
     */
    for (uint32_t i = 0; i < g->n_bufs; i++) {
        if (g->bufs[i].user_va == va) {
            *out_addr = va;
            if (out_size)  *out_size  = g->bufs[i].size_bytes;
            if (out_pitch) *out_pitch = g->bufs[i].pitch;
            if (out_fmt)   *out_fmt   = g->bufs[i].fmt;
            return 0;
        }
    }

    /* Handle not found */
    *out_addr = NULL;
    if (out_size)  *out_size  = 0;
    if (out_pitch) *out_pitch = 0;
    if (out_fmt)   *out_fmt   = 0;
    return -EINVAL;
}

/* ── Cursor ────────────────────────────────────────────────────────── */

int gfx2d_cursor_set(gfx2d_t *g, uint32_t w, uint32_t h,
                     int32_t hot_x, int32_t hot_y,
                     const uint32_t *argb_pixels) {
    if (!g || !argb_pixels) return -EINVAL;

    uint32_t pixel_bytes = w * h * 4u;
    uint32_t total = (uint32_t)sizeof(mvc3_cursor_set_t) + pixel_bytes;
    if (total > sizeof(mvc3_cursor_set_t) + 128u * 128u * 4u) return -EINVAL;

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return -ENOMEM;

    mvc3_cursor_set_t *cs = (mvc3_cursor_set_t *)buf;
    memset(cs, 0, sizeof(*cs));
    hdr_init(&cs->hdr, MVC3_CMD_CURSOR_SET, total);
    cs->w     = w;
    cs->h     = h;
    cs->hot_x = hot_x;
    cs->hot_y = hot_y;
    memcpy(buf + sizeof(mvc3_cursor_set_t), argb_pixels, pixel_bytes);

    int rc = write_full(g->fd, buf, total);
    free(buf);
    return rc;
}

int gfx2d_cursor_move(gfx2d_t *g, int32_t x, int32_t y) {
    if (!g) return -EINVAL;
    mvc3_cursor_move_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    hdr_init(&pkt.hdr, MVC3_CMD_CURSOR_MOVE, sizeof(pkt));
    pkt.x = x;
    pkt.y = y;
    return write_full(g->fd, &pkt, sizeof(pkt));
}

int gfx2d_cursor_show(gfx2d_t *g, int visible) {
    if (!g) return -EINVAL;
    mvc3_cursor_show_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    hdr_init(&pkt.hdr, MVC3_CMD_CURSOR_SHOW, sizeof(pkt));
    pkt.visible = visible ? 1u : 0u;
    return write_full(g->fd, &pkt, sizeof(pkt));
}