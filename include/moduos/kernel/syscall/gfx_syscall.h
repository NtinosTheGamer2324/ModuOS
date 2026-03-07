#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GPU capability query syscall */
uint32_t sys_gfx_get_caps(void);

/* GPU info syscall */
typedef struct {
    char driver_name[64];
    uint32_t caps;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
} gfx_info_t;

int sys_gfx_get_info(gfx_info_t *out);

/* 3D triangle rendering syscall (software or hardware) */
int sys_gfx_draw_triangle(int32_t x0, int32_t y0, uint32_t color0,
                          int32_t x1, int32_t y1, uint32_t color1,
                          int32_t x2, int32_t y2, uint32_t color2);

#ifdef __cplusplus
}
#endif
