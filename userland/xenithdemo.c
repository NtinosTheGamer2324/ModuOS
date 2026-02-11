#include "libc.h"
#include "string.h"
#include "xenith26_proto.h"
#include "xenith26_shm.h"

/*
 * xenithdemo.sqr
 *
 * Simple Xenith26 client:
 *  - creates 2 windows
 *  - allocates 2 shared buffers (XRGB8888)
 *  - draws patterns into them
 *  - attaches buffers and presents
 */

static int send_msg(int fd, uint16_t type, const void *payload, uint32_t payload_len) {
    uint8_t buf[512];
    if (payload_len > (sizeof(buf) - sizeof(x26_hdr_t))) return -1;

    x26_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.magic = X26_MAGIC;
    h.type = type;
    h.size = (uint32_t)(sizeof(x26_hdr_t) + payload_len);

    memcpy(buf, &h, sizeof(h));
    if (payload_len) memcpy(buf + sizeof(h), payload, payload_len);

    return (write(fd, buf, h.size) == (ssize_t)h.size) ? 0 : -1;
}

static int recv_msg(int fd, x26_hdr_t *out_h, uint8_t *out_payload, uint32_t cap) {
    uint8_t buf[256];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0) return 0;
    if (n < (ssize_t)sizeof(x26_hdr_t)) return 0;

    x26_hdr_t *h = (x26_hdr_t*)buf;
    if (h->magic != X26_MAGIC) return 0;

    uint32_t payload_len = h->size - (uint32_t)sizeof(x26_hdr_t);
    if (payload_len > cap) payload_len = cap;

    if (out_h) *out_h = *h;
    if (out_payload && payload_len) memcpy(out_payload, buf + sizeof(x26_hdr_t), payload_len);

    return (int)payload_len + (int)sizeof(x26_hdr_t);
}

static void draw_pattern(uint32_t *p, uint32_t stride_px, uint32_t w, uint32_t h, uint32_t t, uint32_t seed) {
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t r = (uint8_t)(((x + (t >> 2) + seed) * 255u) / (w ? w : 1));
            uint8_t g = (uint8_t)(((y + (t >> 3) + seed) * 255u) / (h ? h : 1));
            uint8_t b = (uint8_t)((x ^ y ^ seed ^ (t >> 1)) & 0xFF);
            p[y * stride_px + x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    int fd = open("$/dev/gui0", O_RDWR | O_NONBLOCK, 0);
    if (fd < 0) {
        puts_raw("xenithdemo: cannot open $/dev/gui0\n");
        return 1;
    }

    /* Create two windows */
    x26_create_window_t cw;
    memset(&cw, 0, sizeof(cw));
    cw.w = 256;
    cw.h = 256;
    cw.fmt = 1;

    (void)send_msg(fd, X26_MSG_CREATE_WINDOW, &cw, sizeof(cw));
    (void)send_msg(fd, X26_MSG_CREATE_WINDOW, &cw, sizeof(cw));

    uint32_t win_ids[2] = {0,0};
    uint32_t got = 0;

    uint8_t payload[64];
    uint64_t start = time_ms();
    while (got < 2 && (time_ms() - start) < 2000) {
        x26_hdr_t h;
        int rn = recv_msg(fd, &h, payload, sizeof(payload));
        if (!rn) { yield(); continue; }

        if (h.type == X26_MSG_WINDOW_CREATED) {
            x26_window_created_t *wc = (x26_window_created_t*)payload;
            if (got < 2) win_ids[got++] = wc->id;
        }
    }

    if (got < 2) {
        puts_raw("xenithdemo: failed to get window ids\n");
        return 2;
    }

    /* Allocate buffers */
    x26_shm_create_req_t b0;
    memset(&b0, 0, sizeof(b0));
    b0.w = 256; b0.h = 256; b0.fmt = 1;
    b0.preferred_addr = X26_SHM_BASE;
    if (x26_shm_create_u(&b0) != 0) return 3;

    x26_shm_create_req_t b1;
    memset(&b1, 0, sizeof(b1));
    b1.w = 256; b1.h = 256; b1.fmt = 1;
    b1.preferred_addr = X26_SHM_BASE + 0x01000000ULL;
    if (x26_shm_create_u(&b1) != 0) return 4;

    uint32_t *p0 = (uint32_t *)(uintptr_t)b0.mapped_addr;
    uint32_t *p1 = (uint32_t *)(uintptr_t)b1.mapped_addr;
    uint32_t stride0 = b0.stride / 4u;
    uint32_t stride1 = b1.stride / 4u;

    /* Attach buffers */
    x26_attach_buffer_t ab;

    ab.id = win_ids[0];
    ab.buf_id = b0.buf_id;
    (void)send_msg(fd, X26_MSG_ATTACH_BUFFER, &ab, sizeof(ab));

    ab.id = win_ids[1];
    ab.buf_id = b1.buf_id;
    (void)send_msg(fd, X26_MSG_ATTACH_BUFFER, &ab, sizeof(ab));

    /* Initial placement (WM may override) */
    x26_map_window_t mw;
    mw.id = win_ids[0]; mw.x = 100; mw.y = 120;
    (void)send_msg(fd, X26_MSG_MAP_WINDOW, &mw, sizeof(mw));
    mw.id = win_ids[1]; mw.x = 220; mw.y = 180;
    (void)send_msg(fd, X26_MSG_MAP_WINDOW, &mw, sizeof(mw));

    puts_raw("xenithdemo: running (2 windows). Use xenithwm to drag.\n");

    for (;;) {
        uint32_t t = (uint32_t)time_ms();
        draw_pattern(p0, stride0, 256, 256, t, 1);
        draw_pattern(p1, stride1, 256, 256, t, 77);

        x26_present_t pr;
        pr.id = win_ids[0]; pr.x = 0; pr.y = 0; pr.w = 256; pr.h = 256;
        (void)send_msg(fd, X26_MSG_PRESENT, &pr, sizeof(pr));
        pr.id = win_ids[1];
        (void)send_msg(fd, X26_MSG_PRESENT, &pr, sizeof(pr));

        yield();
    }
}
