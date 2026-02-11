#include "moduos/kernel/dma.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/memory.h" // kmalloc, kmalloc_aligned
#include "moduos/kernel/memory/string.h"

/*
 * v1 implementation:
 * - allocate contiguous physical frames
 * - map them into kernel virtual space using kmalloc'd VA range
 *   (we reuse the kernel heap virtual allocator by allocating pages from kheap).
 */

static int is_pow2_u64(uint64_t x) { return x && ((x & (x - 1)) == 0); }

int dma_alloc(dma_buffer_t *out, size_t size, size_t align) {
    if (!out || size == 0) return -1;

    // UHCI/EHCI/etc commonly need 16-byte or 4KiB alignment. Physical pages are always 4KiB aligned,
    // but callers may request larger alignment (e.g., 4096 for UHCI frame list).
    if (align == 0) align = 4096;
    if (!is_pow2_u64(align)) return -1;

    // We allocate whole pages.
    const size_t page_sz = 4096;
    size_t pages = (size + page_sz - 1) / page_sz;

    // --- Physical contiguous allocation (optionally aligned) ---
    uint64_t phys = 0;
    if (align <= page_sz) {
        phys = phys_alloc_contiguous(pages);
    } else {
        // Allocate a contiguous run whose starting address is aligned to `align`.
        phys = phys_alloc_contiguous_aligned(pages, (uint64_t)align);
    }
    if (!phys) return -2;

    // --- Virtual allocation (must be page-aligned before we remap) ---
    void *virt = kmalloc_aligned(pages * page_sz, page_sz);
    if (!virt) {
        for (size_t i = 0; i < pages; i++) phys_ref_dec(phys + (uint64_t)i * page_sz);
        return -3;
    }

    // --- Remap the allocated VA range to our contiguous physical pages ---
    // IMPORTANT: kmalloc/kmalloc_aligned returns memory that is already mapped to some physical pages.
    // If we simply overwrite the PTEs, we leak those frames. Free the old backing frames first.
    uint64_t vbase = (uint64_t)(uintptr_t)virt;
    for (size_t i = 0; i < pages; i++) {
        uint64_t vaddr = vbase + (uint64_t)i * page_sz;
        uint64_t old_phys = paging_virt_to_phys(vaddr);
        if (old_phys) {
            phys_ref_dec(old_phys & ~0xFFFULL);
        }
        paging_map_page(vaddr, phys + (uint64_t)i * page_sz, PFLAG_PRESENT | PFLAG_WRITABLE);
    }

    out->virt = virt;
    out->phys = phys;
    out->size = pages * page_sz;

    memset(out->virt, 0, out->size);
    return 0;
}

void dma_free(dma_buffer_t *buf) {
    if (!buf || !buf->virt || !buf->phys || !buf->size) return;

    size_t pages = buf->size / 4096;
    uint64_t vbase = (uint64_t)(uintptr_t)buf->virt;

    for (size_t i = 0; i < pages; i++) {
        paging_unmap_page(vbase + i * 4096);
        phys_ref_dec(buf->phys + i * 4096);
    }

    /* free the virtual allocation metadata */
    kfree(buf->virt);

    buf->virt = NULL;
    buf->phys = 0;
    buf->size = 0;
}
