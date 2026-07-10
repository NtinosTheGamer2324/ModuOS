/*
 * mvc3_sqrm.c — ModuOS Video Controller v3 — SQRM GENERIC module
 *
 * Registers $/dev/mvc/mvi0 as a DevFS node.
 *
 * Copyright © 2025-2026 New Technologies Software — GPL v2.0
 */

#include <stdint.h>
#include <stddef.h>

#include "sqrm_sdk.h"
#include "moduos/fs/devfs.h"
#include "mvc3.h"

/* ── Module descriptor ─────────────────────────────────────────────── */

SQRM_DEFINE_MODULE(SQRM_TYPE_GENERIC, "mvc3");

/* ── Tunables ──────────────────────────────────────────────────────── */

#define MVC3_DEFAULT_RING_BYTES  (1024u * 1024u)
#define MVC3_MAX_RING_BYTES      (4u * 1024u * 1024u)
#define MVC3_MAX_SESSIONS        4u
#define MVC3_MAX_MAPPINGS        8u

#define MVC3_STR2(x) #x
#define MVC3_STR(x)  MVC3_STR2(x)
#define MVC3_DEFAULT_RING_BYTES_STR MVC3_STR(MVC3_DEFAULT_RING_BYTES)

/* ── Globals ───────────────────────────────────────────────────────── */

static const sqrm_kernel_api_t *g_api;

/* ── Logging ───────────────────────────────────────────────────────── */

static void log(const char *s) {
    g_api->com_write_string(0x3F8, "[mvc3] ");
    g_api->com_write_string(0x3F8, s);
    g_api->com_write_string(0x3F8, "\n");
}

#define MVC3_COM2_PORT 0x2F8

static void log2(const char *s) {
    g_api->com_write_string(MVC3_COM2_PORT, "[mvc3] ");
    g_api->com_write_string(MVC3_COM2_PORT, s);
    g_api->com_write_string(MVC3_COM2_PORT, "\n");
}

/* Minimal unsigned 64-bit -> hex string, zero-padded to 16 chars, "0x" prefixed. */
static void u64_to_hex(uint64_t v, char *out /* must hold 19 bytes: "0x" + 16 hex + NUL */) {
    static const char digits[] = "0123456789abcdef";
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 16; i++) {
        out[2 + i] = digits[(v >> ((15 - i) * 4)) & 0xF];
    }
    out[18] = '\0';
}

/* Logs "name=0x..." to COM2, no trailing newline. */
static void log2_kv(const char *name, uint64_t value) {
    char hexbuf[19];
    u64_to_hex(value, hexbuf);
    g_api->com_write_string(MVC3_COM2_PORT, name);
    g_api->com_write_string(MVC3_COM2_PORT, "=");
    g_api->com_write_string(MVC3_COM2_PORT, hexbuf);
}

/* ── Per-session off-screen buffer tracking ────────────────────────── */

typedef struct {
    void    *kva;
    uint64_t size;
    uint64_t user_va;
} mvc3_mapping_t;

/* ── Per-session state ─────────────────────────────────────────────── */

typedef struct {
    int      active;

    uint8_t  resp_buf[256];
    uint32_t resp_len;
    uint32_t resp_off;

    mvc3_ring_slot_t *ring;
    uint64_t          ring_bytes;
    uint64_t          ring_slot_count;
    uint64_t          ring_head;
    uint64_t          ring_user_va;
    int               ring_is_mapped;

    uint8_t  pkt_buf[4096];
    uint32_t pkt_len;

    mvc3_mapping_t mappings[MVC3_MAX_MAPPINGS];
    uint64_t       n_mappings;
} mvc3_session_t;

static mvc3_session_t g_sessions[MVC3_MAX_SESSIONS];

/* ── Utility ───────────────────────────────────────────────────────── */

static void *memset_local(void *dst, int c, size_t n) {
    unsigned char *p = (unsigned char *)dst;
    while (n--) *p++ = (unsigned char)c;
    return dst;
}

static void *memcpy_local(void *dst, const void *src, size_t n) {
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

static void *memmove_local(void *dst, const void *src, size_t n) {
    unsigned char       *d     = (unsigned char *)dst;
    const unsigned char *s_ptr = (const unsigned char *)src;
    if (d == s_ptr || n == 0) return dst;
    if (d < s_ptr) {
        while (n--) *d++ = *s_ptr++;
    } else {
        d += n; s_ptr += n;
        while (n--) *--d = *--s_ptr;
    }
    return dst;
}

static mvc3_session_t *session_alloc(void) {
    for (uint32_t i = 0; i < MVC3_MAX_SESSIONS; i++) {
        if (!g_sessions[i].active) {
            memset_local(&g_sessions[i], 0, sizeof(g_sessions[i]));
            g_sessions[i].active = 1;
            return &g_sessions[i];
        }
    }
    return (void *)0;
}

static void session_free(mvc3_session_t *s) {
    if (!s) return;
    if (s->ring) {
        if (!s->ring_is_mapped)
            g_api->kfree(s->ring);
        s->ring           = (void *)0;
        s->ring_is_mapped = 0;
        s->ring_bytes = s->ring_slot_count = s->ring_head = 0;
        s->ring_user_va = 0;
    }
    for (uint64_t i = 0; i < s->n_mappings; i++) {
        if (s->mappings[i].kva) {
            g_api->kfree(s->mappings[i].kva);
            s->mappings[i].kva = (void *)0;
        }
    }
    s->n_mappings = 0;
    s->active = 0;
}

static void session_push_resp(mvc3_session_t *s, const void *data, uint32_t len) {
    if (len > sizeof(s->resp_buf)) len = sizeof(s->resp_buf);
    memcpy_local(s->resp_buf, data, len);
    s->resp_len = len;
    s->resp_off = 0;
}

/* ── Software rasterizer ───────────────────────────────────────────── */

static uint32_t argb_to_native(const framebuffer_t *fb, uint32_t argb) {
    if (fb->bpp == 32) return argb;
    if (fb->bpp == 16) {
        uint8_t r = (argb >> 16) & 0xFF;
        uint8_t g = (argb >>  8) & 0xFF;
        uint8_t b = (argb >>  0) & 0xFF;
        return (uint32_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
    return argb;
}

static void sw_put_pixel(const framebuffer_t *fb, uint32_t x, uint32_t y,
                         uint32_t native_pixel) {
    uint8_t *row = (uint8_t *)fb->addr + (uint64_t)y * fb->pitch;
    if (fb->bpp == 32)      ((uint32_t *)row)[x] = native_pixel;
    else if (fb->bpp == 16) ((uint16_t *)row)[x] = (uint16_t)native_pixel;
}

static int sw_clip_rect(const framebuffer_t *fb,
                        uint32_t *x, uint32_t *y,
                        uint32_t *w, uint32_t *h) {
    if (*x >= fb->width || *y >= fb->height) return 0;
    if (*x + *w > fb->width)  *w = fb->width  - *x;
    if (*y + *h > fb->height) *h = fb->height - *y;
    return (*w && *h) ? 1 : 0;
}

static void sw_fill_rect(const framebuffer_t *fb,
                         uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         uint32_t argb) {
    if (!fb || !fb->addr) return;
    if (!sw_clip_rect(fb, &x, &y, &w, &h)) return;
    uint32_t pixel = argb_to_native(fb, argb);
    if (fb->bpp == 32) {
        uint8_t  *base = (uint8_t *)fb->addr + (uint64_t)y * fb->pitch + (uint64_t)x * 4;
        uint32_t *fr   = (uint32_t *)base;
        for (uint32_t i = 0; i < w; i++) fr[i] = pixel;
        size_t rb = (size_t)w * 4u;
        for (uint32_t r = 1; r < h; r++)
            memcpy_local(base + (uint64_t)r * fb->pitch, base, rb);
    } else {
        for (uint32_t r = 0; r < h; r++)
            for (uint32_t c = 0; c < w; c++)
                sw_put_pixel(fb, x + c, y + r, pixel);
    }
}

static void sw_blit_rect(const framebuffer_t *fb,
                         uint32_t sx, uint32_t sy,
                         uint32_t dx, uint32_t dy,
                         uint32_t w, uint32_t h) {
    if (!fb || !fb->addr) return;
    if (sx >= fb->width || sy >= fb->height) return;
    if (sx + w > fb->width)  w = fb->width  - sx;
    if (sy + h > fb->height) h = fb->height - sy;
    if (dx >= fb->width || dy >= fb->height) return;
    if (dx + w > fb->width)  w = fb->width  - dx;
    if (dy + h > fb->height) h = fb->height - dy;
    if (!w || !h) return;

    uint8_t *base      = (uint8_t *)fb->addr;
    uint32_t bpp_bytes = (fb->bpp + 7u) / 8u;
    size_t   row_bytes = (size_t)w * bpp_bytes;

    if (dy < sy || (dy == sy && dx < sx)) {
        for (uint32_t r = 0; r < h; r++) {
            uint8_t *sr = base + (uint64_t)(sy + r) * fb->pitch + (uint64_t)sx * bpp_bytes;
            uint8_t *dr = base + (uint64_t)(dy + r) * fb->pitch + (uint64_t)dx * bpp_bytes;
            memmove_local(dr, sr, row_bytes);
        }
    } else {
        for (int32_t r = (int32_t)h - 1; r >= 0; r--) {
            uint8_t *sr = base + (uint64_t)(sy + (uint32_t)r) * fb->pitch + (uint64_t)sx * bpp_bytes;
            uint8_t *dr = base + (uint64_t)(dy + (uint32_t)r) * fb->pitch + (uint64_t)dx * bpp_bytes;
            memmove_local(dr, sr, row_bytes);
        }
    }
}

/*
 * sw_blit_buf — blit from a user-mapped off-screen buffer to the screen.
 *
 * user_va is the USER VA of the buffer as returned by gfx2d_alloc_buf.
 * The kernel can read userspace pages freely during a write() syscall,
 * so this works without any special mapping for the copy-batch path.
 * When the zero-copy path is active the buffer is also mapped by
 * devfs_mmap_region, so the same user VA is valid either way.
 */
static void sw_blit_buf(const framebuffer_t *fb,
                        uint64_t user_va,
                        uint32_t src_x, uint32_t src_y,
                        uint32_t dst_x, uint32_t dst_y,
                        uint32_t w, uint32_t h,
                        uint32_t src_pitch) {
    if (!fb || !fb->addr || user_va == 0) return;
    if (dst_x >= fb->width || dst_y >= fb->height) return;
    if (dst_x + w > fb->width)  w = fb->width  - dst_x;
    if (dst_y + h > fb->height) h = fb->height - dst_y;
    if (!w || !h) return;

    const uint8_t *src_base = (const uint8_t *)(uintptr_t)user_va;
    uint8_t       *dst_base = (uint8_t *)fb->addr;
    uint32_t bpp_bytes = (fb->bpp + 7u) / 8u;

    for (uint32_t r = 0; r < h; r++) {
        const uint32_t *src_row = (const uint32_t *)(
            src_base + (uint64_t)(src_y + r) * src_pitch + (uint64_t)src_x * 4u);
        uint8_t *dst_row = dst_base
            + (uint64_t)(dst_y + r) * fb->pitch
            + (uint64_t)dst_x * bpp_bytes;
        if (fb->bpp == 32) {
            memcpy_local(dst_row, src_row, (size_t)w * 4u);
        } else {
            for (uint32_t c = 0; c < w; c++) {
                uint32_t pix = argb_to_native(fb, src_row[c]);
                if (fb->bpp == 16) ((uint16_t *)dst_row)[c] = (uint16_t)pix;
            }
        }
    }
}

/* ── Draw-slot dispatcher ──────────────────────────────────────────── */

static void dispatch_slot(mvc3_session_t *s, const mvc3_ring_slot_t *slot) {
    if (!slot) return;

    const framebuffer_t *fb = g_api->gfx_get_framebuffer ? g_api->gfx_get_framebuffer() : NULL;
    if (!fb) return;

    uint32_t caps = g_api->gfx_get_caps ? g_api->gfx_get_caps() : 0u;
    int hw_2d = (caps & SQRM_GPU_CAP_2D_ACCEL);

    switch (slot->op) {
    case MVC3_OP_FILL_RECT:
        if (hw_2d && g_api->gfx_fill_rect)
            g_api->gfx_fill_rect(slot->u.fill.x, slot->u.fill.y, slot->u.fill.w, slot->u.fill.h, slot->u.fill.argb);
        else
            sw_fill_rect(fb, slot->u.fill.x, slot->u.fill.y, slot->u.fill.w, slot->u.fill.h, slot->u.fill.argb);
        break;

    case MVC3_OP_BLIT:
        log2("BLIT received from userland");
        log2_kv("  src_x", slot->u.blit.src_x);
        g_api->com_write_string(MVC3_COM2_PORT, "\n");
        log2_kv("  src_y", slot->u.blit.src_y);
        g_api->com_write_string(MVC3_COM2_PORT, "\n");
        log2_kv("  dst_x", slot->u.blit.dst_x);
        g_api->com_write_string(MVC3_COM2_PORT, "\n");
        log2_kv("  dst_y", slot->u.blit.dst_y);
        g_api->com_write_string(MVC3_COM2_PORT, "\n");
        log2_kv("  w", slot->u.blit.w);
        g_api->com_write_string(MVC3_COM2_PORT, "\n");
        log2_kv("  h", slot->u.blit.h);
        g_api->com_write_string(MVC3_COM2_PORT, "\n");
    
        if (hw_2d && g_api->gfx_blit_rect)
            g_api->gfx_blit_rect(slot->u.blit.src_x, slot->u.blit.src_y, slot->u.blit.dst_x, slot->u.blit.dst_y, slot->u.blit.w, slot->u.blit.h);
        else
            sw_blit_rect(fb, slot->u.blit.src_x, slot->u.blit.src_y, slot->u.blit.dst_x, slot->u.blit.dst_y, slot->u.blit.w, slot->u.blit.h);
        break;

    case MVC3_OP_BLIT_BUF: {
        log2("BLIT_BUF: received");
    
        uint64_t user_va  = (uint64_t)slot->u.blit_buf.handle;
        uint32_t bpp_bytes_src = 4; /* buffers are always ARGB8888 */
        uint64_t row_bytes = (uint64_t)slot->u.blit_buf.w * bpp_bytes_src;
        uint64_t copy_bytes = row_bytes * slot->u.blit_buf.h;
    
        log2_kv("  handle/user_va", user_va);
        g_api->com_write_string(MVC3_COM2_PORT, "\n");
        log2_kv("  src_x", slot->u.blit_buf.src_x);
        g_api->com_write_string(MVC3_COM2_PORT, "\n");
        log2_kv("  src_y", slot->u.blit_buf.src_y);
        g_api->com_write_string(MVC3_COM2_PORT, "\n");
        log2_kv("  dst_x", slot->u.blit_buf.dst_x);
        g_api->com_write_string(MVC3_COM2_PORT, "\n");
        log2_kv("  dst_y", slot->u.blit_buf.dst_y);
        g_api->com_write_string(MVC3_COM2_PORT, "\n");
        log2_kv("  w", slot->u.blit_buf.w);
        g_api->com_write_string(MVC3_COM2_PORT, "\n");
        log2_kv("  h", slot->u.blit_buf.h);
        g_api->com_write_string(MVC3_COM2_PORT, "\n");
        log2_kv("  src_pitch", slot->u.blit_buf.src_pitch);
        g_api->com_write_string(MVC3_COM2_PORT, "\n");
        log2_kv("  row_bytes (computed)", row_bytes);
        g_api->com_write_string(MVC3_COM2_PORT, "\n");
        log2_kv("  copy_bytes (computed)", copy_bytes);
        g_api->com_write_string(MVC3_COM2_PORT, "\n");
    
        /* Bound the copy to something sane before allocating. */
        if (copy_bytes == 0 || copy_bytes > (16u * 1024u * 1024u)) {
            log2_kv("  BAIL: copy_bytes out of range", copy_bytes);
            g_api->com_write_string(MVC3_COM2_PORT, "\n");
            break;
        }
    
        if (user_va == 0) {
            log2("  BAIL: handle/user_va is NULL");
            break;
        }
    
        uint8_t *tmp = (uint8_t *)g_api->kmalloc(copy_bytes);
        log2_kv("  tmp kva", (uint64_t)(uintptr_t)tmp);
        g_api->com_write_string(MVC3_COM2_PORT, "\n");
        if (!tmp) {
            log2("  BAIL: kmalloc failed");
            break;
        }
    
        /* Copy row-by-row from the user buffer (respecting src_pitch)
         * into a tightly-packed kernel buffer using a real usercopy. */
        int ok = 1;
        for (uint32_t r = 0; r < slot->u.blit_buf.h; r++) {
            uint64_t src_uva = user_va
                              + (uint64_t)(slot->u.blit_buf.src_y + r) * slot->u.blit_buf.src_pitch
                              + (uint64_t)slot->u.blit_buf.src_x * bpp_bytes_src;
            uint64_t dst_kva = (uint64_t)(uintptr_t)(tmp + (uint64_t)r * row_bytes);
        
            if (r == 0 || r == slot->u.blit_buf.h - 1) {
                /* log first and last row so we can spot bad VA arithmetic */
                log2_kv("  usercopy row", r);
                g_api->com_write_string(MVC3_COM2_PORT, "\n");
                log2_kv("    src_uva", src_uva);
                g_api->com_write_string(MVC3_COM2_PORT, "\n");
                log2_kv("    dst_kva", dst_kva);
                g_api->com_write_string(MVC3_COM2_PORT, "\n");
                log2_kv("    row_bytes", row_bytes);
                g_api->com_write_string(MVC3_COM2_PORT, "\n");
            }
        
            if (g_api->usercopy_from_user(tmp + (uint64_t)r * row_bytes,
                                    (const void *)(uintptr_t)src_uva,
                                    row_bytes) != (int *)0) {
                log2_kv("  BAIL: usercopy_from_user failed at row", r);
                g_api->com_write_string(MVC3_COM2_PORT, "\n");
                log2_kv("    src_uva was", src_uva);
                g_api->com_write_string(MVC3_COM2_PORT, "\n");
                ok = 0;
                break;
            }
        }
    
        log2_kv("  usercopy ok", (uint64_t)ok);
        g_api->com_write_string(MVC3_COM2_PORT, "\n");
    
        if (ok) {
            log2_kv("  caps", (uint64_t)caps);
            g_api->com_write_string(MVC3_COM2_PORT, "\n");
            log2_kv("  SQRM_GPU_CAP_BLIT_BUF set",
                    (uint64_t)!!(caps & SQRM_GPU_CAP_BLIT_BUF));
            g_api->com_write_string(MVC3_COM2_PORT, "\n");
            log2_kv("  gfx_blit_buffer ptr",
                    (uint64_t)(uintptr_t)g_api->gfx_blit_buffer);
            g_api->com_write_string(MVC3_COM2_PORT, "\n");
            
            if ((caps & SQRM_GPU_CAP_BLIT_BUF) && g_api->gfx_blit_buffer) {
                log2("  path: HW gfx_blit_buffer");
                g_api->gfx_blit_buffer(slot->u.blit_buf.dst_x, slot->u.blit_buf.dst_y,
                                       slot->u.blit_buf.w,     slot->u.blit_buf.h,
                                       tmp,                    (uint32_t)row_bytes);
                log2("  HW blit done");
            } else {
                log2("  path: SW sw_blit_buf");
                const framebuffer_t *fb2 = fb; /* fb already fetched above */
                log2_kv("  fb ptr",   (uint64_t)(uintptr_t)fb2);
                g_api->com_write_string(MVC3_COM2_PORT, "\n");
                if (fb2) {
                    log2_kv("  fb->width",  fb2->width);
                    g_api->com_write_string(MVC3_COM2_PORT, "\n");
                    log2_kv("  fb->height", fb2->height);
                    g_api->com_write_string(MVC3_COM2_PORT, "\n");
                    log2_kv("  fb->bpp",    fb2->bpp);
                    g_api->com_write_string(MVC3_COM2_PORT, "\n");
                    log2_kv("  fb->pitch",  fb2->pitch);
                    g_api->com_write_string(MVC3_COM2_PORT, "\n");
                    log2_kv("  fb->addr",   (uint64_t)(uintptr_t)fb2->addr);
                    g_api->com_write_string(MVC3_COM2_PORT, "\n");
                }
                sw_blit_buf(fb2, (uint64_t)(uintptr_t)tmp,
                            0, 0,
                            slot->u.blit_buf.dst_x, slot->u.blit_buf.dst_y,
                            slot->u.blit_buf.w,     slot->u.blit_buf.h,
                            (uint32_t)row_bytes);
                log2("  SW blit done");
            }
        }
    
        g_api->kfree(tmp);
        log2("BLIT_BUF: done");
        break;
    }
    default: break;
    }
}

/* ── Command handlers ──────────────────────────────────────────────── */

static void handle_get_info(mvc3_session_t *s) {
    mvc3_get_info_resp_t resp;
    memset_local(&resp, 0, sizeof(resp));
    resp.hdr.magic       = MVC3_MAGIC;
    resp.hdr.abi_version = MVC3_ABI_VERSION;
    resp.hdr.cmd         = MVC3_CMD_GET_INFO;
    resp.hdr.size_bytes  = sizeof(resp);

    const framebuffer_t *fb = g_api->gfx_get_framebuffer
                            ? g_api->gfx_get_framebuffer() : (void *)0;
    if (fb) {
        resp.width  = fb->width;
        resp.height = fb->height;
        resp.pitch  = fb->pitch;
        resp.bpp    = fb->bpp;
        resp.fmt    = (uint32_t)fb->fmt;
    }
    resp.caps = g_api->gfx_get_caps ? g_api->gfx_get_caps() : 0u;

    const char *drv = g_api->get_gpu_driver_name
                    ? g_api->get_gpu_driver_name() : "unknown";
    uint32_t di = 0;
    while (drv[di] && di + 1 < sizeof(resp.driver)) { resp.driver[di] = drv[di]; di++; }
    resp.driver[di] = 0;

    session_push_resp(s, &resp, sizeof(resp));
}

static void handle_map_ring(mvc3_session_t *s, const mvc3_map_ring_req_t *req) {
    mvc3_map_ring_resp_t resp;
    memset_local(&resp, 0, sizeof(resp));
    resp.hdr.magic       = MVC3_MAGIC;
    resp.hdr.abi_version = MVC3_ABI_VERSION;
    resp.hdr.cmd         = MVC3_CMD_MAP_RING;
    resp.hdr.size_bytes  = sizeof(resp);
    resp.slot_size       = sizeof(mvc3_ring_slot_t);

    if (s->ring) {
        if (!s->ring_is_mapped)
            g_api->kfree(s->ring);
        s->ring           = (void *)0;
        s->ring_is_mapped = 0;
        s->ring_bytes = s->ring_slot_count = s->ring_head = 0;
        s->ring_user_va = 0;
    }

    uint64_t want = req->requested_size;
    if (want < sizeof(mvc3_ring_slot_t)) want = MVC3_DEFAULT_RING_BYTES;
    if (want > MVC3_MAX_RING_BYTES)      want = MVC3_MAX_RING_BYTES;

    uint64_t slot_sz = sizeof(mvc3_ring_slot_t);
    want = ((want + slot_sz - 1u) / slot_sz) * slot_sz;
    want = (want + 0xFFFu) & ~0xFFFu;
    
    s->ring = (mvc3_ring_slot_t *)g_api->kmalloc(want);
    if (!s->ring) {
        log("MAP_RING: kmalloc failed");
        session_push_resp(s, &resp, sizeof(resp));
        return;
    }
    memset_local(s->ring, 0, want);
    s->ring_bytes      = want;
    s->ring_slot_count = want / slot_sz;
    s->ring_head       = 0;
    s->ring_user_va    = 0;

    if (g_api->devfs_mmap_region) {
        void *uva = g_api->devfs_mmap_region(
            (uint64_t)(uintptr_t)s->ring, want, 3, 0 /*virt*/);
        if (uva != (void *)-1) {
            s->ring_user_va   = (uint64_t)(uintptr_t)uva;
            s->ring_is_mapped = 1;
            resp.user_addr    = s->ring_user_va;
            resp.actual_size  = want;
            log("MAP_RING: zero-copy ring mapped into userland");
        } else {
            resp.user_addr   = 0;
            resp.actual_size = want;
            log("MAP_RING: mmap failed, copy-batch fallback");
        }
    } else {
        resp.user_addr   = 0;
        resp.actual_size = want;
        log("MAP_RING: devfs_mmap_region unavailable, copy-batch fallback");
    }

    session_push_resp(s, &resp, sizeof(resp));
}

static void handle_submit(mvc3_session_t *s, const mvc3_submit_t *sub) {
    if (!s->ring) return;
    uint64_t count = sub->count;
    if (count > s->ring_slot_count) count = s->ring_slot_count;
    for (uint64_t i = 0; i < count; i++) {
        dispatch_slot(s, &s->ring[(s->ring_head + i) % s->ring_slot_count]);
    }
    s->ring_head = (s->ring_head + count) % s->ring_slot_count;
}

static void handle_enqueue(mvc3_session_t *s, const mvc3_enqueue_t *pkt) {
    dispatch_slot(s, &pkt->slot);
}

static void handle_flush(const mvc3_flush_t *pkt) {
    uint32_t caps = g_api->gfx_get_caps ? g_api->gfx_get_caps() : 0u;
    if ((caps & SQRM_GPU_CAP_2D_ACCEL) && g_api->gfx_flush)
        g_api->gfx_flush(pkt->x, pkt->y, pkt->w, pkt->h);
}

static void handle_alloc_buf(mvc3_session_t *s, const mvc3_alloc_buf_req_t *req) {
    mvc3_alloc_buf_resp_t resp;
    memset_local(&resp, 0, sizeof(resp));
    resp.hdr.magic       = MVC3_MAGIC;
    resp.hdr.abi_version = MVC3_ABI_VERSION;
    resp.hdr.cmd         = MVC3_CMD_ALLOC_BUF;
    resp.hdr.size_bytes  = sizeof(resp);

    if (s->n_mappings >= MVC3_MAX_MAPPINGS) {
        log("ALLOC_BUF: mapping table full");
        session_push_resp(s, &resp, sizeof(resp));
        return;
    }

    uint64_t sz = ((uint64_t)req->size_bytes + 0xFFFu) & ~(uint64_t)0xFFF;
    void *buf = g_api->kmalloc(sz);
    if (!buf) {
        session_push_resp(s, &resp, sizeof(resp));
        return;
    }
    memset_local(buf, 0, sz);

    uint32_t idx = s->n_mappings++;
    s->mappings[idx].kva     = buf;
    s->mappings[idx].size    = sz;
    s->mappings[idx].user_va = 0;

    const framebuffer_t *fb = g_api->gfx_get_framebuffer
                            ? g_api->gfx_get_framebuffer() : (void *)0;

    /*
     * Return the full 64-bit kernel VA as the handle.
     * Truncating to 32 bits would corrupt any KVA above 4 GiB (e.g.
     * 0xFFFF800001xxxxxx) and break the mvi0_mmap mapping-table lookup.
     */
    resp.handle = (uint64_t)(uintptr_t)buf;
    resp.pitch  = fb ? fb->pitch : 0;
    session_push_resp(s, &resp, sizeof(resp));
}

static void handle_map_buf(mvc3_session_t *s, const mvc3_map_buf_req_t *req) {
    mvc3_map_buf_resp_t resp;
    memset_local(&resp, 0, sizeof(resp));
    resp.hdr.magic       = MVC3_MAGIC;
    resp.hdr.abi_version = MVC3_ABI_VERSION;
    resp.hdr.cmd         = MVC3_CMD_MAP_BUF;
    resp.hdr.size_bytes  = sizeof(resp);

    /* req->handle is the full 64-bit KVA returned by ALLOC_BUF */
    void    *kva = (void *)(uintptr_t)req->handle;
    uint64_t sz  = 0;
    uint64_t mi  = s->n_mappings;

    for (uint64_t i = 0; i < s->n_mappings; i++) {
        if (s->mappings[i].kva == kva) { sz = s->mappings[i].size; mi = i; break; }
    }

    if (sz == 0 || !g_api->devfs_mmap_region) {
        resp.user_addr  = (uint64_t)(uintptr_t)kva;
        resp.size_bytes = (uint32_t)sz;
        session_push_resp(s, &resp, sizeof(resp));
        return;
    }

    if (s->mappings[mi].user_va != 0) {
        resp.user_addr  = s->mappings[mi].user_va;
        resp.size_bytes = (uint32_t)sz;
    } else {
        void *uva = g_api->devfs_mmap_region(
            (uint64_t)(uintptr_t)kva, sz, 3, 0 /*virt*/);
        if (uva != (void *)-1)
            s->mappings[mi].user_va = (uint64_t)(uintptr_t)uva;
        resp.user_addr  = s->mappings[mi].user_va
                        ? s->mappings[mi].user_va
                        : (uint64_t)(uintptr_t)kva;
        resp.size_bytes = (uint32_t)sz;
    }

    const framebuffer_t *fb = g_api->gfx_get_framebuffer
                            ? g_api->gfx_get_framebuffer() : (void *)0;
    if (fb) { resp.pitch = fb->pitch; resp.fmt = (uint32_t)fb->fmt; }

    session_push_resp(s, &resp, sizeof(resp));
}

static void handle_cursor_move(const mvc3_cursor_move_t *pkt) {
    if (g_api->gfx_cursor_move) g_api->gfx_cursor_move(pkt->x, pkt->y);
}
static void handle_cursor_show(const mvc3_cursor_show_t *pkt) {
    if (g_api->gfx_cursor_show) g_api->gfx_cursor_show((int)pkt->visible);
}

/* ── Packet processor ──────────────────────────────────────────────── */

static uint32_t process_packet(mvc3_session_t *s) {
    if (s->pkt_len < sizeof(mvc3_hdr_t)) return 0;
    const mvc3_hdr_t *hdr = (const mvc3_hdr_t *)s->pkt_buf;
    if (hdr->magic != MVC3_MAGIC)             return 1;
    if (hdr->size_bytes < sizeof(mvc3_hdr_t)) return 1;
    if (s->pkt_len < hdr->size_bytes)         return 0;

    uint32_t consumed = hdr->size_bytes;
    switch ((mvc3_cmd_t)hdr->cmd) {
    case MVC3_CMD_GET_INFO:   handle_get_info(s); break;
    case MVC3_CMD_MAP_RING:
        if (consumed >= sizeof(mvc3_map_ring_req_t))
            handle_map_ring(s, (const mvc3_map_ring_req_t *)s->pkt_buf);
        break;
    case MVC3_CMD_SUBMIT:
        if (consumed >= sizeof(mvc3_submit_t))
            handle_submit(s, (const mvc3_submit_t *)s->pkt_buf);
        break;
    case MVC3_CMD_ENQUEUE:
        if (consumed >= sizeof(mvc3_enqueue_t))
            handle_enqueue(s, (const mvc3_enqueue_t *)s->pkt_buf);
        break;
    case MVC3_CMD_FLUSH:
        if (consumed >= sizeof(mvc3_flush_t))
            handle_flush((const mvc3_flush_t *)s->pkt_buf);
        break;
    case MVC3_CMD_ALLOC_BUF:
        if (consumed >= sizeof(mvc3_alloc_buf_req_t))
            handle_alloc_buf(s, (const mvc3_alloc_buf_req_t *)s->pkt_buf);
        break;
    case MVC3_CMD_MAP_BUF:
        if (consumed >= sizeof(mvc3_map_buf_req_t))
            handle_map_buf(s, (const mvc3_map_buf_req_t *)s->pkt_buf);
        break;
    case MVC3_CMD_CURSOR_MOVE:
        if (consumed >= sizeof(mvc3_cursor_move_t))
            handle_cursor_move((const mvc3_cursor_move_t *)s->pkt_buf);
        break;
    case MVC3_CMD_CURSOR_SHOW:
        if (consumed >= sizeof(mvc3_cursor_show_t))
            handle_cursor_show((const mvc3_cursor_show_t *)s->pkt_buf);
        break;
    default: break;
    }
    return consumed;
}

/* ── DevFS callbacks ───────────────────────────────────────────────── */

static void *mvi0_open(void *ctx, int flags) {
    (void)ctx; (void)flags;
    mvc3_session_t *s = session_alloc();
    if (!s) { log("mvi0: open failed — no free sessions"); return (void *)0; }
    log("mvi0: opened");
    return (void *)s;
}

static int mvi0_close(void *ctx) {
    mvc3_session_t *s = (mvc3_session_t *)ctx;
    if (s) session_free(s);
    log("mvi0: closed");
    return 0;
}

static ssize_t mvi0_read(void *ctx, void *buf, size_t count) {
    mvc3_session_t *s = (mvc3_session_t *)ctx;
    if (!s || s->resp_len == 0) return 0;
    uint32_t avail = s->resp_len - s->resp_off;
    if ((uint32_t)count < avail) avail = (uint32_t)count;
    memcpy_local(buf, s->resp_buf + s->resp_off, avail);
    s->resp_off += avail;
    if (s->resp_off >= s->resp_len) { s->resp_len = 0; s->resp_off = 0; }
    return (ssize_t)avail;
}

static ssize_t mvi0_write(void *ctx, const void *buf, size_t count) {
    mvc3_session_t *s = (mvc3_session_t *)ctx;
    if (!s || !buf || count == 0) return 0;

    const uint8_t *src  = (const uint8_t *)buf;
    size_t         left = count;

    while (left > 0) {
        uint32_t space = sizeof(s->pkt_buf) - s->pkt_len;
        if (space == 0) { s->pkt_len = 0; space = sizeof(s->pkt_buf); }
        uint32_t copy = (uint32_t)(left < space ? left : space);
        memcpy_local(s->pkt_buf + s->pkt_len, src, copy);
        s->pkt_len += copy; src += copy; left -= copy;

        while (s->pkt_len >= sizeof(mvc3_hdr_t)) {
            uint32_t consumed = process_packet(s);
            if (consumed == 0) break;
            uint32_t remaining = s->pkt_len - consumed;
            if (remaining > 0)
                memmove_local(s->pkt_buf, s->pkt_buf + consumed, remaining);
            s->pkt_len = remaining;
        }
    }
    return (ssize_t)count;
}

/*
 * mvi0_mmap — DevFS mmap hook
 *
 * MVC3_OFF_RING                 → map the command ring
 * MVC3_OFF_FB                   → map the live framebuffer (MMIO)
 * MVC3_OFF_BUF_BASE + kva       → map an off-screen buffer by its KVA
 */
static void *mvi0_mmap(void *ctx, void *hint, size_t length,
                       int prot, int flags, uint64_t offset) {
    (void)hint; (void)flags;
    mvc3_session_t *s = (mvc3_session_t *)ctx;
    if (!s || !g_api->devfs_mmap_region) return (void *)-1;

    if (offset == MVC3_OFF_FB) {
        const framebuffer_t *fb = g_api->gfx_get_framebuffer
                                ? g_api->gfx_get_framebuffer() : (void *)0;
        if (!fb || !fb->phys_addr || !fb->size_bytes) return (void *)-1;
        size_t sz = (length == 0) ? (size_t)fb->size_bytes : length;
        return g_api->devfs_mmap_region(fb->phys_addr, sz, prot, 1 /*phys*/);
    }

    if (offset == MVC3_OFF_RING) {
        if (!s->ring || s->ring_bytes == 0) return (void *)-1;
        if (s->ring_user_va) return (void *)(uintptr_t)s->ring_user_va;
        size_t sz = (length == 0) ? s->ring_bytes : length;
        void *uva = g_api->devfs_mmap_region(
            (uint64_t)(uintptr_t)s->ring, sz, prot, 0);
        if (uva != (void *)-1) s->ring_user_va = (uint64_t)(uintptr_t)uva;
        return uva;
    }

    if (offset >= MVC3_OFF_BUF_BASE) {
        /*
         * Recover the 64-bit KVA: offset = MVC3_OFF_BUF_BASE + kva
         * This works because MVC3_OFF_BUF_BASE (128 GiB) is safely above
         * any 32-bit address and well below kernel heap VAs (~0xFFFF8...).
         */
        void *kva = (void *)(uintptr_t)(offset - MVC3_OFF_BUF_BASE);
        for (uint64_t i = 0; i < s->n_mappings; i++) {
            if (s->mappings[i].kva == kva) {
                if (s->mappings[i].user_va)
                    return (void *)(uintptr_t)s->mappings[i].user_va;
                size_t sz = (length == 0) ? s->mappings[i].size : length;
                void *uva = g_api->devfs_mmap_region(
                    (uint64_t)(uintptr_t)kva, sz, prot, 0);
                if (uva != (void *)-1)
                    s->mappings[i].user_va = (uint64_t)(uintptr_t)uva;
                return uva;
            }
        }
        return (void *)-1;
    }

    return (void *)-1;
}

/* ── DevFS ops ─────────────────────────────────────────────────────── */

static devfs_device_ops_t g_ops_mvi0 = {
    .name        = "mvi0",
    .open        = mvi0_open,
    .read        = mvi0_read,
    .write       = mvi0_write,
    .close       = mvi0_close,
    .mmap        = mvi0_mmap,
    .can_replace = (void *)0,
};

/* ── Self-test ─────────────────────────────────────────────────────── */

#define MVC3_SELFTEST_W 64u
#define MVC3_SELFTEST_H 64u

static void mvc3_selftest_blit(void) {
    const framebuffer_t *fb = g_api->gfx_get_framebuffer
                            ? g_api->gfx_get_framebuffer() : (void *)0;
    if (!fb || !fb->addr) {
        log("selftest: no framebuffer, skipping");
        return;
    }

    uint32_t caps = g_api->gfx_get_caps ? g_api->gfx_get_caps() : 0u;

    /* ── Red buffer ── */
    uint64_t  src_pitch_red = (uint64_t)MVC3_SELFTEST_W * 4u;
    uint64_t  src_size_red  = src_pitch_red * MVC3_SELFTEST_H;
    uint32_t *src_buf_red   = (uint32_t *)g_api->kmalloc(src_size_red);
    if (!src_buf_red) {
        log("selftest: kmalloc failed for red buffer, skipping");
        return;
    }
    for (uint64_t i = 0; i < (src_size_red / 4u); i++)
        src_buf_red[i] = 0xFFFF0000;

    log2_kv("selftest caps", (uint64_t)caps);
    g_api->com_write_string(MVC3_COM2_PORT, "\n");
    log2_kv("selftest SQRM_GPU_CAP_BLIT_BUF set",
            (uint64_t)!!(caps & SQRM_GPU_CAP_BLIT_BUF));
    g_api->com_write_string(MVC3_COM2_PORT, "\n");
    log2_kv("selftest g_api->gfx_blit_buffer",
            (uint64_t)(uintptr_t)g_api->gfx_blit_buffer);
    g_api->com_write_string(MVC3_COM2_PORT, "\n");
    log2_kv("selftest fb->addr", (uint64_t)(uintptr_t)fb->addr);
    g_api->com_write_string(MVC3_COM2_PORT, "\n");
    log2_kv("selftest fb->width",  (uint64_t)fb->width);
    g_api->com_write_string(MVC3_COM2_PORT, "\n");
    log2_kv("selftest fb->height", (uint64_t)fb->height);
    g_api->com_write_string(MVC3_COM2_PORT, "\n");
    log2_kv("selftest src_buf_red", (uint64_t)(uintptr_t)src_buf_red);
    g_api->com_write_string(MVC3_COM2_PORT, "\n");

    if ((caps & SQRM_GPU_CAP_BLIT_BUF) && g_api->gfx_blit_buffer) {
        log2("selftest red: taking HW path");
        g_api->gfx_blit_buffer(0, 0, MVC3_SELFTEST_W, MVC3_SELFTEST_H,
                               src_buf_red, (uint32_t)src_pitch_red);
        log2("selftest red: HW path done");
    } else {
        log2("selftest red: taking SW path");
        log2_kv("  reason: cap_set",
                (uint64_t)!!(caps & SQRM_GPU_CAP_BLIT_BUF));
        g_api->com_write_string(MVC3_COM2_PORT, "\n");
        log2_kv("  reason: fn_ptr",
                (uint64_t)(uintptr_t)g_api->gfx_blit_buffer);
        g_api->com_write_string(MVC3_COM2_PORT, "\n");
        sw_blit_buf(fb, (uint64_t)(uintptr_t)src_buf_red,
                    0, 0, 0, 0,
                    MVC3_SELFTEST_W, MVC3_SELFTEST_H,
                    (uint32_t)src_pitch_red);
        log2("selftest red: SW path done");
    }

    g_api->kfree(src_buf_red);
    log("selftest: blitted red square at (0,0)");

    /* ── Green buffer ── */
    uint64_t  src_pitch = (uint64_t)MVC3_SELFTEST_W * 4u;
    uint64_t  src_size  = src_pitch * MVC3_SELFTEST_H;
    uint32_t *src_buf   = (uint32_t *)g_api->kmalloc(src_size);
    if (!src_buf) {
        log("selftest: kmalloc failed for green buffer, skipping");
        return;
    }
    for (uint64_t i = 0; i < (src_size / 4u); i++)
        src_buf[i] = 0xFF00FF00;

    log2_kv("selftest green src_buf", (uint64_t)(uintptr_t)src_buf);
    g_api->com_write_string(MVC3_COM2_PORT, "\n");

    if ((caps & SQRM_GPU_CAP_BLIT_BUF) && g_api->gfx_blit_buffer) {
        log2("selftest green: taking HW path");
        g_api->gfx_blit_buffer(100, 100, MVC3_SELFTEST_W, MVC3_SELFTEST_H,
                               src_buf, (uint32_t)src_pitch);
        log2("selftest green: HW path done");
    } else {
        log2("selftest green: taking SW path");
        sw_blit_buf(fb, (uint64_t)(uintptr_t)src_buf,
                    0, 0, 100, 100,
                    MVC3_SELFTEST_W, MVC3_SELFTEST_H,
                    (uint32_t)src_pitch);
        log2("selftest green: SW path done");
    }

    /* ── Flush ── */
    if ((caps & SQRM_GPU_CAP_2D_ACCEL) && g_api->gfx_flush)
        g_api->gfx_flush(0, 0, fb->width, fb->height);

    g_api->kfree(src_buf);
    log("selftest: blitted green square at (100,100)");
    log("selftest: blit self-test complete");
}

/* ── Module init ───────────────────────────────────────────────────── */

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    if (!api || !api->devfs_register_path) return -1;
    g_api = api;
    memset_local(g_sessions, 0, sizeof(g_sessions));

    devfs_owner_t owner = { DEVFS_OWNER_KERNEL, "mvc3" };
    api->devfs_register_path("mvc/mvi0", &g_ops_mvi0, (void *)0);

    log2_kv("sizeof ring_slot", sizeof(mvc3_ring_slot_t));
    log2_kv("sizeof enqueue",   sizeof(mvc3_enqueue_t));

    log("$/dev/mvc/mvi0 registered");
    log("ring  : " MVC3_DEFAULT_RING_BYTES_STR " byte default (zero-copy when mmap available)");
    log("fb    : mappable via MVC3_OFF_FB offset");
    log("bufs  : mappable via MVC3_OFF_BUF_BASE + kva offset");

    mvc3_selftest_blit();

    return 0;
}