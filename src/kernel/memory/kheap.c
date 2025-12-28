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

#define ALLOC_MAGIC   0x4E54534654574152ULL  // NTSFTWAR aka NTSoftware aka New Technologies Software
#define FREED_MAGIC   0x46524545444D4147ULL  // FREEDMAG (diagnostic only)
#define ALIGNED_MAGIC 0x414C49474E45444DULL  // ALIGNEDM (prefix for kmalloc_aligned)
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

/* Prefix stored immediately before an aligned pointer returned by kmalloc_aligned().
 * This allows kfree() to recover the original kmalloc() pointer.
 */
struct aligned_prefix {
    uint64_t magic;
    void *raw;
};

static struct free_node free_nodes_pool[MAX_FREE_NODES];
static struct free_node *free_list = NULL;

/* forward decls (used by early helpers like kheap_check_cycle) */
static void uint64_to_hex(uint64_t v, char *buf, size_t buf_len);

/* Single-core IRQ-safe heap lock: disable interrupts while manipulating heap state. */
static uint64_t kheap_irq_flags = 0;
static int kheap_lock_count = 0;

static inline void kheap_lock(void) {
    uint64_t rflags;
    __asm__ volatile(
        "pushfq\n\t"
        "popq %0\n\t"
        "cli\n\t"
        : "=r"(rflags)
        :
        : "memory"
    );

    /* Re-entrant: only save original IF on first entry */
    if (kheap_lock_count == 0) {
        kheap_irq_flags = rflags;
    }
    kheap_lock_count++;
}

static inline void kheap_unlock(void) {
    if (kheap_lock_count <= 0) return;
    kheap_lock_count--;
    if (kheap_lock_count == 0) {
        if (kheap_irq_flags & (1ULL << 9)) {
            __asm__ volatile("sti" ::: "memory");
        }
    }
}

#define KHEAP_UNLOCK_AND_RETURN() do { kheap_unlock(); return; } while (0)
#define KHEAP_UNLOCK_AND_RETURN_VAL(v) do { kheap_unlock(); return (v); } while (0)

static void kheap_check_cycle(void) {
    /* Floyd cycle detection on free_list */
    struct free_node *slow = free_list;
    struct free_node *fast = free_list;
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) {
            com_write_string(COM1_PORT, "[KHEAP] FATAL: free_list cycle detected at node=\n");
            char hb[32]; uint64_to_hex((uint64_t)(uintptr_t)slow, hb, sizeof(hb));
            com_write_string(COM1_PORT, hb);
            com_write_string(COM1_PORT, "\n");
            for (;;) { __asm__ volatile("cli; hlt"); }
        }
    }
}

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
    kheap_check_cycle();

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
    kheap_check_cycle();

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
    kheap_lock();
    if (size == 0) KHEAP_UNLOCK_AND_RETURN_VAL(NULL);
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
            log_oom(size, "Virtual limit reached"); kheap_unlock(); return NULL;
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
        log_oom(size, "Phys memory low"); kheap_unlock(); return NULL;
    }

    uint64_t phys = phys_alloc_contiguous(pages);
    if (!phys) {
        if (used_from_bump) heap_alloc_next -= pages * PAGE_SIZE;
        else insert_and_coalesce(virt, pages);
        log_oom(size, "Phys fragmentation"); kheap_unlock(); return NULL;
    }

    if (paging_map_range(virt, phys, pages * PAGE_SIZE, KHEAP_PAGE_FLAGS) != 0) {
        for (uint64_t i = 0; i < pages; ++i) phys_free_frame(phys + i * PAGE_SIZE);
        if (used_from_bump) heap_alloc_next -= pages * PAGE_SIZE;
        else insert_and_coalesce(virt, pages);
        log_oom(size, "Paging failure"); kheap_unlock(); return NULL;
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
    void *ret = (void *)((uintptr_t)virt + sizeof(struct alloc_header));
    kheap_unlock();
    return ret;
}

void kfree(void *ptr) {
    kheap_lock();
    if (!ptr) KHEAP_UNLOCK_AND_RETURN();

    uint64_t p = (uint64_t)(uintptr_t)ptr;

    /* Basic range check: we only support freeing kernel heap pointers. */
    if (p < KHEAP_START || p >= KHEAP_MAX) {
        com_write_string(COM1_PORT, "[KHEAP] WARNING: kfree on non-heap ptr=");
        char pb[32]; uint64_to_hex(p, pb, sizeof(pb));
        com_write_string(COM1_PORT, pb);
        com_write_string(COM1_PORT, "\n");
        KHEAP_UNLOCK_AND_RETURN();
    }

    /* Handle kmalloc_aligned() pointers first.
     * The aligned prefix lives immediately before the pointer.
     */
    {
        uint64_t prefix_addr = p - sizeof(struct aligned_prefix);
        if (prefix_addr >= KHEAP_START) {
            if (paging_virt_to_phys(prefix_addr) != 0) {
                struct aligned_prefix *ap = (struct aligned_prefix *)(uintptr_t)prefix_addr;
                if (ap->magic == ALIGNED_MAGIC && ap->raw) {
                    void *raw = ap->raw;
                    ap->magic = 0;
                    ap->raw = NULL;
                    kfree(raw);
                    KHEAP_UNLOCK_AND_RETURN();
                }
            }
        }
    }

    /* Normal kmalloc() pointer: header is located immediately before returned pointer. */
    uint64_t hdr_addr = p - sizeof(struct alloc_header);
    if (hdr_addr < KHEAP_START) {
        com_write_string(COM1_PORT, "[KHEAP] WARNING: kfree ptr underflow ptr=");
        char pb[32]; uint64_to_hex(p, pb, sizeof(pb));
        com_write_string(COM1_PORT, pb);
        com_write_string(COM1_PORT, "\n");
        KHEAP_UNLOCK_AND_RETURN();
    }

    /* If the header page is not mapped, this is almost certainly a double-free
     * (we unmap on free) or an invalid pointer.
     */
    if (paging_virt_to_phys(hdr_addr) == 0) {
        com_write_string(COM1_PORT, "[KHEAP] WARNING: kfree on unmapped header (double free?) ptr=");
        char pb[32]; uint64_to_hex(p, pb, sizeof(pb));
        com_write_string(COM1_PORT, pb);
        com_write_string(COM1_PORT, " hdr=");
        char hb[32]; uint64_to_hex(hdr_addr, hb, sizeof(hb));
        com_write_string(COM1_PORT, hb);
        com_write_string(COM1_PORT, "\n");
        KHEAP_UNLOCK_AND_RETURN();
    }

    struct alloc_header *hdr = (struct alloc_header *)(uintptr_t)hdr_addr;

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
        /* If it was already freed, avoid touching more state. */
        if (hdr->magic == FREED_MAGIC) {
            com_write_string(COM1_PORT, "[KHEAP] WARNING: double free ptr=");
            char pb[32]; uint64_to_hex(p, pb, sizeof(pb));
            com_write_string(COM1_PORT, pb);
            com_write_string(COM1_PORT, "\n");
            KHEAP_UNLOCK_AND_RETURN();
        }

        com_write_string(COM1_PORT, "[KHEAP] WARNING: Corrupt/Invalid Free! magic=");
        char mb[32]; uint64_to_hex(hdr->magic, mb, sizeof(mb));
        com_write_string(COM1_PORT, mb);
        com_write_string(COM1_PORT, "\n");
        KHEAP_UNLOCK_AND_RETURN();
    }

    com_printf(COM1_PORT, "[KHEAP]   size=%u pages=%u phys_base=0x%08x%08x\n",
               (uint32_t)hdr->size, (uint32_t)hdr->pages,
               (uint32_t)(hdr->phys_base >> 32), (uint32_t)(hdr->phys_base & 0xFFFFFFFFu));

    uint64_t phys_base = hdr->phys_base;
    uint64_t pages = hdr->pages;
    uint64_t virt = (uint64_t)(uintptr_t)hdr;

    hdr->magic = FREED_MAGIC;

    for (uint64_t i = 0; i < pages; i++) phys_free_frame(phys_base + i * PAGE_SIZE);
    for (uint64_t i = 0; i < pages; i++) paging_unmap_page(virt + i * PAGE_SIZE);
    insert_and_coalesce(virt, pages);
    kheap_unlock();
}

void *kmalloc_aligned(size_t size, size_t alignment) {
    if (size == 0) return NULL;
    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    /* Require power-of-two alignment for the bitmask rounding. */
    if ((alignment & (alignment - 1)) != 0) {
        /* Round up to next power-of-two (conservative). */
        size_t a = sizeof(void *);
        while (a < alignment) a <<= 1;
        alignment = a;
    }

    size_t extra = (alignment - 1) + sizeof(struct aligned_prefix);
    void *raw = kmalloc(size + extra);
    if (!raw) return NULL;

    uintptr_t base = (uintptr_t)raw;
    uintptr_t aligned = (base + sizeof(struct aligned_prefix) + (alignment - 1)) & ~(uintptr_t)(alignment - 1);

    struct aligned_prefix *ap = (struct aligned_prefix *)(aligned - sizeof(struct aligned_prefix));
    ap->magic = ALIGNED_MAGIC;
    ap->raw = raw;

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
    com_write_string(COM1_PORT, "Allocs: "); uint64_to_dec(total_allocations, buf, sizeof(buf)); com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, " | OOM: "); uint64_to_dec(failed_allocations, buf, sizeof(buf)); com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, "\n--------------------\n");
}