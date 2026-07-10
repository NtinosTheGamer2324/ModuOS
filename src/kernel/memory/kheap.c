// kheap.c - Kernel heap allocator
// SPDX-License-Identifier: GPL-2.0-only

#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/spinlock.h"
#include "moduos/kernel/debug.h"
#include <stdint.h>
#include <stddef.h>

/* --- CONFIGURATION --- */
#define KHEAP_START      0xFFFF800000000000ULL
/* KHEAP_MAX is now set at runtime in kheap_init() based on available RAM. */
#define KHEAP_PAGE_FLAGS (PFLAG_PRESENT | PFLAG_WRITABLE)

#if defined(FBCON_DEBUG) && (FBCON_DEBUG >= 2)
#  define KHEAP_DEBUG 2
#else
#  define KHEAP_DEBUG 1
#endif

#ifndef PAGE_SIZE
#error "PAGE_SIZE must be defined"
#endif

#define ALLOC_MAGIC   0x4E54534654574152ULL
#define FREED_MAGIC   0x46524545444D4147ULL
#define ALIGNED_MAGIC 0x414C49474E45444DULL
#define MAX_FREE_NODES 256

/* --- STRUCTURES --- */
struct alloc_header {
    uint64_t magic;
    uint64_t size;
    uint64_t pages;
    uint64_t phys_base;
};

struct free_node {
    uint64_t virt;
    uint64_t pages;
    struct free_node *next;
    int used;
};

struct aligned_prefix {
    uint64_t magic;
    void    *raw;
};

static struct free_node  free_nodes_pool[MAX_FREE_NODES];
static struct free_node *free_list = NULL;

static uint64_t heap_alloc_next   = KHEAP_START;
static uint64_t total_allocations = 0;
static uint64_t failed_allocations = 0;
uint64_t kheap_max = 0;

static spinlock_t kheap_spinlock __attribute__((aligned(64)));

/* --- INTERNAL HELPERS --- */

static void uint64_to_dec(uint64_t v, char *buf, size_t buf_len) {
    if (buf_len == 0) return;
    if (v == 0) { if (buf_len >= 2) { buf[0] = '0'; buf[1] = 0; } return; }
    char tmp[32]; int pos = 0;
    while (v > 0 && pos < (int)sizeof(tmp)) { tmp[pos++] = '0' + (v % 10); v /= 10; }
    int out = 0;
    while (pos > 0 && out + 1 < (int)buf_len) buf[out++] = tmp[--pos];
    buf[out] = 0;
}

static void uint64_to_hex(uint64_t v, char *buf, size_t buf_len) {
    if (buf_len < 3) return;
    buf[0] = '0'; buf[1] = 'x';
    int pos = 2;
    for (int i = 15; i >= 0 && pos + 1 < (int)buf_len; i--) {
        uint8_t nibble = (v >> (i * 4)) & 0xF;
        buf[pos++] = nibble < 10 ? '0' + nibble : 'a' + (nibble - 10);
    }
    buf[pos] = 0;
}

static void log_oom(size_t requested_size, const char *reason) {
    com_write_string(COM1_PORT, "[KHEAP] OUT OF MEMORY: ");
    char buf[32]; uint64_to_dec(requested_size, buf, sizeof(buf));
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, " bytes - ");
    com_write_string(COM1_PORT, reason);
    com_write_string(COM1_PORT, "\n");
    failed_allocations++;
}

/* --- FREE LIST MANAGEMENT --- */

static struct free_node *alloc_free_node(void) {
    for (size_t i = 0; i < MAX_FREE_NODES; ++i) {
        if (!free_nodes_pool[i].used) {
            free_nodes_pool[i].used = 1;
            return &free_nodes_pool[i];
        }
    }
    return NULL;
}

/*
 * Cycle detection on free_list using Floyd's algorithm.
 * Halts with a diagnostic if a cycle is found — this should never happen
 * in correct operation; it indicates a double-free or memory corruption.
 */
static void kheap_check_cycle(void) {
    struct free_node *slow = free_list;
    struct free_node *fast = free_list;
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) {
            com_write_string(COM1_PORT, "[KHEAP] FATAL: free_list cycle at node=");
            char hb[32]; uint64_to_hex((uint64_t)(uintptr_t)slow, hb, sizeof(hb));
            com_write_string(COM1_PORT, hb);
            com_write_string(COM1_PORT, "\n");
            for (;;) __asm__ volatile("cli; hlt");
        }
    }
}

static void insert_and_coalesce(uint64_t virt, uint64_t pages) {
    kheap_check_cycle();
    if (pages == 0) return;

    /* Find insertion point (sorted by address). */
    struct free_node *prev = NULL;
    struct free_node *cur  = free_list;
    while (cur && cur->virt < virt) { prev = cur; cur = cur->next; }

    /* Try to merge with predecessor. */
    if (prev && (prev->virt + prev->pages * PAGE_SIZE == virt)) {
        prev->pages += pages;
        /* Try to also merge with successor. */
        if (cur && (prev->virt + prev->pages * PAGE_SIZE == cur->virt)) {
            prev->pages += cur->pages;
            prev->next   = cur->next;
            cur->used    = 0;
        }
        return;
    }

    /* Try to merge with successor only. */
    if (cur && (virt + pages * PAGE_SIZE == cur->virt)) {
        cur->virt   = virt;
        cur->pages += pages;
        return;
    }

    /* No merge possible — allocate a new node. */
    struct free_node *n = alloc_free_node();
    if (!n) { com_write_string(COM1_PORT, "[KHEAP] ERR: free node pool exhausted\n"); return; }
    n->virt  = virt;
    n->pages = pages;
    n->next  = cur;
    if (prev) prev->next = n; else free_list = n;
}

static uint64_t find_and_remove_free_block(uint64_t pages) {
    kheap_check_cycle();
    struct free_node *prev = NULL;
    struct free_node *cur  = free_list;
    while (cur) {
        if (cur->pages >= pages) {
            uint64_t v = cur->virt;
            if (cur->pages == pages) {
                if (prev) prev->next = cur->next; else free_list = cur->next;
                cur->used = 0;
            } else {
                cur->virt  += pages * PAGE_SIZE;
                cur->pages -= pages;
            }
            return v;
        }
        prev = cur; cur = cur->next;
    }
    return 0;
}

/* --- PUBLIC API --- */

void kheap_init(void) {
    spinlock_init(&kheap_spinlock);

    /* Dynamic heap ceiling: scale with physical RAM, capped at 2 GiB.
     * We query the physical allocator for the total managed frame count.
     * If phys isn't ready yet, fall back to a safe 64 MiB minimum.
     */
    uint64_t total_bytes = phys_total_frames() * (uint64_t)PAGE_SIZE;

    uint64_t heap_size;
    if      (total_bytes < 512ULL * 1024 * 1024)          heap_size =  64ULL * 1024 * 1024;
    else if (total_bytes < 2ULL  * 1024 * 1024 * 1024)    heap_size = 256ULL * 1024 * 1024;
    else if (total_bytes < 8ULL  * 1024 * 1024 * 1024)    heap_size = 512ULL * 1024 * 1024;
    else                                                    heap_size =   2ULL * 1024 * 1024 * 1024;

    /* Never exceed 1/8 of total RAM (keep headroom for process mappings etc.) */
    uint64_t cap = total_bytes / 8;
    if (cap < 64ULL * 1024 * 1024) cap = 64ULL * 1024 * 1024;  /* always at least 64 MiB */
    if (heap_size > cap) heap_size = cap;

    kheap_max = KHEAP_START + heap_size;

    com_write_string(COM1_PORT, "[KHEAP] heap max=");
    char buf[32];
    /* reuse your existing uint64_to_hex helper */
    uint64_to_hex(kheap_max, buf, sizeof(buf));
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, "\n");
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;

    spinlock_lock(&kheap_spinlock);

    size_t   total_size = size + sizeof(struct alloc_header);
    uint64_t pages      = (total_size + PAGE_SIZE - 1) / PAGE_SIZE;

    /* Try freelist first, then bump allocator. */
    uint64_t virt         = find_and_remove_free_block(pages);
    int      used_bump    = 0;

    if (!virt) {
        if (heap_alloc_next + pages * PAGE_SIZE > kheap_max) {
            log_oom(size, "virtual address space exhausted");
            spinlock_unlock(&kheap_spinlock);
            return NULL;
        }
        virt            = heap_alloc_next;
        heap_alloc_next += pages * PAGE_SIZE;
        used_bump        = 1;
    }

    /* Check physical memory availability before allocating. */
    if (phys_count_free_frames() < pages) {
        if (used_bump) heap_alloc_next -= pages * PAGE_SIZE;
        else           insert_and_coalesce(virt, pages);
        log_oom(size, "insufficient physical frames");
        spinlock_unlock(&kheap_spinlock);
        return NULL;
    }

    uint64_t phys = phys_alloc_contiguous(pages);
    if (!phys) {
        if (used_bump) heap_alloc_next -= pages * PAGE_SIZE;
        else           insert_and_coalesce(virt, pages);
        log_oom(size, "physical allocator fragmentation");
        spinlock_unlock(&kheap_spinlock);
        return NULL;
    }

    /*
     * Map into the master kernel CR3, not whichever CR3 happens to be
     * loaded (may be a process CR3 during fork/exec).  After mapping,
     * mirror the updated high-half PML4 slots back into the current CR3
     * so the new pages are immediately accessible.
     */
    {
        uint64_t cur_cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cur_cr3));
        cur_cr3 &= 0xFFFFFFFFFFFFF000ULL;

        uint64_t master_cr3 = paging_get_master_cr3() & 0xFFFFFFFFFFFFF000ULL;
        int switched = (master_cr3 && cur_cr3 != master_cr3);

        if (switched)
            __asm__ volatile("mov %0, %%cr3" :: "r"(master_cr3) : "memory");

        int map_rc = paging_map_range(virt, phys, pages * PAGE_SIZE, KHEAP_PAGE_FLAGS);

        if (switched) {
            __asm__ volatile("mov %0, %%cr3" :: "r"(cur_cr3) : "memory");
            uint64_t *master_pml4 = (uint64_t *)phys_to_virt_kernel(master_cr3);
            uint64_t *cur_pml4    = (uint64_t *)phys_to_virt_kernel(cur_cr3);
            if (master_pml4 && cur_pml4) {
                for (int i = 256; i < 512; i++) cur_pml4[i] = master_pml4[i];
                __asm__ volatile("mov %0, %%cr3" :: "r"(cur_cr3) : "memory");
            }
        }

        if (map_rc != 0) {
            for (uint64_t i = 0; i < pages; i++) phys_ref_dec(phys + i * PAGE_SIZE);
            if (used_bump) heap_alloc_next -= pages * PAGE_SIZE;
            else           insert_and_coalesce(virt, pages);
            log_oom(size, "paging_map_range failed");
            spinlock_unlock(&kheap_spinlock);
            return NULL;
        }
    }

    /* Verify every page is present and maps to the expected physical address. */
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t vaddr    = virt + i * PAGE_SIZE;
        uint64_t expected = phys + i * PAGE_SIZE;
        uint64_t pa       = paging_virt_to_phys(vaddr);
        if (pa == 0 || (pa & ~0xFFFULL) != (expected & ~0xFFFULL)) {
            char hb[32];
            com_write_string(COM1_PORT,
                pa == 0 ? "[KHEAP] FATAL: mapped page not present vaddr="
                        : "[KHEAP] FATAL: mapped page phys mismatch vaddr=");
            uint64_to_hex(vaddr, hb, sizeof(hb));
            com_write_string(COM1_PORT, hb);
            com_write_string(COM1_PORT, "\n");
            for (;;) __asm__ volatile("cli; hlt");
        }
    }

    __asm__ volatile("mov %%cr3, %%rax\n\tmov %%rax, %%cr3" ::: "rax", "memory");

    struct alloc_header *hdr = (struct alloc_header *)(uintptr_t)virt;
    hdr->magic     = ALLOC_MAGIC;
    hdr->size      = size;
    hdr->pages     = pages;
    hdr->phys_base = phys;

    total_allocations++;
    spinlock_unlock(&kheap_spinlock);
    return (void *)((uintptr_t)virt + sizeof(struct alloc_header));
}

void kfree(void *ptr) {
    if (!ptr) return;

    uint64_t p = (uint64_t)(uintptr_t)ptr;

    if (p < KHEAP_START || p >= kheap_max) {
        com_write_string(COM1_PORT, "[KHEAP] WARNING: kfree on non-heap pointer=");
        char pb[32]; uint64_to_hex(p, pb, sizeof(pb));
        com_write_string(COM1_PORT, pb);
        com_write_string(COM1_PORT, "\n");
        return;
    }

    /*
     * Check for aligned_prefix BEFORE checking the alloc_header.
     * The prefix sits immediately before the aligned pointer; the header
     * sits before the raw pointer stored inside the prefix.
     *
     * Guard: only inspect the prefix if the address is within the heap
     * and the page is mapped.
     */
    {
        uint64_t prefix_addr = p - sizeof(struct aligned_prefix);
        if (prefix_addr >= KHEAP_START && paging_virt_to_phys(prefix_addr) != 0) {
            struct aligned_prefix *ap =
                (struct aligned_prefix *)(uintptr_t)prefix_addr;
            if (ap->magic == ALIGNED_MAGIC && ap->raw != NULL) {
                void *raw = ap->raw;
                /* Poison the prefix so a double-free is detectable. */
                ap->magic = 0;
                ap->raw   = NULL;
                kfree(raw);   /* recurses once to free the underlying allocation */
                return;
            }
        }
    }

    uint64_t hdr_addr = p - sizeof(struct alloc_header);
    if (hdr_addr < KHEAP_START) {
        com_write_string(COM1_PORT, "[KHEAP] WARNING: kfree pointer underflow\n");
        return;
    }

    if (paging_virt_to_phys(hdr_addr) == 0) {
        com_write_string(COM1_PORT, "[KHEAP] WARNING: kfree on unmapped header (double free?)\n");
        return;
    }

    struct alloc_header *hdr = (struct alloc_header *)(uintptr_t)hdr_addr;

    if (hdr->magic == FREED_MAGIC) {
        com_write_string(COM1_PORT, "[KHEAP] WARNING: double free detected ptr=");
        char pb[32]; uint64_to_hex(p, pb, sizeof(pb));
        com_write_string(COM1_PORT, pb);
        com_write_string(COM1_PORT, "\n");
        return;
    }

    if (hdr->magic != ALLOC_MAGIC) {
        com_write_string(COM1_PORT, "[KHEAP] WARNING: corrupt/invalid free magic=");
        char mb[32]; uint64_to_hex(hdr->magic, mb, sizeof(mb));
        com_write_string(COM1_PORT, mb);
        com_write_string(COM1_PORT, "\n");
        return;
    }

    spinlock_lock(&kheap_spinlock);

    uint64_t phys_base = hdr->phys_base;
    uint64_t pages     = hdr->pages;
    uint64_t virt      = (uint64_t)(uintptr_t)hdr;

    /* Poison the header so use-after-free is detectable. */
    hdr->magic = FREED_MAGIC;

    for (uint64_t i = 0; i < pages; i++) phys_ref_dec(phys_base + i * PAGE_SIZE);

    /* Unmap from master CR3, then mirror back into the current CR3. */
    {
        uint64_t cur_cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cur_cr3));
        cur_cr3 &= 0xFFFFFFFFFFFFF000ULL;

        uint64_t master_cr3 = paging_get_master_cr3() & 0xFFFFFFFFFFFFF000ULL;
        int switched = (master_cr3 && cur_cr3 != master_cr3);

        if (switched)
            __asm__ volatile("mov %0, %%cr3" :: "r"(master_cr3) : "memory");

        for (uint64_t i = 0; i < pages; i++)
            paging_unmap_page(virt + i * PAGE_SIZE);

        if (switched) {
            __asm__ volatile("mov %0, %%cr3" :: "r"(cur_cr3) : "memory");
            uint64_t *master_pml4 = (uint64_t *)phys_to_virt_kernel(master_cr3);
            uint64_t *cur_pml4    = (uint64_t *)phys_to_virt_kernel(cur_cr3);
            if (master_pml4 && cur_pml4) {
                for (int i = 256; i < 512; i++) cur_pml4[i] = master_pml4[i];
                __asm__ volatile("mov %0, %%cr3" :: "r"(cur_cr3) : "memory");
            }
        }
    }

    insert_and_coalesce(virt, pages);
    spinlock_unlock(&kheap_spinlock);
}

void *kmalloc_aligned(size_t size, size_t alignment) {
    if (size == 0)                        return NULL;
    if (alignment < sizeof(void *))       alignment = sizeof(void *);
    if (alignment & (alignment - 1)) {
        /* Round up to next power of two. */
        size_t a = sizeof(void *);
        while (a < alignment) a <<= 1;
        alignment = a;
    }

    size_t extra = (alignment - 1) + sizeof(struct aligned_prefix);
    void  *raw   = kmalloc(size + extra);
    if (!raw) return NULL;

    uintptr_t base    = (uintptr_t)raw;
    uintptr_t aligned = (base + sizeof(struct aligned_prefix) + (alignment - 1))
                        & ~(uintptr_t)(alignment - 1);

    struct aligned_prefix *ap =
        (struct aligned_prefix *)(aligned - sizeof(struct aligned_prefix));
    ap->magic = ALIGNED_MAGIC;
    ap->raw   = raw;

    return (void *)aligned;
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p) memset(p, 0, size);
    return p;
}

void kheap_stats(void) {
    char buf[32];
    com_write_string(COM1_PORT, "\n--- KHEAP STATS ---\n");
    com_write_string(COM1_PORT, "Allocs: ");
    uint64_to_dec(total_allocations, buf, sizeof(buf));
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, " | OOM: ");
    uint64_to_dec(failed_allocations, buf, sizeof(buf));
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, "\n--------------------\n");
}