/*
 * QXL Ring Buffer Implementation
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

#ifndef QXL_RING_H
#define QXL_RING_H

#include <stdint.h>

#define QXL_RING_SIZE 32

typedef struct QXLRing {
    uint32_t notify_on_prod;
    uint32_t notify_on_cons;
    uint32_t cons;
    uint32_t prod;
    uint8_t data[0];
} QXLRing;

typedef struct QXLRingHeader {
    uint32_t num_items;
    uint32_t prod;
    uint32_t notify_on_prod;
    uint32_t cons;
    uint32_t notify_on_cons;
} QXLRingHeader;

/* Ring macros for producer/consumer operations */
#define QXL_RING_PROD_ITEM(ring, idx) \
    ((ring)->data + ((idx) & ((ring)->num_items - 1)) * sizeof(*(ring)->items))

#define QXL_RING_CONS_ITEM(ring, idx) \
    ((ring)->data + ((idx) & ((ring)->num_items - 1)) * sizeof(*(ring)->items))

#define QXL_RING_PROD_SPACE(ring) \
    ((ring)->num_items - ((ring)->prod - (ring)->cons))

#define QXL_RING_CONS_AVAILABLE(ring) \
    ((ring)->prod - (ring)->cons)

#define QXL_RING_INDEX_MASK(ring) \
    ((ring)->num_items - 1)

#endif /* QXL_RING_H */