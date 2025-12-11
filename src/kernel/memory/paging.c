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

/* phys_to_virt offset. Starts at 0 (identity). */
static uint64_t phys_offset = 0; /* 0 means identity mapping */

/* MMIO virtual address space tracking */
#define IOREMAP_BASE 0xFFFFFF8000000000ULL
static uint64_t ioremap_next = IOREMAP_BASE;

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

static uint64_t *alloc_pt_page(void) {
    uint64_t phys = phys_alloc_frame();
    if (!phys) return NULL;

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

void paging_init(void) {
    if (pml4) {
        com_write_string(COM1_PORT, "[PAGING] Already initialized\n");
        return;
    }

    com_write_string(COM1_PORT, "[PAGING] Initializing AMD64 paging...\n");

    uint64_t old_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(old_cr3));

    com_write_string(COM1_PORT, "[PAGING] Bootloader CR3: ");
    char tmpbuf[32];
    format_hex64(tmpbuf, old_cr3);
    com_write_string(COM1_PORT, tmpbuf);
    com_write_string(COM1_PORT, "\n");

    // CRITICAL FIX: Just use the bootloader's PML4 directly!
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
}

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

void* ioremap(uint64_t phys_addr, uint64_t size) {
    com_write_string(COM1_PORT, "[IOREMAP] Called with phys=0x");
    com_printf(COM1_PORT, "%08x", (uint32_t)(phys_addr >> 32));
    com_printf(COM1_PORT, "%08x", (uint32_t)(phys_addr & 0xFFFFFFFF));
    com_printf(COM1_PORT, ", size=0x%x\n", (uint32_t)size);
    
    if (!pml4) {
        com_write_string(COM1_PORT, "[IOREMAP] PML4 is NULL, calling paging_init()...\n");
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
    
    com_printf(COM1_PORT, "[IOREMAP] Aligned phys=0x%08x, offset=0x%x, size=0x%x\n", 
               (uint32_t)phys_base, (uint32_t)offset, (uint32_t)aligned_size);
    
    // Allocate virtual address from high memory region
    uint64_t virt_base = ioremap_next;
    ioremap_next += aligned_size;
    
    com_write_string(COM1_PORT, "[IOREMAP] Allocated virtual range: 0x");
    com_printf(COM1_PORT, "%08x%08x - 0x", 
               (uint32_t)(virt_base >> 32), (uint32_t)(virt_base & 0xFFFFFFFF));
    com_printf(COM1_PORT, "%08x%08x\n",
               (uint32_t)((virt_base + aligned_size - 1) >> 32),
               (uint32_t)((virt_base + aligned_size - 1) & 0xFFFFFFFF));
    
    // Set MMIO flags: Present, Writable, Cache Disable, Write-Through
    uint64_t flags = PFLAG_WRITABLE | PFLAG_PWT | PFLAG_PCD;
    
    com_printf(COM1_PORT, "[IOREMAP] Mapping with flags: 0x%x (W+PCD+PWT)\n", (uint32_t)flags);
    
    // Map each page
    uint64_t pages = aligned_size / PAGE_SIZE;
    com_printf(COM1_PORT, "[IOREMAP] Mapping %d pages...\n", (uint32_t)pages);
    
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
    
    com_write_string(COM1_PORT, "[IOREMAP] All pages mapped successfully\n");
    
    // CRITICAL: Flush TLB for entire mapped region
    com_write_string(COM1_PORT, "[IOREMAP] Flushing TLB...\n");
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t vaddr = virt_base + (i * PAGE_SIZE);
        __asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
    }
    
    // Also do a full CR3 reload to be absolutely sure
    __asm__ volatile(
        "mov %%cr3, %%rax\n"
        "mov %%rax, %%cr3\n"
        ::: "rax", "memory"
    );
    
    com_write_string(COM1_PORT, "[IOREMAP] TLB flushed\n");
    
    // Add memory fence to ensure all writes are complete
    __asm__ volatile("mfence" ::: "memory");
    
    uint64_t result_virt = virt_base + offset;
    com_write_string(COM1_PORT, "[IOREMAP] Returning virtual address: 0x");
    com_printf(COM1_PORT, "%08x%08x\n",
               (uint32_t)(result_virt >> 32), (uint32_t)(result_virt & 0xFFFFFFFF));
    
    // Verify the mapping by checking the page tables
    com_write_string(COM1_PORT, "[IOREMAP] Verifying mapping with virt_to_phys...\n");
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
    
    return (void*)result_virt;
}