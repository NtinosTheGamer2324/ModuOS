/*
 * QXL Surface Management
 *
 * Copyright (c) 2026 NTSoftware
 *
 * These headers were reverse-engineered and implemented based on
 * incomplete documentation and device behavior.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the "Software"),
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, under the condition that the above copyright notice
 * and this permission notice are included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
 *
 * Authorship: NTSoftware
 */

#ifndef QXL_SURFACE_H
#define QXL_SURFACE_H

#include <stdint.h>

/* Surface flags */
#define QXL_SURF_FLAG_KEEP_DATA (1 << 0)

/* Surface stride alignment */
#define QXL_SURF_STRIDE_ALIGNMENT 4

typedef struct QXLSurfaceCreate {
    uint32_t width;
    uint32_t height;
    int32_t stride;
    uint32_t format;
    uint32_t position;
    uint32_t flags;
    uint32_t type;
    uint64_t mem;
} QXLSurfaceCreate;

typedef struct QXLSurface {
    uint32_t format;
    uint32_t width;
    uint32_t height;
    int32_t stride;
    uint64_t data;
} QXLSurface;

#endif /* QXL_SURFACE_H */