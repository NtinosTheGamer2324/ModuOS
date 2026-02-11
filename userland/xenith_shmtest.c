#include "libc.h"
#include "string.h"
#include "xenith26_shm.h"

/*
 * xenith_shmtest.sqr
 *
 * Basic test for Xenith26 shared buffer syscalls:
 *  - create a 256x256 XRGB8888 buffer
 *  - map it at a fixed address
 *  - write a gradient pattern
 */

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    x26_shm_create_req_t req;
    memset(&req, 0, sizeof(req));
    req.w = 256;
    req.h = 256;
    req.fmt = 1; /* MD64API_GRP_FMT_XRGB8888 */
    req.preferred_addr = X26_SHM_BASE;

    int rc = x26_shm_create_u(&req);
    if (rc != 0) {
        puts_raw("xenith_shmtest: create failed\n");
        return 1;
    }

    puts_raw("xenith_shmtest: created buf_id=");
    char tmp[16];
    itoa((int)req.buf_id, tmp, 10);
    puts_raw(tmp);
    puts_raw("\n");

    uint32_t *p = (uint32_t *)(uintptr_t)req.mapped_addr;
    uint32_t w = req.w, h = req.h;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t r = (uint8_t)((x * 255u) / (w ? w : 1));
            uint8_t g = (uint8_t)((y * 255u) / (h ? h : 1));
            uint8_t b = (uint8_t)((x ^ y) & 0xFF);
            p[y * (req.stride / 4u) + x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }

    puts_raw("xenith_shmtest: wrote pattern\n");
    return 0;
}
