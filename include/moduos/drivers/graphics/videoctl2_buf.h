#pragma once

#include <stdint.h>
#include "videoctl.h"

/* Buffer allocation / mapping commands (v2)
 * These are synchronous control commands (not enqueued).
 */

typedef struct {
    videoctl2_hdr_t hdr;   /* cmd = VIDEOCTL_CMD2_ALLOC_BUF */
    uint32_t size_bytes;   /* requested size */
    uint32_t fmt;          /* MD64API_GRP_FMT_XRGB8888 or MD64API_GRP_FMT_RGB565 */

    /* out */
    uint32_t handle;       /* allocated handle (starts at 1) */
    uint32_t pitch;        /* bytes per row for the buffer */
    uint32_t reserved;
} videoctl2_alloc_buf_t;

typedef struct {
    videoctl2_hdr_t hdr;   /* cmd = VIDEOCTL_CMD2_MAP_BUF */
    uint32_t handle;
    uint32_t reserved;

    /* out */
    uint64_t user_addr;    /* mapped user virtual address */
    uint32_t size_bytes;
    uint32_t pitch;
    uint32_t fmt;
} videoctl2_map_buf_t;
