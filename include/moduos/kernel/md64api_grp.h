#ifndef MODUOS_KERNEL_MD64API_GRP_H
#define MODUOS_KERNEL_MD64API_GRP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MD64API Graphics (GRP)
 *
 * This is a small, file-based API that targets devices under:
 *   $/dev/graphics/
 *
 * The canonical primary device is:
 *   $/dev/graphics/video0
 */

#define MD64API_GRP_DEFAULT_DEVICE "$/dev/graphics/video0"

typedef enum {
    MD64API_GRP_MODE_TEXT     = 0,
    MD64API_GRP_MODE_GRAPHICS = 1,
} md64api_grp_mode_t;

typedef enum {
    MD64API_GRP_FMT_UNKNOWN   = 0,
    MD64API_GRP_FMT_RGB565    = 1,
    MD64API_GRP_FMT_XRGB8888  = 2,
} md64api_grp_format_t;

/* Binary payload returned by reading from $/dev/graphics/video0 */
typedef struct __attribute__((packed)) {
    uint64_t fb_addr;     /* virtual address of linear framebuffer (0 in text mode) */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t  bpp;
    uint8_t  mode;        /* md64api_grp_mode_t */
    uint8_t  fmt;         /* md64api_grp_format_t */
    uint8_t  reserved;
} md64api_grp_video_info_t;

#ifdef __cplusplus
}
#endif

#endif
