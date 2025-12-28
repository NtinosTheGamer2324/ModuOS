#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Simple DMA buffer API.
 *
 * Guarantees physically contiguous memory suitable for bus-master DMA.
 * Returns both a kernel virtual address and the physical address.
 */

typedef struct {
    void *virt;
    uint64_t phys;
    size_t size;
} dma_buffer_t;

int dma_alloc(dma_buffer_t *out, size_t size, size_t align);
void dma_free(dma_buffer_t *buf);

#ifdef __cplusplus
}
#endif
