#pragma once
/*
 * mvc3.h — ModuOS Video Controller v3 wire protocol
 *
 * Shared between the mvc3 SQRM kernel module and the userland gfx2d library.
 * Both sides must agree on this layout exactly; treat it as ABI.
 *
 * Packet structure
 * ────────────────
 * Every message in both directions starts with mvc3_hdr_t.
 * The kernel reads `size_bytes` to know how many bytes to consume from
 * the ring / write() call.  Userland reads `size_bytes` from response
 * packets to know how many bytes to pull from the device node.
 *
 * Ring-path (fast)
 * ────────────────
 * 1. Userland sends MVC3_CMD_MAP_RING  →  kernel kmalloc's the ring and
 *    replies with the virtual address and actual size.
 * 2. Userland writes MVC3_CMD_SUBMITs to the ring directly (no syscall).
 * 3. One MVC3_CMD_FLUSH write() per frame tells the kernel "process N
 *    ring slots then present".
 *
 * Copy-batch path (fallback)
 * ──────────────────────────
 * Userland accumulates MVC3_CMD_ENQUEUEs in a malloc'd buffer, then
 * write()s the whole buffer in one call followed by MVC3_CMD_FLUSH.
 *
 * Copyright © 2025-2026 ModuOS Project Contributors — GPL v2.0
 */

#include <stdint.h>

/* ── Magic & version ─────────────────────────────────────────────────── */

#define MVC3_MAGIC       0x4D564333u   /* 'M','V','C','3' */
#define MVC3_ABI_VERSION 1u

/* ── Command opcodes ─────────────────────────────────────────────────── */

typedef enum {
    /* Control (bidirectional, require a read() response) */
    MVC3_CMD_GET_INFO    = 0x0001u,   /* query framebuffer info & caps    */
    MVC3_CMD_MAP_RING    = 0x0002u,   /* request kernel-owned ring buffer */

    /* Render commands (write-only, no response) */
    MVC3_CMD_ENQUEUE     = 0x0010u,   /* copy-batch: one draw op          */
    MVC3_CMD_SUBMIT      = 0x0011u,   /* ring-path: commit N ring slots   */
    MVC3_CMD_FLUSH       = 0x0012u,   /* present a screen region          */

    /* Buffer management (bidirectional) */
    MVC3_CMD_ALLOC_BUF   = 0x0020u,   /* allocate an off-screen buffer    */
    MVC3_CMD_MAP_BUF     = 0x0021u,   /* map buffer into userspace        */

    /* Cursor (write-only) */
    MVC3_CMD_CURSOR_SET  = 0x0030u,   /* upload cursor bitmap             */
    MVC3_CMD_CURSOR_MOVE = 0x0031u,   /* reposition cursor                */
    MVC3_CMD_CURSOR_SHOW = 0x0032u,   /* show / hide cursor               */
} mvc3_cmd_t;

/* ── Draw operation sub-opcodes (used inside MVC3_CMD_ENQUEUE / ring) ── */

typedef enum {
    MVC3_OP_FILL_RECT = 1u,   /* fill a solid rectangle               */
    MVC3_OP_BLIT      = 2u,   /* screen-to-screen blit                */
    MVC3_OP_BLIT_BUF  = 3u,   /* off-screen-buffer → screen blit      */
} mvc3_draw_op_t;

/* ── Common packet header ────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t magic;         /* must be MVC3_MAGIC                       */
    uint32_t abi_version;   /* must be MVC3_ABI_VERSION                 */
    uint32_t cmd;           /* mvc3_cmd_t                               */
    uint32_t size_bytes;    /* total byte-length of this packet         */
} mvc3_hdr_t;

/* ── GET_INFO ─────────────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    mvc3_hdr_t hdr;
} mvc3_get_info_req_t;

typedef struct __attribute__((packed)) {
    mvc3_hdr_t hdr;
    uint32_t   width;
    uint32_t   height;
    uint32_t   pitch;
    uint8_t    bpp;
    uint8_t    _pad[3];
    uint32_t   fmt;           /* framebuffer_format_t value             */
    uint32_t   caps;          /* SQRM_GPU_CAP_* bitmask                 */
    char       driver[32];    /* GPU driver name string                 */
} mvc3_get_info_resp_t;

/* ── MAP_RING ─────────────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    mvc3_hdr_t hdr;
    uint32_t   requested_size;
} mvc3_map_ring_req_t;

typedef struct __attribute__((packed)) {
    mvc3_hdr_t hdr;
    uint64_t   user_addr;     /* ring base in userspace VA; 0 = failed  */
    uint32_t   actual_size;
    uint32_t   slot_size;     /* sizeof(mvc3_ring_slot_t)               */
} mvc3_map_ring_resp_t;

/* ── Ring slot (zero-copy path) ─────────────────────────────────────── */

/*
 * Fixed-size 64-byte slot (one cache line).
 * Written directly into the mapped ring by userland.
 */
typedef struct __attribute__((packed, aligned(64))) {
    uint32_t op;          /* mvc3_draw_op_t                            */
    uint32_t _pad;

    union {
        /* MVC3_OP_FILL_RECT */
        struct {
            uint32_t x, y, w, h;
            uint32_t argb;
        } fill;

        /* MVC3_OP_BLIT */
        struct {
            uint32_t src_x, src_y;
            uint32_t dst_x, dst_y;
            uint32_t w, h;
        } blit;

        /* MVC3_OP_BLIT_BUF
         * handle is the USER VA of the off-screen buffer (32-bit on
         * ModuOS userland ABI, since user VA < 4 GiB). */
        struct {
            uint32_t handle;
            uint32_t src_x, src_y;
            uint32_t dst_x, dst_y;
            uint32_t w, h;
            uint32_t src_pitch;
            uint32_t src_fmt;
        } blit_buf;

        uint8_t _raw[56];   /* pad to 64 bytes total */
    } u;
} mvc3_ring_slot_t;

/* ── SUBMIT ──────────────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    mvc3_hdr_t hdr;
    uint32_t   count;
} mvc3_submit_t;

/* ── ENQUEUE ─────────────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    mvc3_hdr_t hdr;
    mvc3_ring_slot_t slot;
} mvc3_enqueue_t;

/* ── FLUSH ───────────────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    mvc3_hdr_t hdr;
    uint32_t   x, y, w, h;
} mvc3_flush_t;

/* ── ALLOC_BUF ───────────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    mvc3_hdr_t hdr;
    uint32_t   size_bytes;
    uint32_t   fmt;
} mvc3_alloc_buf_req_t;

typedef struct __attribute__((packed)) {
    mvc3_hdr_t hdr;
    /*
     * handle is the full 64-bit kernel VA of the allocation.
     * Must be 64-bit: kernel heap lives above 4 GiB (0xFFFF800000000000+)
     * and a 32-bit field would silently truncate it, breaking mvi0_mmap's
     * mapping-table lookup.  Userland never dereferences this value directly;
     * it passes it back in MAP_BUF and the kernel maps it into user VA.
     */
    uint64_t   handle;    /* 64-bit KVA; 0 = allocation failed          */
    uint32_t   pitch;
    uint32_t   _pad;      /* keep total size 8-byte aligned             */
} mvc3_alloc_buf_resp_t;

/* ── MAP_BUF ─────────────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    mvc3_hdr_t hdr;
    uint64_t   handle;    /* 64-bit KVA returned by ALLOC_BUF           */
} mvc3_map_buf_req_t;

typedef struct __attribute__((packed)) {
    mvc3_hdr_t hdr;
    uint64_t   user_addr; /* mapped user VA; 0 = failed                 */
    uint32_t   size_bytes;
    uint32_t   pitch;
    uint32_t   fmt;
    uint32_t   _pad;
} mvc3_map_buf_resp_t;

/* ── CURSOR_SET ──────────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    mvc3_hdr_t hdr;
    uint32_t   w, h;
    int32_t    hot_x, hot_y;
    /* uint32_t pixels[w*h] follows in the byte stream */
} mvc3_cursor_set_t;

/* ── CURSOR_MOVE ─────────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    mvc3_hdr_t hdr;
    int32_t    x, y;
} mvc3_cursor_move_t;

/* ── CURSOR_SHOW ─────────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    mvc3_hdr_t hdr;
    uint32_t   visible;
} mvc3_cursor_show_t;

/* ── mmap offset sentinels ───────────────────────────────────────────
 * Pass as `offset` to dev_mmap() / mvi0_mmap() to select which region
 * of the mvi0 device to map into the calling process's address space.
 *
 * MVC3_OFF_RING     — the command ring (allocated on MAP_RING).
 * MVC3_OFF_FB       — the live framebuffer (MMIO, physically contiguous).
 * MVC3_OFF_BUF_BASE — add the full 64-bit KVA handle from ALLOC_BUF to
 *                     get the per-buffer offset for dev_mmap().
 *                     e.g. offset = MVC3_OFF_BUF_BASE + resp.handle
 * ──────────────────────────────────────────────────────────────────── */
#define MVC3_OFF_RING     0x00000000ULL
#define MVC3_OFF_FB       0x10000000ULL
#define MVC3_OFF_BUF_BASE 0x20000000000ULL  /* 128 GiB — above any 32-bit addr,
                                               below typical kernel heap VA */