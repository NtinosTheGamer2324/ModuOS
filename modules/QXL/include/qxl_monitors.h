/*
 * QXL Monitor Configuration
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

#ifndef QXL_MONITORS_H
#define QXL_MONITORS_H

#include <stdint.h>

#define QXL_MAX_MONITORS 16

/* Monitor flags */
#define QXL_CLIENT_MONITOR_CONFIG_ENABLED (1 << 0)

typedef struct QXLMonitorsConfig {
    uint16_t count;
    uint16_t max_allowed;
    struct QXLHead heads[QXL_MAX_MONITORS];
} QXLMonitorsConfig;

typedef struct QXLHead {
    uint32_t id;
    uint32_t surface_id;
    uint32_t width;
    uint32_t height;
    uint32_t x;
    uint32_t y;
    uint32_t flags;
} QXLHead;

typedef struct QXLClientInfo {
    uint64_t cache_size;
    uint64_t accel_level;
    uint64_t stream_level;
    uint64_t jpeg_state;
    uint64_t zlib_glz_state;
} QXLClientInfo;

#endif /* QXL_MONITORS_H */