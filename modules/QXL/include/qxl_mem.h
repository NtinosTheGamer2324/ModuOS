/*
 * QXL Memory Management and Slots
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

#ifndef QXL_MEM_H
#define QXL_MEM_H

#include <stdint.h>

/* Memory slot groups */
#define QXL_NUM_MEM_SLOT_GROUPS 8

/* Chunk flags */
#define QXL_CHUNK_LINEAR     (1 << 0)
#define QXL_CHUNK_INDIRECT   (1 << 1)

typedef struct QXLMemSlot {
    uint64_t mem_start;
    uint64_t mem_end;
    uint64_t generation;
    uint64_t high_bits;
} QXLMemSlot;

typedef struct QXLDataChunk {
    uint32_t data_size;
    uint64_t prev_chunk;
    uint64_t next_chunk;
    uint8_t data[0];
} QXLDataChunk;

typedef struct QXLReleaseInfo {
    uint64_t id;
    uint64_t next;
} QXLReleaseInfo;

typedef struct QXLReleaseRing {
    uint32_t notify_on_prod;
    uint32_t notify_on_cons;
    uint32_t cons;
    uint32_t prod;
    uint64_t elements[64];
} QXLReleaseRing;

/* Memory address conversion macros */
#define QXL_ADDR_TO_SLOT(addr, slot_id_bits) \
    (((addr) >> (64 - (slot_id_bits))) & ((1ULL << (slot_id_bits)) - 1))

#define QXL_ADDR_TO_OFFSET(addr, gen_bits, slot_id_bits) \
    ((addr) & ((1ULL << (64 - (gen_bits) - (slot_id_bits))) - 1))

#define QXL_ADDR_FROM_SLOT_GEN(slot, gen, offset, gen_bits, slot_id_bits) \
    (((uint64_t)(slot) << (64 - (slot_id_bits))) | \
     ((uint64_t)(gen) << (64 - (gen_bits) - (slot_id_bits))) | \
     (offset))

#endif /* QXL_MEM_H */