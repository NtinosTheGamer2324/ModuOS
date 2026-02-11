#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
} gfx_mode_t;

#ifdef __cplusplus
}
#endif
