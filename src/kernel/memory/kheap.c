#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/COM/com.h"
#include <stdint.h>
#include <stddef.h>

#define KHEAP_START 0xFFFF800000000000ULL
#define KHEAP_MAX   (KHEAP_START + (32 * 1024 * 1024ULL)) /* 32 MiB heap */
#define KHEAP_PAGE_FLAGS (PFLAG_PRESENT | PFLAG_WRITABLE)

#ifndef PAGE_SIZE
#error "PAGE_SIZE must be defined"
#endif

/* Allocation header - placed at the start of each allocated virtual range */
struct alloc_header {
    uint64_t magic;      /* Magic number for validation */
    uint64_t size;       /* Size in bytes requested (without header) */
    uint64_t pages;      /* Number of pages allocated for this allocation */
    uint64_t phys_base;  /* Physical base address (contiguous region) */
};

#define ALLOC_MAGIC 0xDEADBEEFCAFEBABEULL

/* free-list node (kept in static pool so we don't kmalloc from inside kheap) */
struct free_node {
    uint64_t virt;           /* virtual base of free block */
    uint64_t pages;          /* size in pages */
    struct free_node *next;
    int used;
};

/* Configuration for the free-node pool */
#define MAX_FREE_NODES 256

static struct free_node free_nodes_pool[MAX_FREE_NODES];
static struct free_node *free_list = NULL;    /* sorted by virt (ascending) */

static uint64_t heap_alloc_next = KHEAP_START;
static uint64_t total_allocations = 0;
static uint64_t failed_allocations = 0;

/* Helper: allocate a free_node from pool */
static struct free_node *alloc_free_node(void) {
    for (size_t i = 0; i < MAX_FREE_NODES; ++i) {
        if (!free_nodes_pool[i].used) {
            free_nodes_pool[i].used = 1;
            free_nodes_pool[i].next = NULL;
            return &free_nodes_pool[i];
        }
    }
    return NULL; /* pool exhausted */
}

/* Helper: free a free_node back to pool */
static void free_free_node(struct free_node *n) {
    if (!n) return;
    n->used = 0;
    n->next = NULL;
}

/* Helper: integer -> decimal string (buf must be at least 32 bytes) */
static void uint64_to_dec(uint64_t v, char *buf, size_t buf_len) {
    if (buf_len == 0) return;
    if (v == 0) {
        buf[0] = '0';
        buf[1] = 0;
        return;
    }
    char tmp[32];
    int pos = 0;
    while (v > 0 && pos < (int)sizeof(tmp)) {
        tmp[pos++] = '0' + (v % 10);
        v /= 10;
    }
    int out = 0;
    while (pos > 0 && out + 1 < (int)buf_len) {
        buf[out++] = tmp[--pos];
    }
    buf[out] = 0;
}

/* Helper: integer -> hex string */
static void uint64_to_hex(uint64_t v, char *buf, size_t buf_len) {
    if (buf_len < 3) return;
    buf[0] = '0';
    buf[1] = 'x';
    int pos = 2;
    for (int i = 15; i >= 0 && pos + 1 < (int)buf_len; i--) {
        uint8_t nibble = (v >> (i * 4)) & 0xF;
        buf[pos++] = nibble < 10 ? '0' + nibble : 'a' + (nibble - 10);
    }
    buf[pos] = 0;
}

/* Helper to log allocation failures */
static void log_oom(size_t requested_size, const char *reason) {
    com_write_string(COM1_PORT, "[KHEAP] OUT OF MEMORY: Failed to allocate ");
    char buf[32];
    uint64_to_dec(requested_size, buf, sizeof(buf));
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, " bytes - ");
    com_write_string(COM1_PORT, reason);
    com_write_string(COM1_PORT, "\n");
    failed_allocations++;
}

/* Insert a free block into free list keeping it sorted by virt and coalescing neighbors */
static void insert_and_coalesce(uint64_t virt, uint64_t pages) {
    if (pages == 0) return;

    /* If free_list is empty, just insert */
    if (!free_list) {
        struct free_node *n = alloc_free_node();
        if (!n) {
            com_write_string(COM1_PORT, "[KHEAP] WARNING: free_node pool exhausted, cannot record free block\n");
            return;
        }
        n->virt = virt;
        n->pages = pages;
        n->next = NULL;
        free_list = n;
        return;
    }

    /* Insert in sorted order by virt */
    struct free_node *prev = NULL;
    struct free_node *cur = free_list;
    while (cur && cur->virt < virt) {
        prev = cur;
        cur = cur->next;
    }

    /* Check if we can merge with previous */
    if (prev) {
        uint64_t prev_end = prev->virt + prev->pages * PAGE_SIZE;
        if (prev_end == virt) {
            /* merge into prev */
            prev->pages += pages;
            /* try to also merge with cur if now adjacent */
            if (cur) {
                uint64_t this_end = prev->virt + prev->pages * PAGE_SIZE;
                if (this_end == cur->virt) {
                    prev->pages += cur->pages;
                    struct free_node *to_free = cur;
                    prev->next = cur->next;
                    free_free_node(to_free);
                }
            }
            return;
        }
    }

    /* Check if we can merge with cur (insert before cur) */
    if (cur) {
        uint64_t this_end = virt + pages * PAGE_SIZE;
        if (this_end == cur->virt) {
            /* expand cur to include this block at beginning */
            cur->virt = virt;
            cur->pages += pages;
            return;
        }
    }

    /* No merges, insert a new node between prev and cur */
    struct free_node *n = alloc_free_node();
    if (!n) {
        com_write_string(COM1_PORT, "[KHEAP] WARNING: free_node pool exhausted, cannot record free block\n");
        return;
    }
    n->virt = virt;
    n->pages = pages;
    n->next = cur;
    if (prev) prev->next = n;
    else free_list = n;
}

/* Find a free block with at least `pages` pages (first-fit). If found, remove/adjust it and return virt.
 * If not found, returns 0.
 */
static uint64_t find_and_remove_free_block(uint64_t pages) {
    struct free_node *prev = NULL;
    struct free_node *cur = free_list;
    while (cur) {
        if (cur->pages >= pages) {
            uint64_t v = cur->virt;
            if (cur->pages == pages) {
                /* exact match: remove node */
                if (prev) prev->next = cur->next;
                else free_list = cur->next;
                free_free_node(cur);
            } else {
                /* allocate from start of the free block */
                cur->virt += pages * PAGE_SIZE;
                cur->pages -= pages;
            }
            return v;
        }
        prev = cur;
        cur = cur->next;
    }
    return 0;
}

/* Count total free pages in the free-list */
static uint64_t free_pages_count(void) {
    uint64_t s = 0;
    struct free_node *cur = free_list;
    while (cur) {
        s += cur->pages;
        cur = cur->next;
    }
    return s;
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;

    /* Account for header and round up to pages */
    size_t total_size = size + sizeof(struct alloc_header);
    uint64_t pages = (total_size + PAGE_SIZE - 1) / PAGE_SIZE;

    char buf[64];
    com_write_string(COM1_PORT, "[KHEAP] kmalloc request: size=");
    uint64_to_dec(size, buf, sizeof(buf));
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, " bytes, pages needed=");
    uint64_to_dec(pages, buf, sizeof(buf));
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, "\n");

    /* Try to find a suitable free virtual block first */
    uint64_t virt = find_and_remove_free_block(pages);

    int used_from_bump = 0;
    if (!virt) {
        /* Use bump allocator area */
        if (heap_alloc_next + pages * PAGE_SIZE > KHEAP_MAX) {
            log_oom(size, "Virtual heap exhausted");
            return NULL;
        }
        virt = heap_alloc_next;
        heap_alloc_next += pages * PAGE_SIZE;
        used_from_bump = 1;
        
        com_write_string(COM1_PORT, "[KHEAP] Allocated from bump allocator at ");
        uint64_to_hex(virt, buf, sizeof(buf));
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, "\n");
    } else {
        com_write_string(COM1_PORT, "[KHEAP] Reusing free block at ");
        uint64_to_hex(virt, buf, sizeof(buf));
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, "\n");
    }

    /* Check physical frames available before attempting contiguous allocation */
    uint64_t free_frames = phys_count_free_frames();
    com_write_string(COM1_PORT, "[KHEAP] Free physical frames: ");
    uint64_to_dec(free_frames, buf, sizeof(buf));
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, "\n");
    
    if (free_frames < pages) {
        /* CRITICAL FIX: Only roll back bump allocator if we actually used it */
        if (used_from_bump) {
            heap_alloc_next -= pages * PAGE_SIZE;
            com_write_string(COM1_PORT, "[KHEAP] Rolled back bump allocator\n");
        } else {
            /* Put the free block back */
            insert_and_coalesce(virt, pages);
            com_write_string(COM1_PORT, "[KHEAP] Returned free block to free list\n");
        }
        log_oom(size, "Insufficient physical memory");
        return NULL;
    }

    /* Allocate contiguous physical frames */
    uint64_t phys = phys_alloc_contiguous(pages);
    if (!phys) {
        /* CRITICAL FIX: Same logic - only roll back if we used bump */
        if (used_from_bump) {
            heap_alloc_next -= pages * PAGE_SIZE;
        } else {
            insert_and_coalesce(virt, pages);
        }
        log_oom(size, "Physical allocation failed (fragmentation?)");
        return NULL;
    }

    com_write_string(COM1_PORT, "[KHEAP] Allocated phys frames at ");
    uint64_to_hex(phys, buf, sizeof(buf));
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, "\n");

    /* Map virtual to physical */
    if (paging_map_range(virt, phys, pages * PAGE_SIZE, KHEAP_PAGE_FLAGS) != 0) {
        /* Mapping failed, free physical pages and return virt to free-list */
        for (uint64_t i = 0; i < pages; ++i)
            phys_free_frame(phys + i * PAGE_SIZE);
        
        /* CRITICAL FIX: Only return to free list if not from bump, otherwise rollback */
        if (used_from_bump) {
            heap_alloc_next -= pages * PAGE_SIZE;
        } else {
            insert_and_coalesce(virt, pages);
        }
        log_oom(size, "Page mapping failed");
        return NULL;
    }

    com_write_string(COM1_PORT, "[KHEAP] Mapped ");
    uint64_to_hex(virt, buf, sizeof(buf));
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, " -> ");
    uint64_to_hex(phys, buf, sizeof(buf));
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, "\n");

    /* Flush TLB */
    __asm__ volatile(
        "mov %%cr3, %%rax\n"
        "mov %%rax, %%cr3\n"
        ::: "rax", "memory"
    );

    /* Initialize allocation header at virtual base (safe because mapped) */
    struct alloc_header *hdr = (struct alloc_header *)(uintptr_t)virt;
    hdr->magic = ALLOC_MAGIC;
    hdr->size = size;
    hdr->pages = pages;
    hdr->phys_base = phys;

    total_allocations++;

    void *result = (void *)((uintptr_t)virt + sizeof(struct alloc_header));
    com_write_string(COM1_PORT, "[KHEAP] Returning pointer ");
    uint64_to_hex((uint64_t)(uintptr_t)result, buf, sizeof(buf));
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, "\n");

    /* Return pointer after header */
    return result;
}

void *kmalloc_aligned(size_t size, size_t alignment)
{
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }

    /* kmalloc always returns at least PAGE_SIZE-aligned base + header */
    size_t total = size + alignment;

    void *raw = kmalloc(total);
    if (!raw)
        return NULL;

    uintptr_t addr = (uintptr_t)raw;

    /* Align returned pointer */
    uintptr_t aligned = (addr + (alignment - 1)) & ~(alignment - 1);

    /*
     * If aligning moved us past 'raw', the header is still valid,
     * because the header is BEFORE 'raw', not before 'aligned'.
     */

    return (void *)aligned;
}


void kfree(void *ptr) {
    if (!ptr) return;

    /* Get header pointer (virtual). Must copy meta before unmapping. */
    struct alloc_header *hdr = (struct alloc_header *)((uintptr_t)ptr - sizeof(struct alloc_header));

    /* Validate magic number and detect double-free */
    if (hdr->magic != ALLOC_MAGIC) {
        com_write_string(COM1_PORT, "[KHEAP] WARNING: Attempted to free invalid pointer or corrupted header!\n");
        return;
    }

    /* Copy header metadata before we unmap pages */
    uint64_t phys_base = hdr->phys_base;
    uint64_t pages = hdr->pages;
    uint64_t virt = (uint64_t)(uintptr_t)hdr;

    /* Mark magic 0 early to prevent re-entrancy/double-free races */
    hdr->magic = 0;

    /* Free physical frames (contiguous assumption) */
    for (uint64_t i = 0; i < pages; i++) {
        phys_free_frame(phys_base + i * PAGE_SIZE);
    }

    /* Unmap virtual pages */
    for (uint64_t i = 0; i < pages; i++) {
        paging_unmap_page(virt + i * PAGE_SIZE);
    }

    /* Add virtual range to free list and coalesce */
    insert_and_coalesce(virt, pages);
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p) {
        memset(p, 0, size);
    }
    return p;
}

/* Optional: Get allocation size */
size_t kmalloc_usable_size(void *ptr) {
    if (!ptr) return 0;

    struct alloc_header *hdr = (struct alloc_header *)((uintptr_t)ptr - sizeof(struct alloc_header));
    if (hdr->magic != ALLOC_MAGIC) return 0;

    /* Sanity check: size must be within pages range */
    uint64_t max_data = hdr->pages * PAGE_SIZE;
    if (hdr->size > max_data) return 0;

    return (size_t)hdr->size;
}

/* Memory statistics */
void kheap_stats(void) {
    com_write_string(COM1_PORT, "\n=== KERNEL HEAP STATISTICS ===\n");

    com_write_string(COM1_PORT, "[KHEAP] Total successful allocations: ");
    char buf[32];
    uint64_to_dec(total_allocations, buf, sizeof(buf));
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, "\n");

    com_write_string(COM1_PORT, "[KHEAP] Failed allocations (OOM): ");
    uint64_to_dec(failed_allocations, buf, sizeof(buf));
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, "\n");

    /* Virtual used = high-water (heap_alloc_next - start) minus pages available in free list */
    uint64_t high = (heap_alloc_next > KHEAP_START) ? (heap_alloc_next - KHEAP_START) : 0;
    uint64_t free_pages = free_pages_count();
    uint64_t free_bytes = free_pages * PAGE_SIZE;
    uint64_t used_bytes = (high > free_bytes) ? (high - free_bytes) : 0;

    com_write_string(COM1_PORT, "[KHEAP] Virtual heap used: ");
    uint64_to_dec(used_bytes / 1024, buf, sizeof(buf));
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, " KiB\n");

    uint64_t free_frames = phys_count_free_frames();
    com_write_string(COM1_PORT, "[KHEAP] Free physical frames: ");
    uint64_to_dec(free_frames, buf, sizeof(buf));
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, " (");
    uint64_to_dec((free_frames * PAGE_SIZE) / (1024 * 1024), buf, sizeof(buf));
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, " MiB)\n");

    com_write_string(COM1_PORT, "==============================\n\n");
}