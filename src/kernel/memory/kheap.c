#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/debug.h"
#include <stdint.h>
#include <stddef.h>

/* --- CONFIGURATION --- */
#define KHEAP_START 0xFFFF800000000000ULL
#define KHEAP_MAX   (KHEAP_START + (32 * 1024 * 1024ULL)) /* 32 MiB heap */
#define KHEAP_PAGE_FLAGS (PFLAG_PRESENT | PFLAG_WRITABLE)
#define KHEAP_DEBUG 1  /* Set to 1 to enable verbose tracing */

#ifndef PAGE_SIZE
#error "PAGE_SIZE must be defined"
#endif

#define ALLOC_MAGIC 0x4E54534654574152ULL  // NTSFTWAR aka NTSoftware aka New Technologies Software
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

static struct free_node free_nodes_pool[MAX_FREE_NODES];
static struct free_node *free_list = NULL;    

static uint64_t heap_alloc_next = KHEAP_START;
static uint64_t total_allocations = 0;
static uint64_t failed_allocations = 0;

/* --- INTERNAL HELPERS --- */

static void uint64_to_dec(uint64_t v, char *buf, size_t buf_len) {
    if (buf_len == 0) return;
    if (v == 0) { buf[0] = '0'; buf[1] = 0; return; }
    char tmp[32]; int pos = 0;
    while (v > 0 && pos < (int)sizeof(tmp)) { tmp[pos++] = '0' + (v % 10); v /= 10; }
    int out = 0;
    while (pos > 0 && out + 1 < (int)buf_len) { buf[out++] = tmp[--pos]; }
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

/* Verbose debug helper */
static void debug_log(const char *msg, uint64_t val, int is_hex) {
#if KHEAP_DEBUG
    char buf[32];
    // KHEAP debug spam can stall the system under QEMU; only print at very verbose level.
    if (kernel_debug_get_level() >= KDBG_ON) {
        com_write_string(COM1_PORT, "[KHEAP DEBUG] ");
        com_write_string(COM1_PORT, msg);
        if (is_hex) uint64_to_hex(val, buf, sizeof(buf));
        else uint64_to_dec(val, buf, sizeof(buf));
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, "\n");
    }
#endif
}

/* --- NODE MANAGEMENT --- */

static struct free_node *alloc_free_node(void) {
    for (size_t i = 0; i < MAX_FREE_NODES; ++i) {
        if (!free_nodes_pool[i].used) {
            free_nodes_pool[i].used = 1;
            return &free_nodes_pool[i];
        }
    }
    return NULL; 
}

static void insert_and_coalesce(uint64_t virt, uint64_t pages) {
    if (pages == 0) return;
    if (!free_list) {
        struct free_node *n = alloc_free_node();
        if (!n) { com_write_string(COM1_PORT, "[KHEAP] ERR: Free pool empty\n"); return; }
        n->virt = virt; n->pages = pages; n->next = NULL;
        free_list = n; return;
    }
    struct free_node *prev = NULL; struct free_node *cur = free_list;
    while (cur && cur->virt < virt) { prev = cur; cur = cur->next; }
    if (prev && (prev->virt + prev->pages * PAGE_SIZE == virt)) {
        prev->pages += pages;
        if (cur && (prev->virt + prev->pages * PAGE_SIZE == cur->virt)) {
            prev->pages += cur->pages; prev->next = cur->next;
            cur->used = 0;
        }
        return;
    }
    if (cur && (virt + pages * PAGE_SIZE == cur->virt)) {
        cur->virt = virt; cur->pages += pages; return;
    }
    struct free_node *n = alloc_free_node();
    if (!n) { com_write_string(COM1_PORT, "[KHEAP] ERR: Free pool empty\n"); return; }
    n->virt = virt; n->pages = pages; n->next = cur;
    if (prev) prev->next = n; else free_list = n;
}

static uint64_t find_and_remove_free_block(uint64_t pages) {
    struct free_node *prev = NULL; struct free_node *cur = free_list;
    while (cur) {
        if (cur->pages >= pages) {
            uint64_t v = cur->virt;
            if (cur->pages == pages) {
                if (prev) prev->next = cur->next; else free_list = cur->next;
                cur->used = 0;
            } else {
                cur->virt += pages * PAGE_SIZE; cur->pages -= pages;
            }
            return v;
        }
        prev = cur; cur = cur->next;
    }
    return 0;
}

/* --- PUBLIC API --- */

void *kmalloc(size_t size) {
    if (size == 0) return NULL;
    size_t total_size = size + sizeof(struct alloc_header);
    uint64_t pages = (total_size + PAGE_SIZE - 1) / PAGE_SIZE;

    debug_log("Allocating pages: ", pages, 0);

    uint64_t virt = find_and_remove_free_block(pages);
    int used_from_bump = 0;

    /* Debug: log allocation source */
    if (virt) {
        debug_log("[KHEAP] alloc source=freelist virt=", virt, 1);
    }

    if (!virt) {
        if (heap_alloc_next + pages * PAGE_SIZE > KHEAP_MAX) {
            log_oom(size, "Virtual limit reached"); return NULL;
        }
        debug_log("[KHEAP] bump before heap_alloc_next=", heap_alloc_next, 1);
        virt = heap_alloc_next;
        heap_alloc_next += pages * PAGE_SIZE;
        debug_log("[KHEAP] bump after  heap_alloc_next=", heap_alloc_next, 1);
        used_from_bump = 1;
    }

    if (phys_count_free_frames() < pages) {
        if (used_from_bump) heap_alloc_next -= pages * PAGE_SIZE;
        else insert_and_coalesce(virt, pages);
        log_oom(size, "Phys memory low"); return NULL;
    }

    uint64_t phys = phys_alloc_contiguous(pages);
    if (!phys) {
        if (used_from_bump) heap_alloc_next -= pages * PAGE_SIZE;
        else insert_and_coalesce(virt, pages);
        log_oom(size, "Phys fragmentation"); return NULL;
    }

    if (paging_map_range(virt, phys, pages * PAGE_SIZE, KHEAP_PAGE_FLAGS) != 0) {
        for (uint64_t i = 0; i < pages; ++i) phys_free_frame(phys + i * PAGE_SIZE);
        if (used_from_bump) heap_alloc_next -= pages * PAGE_SIZE;
        else insert_and_coalesce(virt, pages);
        log_oom(size, "Paging failure"); return NULL;
    }

    /* Debug: verify every mapped heap page is present in the page tables */
    for (uint64_t i = 0; i < pages; ++i) {
        uint64_t vaddr = virt + i * PAGE_SIZE;
        uint64_t pa = paging_virt_to_phys(vaddr);
        if (pa == 0) {
            com_write_string(COM1_PORT, "[KHEAP] FATAL: paging_map_range reported success but page is not present. vaddr=");
            char hb[32]; uint64_to_hex(vaddr, hb, sizeof(hb));
            com_write_string(COM1_PORT, hb);
            com_write_string(COM1_PORT, "\n");
            for (;;) { __asm__ volatile("cli; hlt"); }
        }
    }

    __asm__ volatile("mov %%cr3, %%rax\n\tmov %%rax, %%cr3" ::: "rax", "memory");

    struct alloc_header *hdr = (struct alloc_header *)(uintptr_t)virt;
    hdr->magic = ALLOC_MAGIC; hdr->size = size; hdr->pages = pages; hdr->phys_base = phys;

    total_allocations++;
    debug_log("KMALLOC SUCCESS: ", (uint64_t)virt, 1);
    return (void *)((uintptr_t)virt + sizeof(struct alloc_header));
}

void kfree(void *ptr) {
    if (!ptr) return;
    struct alloc_header *hdr = (struct alloc_header *)((uintptr_t)ptr - sizeof(struct alloc_header));

    /* Debug: log frees (helps detect unexpected frees/double frees) */
    {
        char hb[32]; uint64_to_hex((uint64_t)(uintptr_t)hdr, hb, sizeof(hb));
        com_write_string(COM1_PORT, "[KHEAP] kfree ptr=");
        char pb[32]; uint64_to_hex((uint64_t)(uintptr_t)ptr, pb, sizeof(pb));
        com_write_string(COM1_PORT, pb);
        com_write_string(COM1_PORT, " hdr=");
        com_write_string(COM1_PORT, hb);
        com_write_string(COM1_PORT, "\n");
    }

    if (hdr->magic != ALLOC_MAGIC) {
        com_write_string(COM1_PORT, "[KHEAP] WARNING: Corrupt/Invalid Free! magic=");
        char mb[32]; uint64_to_hex(hdr->magic, mb, sizeof(mb));
        com_write_string(COM1_PORT, mb);
        com_write_string(COM1_PORT, "\n");
        return;
    }

    com_printf(COM1_PORT, "[KHEAP]   size=%u pages=%u phys_base=0x%08x%08x\n",
               (uint32_t)hdr->size, (uint32_t)hdr->pages,
               (uint32_t)(hdr->phys_base >> 32), (uint32_t)(hdr->phys_base & 0xFFFFFFFFu));

    uint64_t phys_base = hdr->phys_base; uint64_t pages = hdr->pages; uint64_t virt = (uint64_t)(uintptr_t)hdr;
    hdr->magic = 0;
    for (uint64_t i = 0; i < pages; i++) phys_free_frame(phys_base + i * PAGE_SIZE);
    for (uint64_t i = 0; i < pages; i++) paging_unmap_page(virt + i * PAGE_SIZE);
    insert_and_coalesce(virt, pages);
}

void *kmalloc_aligned(size_t size, size_t alignment) {
    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    void *raw = kmalloc(size + alignment);
    if (!raw) return NULL;
    return (void *)(((uintptr_t)raw + (alignment - 1)) & ~(alignment - 1));
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p) memset(p, 0, size);
    return p;
}

void kheap_stats(void) {
    char buf[32];
    com_write_string(COM1_PORT, "\n--- KHEAP STATS ---\n");
    com_write_string(COM1_PORT, "Allocs: "); uint64_to_dec(total_allocations, buf, sizeof(buf)); com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, " | OOM: "); uint64_to_dec(failed_allocations, buf, sizeof(buf)); com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, "\n--------------------\n");
}