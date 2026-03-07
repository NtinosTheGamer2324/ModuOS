#include "libc.h"
#include "NodGL.h"

#define LOGO_W 64
#define LOGO_H 32

typedef struct {
    int x, y;
} DVDPOS;

int dx = 4; // Velocity
int dy = 4;
uint32_t color = 0xFFFFFF00;

uint32_t get_rand_color() {
    return 0xFF000000 | (rand() % 0xFFFFFF); 
}

void dvd_update(DVDPOS *pos, uint32_t sw, uint32_t sh) {
    pos->x += dx;
    pos->y += dy;

    int hit = 0;
    if (pos->x <= 0 || pos->x + LOGO_W >= (int)sw) {
        dx *= -1;
        hit = 1;
    }
    if (pos->y <= 0 || pos->y + LOGO_H >= (int)sh) {
        dy *= -1;
        hit = 1;
    }

    if (hit) color = get_rand_color();
}

int md_main(long argc, char **argv) {
    NodGL_Device device;
    NodGL_Context ctx;
    NodGL_FeatureLevel actual;
    uint32_t sw, sh;

    if (NodGL_CreateDevice(NodGL_FEATURE_LEVEL_2_0, &device, &ctx, &actual) != NodGL_OK) return 1;
    NodGL_GetScreenResolution(device, &sw, &sh);

    DVDPOS pos = {100, 100};

    while (1) {
        dvd_update(&pos, sw, sh);

        NodGL_ClearContext(ctx, NodGL_CLEAR_COLOR, 0xFF000000, 1.0f, 0);

        NodGL_FillRectContext(ctx, pos.x, pos.y, LOGO_W, LOGO_H, color);

        NodGL_PresentContext(ctx, 1);
    }
    return 0;
}