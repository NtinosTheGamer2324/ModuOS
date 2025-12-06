/*
 * paging.c — 4-level x86_64 paging helpers (4 KiB pages)
 *
 * Robust phys->virt handling:
 *  - Default: use identity mapping (safe for early boot when bootloader left identity).
 *  - Allow switching to a higher-half direct map at runtime via paging_set_phys_offset().
 *
 * Integration:
 *  - If you later create a direct-map region (phys -> virt at phys + OFFSET),
 *    call paging_set_phys_offset(OFFSET).
 */

#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/COM/com.h"
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

/* phys_to_virt offset. Starts at 0 (identity). Kernel can call paging_set_phys_offset()
 * to switch this to a higher-half direct-map when that mapping is actually in place.
 */
static uint64_t phys_offset = 0; /* 0 means identity mapping */

/* API to set the phys->virt offset when direct map is established */
void paging_set_phys_offset(uint64_t offset) {
    phys_offset = offset;
}

/* Convert a physical address to the kernel virtual address where it's accessible.
 * If phys_offset == 0 we assume identity mapping (virt == phys).
 * If phys_offset != 0 we map as virt = phys + phys_offset.
 *
 * The caller must ensure the chosen mapping is actually present before switching
 * phys_offset to a non-zero value.
 */
static inline void *phys_to_virt(uint64_t phys) {
    if (phys == 0) return NULL;
    if (phys_offset == 0) return (void *)(uintptr_t)phys;
    return (void *)(uintptr_t)(phys + phys_offset);
}

/* Allocate one physical page for a page-table and return its virtual pointer. */
static uint64_t *alloc_pt_page(void) {
    uint64_t phys = phys_alloc_frame();
    if (!phys) return NULL;

    void *v = phys_to_virt(phys);
    if (!v) {
        /* Should not happen if identity mapping is available for low memory.
         * If you switch to a non-zero phys_offset later you must ensure that
         * offset actually provides access to the returned phys frames.
         */
        return NULL;
    }

    memset(v, 0, PAGE_SIZE);
    return (uint64_t *)v;
}

uint64_t *paging_get_pml4(void) {
    return (uint64_t *)pml4;
}

uint64_t paging_get_pml4_phys(void) {
    return pml4_phys;
}

/* small helper to print 64-bit hex into buffer (buf must be >=19 bytes) */
static void format_hex64(char *buf, uint64_t v) {
    const char hex[] = "0123456789abcdef";
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        buf[2 + i] = hex[(v >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = 0;
}

void paging_init(void) {
    if (pml4) {
        com_write_string(COM1_PORT, "[PAGING] Already initialized\n");
        return;
    }

    com_write_string(COM1_PORT, "[PAGING] Allocating PML4...\n");

    /* Get current CR3 from bootloader (physical address) */
    uint64_t old_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(old_cr3));

    com_write_string(COM1_PORT, "[PAGING] Bootloader CR3: ");
    char tmpbuf[32];
    format_hex64(tmpbuf, old_cr3);
    com_write_string(COM1_PORT, tmpbuf);
    com_write_string(COM1_PORT, "\n");

    /* allocate a PML4 page */
    uint64_t *new_pml4_virt = alloc_pt_page();
    if (!new_pml4_virt) {
        com_write_string(COM1_PORT, "[PAGING] FATAL: Cannot allocate PML4!\n");
        com_write_string(COM1_PORT, "[PAGING] phys_alloc_frame() returned 0\n");
        return;
    }

    /* compute physical address of allocated page (virt - phys_offset if non-zero) */
    uint64_t new_pml4_phys;
    {
        uint64_t v = (uint64_t)(uintptr_t)new_pml4_virt;
        if (phys_offset == 0) new_pml4_phys = v;
        else new_pml4_phys = v - phys_offset;
    }

    pml4 = new_pml4_virt;
    pml4_phys = new_pml4_phys;

    com_write_string(COM1_PORT, "[PAGING] New PML4 allocated at: ");
    format_hex64(tmpbuf, pml4_phys);
    com_write_string(COM1_PORT, tmpbuf);
    com_write_string(COM1_PORT, "\n");

    com_write_string(COM1_PORT, "[PAGING] Copying bootloader's page table entries...\n");

    /* old_cr3 is a physical address of the bootloader PML4; access via phys_to_virt */
    uint64_t *old_pml4 = (uint64_t *)phys_to_virt(old_cr3 & PAGE_MASK);
    if (!old_pml4) {
        com_write_string(COM1_PORT, "[PAGING] FATAL: cannot access old PML4 (phys_to_virt failed)\n");
        return;
    }

    for (int i = 0; i < PT_ENTRIES; ++i) {
        if (old_pml4[i] & PFLAG_PRESENT) pml4[i] = old_pml4[i];
        else pml4[i] = 0;
    }

    com_write_string(COM1_PORT, "[PAGING] Loading new CR3...\n");

    /* Load CR3 with physical address of our new PML4 */
    __asm__ volatile("mov %0, %%cr3" :: "r"(pml4_phys) : "memory");

    com_write_string(COM1_PORT, "[PAGING] CR3 loaded successfully!\n");
}

/* Get or create next-level page table.
 * 'table' is a virtual pointer to a page-table page (so we index it directly).
 */
static uint64_t *get_or_create(uint64_t *table, unsigned idx) {
    uint64_t ent = table[idx];
    if (ent & PFLAG_PRESENT) {
        uint64_t phys = ent & PAGE_MASK;
        return (uint64_t *)phys_to_virt(phys);
    } else {
        uint64_t *next = alloc_pt_page();
        if (!next) return NULL;
        uint64_t v = (uint64_t)(uintptr_t)next;
        uint64_t next_phys = (phys_offset == 0) ? v : (v - phys_offset);
        table[idx] = (next_phys & PAGE_MASK) | (PFLAG_PRESENT | PFLAG_WRITABLE);
        return next;
    }
}

int paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!pml4) paging_init();
    if (!pml4) return -1;

    unsigned i4 = (virt >> 39) & 0x1FF;
    unsigned i3 = (virt >> 30) & 0x1FF;
    unsigned i2 = (virt >> 21) & 0x1FF;
    unsigned i1 = (virt >> 12) & 0x1FF;

    uint64_t *pdpt = get_or_create(pml4, i4);
    if (!pdpt) return -1;
    uint64_t *pd = get_or_create(pdpt, i3);
    if (!pd) return -1;
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

int paging_map_range(uint64_t virt_base, uint64_t phys_base, uint64_t size, uint64_t flags) {
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    char tmp[32];
    for (uint64_t i = 0; i < pages; ++i) {
        uint64_t vaddr = virt_base + i * PAGE_SIZE;
        uint64_t paddr = phys_base + i * PAGE_SIZE;
        if (paging_map_page(vaddr, paddr, flags) != 0) {
            com_write_string(COM1_PORT, "[PAGING] Failed to map page at virt=0x");
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
    }
    return 0;
}

uint64_t paging_virt_to_phys(uint64_t virt) {
    // Get current PML4
    uint64_t *pml4 = paging_get_pml4();
    
    // Extract indices from virtual address
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;
    uint64_t offset   = virt & 0xFFF;
    
    // Check PML4 entry
    if (!(pml4[pml4_idx] & PFLAG_PRESENT)) {
        return 0; // Not mapped
    }
    
    // Get PDPT
    uint64_t *pdpt = (uint64_t*)(pml4[pml4_idx] & 0xFFFFFFFFF000ULL);
    if (!(pdpt[pdpt_idx] & PFLAG_PRESENT)) {
        return 0; // Not mapped
    }
    
    // Get PD
    uint64_t *pd = (uint64_t*)(pdpt[pdpt_idx] & 0xFFFFFFFFF000ULL);
    if (!(pd[pd_idx] & PFLAG_PRESENT)) {
        return 0; // Not mapped
    }
    
    // Check for 2MB page
    if (pd[pd_idx] & (1ULL << 7)) {
        // 2MB page
        uint64_t page_phys = pd[pd_idx] & 0xFFFFFFFE00000ULL;
        uint64_t page_offset = virt & 0x1FFFFF;
        return page_phys | page_offset;
    }
    
    // Get PT
    uint64_t *pt = (uint64_t*)(pd[pd_idx] & 0xFFFFFFFFF000ULL);
    if (!(pt[pt_idx] & PFLAG_PRESENT)) {
        return 0; // Not mapped
    }
    
    // Get physical address
    uint64_t page_phys = pt[pt_idx] & 0xFFFFFFFFF000ULL;
    return page_phys | offset;
}
