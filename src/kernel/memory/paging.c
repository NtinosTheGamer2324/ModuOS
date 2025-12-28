#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/debug.h"
#include <stdint.h>
#include <stddef.h>
#include "moduos/kernel/memory/string.h"

#define PT_ENTRIES 512
#define PAGE_MASK (~0xFFFULL)
#define PAGE_SIZE  4096ULL

/* VGA text buffer is at 0xB8000 - we must preserve this! */
#define VGA_TEXT_BUFFER 0xB8000ULL
#define VGA_TEXT_SIZE   0x1000ULL  /* 4KB */

static uint64_t *pml4 = NULL;       /* virtual pointer (via phys_to_virt) to PML4 */
static uint64_t pml4_phys = 0;      /* physical address of PML4 */

/* phys_to_virt offset. Starts at 0 (identity). */
static uint64_t phys_offset = 0; /* 0 means identity mapping */

/* MMIO virtual address space tracking
 * IMPORTANT: don't hardcode a high virtual base that might collide with bootloader mappings.
 * We pick a free PML4 slot on first use.
 */
static uint64_t ioremap_base = 0;
static uint64_t ioremap_next = 0;

void paging_set_phys_offset(uint64_t offset) {
    phys_offset = offset;
}

static inline void *phys_to_virt(uint64_t phys) {
    if (phys == 0) return NULL;
    if (phys_offset == 0) return (void *)(uintptr_t)phys;
    return (void *)(uintptr_t)(phys + phys_offset);
}

void *phys_to_virt_kernel(uint64_t phys) {
    // Identity mapping for kernel (phys_offset = 0)
    return (void *)(uintptr_t)phys;
}

/*
 * Page-table pages must be accessible immediately (we memset() them).
 * This kernel currently identity-maps only the first ~512MB early on.
 * So we must allocate paging structures from low physical memory.
 */
#ifndef PAGING_PT_ALLOC_LIMIT
// IMPORTANT: page-table pages must be allocated from memory that is *already* accessible
// when we memset() them. During early identity mapping we cannot assume all <512MB is
// already mapped, so keep this very low.
#define PAGING_PT_ALLOC_LIMIT 0x04000000ULL /* 64MB */
#endif

static uint64_t *alloc_pt_page(void) {
    // Page tables must be allocated from *already identity-mapped* physical memory.
    // If we allocate a page table above the currently mapped region and then memset() it,
    // we will page fault and likely hang/triple-fault.
    uint64_t phys = phys_alloc_frame_below(PAGING_PT_ALLOC_LIMIT);
    if (!phys) {
        // Do NOT fall back to arbitrary frames.
        return NULL;
    }

    void *v = phys_to_virt(phys);
    if (!v) return NULL;

    memset(v, 0, PAGE_SIZE);
    return (uint64_t *)v;
}

uint64_t *paging_get_pml4(void) {
    return (uint64_t *)pml4;
}

uint64_t paging_get_pml4_phys(void) {
    return pml4_phys;
}

static void format_hex64(char *buf, uint64_t v) {
    const char hex[] = "0123456789abcdef";
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        buf[2 + i] = hex[(v >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = 0;
}

static void paging_reserve_bootloader_tables(void);

void paging_init(void) {
    if (pml4) {
        com_write_string(COM1_PORT, "[PAGING] Already initialized\n");
        return;
    }

    /* Defensive: ensure ioremap allocator starts from a known state even if .bss wasn't cleared */
    ioremap_base = 0;
    ioremap_next = 0;

    if (kernel_debug_is_on()) {
        com_write_string(COM1_PORT, "[PAGING] Initializing AMD64 paging...\n");
    }

    uint64_t old_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(old_cr3));

    com_write_string(COM1_PORT, "[PAGING] Bootloader CR3: ");
    char tmpbuf[32];
    format_hex64(tmpbuf, old_cr3);
    com_write_string(COM1_PORT, tmpbuf);
    com_write_string(COM1_PORT, "\n");

    //  Just use the bootloader's PML4 directly!
    // Don't try to create a new one - that requires allocating memory
    // which itself needs page tables to access!
    pml4_phys = old_cr3 & PAGE_MASK;
    pml4 = (uint64_t *)phys_to_virt(pml4_phys);
    
    if (!pml4) {
        com_write_string(COM1_PORT, "[PAGING] FATAL: Cannot access bootloader PML4\n");
        return;
    }

    com_write_string(COM1_PORT, "[PAGING] Using bootloader's PML4 at: ");
    format_hex64(tmpbuf, pml4_phys);
    com_write_string(COM1_PORT, tmpbuf);
    com_write_string(COM1_PORT, "\n");
    
    com_write_string(COM1_PORT, "[PAGING] Bootloader has already set up identity mapping\n");
    com_write_string(COM1_PORT, "[PAGING] We will extend it as needed\n");

    /* Reserve bootloader paging-structure pages in the physical allocator.
     * Otherwise phys_alloc_frame() may hand them out and they can be zeroed/overwritten,
     * leading to immediate crashes on iretq after faults (GPF/triple fault).
     */
    paging_reserve_bootloader_tables();
}

/* Walk current page tables and reserve every paging-structure page (PML4/PDPT/PD/PT)
 * so the physical allocator will never hand them out.
 * Assumes paging structures are identity-mapped at this stage.
 */
static void paging_reserve_bootloader_tables(void) {
    if (!pml4 || !pml4_phys) return;

    /* reserve PML4 itself */
    phys_reserve_range(pml4_phys, PAGE_SIZE);

    /* Very small visited set (bootloader tables are usually small). */
    uint64_t seen[2048];
    size_t seen_n = 0;

    #define SEEN_HAS(x) ({ int _f=0; for (size_t _i=0; _i<seen_n; _i++) if (seen[_i]==(x)) { _f=1; break; } _f; })
    #define SEEN_ADD(x) do { if (seen_n < (sizeof(seen)/sizeof(seen[0]))) seen[seen_n++] = (x); } while(0)

    for (unsigned i4 = 0; i4 < 512; i4++) {
        uint64_t e4 = pml4[i4];
        if (!(e4 & PFLAG_PRESENT)) continue;
        uint64_t pdpt_phys = e4 & PAGE_MASK;
        if (!SEEN_HAS(pdpt_phys)) {
            phys_reserve_range(pdpt_phys, PAGE_SIZE);
            SEEN_ADD(pdpt_phys);
        }

        uint64_t *pdpt = (uint64_t*)phys_to_virt(pdpt_phys);
        if (!pdpt) continue;

        for (unsigned i3 = 0; i3 < 512; i3++) {
            uint64_t e3 = pdpt[i3];
            if (!(e3 & PFLAG_PRESENT)) continue;
            if (e3 & (1ULL<<7)) continue; /* 1GiB huge page */
            uint64_t pd_phys = e3 & PAGE_MASK;
            if (!SEEN_HAS(pd_phys)) {
                phys_reserve_range(pd_phys, PAGE_SIZE);
                SEEN_ADD(pd_phys);
            }

            uint64_t *pd = (uint64_t*)phys_to_virt(pd_phys);
            if (!pd) continue;

            for (unsigned i2 = 0; i2 < 512; i2++) {
                uint64_t e2 = pd[i2];
                if (!(e2 & PFLAG_PRESENT)) continue;
                if (e2 & (1ULL<<7)) continue; /* 2MiB huge page */
                uint64_t pt_phys = e2 & PAGE_MASK;
                if (!SEEN_HAS(pt_phys)) {
                    phys_reserve_range(pt_phys, PAGE_SIZE);
                    SEEN_ADD(pt_phys);
                }
            }
        }
    }

    #undef SEEN_HAS
    #undef SEEN_ADD
}


static uint64_t *get_or_create(uint64_t *table, unsigned idx) {
    uint64_t ent = table[idx];

    /* If entry is a huge page mapping, caller must split it before treating it as a table pointer. */
    if ((ent & PFLAG_PRESENT) && (ent & (1ULL << 7))) {
        if (kernel_debug_is_on()) {
            com_write_string(COM1_PORT, "[PAGING] get_or_create: encountered huge page entry; split required\n");
        }
        return NULL;
    }

    if (ent & PFLAG_PRESENT) {
        uint64_t phys = ent & PAGE_MASK;

        /* IMPORTANT:
         * Do NOT replace an already-present paging-structure page just because its physical
         * address is "high".
         *
         * The old logic attempted to "play safe" during early boot by allocating a new low
         * page-table page when the existing table lived above PAGING_PT_ALLOC_LIMIT.
         * That silently discards existing mappings in that subtree (including kernel heap),
         * which can later surface as page faults in seemingly unrelated code paths.
         *
         * We rely on the bootloader's identity mapping (and later early_identity_map_all())
         * to make the paging structures accessible.
         */

        uint64_t *virt = (uint64_t *)phys_to_virt(phys);
        if (!virt && kernel_debug_is_on()) {
            com_write_string(COM1_PORT, "[PAGING] get_or_create: present but phys_to_virt failed for table idx\n");
        }
        return virt;
    } else {
        uint64_t *next = alloc_pt_page();
        if (!next) {
            if (kernel_debug_is_on()) {
                com_write_string(COM1_PORT, "[PAGING] get_or_create: alloc_pt_page failed\n");
            }
            return NULL;
        }
        uint64_t v = (uint64_t)(uintptr_t)next;
        uint64_t next_phys = (phys_offset == 0) ? v : (v - phys_offset);
        table[idx] = (next_phys & PAGE_MASK) | (PFLAG_PRESENT | PFLAG_WRITABLE);
        return next;
    }
}

int paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!pml4) paging_init();
    if (!pml4) return -1;

    // Per-page map logging is extremely expensive on QEMU serial; keep it only for very verbose debugging.
    if (kernel_debug_get_level() >= KDBG_ON) {
        com_write_string(COM1_PORT, "[PAGING] map_page virt=");
        char tmp[32];
        format_hex64(tmp, virt);
        com_write_string(COM1_PORT, tmp);
        com_write_string(COM1_PORT, " phys=");
        format_hex64(tmp, phys);
        com_write_string(COM1_PORT, tmp);
        com_write_string(COM1_PORT, "\n");
    }

    unsigned i4 = (virt >> 39) & 0x1FF;
    unsigned i3 = (virt >> 30) & 0x1FF;
    unsigned i2 = (virt >> 21) & 0x1FF;
    unsigned i1 = (virt >> 12) & 0x1FF;

    uint64_t *pdpt = get_or_create(pml4, i4);
    if (!pdpt) {
        /* If PML4 entry is huge (shouldn't happen), fail. */
        return -1;
    }

    /* If mapping a user page, all paging levels must have the USER bit set. */
    if (flags & PFLAG_USER) {
        pml4[i4] |= PFLAG_USER;
    }

    /* Split 1GB huge page at PDPT level if present */
    uint64_t ent3 = pdpt[i3];
    if ((ent3 & PFLAG_PRESENT) && (ent3 & (1ULL << 7))) {
        /* Replace with a page directory mapping 2MB pages for that 1GB region */
        uint64_t *new_pd = alloc_pt_page();
        if (!new_pd) return -1;
        uint64_t new_pd_phys = (phys_offset == 0) ? (uint64_t)(uintptr_t)new_pd : ((uint64_t)(uintptr_t)new_pd - phys_offset);

        uint64_t base_phys = ent3 & 0xFFFFFFFFC0000000ULL;
        uint64_t huge_flags = ent3 & 0xFFFULL;

        for (int j = 0; j < 512; j++) {
            new_pd[j] = (base_phys + ((uint64_t)j << 21)) | huge_flags | (1ULL << 7);
        }

        pdpt[i3] = (new_pd_phys & PAGE_MASK) | PFLAG_PRESENT | PFLAG_WRITABLE;
        __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
    }

    if (flags & PFLAG_USER) {
        pdpt[i3] |= PFLAG_USER;
    }

    uint64_t *pd = get_or_create(pdpt, i3);
    if (!pd) return -1;

    /* Split 2MB huge page at PD level if present */
    uint64_t ent2 = pd[i2];
    if ((ent2 & PFLAG_PRESENT) && (ent2 & (1ULL << 7))) {
        uint64_t *new_pt = alloc_pt_page();
        if (!new_pt) return -1;
        uint64_t new_pt_phys = (phys_offset == 0) ? (uint64_t)(uintptr_t)new_pt : ((uint64_t)(uintptr_t)new_pt - phys_offset);

        uint64_t base_phys = ent2 & 0xFFFFFFFFFFE00000ULL;
        uint64_t base_flags = ent2 & 0xFFFULL;

        for (int j = 0; j < 512; j++) {
            new_pt[j] = (base_phys + ((uint64_t)j << 12)) | (base_flags & ~((1ULL << 7))) | PFLAG_PRESENT;
        }

        pd[i2] = (new_pt_phys & PAGE_MASK) | PFLAG_PRESENT | PFLAG_WRITABLE;
        __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
    }

    if (flags & PFLAG_USER) {
        pd[i2] |= PFLAG_USER;
    }

    uint64_t *pt = get_or_create(pd, i2);
    if (!pt) return -1;

    uint64_t entry = (phys & PAGE_MASK) | (flags & 0xFFFULL) | PFLAG_PRESENT;
    pt[i1] = entry;

    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
    return 0;
}

int paging_unmap_page(uint64_t virt) {
    if (!pml4) return -1;

    unsigned i4 = (virt >> 39) & 0x1FF;
    unsigned i3 = (virt >> 30) & 0x1FF;
    unsigned i2 = (virt >> 21) & 0x1FF;
    unsigned i1 = (virt >> 12) & 0x1FF;

    uint64_t ent4 = pml4[i4];
    if (!(ent4 & PFLAG_PRESENT)) return -1;
    uint64_t *pdpt = (uint64_t *)phys_to_virt(ent4 & PAGE_MASK);
    if (!pdpt) return -1;

    uint64_t ent3 = pdpt[i3];
    if (!(ent3 & PFLAG_PRESENT)) return -1;
    uint64_t *pd = (uint64_t *)phys_to_virt(ent3 & PAGE_MASK);
    if (!pd) return -1;

    uint64_t ent2 = pd[i2];
    if (!(ent2 & PFLAG_PRESENT)) return -1;
    uint64_t *pt = (uint64_t *)phys_to_virt(ent2 & PAGE_MASK);
    if (!pt) return -1;

    pt[i1] = 0;
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
    return 0;
}

int paging_map_2m_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!pml4) paging_init();
    if (!pml4) return -1;

    // Require 2MB alignment
    if ((virt & 0x1FFFFFULL) || (phys & 0x1FFFFFULL)) return -1;

    unsigned i4 = (virt >> 39) & 0x1FF;
    unsigned i3 = (virt >> 30) & 0x1FF;
    unsigned i2 = (virt >> 21) & 0x1FF;

    uint64_t *pdpt = get_or_create(pml4, i4);
    if (!pdpt) return -1;

    // If PDPT entry is a 1GB huge page, split it into a PD of 2MB huge pages
    uint64_t ent3 = pdpt[i3];
    if ((ent3 & PFLAG_PRESENT) && (ent3 & (1ULL << 7))) {
        uint64_t *new_pd = alloc_pt_page();
        if (!new_pd) return -1;
        uint64_t new_pd_phys = (phys_offset == 0) ? (uint64_t)(uintptr_t)new_pd : ((uint64_t)(uintptr_t)new_pd - phys_offset);

        uint64_t base_phys = ent3 & 0xFFFFFFFFC0000000ULL;
        uint64_t huge_flags = ent3 & 0xFFFULL;

        for (int j = 0; j < 512; j++) {
            new_pd[j] = (base_phys + ((uint64_t)j << 21)) | huge_flags | (1ULL << 7);
        }

        pdpt[i3] = (new_pd_phys & PAGE_MASK) | PFLAG_PRESENT | PFLAG_WRITABLE;
        __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
    }

    uint64_t *pd = get_or_create(pdpt, i3);
    if (!pd) return -1;

    // If PD entry is already a table (4KB pages), don't overwrite it.
    // If it's already a huge page, also leave it.
    uint64_t ent2 = pd[i2];
    if (ent2 & PFLAG_PRESENT) {
        return 0;
    }

    // Set 2MB huge page entry (PS bit)
    uint64_t entry = (phys & 0xFFFFFFFFFFE00000ULL) | (flags & 0xFFFULL) | PFLAG_PRESENT | (1ULL << 7);
    pd[i2] = entry;
    // Don't invlpg per 4KB page; one invlpg on the 2MB base is enough
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
    return 0;
}

int paging_map_2m_range(uint64_t virt_base, uint64_t phys_base, uint64_t size, uint64_t flags) {
    const uint64_t huge_sz = 2ULL * 1024 * 1024;
    if ((virt_base & (huge_sz - 1)) || (phys_base & (huge_sz - 1))) return -1;

    uint64_t pages = (size + huge_sz - 1) / huge_sz;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t vaddr = virt_base + i * huge_sz;
        uint64_t paddr = phys_base + i * huge_sz;
        if (paging_map_2m_page(vaddr, paddr, flags) != 0) return -1;
    }
    return 0;
}

int paging_map_range(uint64_t virt_base, uint64_t phys_base, uint64_t size, uint64_t flags) {
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    char tmp[32];
    for (uint64_t i = 0; i < pages; ++i) {
        uint64_t vaddr = virt_base + i * PAGE_SIZE;
        uint64_t paddr = phys_base + i * PAGE_SIZE;
        if (paging_map_page(vaddr, paddr, flags) != 0) {
            com_write_string(COM1_PORT, "[PAGING] Failed to map page at virt=");
            format_hex64(tmp, vaddr);
            com_write_string(COM1_PORT, tmp);
            com_write_string(COM1_PORT, "\n");
            return -1;
        }
    }
    return 0;
}

uint64_t paging_create_process_pml4(void) {
    if (!pml4) {
        paging_init();
        if (!pml4) return 0;
    }

    uint64_t *new_pml4 = alloc_pt_page();
    if (!new_pml4) return 0;

    uint64_t v = (uint64_t)(uintptr_t)new_pml4;
    uint64_t new_phys = (phys_offset == 0) ? v : (v - phys_offset);

    for (int i = 0; i < PT_ENTRIES; ++i) {
        uint64_t e = pml4[i];
        if (e & PFLAG_PRESENT) new_pml4[i] = e;
        else new_pml4[i] = 0;
    }

    return new_phys;
}

static uint64_t *get_or_create_in_pml4(uint64_t *pml4_virt, unsigned idx4) {
    uint64_t ent = pml4_virt[idx4];
    if (ent & PFLAG_PRESENT) {
        uint64_t phys = ent & PAGE_MASK;
        return (uint64_t *)phys_to_virt(phys);
    } else {
        uint64_t *next = alloc_pt_page();
        if (!next) return NULL;
        uint64_t v = (uint64_t)(uintptr_t)next;
        uint64_t next_phys = (phys_offset == 0) ? v : (v - phys_offset);
        pml4_virt[idx4] = (next_phys & PAGE_MASK) | (PFLAG_PRESENT | PFLAG_WRITABLE);
        return next;
    }
}

int paging_map_range_to_pml4(uint64_t *pml4_virt, uint64_t virt_base, uint64_t phys_base, uint64_t size, uint64_t flags) {
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; ++i) {
        uint64_t vaddr = virt_base + i * PAGE_SIZE;
        uint64_t paddr = phys_base + i * PAGE_SIZE;

        unsigned i4 = (vaddr >> 39) & 0x1FF;
        unsigned i3 = (vaddr >> 30) & 0x1FF;
        unsigned i2 = (vaddr >> 21) & 0x1FF;
        unsigned i1 = (vaddr >> 12) & 0x1FF;

        uint64_t *pdpt = get_or_create_in_pml4(pml4_virt, i4);
        if (!pdpt) return -1;
        uint64_t *pd = get_or_create(pdpt, i3);
        if (!pd) return -1;
        uint64_t *pt = get_or_create(pd, i2);
        if (!pt) return -1;

        uint64_t entry = (paddr & PAGE_MASK) | (flags & 0xFFFULL) | PFLAG_PRESENT;
        pt[i1] = entry;

        /* Ensure user bit is visible at PT level too (redundant but explicit). */
        if (flags & PFLAG_USER) {
            pt[i1] |= PFLAG_USER;
        }
    }
    return 0;
}

uint64_t paging_virt_to_phys(uint64_t virt) {
    uint64_t *current_pml4 = paging_get_pml4();
    if (!current_pml4) {
        /* PML4 not initialized yet - try to use CR3 directly */
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        
        /* In identity mapping, we can access page tables directly */
        uint64_t pml4_phys = cr3 & 0xFFFFFFFFFFFFF000ULL;
        current_pml4 = (uint64_t*)(uintptr_t)pml4_phys;
    }
    
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;
    uint64_t offset   = virt & 0xFFF;
    
    /* Read PML4 entry */
    uint64_t pml4_entry = current_pml4[pml4_idx];
    if (!(pml4_entry & PFLAG_PRESENT)) {
        return 0;
    }
    
    /* Get PDPT physical address and access it (identity mapped) */
    uint64_t pdpt_phys = pml4_entry & 0xFFFFFFFFFFFFF000ULL;
    uint64_t *pdpt = (uint64_t*)(uintptr_t)pdpt_phys;
    
    uint64_t pdpt_entry = pdpt[pdpt_idx];
    if (!(pdpt_entry & PFLAG_PRESENT)) {
        return 0;
    }
    
    /* Check for 1GB huge page */
    if (pdpt_entry & (1ULL << 7)) {
        uint64_t page_phys = pdpt_entry & 0xFFFFFFFFC0000000ULL;
        uint64_t page_offset = virt & 0x3FFFFFFF;
        return page_phys | page_offset;
    }
    
    /* Get PD physical address */
    uint64_t pd_phys = pdpt_entry & 0xFFFFFFFFFFFFF000ULL;
    uint64_t *pd = (uint64_t*)(uintptr_t)pd_phys;
    
    uint64_t pd_entry = pd[pd_idx];
    if (!(pd_entry & PFLAG_PRESENT)) {
        return 0;
    }
    
    /* Check for 2MB huge page */
    if (pd_entry & (1ULL << 7)) {
        uint64_t page_phys = pd_entry & 0xFFFFFFFFFE000000ULL;
        uint64_t page_offset = virt & 0x1FFFFF;
        return page_phys | page_offset;
    }
    
    /* Get PT physical address */
    uint64_t pt_phys = pd_entry & 0xFFFFFFFFFFFFF000ULL;
    uint64_t *pt = (uint64_t*)(uintptr_t)pt_phys;
    
    uint64_t pt_entry = pt[pt_idx];
    if (!(pt_entry & PFLAG_PRESENT)) {
        return 0;
    }
    
    /* Regular 4KB page */
    uint64_t page_phys = pt_entry & 0xFFFFFFFFFFFFF000ULL;
    return page_phys | offset;
}

static int is_canonical_high(uint64_t v) {
    /* canonical high half has bits 63..48 all 1 */
    return ((v >> 48) == 0xFFFFULL);
}

static uint64_t pick_ioremap_base(void) {
    /* Find an unused kernel (high-half) PML4 slot.
     * IMPORTANT: avoid colliding with the kernel heap region at 0xFFFF8000_0000_0000
     * which occupies PML4 index 256.
     */
    if (!pml4) return 0;

    for (int idx = 510; idx >= 256; --idx) {
        /* PML4[256] corresponds to 0xFFFF8000_0000_0000 (kernel heap); never use it for MMIO. */
        if (idx == 256) continue;

        if (!(pml4[idx] & PFLAG_PRESENT)) {
            /* canonical high-half address for this PML4 index */
            uint64_t base = 0xFFFF000000000000ULL | ((uint64_t)idx << 39);
            return base;
        }
    }

    return 0;
}

void* ioremap(uint64_t phys_addr, uint64_t size) {
    if (kernel_debug_is_on()) {
        com_write_string(COM1_PORT, "[IOREMAP] Called with phys=0x");
        com_printf(COM1_PORT, "%08x", (uint32_t)(phys_addr >> 32));
        com_printf(COM1_PORT, "%08x", (uint32_t)(phys_addr & 0xFFFFFFFF));
        com_printf(COM1_PORT, ", size=0x%x\n", (uint32_t)size);
    }

    if (!pml4) {
        if (kernel_debug_is_on()) {
            com_write_string(COM1_PORT, "[IOREMAP] PML4 is NULL, calling paging_init()...\n");
        }
        paging_init();
        if (!pml4) {
            com_write_string(COM1_PORT, "[IOREMAP] ERROR: Failed to initialize paging\n");
            return NULL;
        }
    }
    
    // Align to page boundary
    uint64_t phys_base = phys_addr & PAGE_MASK;
    uint64_t offset = phys_addr & 0xFFF;
    uint64_t aligned_size = ((size + offset + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
    
    if (kernel_debug_is_on()) {
        com_printf(COM1_PORT, "[IOREMAP] Aligned phys=0x%08x, offset=0x%x, size=0x%x\n", 
                   (uint32_t)phys_base, (uint32_t)offset, (uint32_t)aligned_size);
    }

    /*
     * Initialize ioremap base on first use.
     * Defensive: if these statics are corrupted (e.g., .bss not cleared), force re-init.
     */
    if (ioremap_base == 0 || ioremap_next == 0 || !is_canonical_high(ioremap_base) || !is_canonical_high(ioremap_next)) {
        ioremap_base = pick_ioremap_base();
        if (ioremap_base == 0) {
            com_write_string(COM1_PORT, "[IOREMAP] ERROR: No free PML4 slot for ioremap\n");
            return NULL;
        }

        /* Extra safety: never allow ioremap to use the kernel heap's high-half base. */
        if (ioremap_base == 0xFFFF800000000000ULL) {
            com_write_string(COM1_PORT, "[IOREMAP] ERROR: ioremap_base collided with KHEAP_START; refusing\n");
            return NULL;
        }

        /* keep first few pages unused (guard) */
        ioremap_next = ioremap_base + 0x10000ULL;
        if (kernel_debug_is_on()) {
            com_write_string(COM1_PORT, "[IOREMAP] Selected ioremap_base = ");
            char tmpbuf[32];
            format_hex64(tmpbuf, ioremap_base);
            com_write_string(COM1_PORT, tmpbuf);
            com_write_string(COM1_PORT, "\n");
        }
    }

    /* Hard safety: never hand out low addresses from ioremap */
    if (!is_canonical_high(ioremap_next)) {
        com_write_string(COM1_PORT, "[IOREMAP] WARNING: ioremap_next was non-canonical; reinitializing\n");
        ioremap_base = 0;
        ioremap_next = 0;
        return ioremap(phys_addr, size);
    }

    // Allocate virtual address from MMIO region
    uint64_t virt_base = ioremap_next;
    ioremap_next += aligned_size;

    if (kernel_debug_is_on()) {
        com_write_string(COM1_PORT, "[IOREMAP] Allocated virtual range: 0x");
        com_printf(COM1_PORT, "%08x%08x - 0x", 
                   (uint32_t)(virt_base >> 32), (uint32_t)(virt_base & 0xFFFFFFFF));
        com_printf(COM1_PORT, "%08x%08x\n",
                   (uint32_t)((virt_base + aligned_size - 1) >> 32),
                   (uint32_t)((virt_base + aligned_size - 1) & 0xFFFFFFFF));
    }

    // Set MMIO flags: Present, Writable, Cache Disable, Write-Through
    uint64_t flags = PFLAG_WRITABLE | PFLAG_PWT | PFLAG_PCD;

    if (kernel_debug_is_on()) {
        com_printf(COM1_PORT, "[IOREMAP] Mapping with flags: 0x%x (W+PCD+PWT)\n", (uint32_t)flags);
    }

    // Map each page
    uint64_t pages = aligned_size / PAGE_SIZE;
    if (kernel_debug_is_on()) {
        com_printf(COM1_PORT, "[IOREMAP] Mapping %d pages...\n", (uint32_t)pages);
    }
    
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t vaddr = virt_base + (i * PAGE_SIZE);
        uint64_t paddr = phys_base + (i * PAGE_SIZE);
        
        int result = paging_map_page(vaddr, paddr, flags);
        if (result != 0) {
            com_printf(COM1_PORT, "[IOREMAP] ERROR: Failed to map page %d (result=%d)\n",
                       (uint32_t)i, result);
            com_write_string(COM1_PORT, "[IOREMAP]   Virtual = 0x");
            com_printf(COM1_PORT, "%08x%08x\n", 
                       (uint32_t)(vaddr >> 32), (uint32_t)(vaddr & 0xFFFFFFFF));
            com_write_string(COM1_PORT, "[IOREMAP]   Physical = 0x");
            com_printf(COM1_PORT, "%08x%08x\n",
                       (uint32_t)(paddr >> 32), (uint32_t)(paddr & 0xFFFFFFFF));
            return NULL;
        }
    }

    if (kernel_debug_is_on()) {
        com_write_string(COM1_PORT, "[IOREMAP] All pages mapped successfully\n");
    }

    /* paging_map_page() already invalidates each page via invlpg.
     * A full CR3 reload here is unnecessary and can be risky on some setups.
     */
    __asm__ volatile("mfence" ::: "memory");

    uint64_t result_virt = virt_base + offset;
    if (kernel_debug_is_on()) {
        com_write_string(COM1_PORT, "[IOREMAP] Returning virtual address: 0x");
        com_printf(COM1_PORT, "%08x%08x\n",
                   (uint32_t)(result_virt >> 32), (uint32_t)(result_virt & 0xFFFFFFFF));

        // Verify the mapping by checking the page tables
        com_write_string(COM1_PORT, "[IOREMAP] Verifying mapping with virt_to_phys...\n");
    }
    if (kernel_debug_is_on()) {
        uint64_t verify_phys = paging_virt_to_phys(result_virt);
        if (verify_phys != phys_addr) {
            com_write_string(COM1_PORT, "[IOREMAP] WARNING: virt_to_phys returned 0x");
            com_printf(COM1_PORT, "%08x%08x, expected 0x%08x%08x\n",
                       (uint32_t)(verify_phys >> 32), (uint32_t)(verify_phys & 0xFFFFFFFF),
                       (uint32_t)(phys_addr >> 32), (uint32_t)(phys_addr & 0xFFFFFFFF));
            if (verify_phys == 0) {
                com_write_string(COM1_PORT, "[IOREMAP] ERROR: Mapping verification failed - page not present!\n");
                return NULL;
            }
        } else {
            com_write_string(COM1_PORT, "[IOREMAP] Mapping verification OK\n");
        }
    }
    
    return (void*)result_virt;
}

void* ioremap_guarded(uint64_t phys_addr, uint64_t size) {
    void *p = ioremap(phys_addr, size);
    if (!p) return NULL;

    // Leave one unmapped guard page after the most-recent ioremap() allocation.
    // ioremap() advanced ioremap_next by the aligned mapping size; advance it by an
    // extra page so future ioremap() allocations won't immediately occupy the guard.
    ioremap_next += PAGE_SIZE;

    if (kernel_debug_is_on()) {
        com_write_string(COM1_PORT, "[IOREMAP] Guard page reserved after mapping\n");
    }

    return p;
}