/*
 * QXL Cursor Definitions and Structures
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

#ifndef QXL_CURSOR_H
#define QXL_CURSOR_H

#include <stdint.h>

/* Cursor types */
enum {
    QXL_CURSOR_TYPE_ALPHA = 0,
    QXL_CURSOR_TYPE_MONO,
    QXL_CURSOR_TYPE_COLOR4,
    QXL_CURSOR_TYPE_COLOR8,
    QXL_CURSOR_TYPE_COLOR16,
    QXL_CURSOR_TYPE_COLOR24,
    QXL_CURSOR_TYPE_COLOR32
};

/* Cursor flags */
#define QXL_CURSOR_CACHE_ME     (1 << 0)
#define QXL_CURSOR_FROM_CACHE   (1 << 1)

typedef struct QXLCursorHeader {
    uint64_t unique;
    uint16_t type;
    uint16_t width;
    uint16_t height;
    uint16_t hot_spot_x;
    uint16_t hot_spot_y;
} QXLCursorHeader;

typedef struct QXLCursor {
    QXLCursorHeader header;
    uint32_t data_size;
    uint8_t chunk[0];
} QXLCursor;

typedef struct QXLCursorRing {
    uint32_t notify_on_prod;
    uint32_t notify_on_cons;
    uint32_t cons;
    uint32_t prod;
} QXLCursorRing;

#endif /* QXL_CURSOR_H */