#include "libc.h"
#include "NodGL.h"

/*
 * gfxtest.sqr
 *
 * Graphics test using NodGL API
 */

static uint32_t pack_argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

int md_main(long argc, char **argv) {
    (void)argc;
    (void)argv;

    puts_raw("gfxtest - NodGL graphics test\n");

    NodGL_Device device;
    NodGL_Context ctx;
    if (NodGL_CreateDevice(NodGL_FEATURE_LEVEL_1_0, &device, &ctx, NULL) != NodGL_OK) {
        puts_raw("gfxtest: NodGL_CreateDevice failed\n");
        return 1;
    }

    puts_raw("Drawing test pattern with NodGL...\n");

    NodGL_ClearContext(ctx, NodGL_CLEAR_COLOR, pack_argb(255, 0, 0, 64), 1.0f, 0);

    NodGL_FillRectContext(ctx, 10, 10, 80, 60, pack_argb(255, 255, 0, 0));
    NodGL_FillRectContext(ctx, 100, 10, 80, 60, pack_argb(255, 0, 255, 0));
    NodGL_FillRectContext(ctx, 190, 10, 60, 60, pack_argb(255, 0, 0, 255));

    NodGL_DrawLineContext(ctx, 0, 100, 300, 100, pack_argb(255, 255, 255, 0), 1);
    NodGL_DrawLineContext(ctx, 150, 0, 150, 200, pack_argb(255, 255, 0, 255), 1);

    for (int y = 120; y < 200; y += 2) {
        uint8_t r = (uint8_t)((y - 120) * 255 / 80);
        NodGL_FillRectContext(ctx, 10, y, 280, 2, pack_argb(255, r, 128, 255 - r));
    }

    NodGL_PresentContext(ctx, 0);

    puts_raw("gfxtest finished.\n");
    NodGL_ReleaseDevice(device);
    return 0;
}
