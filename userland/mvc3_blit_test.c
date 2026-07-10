/*
 * mvc3_blit_test.c — MVC3 blit test application
 *
 * Tests all three blit paths provided by the MVC3 video controller:
 *
 *   1. MVC3_OP_FILL_RECT  — baseline fill (sanity check)
 *   2. MVC3_OP_BLIT       — screen-to-screen blit (copy a filled region)
 *   3. MVC3_OP_BLIT_BUF   — off-screen buffer → screen blit
 *        a. Copy-batch path  (ENQUEUE over write())
 *        b. Zero-copy path   (MAP_RING + SUBMIT, if mmap available)
 *
 * Build (example):
 *   moduos-cc mvc3_blit_test.c -o mvc3_blit_test
 *
 * Copyright © 2025-2026 New Technologies Software — GPL v2.0
 */

#include "libc.h"
#include "mvc3.h"

/* ── Helpers ──────────────────────────────────────────────────────── */

#define MVI0_PATH "$/dev/mvc/mvi0"

/* ARGB colour constants */
#define COL_RED    0xFFFF0000u
#define COL_GREEN  0xFF00FF00u
#define COL_BLUE   0xFF0000FFu
#define COL_YELLOW 0xFFFFFF00u
#define COL_CYAN   0xFF00FFFFu
#define COL_WHITE  0xFFFFFFFFu
#define COL_BLACK  0xFF000000u

/* Simple pass/fail counters */
static int g_pass = 0;
static int g_fail = 0;

static void test_pass(const char *name) {
    printf("[PASS] %s\n", name);
    g_pass++;
}

static void test_fail(const char *name, const char *reason) {
    printf("[FAIL] %s — %s\n", name, reason);
    g_fail++;
}

/* ── Protocol helpers ─────────────────────────────────────────────── */

/*
 * Send a raw packet to the device and, if resp_buf is non-NULL,
 * read back a response of exactly resp_size bytes.
 * Returns 0 on success, -1 on error.
 */
static int mvc3_transact(int fd,
                         const void *req, uint32_t req_size,
                         void *resp_buf, uint32_t resp_size) {
    ssize_t w = write(fd, req, req_size);
    if (w != (ssize_t)req_size) return -1;

    if (resp_buf && resp_size > 0) {
        ssize_t r = read(fd, resp_buf, resp_size);
        if (r != (ssize_t)resp_size) return -1;
        mvc3_hdr_t *h = (mvc3_hdr_t *)resp_buf;
        if (h->magic != MVC3_MAGIC) return -1;
    }
    return 0;
}

/* ── Individual tests ─────────────────────────────────────────────── */

/*
 * Test 1: GET_INFO
 * Verify the device responds with a valid header and non-zero dimensions.
 */
static void test_get_info(int fd) {
    mvc3_get_info_req_t req;
    mvc3_get_info_resp_t resp;

    req.hdr.magic       = MVC3_MAGIC;
    req.hdr.abi_version = MVC3_ABI_VERSION;
    req.hdr.cmd         = MVC3_CMD_GET_INFO;
    req.hdr.size_bytes  = sizeof(req);

    if (mvc3_transact(fd, &req, sizeof(req), &resp, sizeof(resp)) != 0) {
        test_fail("GET_INFO", "transact failed");
        return;
    }

    if (resp.hdr.cmd != MVC3_CMD_GET_INFO) {
        test_fail("GET_INFO", "wrong cmd in response");
        return;
    }
    if (resp.width == 0 || resp.height == 0) {
        test_fail("GET_INFO", "zero-sized framebuffer reported");
        return;
    }

    printf("  fb: %ux%u %ubpp pitch=%u driver=%s caps=0x%x\n",
           resp.width, resp.height, resp.bpp, resp.pitch,
           resp.driver, resp.caps);
    test_pass("GET_INFO");
}

/*
 * Test 2: FILL_RECT via copy-batch (ENQUEUE)
 * Draws a series of coloured rectangles in the top-left corner.
 */
static void test_fill_rect_enqueue(int fd) {
    struct { uint32_t x, y, w, h; uint32_t argb; } fills[] = {
        {  0,   0, 100, 100, COL_RED    },
        {100,   0, 100, 100, COL_GREEN  },
        {200,   0, 100, 100, COL_BLUE   },
        {300,   0, 100, 100, COL_YELLOW },
    };

    int ok = 1;
    for (int i = 0; i < 4; i++) {
        mvc3_enqueue_t pkt;
        pkt.hdr.magic       = MVC3_MAGIC;
        pkt.hdr.abi_version = MVC3_ABI_VERSION;
        pkt.hdr.cmd         = MVC3_CMD_ENQUEUE;
        pkt.hdr.size_bytes  = sizeof(pkt);
        pkt.slot.op         = MVC3_OP_FILL_RECT;
        pkt.slot._pad       = 0;
        pkt.slot.u.fill.x   = fills[i].x;
        pkt.slot.u.fill.y   = fills[i].y;
        pkt.slot.u.fill.w   = fills[i].w;
        pkt.slot.u.fill.h   = fills[i].h;
        pkt.slot.u.fill.argb= fills[i].argb;

        if (write(fd, &pkt, sizeof(pkt)) != (ssize_t)sizeof(pkt)) {
            ok = 0;
            break;
        }
    }

    /* Flush the region */
    mvc3_flush_t flush;
    flush.hdr.magic       = MVC3_MAGIC;
    flush.hdr.abi_version = MVC3_ABI_VERSION;
    flush.hdr.cmd         = MVC3_CMD_FLUSH;
    flush.hdr.size_bytes  = sizeof(flush);
    flush.x = 0; flush.y = 0; flush.w = 400; flush.h = 100;
    write(fd, &flush, sizeof(flush));

    if (ok) test_pass("FILL_RECT (ENQUEUE copy-batch)");
    else    test_fail("FILL_RECT (ENQUEUE copy-batch)", "write failed");
}

/*
 * Test 3: BLIT (screen-to-screen)
 * Draws a magenta square at (0,200), then blits a copy to (200,200).
 * Expected result: two magenta/pink squares side by side. This is correct.
 */
static void test_blit_screen_to_screen(int fd) {
    /* First draw a known source: magenta square at (0,200) */
    {
        mvc3_enqueue_t pkt;
        pkt.hdr.magic       = MVC3_MAGIC;
        pkt.hdr.abi_version = MVC3_ABI_VERSION;
        pkt.hdr.cmd         = MVC3_CMD_ENQUEUE;
        pkt.hdr.size_bytes  = sizeof(pkt);
        pkt.slot.op              = MVC3_OP_FILL_RECT;
        pkt.slot._pad            = 0;
        pkt.slot.u.fill.x        = 0;
        pkt.slot.u.fill.y        = 200;
        pkt.slot.u.fill.w        = 120;
        pkt.slot.u.fill.h        = 120;
        pkt.slot.u.fill.argb     = 0xFFFF00FFu; /* magenta */
        write(fd, &pkt, sizeof(pkt));
    }

    /* Now BLIT it to (200, 200) */
    {
        mvc3_enqueue_t pkt;
        pkt.hdr.magic       = MVC3_MAGIC;
        pkt.hdr.abi_version = MVC3_ABI_VERSION;
        pkt.hdr.cmd         = MVC3_CMD_ENQUEUE;
        pkt.hdr.size_bytes  = sizeof(pkt);
        pkt.slot.op              = MVC3_OP_BLIT;
        pkt.slot._pad            = 0;
        pkt.slot.u.blit.src_x    = 0;
        pkt.slot.u.blit.src_y    = 200;
        pkt.slot.u.blit.dst_x    = 200;
        pkt.slot.u.blit.dst_y    = 200;
        pkt.slot.u.blit.w        = 120;
        pkt.slot.u.blit.h        = 120;
        if (write(fd, &pkt, sizeof(pkt)) != (ssize_t)sizeof(pkt)) {
            test_fail("BLIT screen-to-screen", "write failed");
            return;
        }
    }

    /* Flush both source and dest */
    mvc3_flush_t flush;
    flush.hdr.magic       = MVC3_MAGIC;
    flush.hdr.abi_version = MVC3_ABI_VERSION;
    flush.hdr.cmd         = MVC3_CMD_FLUSH;
    flush.hdr.size_bytes  = sizeof(flush);
    flush.x = 0; flush.y = 200; flush.w = 400; flush.h = 120;
    write(fd, &flush, sizeof(flush));

    test_pass("BLIT screen-to-screen (MVC3_OP_BLIT)");
}

/*
 * Test 4: BLIT_BUF via copy-batch (ENQUEUE)
 * Allocates a local userland buffer, fills it with a checkerboard pattern,
 * then blits it to screen via ENQUEUE (usercopy path).
 */
static void test_blit_buf_copybatch(int fd) {
    uint32_t BUF_W = 128, BUF_H = 128;
    uint32_t src_pitch = BUF_W * 4u;
    uint32_t buf_bytes = src_pitch * BUF_H;

    uint32_t *local_buf = (uint32_t *)malloc(buf_bytes);
    if (!local_buf) {
        test_fail("BLIT_BUF copy-batch", "malloc failed");
        return;
    }

    /* Checkerboard: 8×8 pixel squares, cyan/white */
    for (uint32_t y = 0; y < BUF_H; y++) {
        for (uint32_t x = 0; x < BUF_W; x++) {
            int cell = ((x / 8) + (y / 8)) & 1;
            local_buf[y * BUF_W + x] = cell ? COL_CYAN : COL_WHITE;
        }
    }

    mvc3_enqueue_t pkt;
    pkt.hdr.magic       = MVC3_MAGIC;
    pkt.hdr.abi_version = MVC3_ABI_VERSION;
    pkt.hdr.cmd         = MVC3_CMD_ENQUEUE;
    pkt.hdr.size_bytes  = sizeof(pkt);
    pkt.slot.op                    = MVC3_OP_BLIT_BUF;
    pkt.slot._pad                  = 0;
    /*
     * FIX: cast to uint64_t, not uint32_t.
     * blit_buf.handle is now a uint64_t field — the full pointer fits.
     */
    pkt.slot.u.blit_buf.handle     = (uint64_t)(uintptr_t)local_buf;
    pkt.slot.u.blit_buf.src_x      = 0;
    pkt.slot.u.blit_buf.src_y      = 0;
    pkt.slot.u.blit_buf.dst_x      = 0;
    pkt.slot.u.blit_buf.dst_y      = 400;
    pkt.slot.u.blit_buf.w          = BUF_W;
    pkt.slot.u.blit_buf.h          = BUF_H;
    pkt.slot.u.blit_buf.src_pitch  = src_pitch;
    pkt.slot.u.blit_buf.src_fmt    = 0;

    if (write(fd, &pkt, sizeof(pkt)) != (ssize_t)sizeof(pkt)) {
        free(local_buf);
        test_fail("BLIT_BUF copy-batch", "write(ENQUEUE) failed");
        return;
    }

    mvc3_flush_t flush;
    flush.hdr.magic       = MVC3_MAGIC;
    flush.hdr.abi_version = MVC3_ABI_VERSION;
    flush.hdr.cmd         = MVC3_CMD_FLUSH;
    flush.hdr.size_bytes  = sizeof(flush);
    flush.x = 0; flush.y = 400; flush.w = BUF_W; flush.h = BUF_H;
    write(fd, &flush, sizeof(flush));

    free(local_buf);
    test_pass("BLIT_BUF copy-batch (MVC3_OP_BLIT_BUF via ENQUEUE)");
}

/*
 * Test 5: BLIT_BUF via zero-copy ring (MAP_RING + SUBMIT)
 * Maps the ring into userspace, writes a slot directly, then submits.
 * Falls back gracefully if mmap is unavailable.
 * DEV_MMAP STUFF IS **BROKEN** THIS WILL PRODUCE A PAGE FAULT
 */
static void test_blit_buf_zerocopy(int fd) {
    /* --- Allocate & map an off-screen buffer --- */
    uint32_t BUF_W = 96, BUF_H = 96;
    uint32_t src_pitch = BUF_W * 4u;
    uint32_t buf_bytes = src_pitch * BUF_H;

    mvc3_alloc_buf_req_t alloc_req;
    alloc_req.hdr.magic       = MVC3_MAGIC;
    alloc_req.hdr.abi_version = MVC3_ABI_VERSION;
    alloc_req.hdr.cmd         = MVC3_CMD_ALLOC_BUF;
    alloc_req.hdr.size_bytes  = sizeof(alloc_req);
    alloc_req.size_bytes      = buf_bytes;
    alloc_req.fmt             = 0;

    mvc3_alloc_buf_resp_t alloc_resp;
    if (mvc3_transact(fd, &alloc_req, sizeof(alloc_req),
                      &alloc_resp, sizeof(alloc_resp)) != 0) {
        test_fail("BLIT_BUF zero-copy", "ALLOC_BUF transact failed");
        return;
    }
    if (alloc_resp.handle == 0) {
        test_fail("BLIT_BUF zero-copy", "ALLOC_BUF returned null handle");
        return;
    }

    /* MAP_BUF to get user VA */
    mvc3_map_buf_req_t map_req;
    map_req.hdr.magic       = MVC3_MAGIC;
    map_req.hdr.abi_version = MVC3_ABI_VERSION;
    map_req.hdr.cmd         = MVC3_CMD_MAP_BUF;
    map_req.hdr.size_bytes  = sizeof(map_req);
    map_req.handle          = alloc_resp.handle;

    mvc3_map_buf_resp_t map_resp;
    if (mvc3_transact(fd, &map_req, sizeof(map_req),
                      &map_resp, sizeof(map_resp)) != 0) {
        test_fail("BLIT_BUF zero-copy", "MAP_BUF transact failed");
        return;
    }
    if (map_resp.user_addr == 0) {
        test_fail("BLIT_BUF zero-copy", "MAP_BUF returned null user_addr");
        return;
    }

    /* Fill the mapped buffer with vertical gradient: black → blue */
    uint32_t *pixels = (uint32_t *)(uintptr_t)map_resp.user_addr;
    for (uint32_t y = 0; y < BUF_H; y++) {
        uint8_t b = (uint8_t)(255u * y / (BUF_H - 1));
        uint32_t col = 0xFF000000u | (uint32_t)b;
        for (uint32_t x = 0; x < BUF_W; x++) {
            pixels[y * BUF_W + x] = col;
        }
    }

    /* --- Map the ring --- */
    mvc3_map_ring_req_t ring_req;
    ring_req.hdr.magic       = MVC3_MAGIC;
    ring_req.hdr.abi_version = MVC3_ABI_VERSION;
    ring_req.hdr.cmd         = MVC3_CMD_MAP_RING;
    ring_req.hdr.size_bytes  = sizeof(ring_req);
    ring_req.requested_size  = 0; /* use kernel default */

    mvc3_map_ring_resp_t ring_resp;
    if (mvc3_transact(fd, &ring_req, sizeof(ring_req),
                      &ring_resp, sizeof(ring_resp)) != 0) {
        test_fail("BLIT_BUF zero-copy", "MAP_RING transact failed");
        return;
    }

    if (ring_resp.user_addr == 0) {
        printf("  MAP_RING: mmap unavailable, falling back to ENQUEUE\n");

        mvc3_enqueue_t pkt;
        pkt.hdr.magic       = MVC3_MAGIC;
        pkt.hdr.abi_version = MVC3_ABI_VERSION;
        pkt.hdr.cmd         = MVC3_CMD_ENQUEUE;
        pkt.hdr.size_bytes  = sizeof(pkt);
        pkt.slot.op                   = MVC3_OP_BLIT_BUF;
        pkt.slot._pad                 = 0;
        pkt.slot.u.blit_buf.handle    = map_resp.user_addr; /* already uint64_t */
        pkt.slot.u.blit_buf.src_x     = 0;
        pkt.slot.u.blit_buf.src_y     = 0;
        pkt.slot.u.blit_buf.dst_x     = 200;
        pkt.slot.u.blit_buf.dst_y     = 400;
        pkt.slot.u.blit_buf.w         = BUF_W;
        pkt.slot.u.blit_buf.h         = BUF_H;
        pkt.slot.u.blit_buf.src_pitch = src_pitch;
        pkt.slot.u.blit_buf.src_fmt   = 0;
        write(fd, &pkt, sizeof(pkt));

        mvc3_flush_t flush;
        flush.hdr.magic       = MVC3_MAGIC;
        flush.hdr.abi_version = MVC3_ABI_VERSION;
        flush.hdr.cmd         = MVC3_CMD_FLUSH;
        flush.hdr.size_bytes  = sizeof(flush);
        flush.x = 200; flush.y = 400; flush.w = BUF_W; flush.h = BUF_H;
        write(fd, &flush, sizeof(flush));

        test_pass("BLIT_BUF zero-copy (fallback to ENQUEUE — no mmap)");
        return;
    }

    printf("  MAP_RING: ring mapped at 0x%llx, %u bytes, slot_size=%u\n",
           (unsigned long long)ring_resp.user_addr,
           ring_resp.actual_size,
           ring_resp.slot_size);

    mvc3_ring_slot_t *ring = (mvc3_ring_slot_t *)(uintptr_t)ring_resp.user_addr;

    ring[0].op                    = MVC3_OP_BLIT_BUF;
    ring[0]._pad                  = 0;
    ring[0].u.blit_buf.handle     = map_resp.user_addr; /* uint64_t — no truncation */
    ring[0].u.blit_buf.src_x      = 0;
    ring[0].u.blit_buf.src_y      = 0;
    ring[0].u.blit_buf.dst_x      = 200;
    ring[0].u.blit_buf.dst_y      = 400;
    ring[0].u.blit_buf.w          = BUF_W;
    ring[0].u.blit_buf.h          = BUF_H;
    ring[0].u.blit_buf.src_pitch  = src_pitch;
    ring[0].u.blit_buf.src_fmt    = 0;

    mvc3_submit_t submit;
    submit.hdr.magic       = MVC3_MAGIC;
    submit.hdr.abi_version = MVC3_ABI_VERSION;
    submit.hdr.cmd         = MVC3_CMD_SUBMIT;
    submit.hdr.size_bytes  = sizeof(submit);
    submit.count           = 1;

    if (write(fd, &submit, sizeof(submit)) != (ssize_t)sizeof(submit)) {
        test_fail("BLIT_BUF zero-copy", "SUBMIT write failed");
        return;
    }

    mvc3_flush_t flush;
    flush.hdr.magic       = MVC3_MAGIC;
    flush.hdr.abi_version = MVC3_ABI_VERSION;
    flush.hdr.cmd         = MVC3_CMD_FLUSH;
    flush.hdr.size_bytes  = sizeof(flush);
    flush.x = 200; flush.y = 400; flush.w = BUF_W; flush.h = BUF_H;
    write(fd, &flush, sizeof(flush));

    test_pass("BLIT_BUF zero-copy (MAP_RING + SUBMIT)");
}

/*
 * Test 6: Stress — many BLIT_BUF slots in one ring batch
 * Queues 8 different coloured stripes via ENQUEUE in one batch.
 *
 * FIX: same as test 4 — handle is now cast to uint64_t.
 */
static void test_blit_buf_batch(int fd) {
    uint32_t BUF_W = 64, BUF_H = 32;
    uint32_t src_pitch = BUF_W * 4u;
    uint32_t buf_bytes = src_pitch * BUF_H;

    uint32_t stripe_cols[] = {
        COL_RED, COL_GREEN, COL_BLUE, COL_YELLOW,
        COL_CYAN, COL_WHITE, 0xFFFF8000u, 0xFF8000FFu,
    };
    int n_stripes = 8;

    int ok = 1;
    for (int i = 0; i < n_stripes; i++) {
        uint32_t *buf = (uint32_t *)malloc(buf_bytes);
        if (!buf) { ok = 0; break; }

        for (uint32_t p = 0; p < BUF_W * BUF_H; p++)
            buf[p] = stripe_cols[i];

        mvc3_enqueue_t pkt;
        pkt.hdr.magic       = MVC3_MAGIC;
        pkt.hdr.abi_version = MVC3_ABI_VERSION;
        pkt.hdr.cmd         = MVC3_CMD_ENQUEUE;
        pkt.hdr.size_bytes  = sizeof(pkt);
        pkt.slot.op                    = MVC3_OP_BLIT_BUF;
        pkt.slot._pad                  = 0;
        /* FIX: uint64_t cast preserves the full pointer */
        pkt.slot.u.blit_buf.handle     = (uint64_t)(uintptr_t)buf;
        pkt.slot.u.blit_buf.src_x      = 0;
        pkt.slot.u.blit_buf.src_y      = 0;
        pkt.slot.u.blit_buf.dst_x      = (uint32_t)(i * BUF_W);
        pkt.slot.u.blit_buf.dst_y      = 600;
        pkt.slot.u.blit_buf.w          = BUF_W;
        pkt.slot.u.blit_buf.h          = BUF_H;
        pkt.slot.u.blit_buf.src_pitch  = src_pitch;
        pkt.slot.u.blit_buf.src_fmt    = 0;

        if (write(fd, &pkt, sizeof(pkt)) != (ssize_t)sizeof(pkt))
            ok = 0;

        free(buf);
        if (!ok) break;
    }

    mvc3_flush_t flush;
    flush.hdr.magic       = MVC3_MAGIC;
    flush.hdr.abi_version = MVC3_ABI_VERSION;
    flush.hdr.cmd         = MVC3_CMD_FLUSH;
    flush.hdr.size_bytes  = sizeof(flush);
    flush.x = 0; flush.y = 600; flush.w = (uint32_t)(n_stripes * BUF_W); flush.h = BUF_H;
    write(fd, &flush, sizeof(flush));

    if (ok) test_pass("BLIT_BUF batch (8 stripes via ENQUEUE)");
    else    test_fail("BLIT_BUF batch", "write failed mid-batch");
}

/*
 * Test 7: CURSOR_MOVE + CURSOR_SHOW
 * Just validates the kernel doesn't crash on cursor commands.
 * NOTE: No visible result is expected unless CURSOR_SET has been called
 * first to upload a bitmap.  This test only checks that the kernel
 * accepts the packets without faulting.
 */
static void test_cursor(int fd) {
    mvc3_cursor_move_t mv;
    mv.hdr.magic       = MVC3_MAGIC;
    mv.hdr.abi_version = MVC3_ABI_VERSION;
    mv.hdr.cmd         = MVC3_CMD_CURSOR_MOVE;
    mv.hdr.size_bytes  = sizeof(mv);
    mv.x = 320; mv.y = 240;
    if (write(fd, &mv, sizeof(mv)) != (ssize_t)sizeof(mv)) {
        test_fail("CURSOR_MOVE", "write failed");
        return;
    }

    mvc3_cursor_show_t sh;
    sh.hdr.magic       = MVC3_MAGIC;
    sh.hdr.abi_version = MVC3_ABI_VERSION;
    sh.hdr.cmd         = MVC3_CMD_CURSOR_SHOW;
    sh.hdr.size_bytes  = sizeof(sh);
    sh.visible = 1;
    if (write(fd, &sh, sizeof(sh)) != (ssize_t)sizeof(sh)) {
        test_fail("CURSOR_SHOW", "write failed");
        return;
    }

    test_pass("CURSOR (move + show — no bitmap set, no visual expected)");
}

/*
 * Test 0: NUKE — fills the entire screen red for 3 seconds.
 * If you see red, the fb pipeline works and the terminal is just overdrawing.
 * If nothing happens, something is broken upstream of sw_blit_buffer.
 */
static void test_nuke_screen(int fd) {
    mvc3_enqueue_t pkt;
    pkt.hdr.magic        = MVC3_MAGIC;
    pkt.hdr.abi_version  = MVC3_ABI_VERSION;
    pkt.hdr.cmd          = MVC3_CMD_ENQUEUE;
    pkt.hdr.size_bytes   = sizeof(pkt);
    pkt.slot.op          = MVC3_OP_FILL_RECT;
    pkt.slot._pad        = 0;
    pkt.slot.u.fill.x    = 25;
    pkt.slot.u.fill.y    = 25;
    pkt.slot.u.fill.w    = 9999;
    pkt.slot.u.fill.h    = 9999;
    pkt.slot.u.fill.argb = COL_RED;

    if (write(fd, &pkt, sizeof(pkt)) != (ssize_t)sizeof(pkt)) {
        test_fail("NUKE screen", "write failed");
        return;
    }

    mvc3_flush_t flush;
    flush.hdr.magic       = MVC3_MAGIC;
    flush.hdr.abi_version = MVC3_ABI_VERSION;
    flush.hdr.cmd         = MVC3_CMD_FLUSH;
    flush.hdr.size_bytes  = sizeof(flush);
    flush.x = 0; flush.y = 0; flush.w = 9999; flush.h = 9999;
    write(fd, &flush, sizeof(flush));

    printf("  Screen should be RED for 3 seconds...\n");
    //sleep(3);
    test_pass("NUKE screen (if you saw red, fb pipeline works)");
}

/* ── Entry point ──────────────────────────────────────────────────── */

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    printf("=== MVC3 Blit Test Suite ===\n");
    printf("Opening %s ...\n", MVI0_PATH);

    int fd = open(MVI0_PATH, O_RDWR, 0);
    if (fd < 0) {
        printf("ERROR: could not open %s (fd=%d)\n", MVI0_PATH, fd);
        return 1;
    }
    printf("Device opened (fd=%d)\n\n", fd);

    printf("sizeof mvc3_ring_slot_t = %u\n", (unsigned)sizeof(mvc3_ring_slot_t));
    printf("sizeof mvc3_enqueue_t   = %u\n", (unsigned)sizeof(mvc3_enqueue_t));
    printf("sizeof mvc3_hdr_t       = %u\n", (unsigned)sizeof(mvc3_hdr_t));

    //test_get_info(fd);
    //test_fill_rect_enqueue(fd);
    //test_blit_screen_to_screen(fd);
    test_nuke_screen(fd);
    //test_blit_buf_copybatch(fd);
    /* test_blit_buf_zerocopy(fd); -- DEV_MMAP is broken, skip */
    //test_blit_buf_batch(fd);
    //test_cursor(fd);

    close(fd);

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);

    if (g_fail > 0) {
        printf("SOME TESTS FAILED.\n");
        return 1;
    }

    printf("All tests passed!\n");
    return 0;
}