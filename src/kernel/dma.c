#include "moduos/kernel/dma.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"

/*
 * v1 implementation:
 * - allocate contiguous physical frames
 * - map them into kernel virtual space using kmalloc'd VA range
 *   (we reuse the kernel heap virtual allocator by allocating pages from kheap).
 */

int dma_alloc(dma_buffer_t *out, size_t size, size_t align) {
    if (!out || size == 0) return -1;
    if (align == 0) align = 4096;

    size_t pages = (size + 4095) / 4096;

    /* Physical contiguous allocation */
    uint64_t phys = phys_alloc_contiguous(pages);
    if (!phys) return -2;

    /* Allocate virtual space (page-aligned) */
    void *virt = kmalloc(pages * 4096);
    if (!virt) {
        /* free contiguous frames */
        for (size_t i = 0; i < pages; i++) phys_free_frame(phys + i * 4096);
        return -3;
    }

    /* Remap the kmalloc region to our contiguous physical pages */
    uint64_t vbase = (uint64_t)(uintptr_t)virt;
    for (size_t i = 0; i < pages; i++) {
        paging_map_page(vbase + i * 4096, phys + i * 4096, PFLAG_PRESENT | PFLAG_WRITABLE);
    }

    out->virt = virt;
    out->phys = phys;
    out->size = pages * 4096;

    /* zero buffer */
    memset(out->virt, 0, out->size);

    return 0;
}

void dma_free(dma_buffer_t *buf) {
    if (!buf || !buf->virt || !buf->phys || !buf->size) return;

    size_t pages = buf->size / 4096;
    uint64_t vbase = (uint64_t)(uintptr_t)buf->virt;

    for (size_t i = 0; i < pages; i++) {
        paging_unmap_page(vbase + i * 4096);
        phys_free_frame(buf->phys + i * 4096);
    }

    /* free the virtual allocation metadata */
    kfree(buf->virt);

    buf->virt = NULL;
    buf->phys = 0;
    buf->size = 0;
}
