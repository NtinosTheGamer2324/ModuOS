#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/debug.h"
#include <stdint.h>
#include <stddef.h>
#include "moduos/kernel/memory/string.h"

/* SMP Safety: Track whether SMP has started */
static int paging_smp_started = 0;

#define ASSERT_BSP_ONLY() do { \
    if (paging_smp_started) { \
        com_write_string(COM1_PORT, "PANIC: Paging operation after SMP start!\n"); \
        for(;;) __asm__ volatile("hlt"); \
    } \
} while(0)

void paging_set_smp_started(void) {
    paging_smp_started = 1;
}

#define PT_ENTRIES 512
#define PAGE_MASK (~0xFFFULL)
#define PAGE_SIZE  4096ULL
#define PAGE_MASK_2M (~0x1FFFFFULL)  /* 2MB page mask */
#define PAGING_PT_ALLOC_LIMIT (1ULL * 1024 * 1024 * 1024)  /* 1 GiB */

/* VGA text buffer is at 0xB8000 - we must preserve this! */
#define VGA_TEXT_BUFFER 0xB8000ULL
#define VGA_TEXT_SIZE   0x1000ULL  /* 4KB */

static uint64_t *pml4 = NULL;       /* virtual pointer (via phys_to_virt) to PML4 */
static uint64_t pml4_phys = 0;      /* physical address of PML4 */

/* Master kernel CR3 - set during boot and used as reference for copying kernel mappings */
static uint64_t kernel_master_cr3 = 0;

/* phys_to_virt offset. Starts at 0 (identity). */
static uint64_t phys_offset = 0; /* 0 means identity mapping */
static int pt_alloc_unrestricted = 0;

/* MMIO virtual address space tracking */
static uint64_t ioremap_base = 0;
static uint64_t ioremap_next = 0;

/* A tiny kernel scratch mapping area (2 pages). */
static uint64_t scratch_base = 0;

void paging_set_phys_offset(uint64_t offset) {
    phys_offset = offset;
}

static inline void *phys_to_virt(uint64_t phys) {
    if (phys == 0) return NULL;
    if (phys_offset == 0) return (void *)(uintptr_t)phys;
    return (void *)(uintptr_t)(phys + phys_offset);
}

void *phys_to_virt_kernel(uint64_t phys) {
    return phys_to_virt(phys);
}

void paging_set_pt_alloc_unrestricted(void) {
    pt_alloc_unrestricted = 1;
}

static uint64_t *alloc_pt_page(void) {
    uint64_t phys = pt_alloc_unrestricted
        ? phys_alloc_frame()
        : phys_alloc_frame_below(PAGING_PT_ALLOC_LIMIT);

    if (!phys) return NULL;
    void *v = phys_to_virt(phys);
    if (!v) { phys_free_frame(phys); return NULL; }
    memset(v, 0, PAGE_SIZE);
    return (uint64_t *)v;
}

uint64_t *paging_get_pml4(void) {
    return (uint64_t *)pml4;
}

uint64_t paging_get_pml4_phys(void) {
    return pml4_phys;
}

uint64_t paging_get_master_cr3(void) {
    return kernel_master_cr3;
}

void paging_switch_cr3(uint64_t new_cr3_phys) {
    if (!new_cr3_phys) return;
    __asm__ volatile("mov %0, %%cr3" :: "r"(new_cr3_phys) : "memory");
    pml4_phys = new_cr3_phys & 0xFFFFFFFFFFFFF000ULL;
    pml4 = (uint64_t*)phys_to_virt(pml4_phys);
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

    uint64_t old_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(old_cr3));
    if (!kernel_master_cr3) {
        kernel_master_cr3 = old_cr3 & PAGE_MASK;
    }

    ioremap_base = 0;
    ioremap_next = 0;
    scratch_base = 0;

    if (kernel_debug_is_on()) {
        com_write_string(COM1_PORT, "[PAGING] Initializing AMD64 paging...\n");
    }

    com_write_string(COM1_PORT, "[PAGING] Bootloader CR3: ");
    char tmpbuf[32];
    format_hex64(tmpbuf, old_cr3);
    com_write_string(COM1_PORT, tmpbuf);
    com_write_string(COM1_PORT, "\n");

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

    paging_reserve_bootloader_tables();
}

static void paging_reserve_bootloader_tables(void) {
    if (!pml4 || !pml4_phys) return;

    phys_reserve_range(pml4_phys, PAGE_SIZE);

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
            if (e3 & (1ULL<<7)) continue;
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
                if (e2 & (1ULL<<7)) continue;
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

    if ((ent & PFLAG_PRESENT) && (ent & (1ULL << 7))) {
        if (kernel_debug_is_on()) {
            com_write_string(COM1_PORT, "[PAGING] get_or_create: encountered huge page entry; split required\n");
        }
        return NULL;
    }

    if (ent & PFLAG_PRESENT) {
        uint64_t phys = ent & PAGE_MASK;
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

/*
 * paging_map_page — always reads the LIVE CR3 rather than trusting the stale
 * global pml4 pointer.  The global pointer goes stale whenever a context switch
 * happens without a paging_switch_cr3() call (e.g. the scheduler's direct CR3
 * write).  Using it would walk the wrong page tables and silently write PTEs
 * into a dead process's address space, causing the "map reported success but
 * page not present" FATAL on the next kmalloc verification.
 */
int paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {

    /* Read the live CR3 every time — never trust the stale global pml4. */
    uint64_t live_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(live_cr3));
    live_cr3 &= 0xFFFFFFFFFFFFF000ULL;
    uint64_t *live_pml4 = (uint64_t *)phys_to_virt(live_cr3);
    if (!live_pml4) return -1;

    unsigned i4 = (virt >> 39) & 0x1FF;
    unsigned i3 = (virt >> 30) & 0x1FF;
    unsigned i2 = (virt >> 21) & 0x1FF;
    unsigned i1 = (virt >> 12) & 0x1FF;

    /* Never allow user mappings in kernel half */
    if ((flags & PFLAG_USER) && i4 >= 256) {
        return -1;
    }

    uint64_t *pdpt = get_or_create(live_pml4, i4);
    if (!pdpt) return -1;

    if (flags & PFLAG_USER) {
        live_pml4[i4] |= PFLAG_USER;
    }

    /* Mirror high-half PML4 entries into kernel_master_cr3 so that heap
     * allocations made while running on a process CR3 are visible in all
     * future process PML4s (which copy from kernel_master_cr3 at fork time). */
    if (i4 >= 256 && kernel_master_cr3) {
        uint64_t master_phys = kernel_master_cr3 & 0xFFFFFFFFFFFFF000ULL;
        if (live_cr3 != master_phys) {
            uint64_t *master = (uint64_t *)phys_to_virt(master_phys);
            if (master) {
                master[i4] = live_pml4[i4];
            }
        }
    }

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

    /* Split 1GB huge page at PDPT level if present */
    uint64_t ent3 = pdpt[i3];
    if ((ent3 & PFLAG_PRESENT) && (ent3 & (1ULL << 7))) {
        uint64_t *new_pd = alloc_pt_page();
        if (!new_pd) return -1;
        uint64_t new_pd_phys = (phys_offset == 0) ? (uint64_t)(uintptr_t)new_pd : ((uint64_t)(uintptr_t)new_pd - phys_offset);

        uint64_t base_phys  = ent3 & 0xFFFFFFFFC0000000ULL;
        uint64_t huge_flags = ent3 & 0xFFFULL;
        for (int j = 0; j < 512; j++)
            new_pd[j] = (base_phys + ((uint64_t)j << 21)) | huge_flags | (1ULL << 7);

        pdpt[i3] = (new_pd_phys & PAGE_MASK) | PFLAG_PRESENT | PFLAG_WRITABLE;
        __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
    }

    uint64_t *pd = get_or_create(pdpt, i3);
    if (!pd) return -1;

    if (flags & PFLAG_USER) {
        pdpt[i3] |= PFLAG_USER;
    }

    /* Split 2MB huge page at PD level if present */
    uint64_t ent2 = pd[i2];
    if ((ent2 & PFLAG_PRESENT) && (ent2 & (1ULL << 7))) {
        uint64_t *new_pt = alloc_pt_page();
        if (!new_pt) return -1;
        uint64_t new_pt_phys = (phys_offset == 0) ? (uint64_t)(uintptr_t)new_pt : ((uint64_t)(uintptr_t)new_pt - phys_offset);

        uint64_t base_phys  = ent2 & 0xFFFFFFFFFFE00000ULL;
        uint64_t base_flags = ent2 & 0xFFFULL;
        for (int j = 0; j < 512; j++)
            new_pt[j] = (base_phys + ((uint64_t)j << 12)) | (base_flags & ~((1ULL << 7))) | PFLAG_PRESENT;

        pd[i2] = (new_pt_phys & PAGE_MASK) | PFLAG_PRESENT | PFLAG_WRITABLE;
        __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
    }

    if (flags & PFLAG_USER) pd[i2] |= PFLAG_USER;

    uint64_t *pt = get_or_create(pd, i2);
    if (!pt) return -1;

    uint64_t entry = (phys & PAGE_MASK) | (flags & 0xFFFULL) | PFLAG_PRESENT;
    pt[i1] = entry;
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
    return 0;
}

int paging_map_range(uint64_t virt_base, uint64_t phys_base, uint64_t size, uint64_t flags) {
    unsigned i4 = (virt_base >> 39) & 0x1FF;
    
    if ((flags & PFLAG_USER) && i4 >= 256)
        return -1;

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

/*
 * paging_unmap_page — same live-CR3 fix as paging_map_page.
 * Also mirrors the cleared PML4 slot back into kernel_master_cr3 so that
 * kfree() unmaps from the authoritative kernel tables, not a stale process CR3.
 */
int paging_unmap_page(uint64_t virt) {
    uint64_t live_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(live_cr3));
    live_cr3 &= 0xFFFFFFFFFFFFF000ULL;
    uint64_t *live_pml4 = (uint64_t *)phys_to_virt(live_cr3);
    if (!live_pml4) return -1;

    unsigned i4 = (virt >> 39) & 0x1FF;
    unsigned i3 = (virt >> 30) & 0x1FF;
    unsigned i2 = (virt >> 21) & 0x1FF;
    unsigned i1 = (virt >> 12) & 0x1FF;

    uint64_t ent4 = live_pml4[i4];
    if (!(ent4 & PFLAG_PRESENT)) return -1;
    uint64_t *pdpt = (uint64_t *)phys_to_virt(ent4 & PAGE_MASK);
    if (!pdpt) return -1;

    uint64_t ent3 = pdpt[i3];
    if (!(ent3 & PFLAG_PRESENT)) return -1;
    if (ent3 & (1ULL << 7)) return -1; /* 1GiB huge page */

    uint64_t *pd = (uint64_t *)phys_to_virt(ent3 & PAGE_MASK);
    if (!pd) return -1;

    uint64_t ent2 = pd[i2];
    if (!(ent2 & PFLAG_PRESENT)) return -1;
    if (ent2 & (1ULL << 7)) return -1; /* 2MiB huge page */

    uint64_t *pt = (uint64_t *)phys_to_virt(ent2 & PAGE_MASK);
    if (!pt) return -1;

    pt[i1] = 0;
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");

    /* Mirror the updated high-half slot back into kernel_master_cr3. */
    if (i4 >= 256 && kernel_master_cr3) {
        uint64_t master_phys = kernel_master_cr3 & 0xFFFFFFFFFFFFF000ULL;
        if (live_cr3 != master_phys) {
            uint64_t *master = (uint64_t *)phys_to_virt(master_phys);
            if (master) master[i4] = live_pml4[i4];
        }
    }

    return 0;
}

int paging_map_2m_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    // Read live CR3 — never trust the stale global pml4
    uint64_t live_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(live_cr3));
    live_cr3 &= 0xFFFFFFFFFFFFF000ULL;
    uint64_t *live_pml4 = (uint64_t *)phys_to_virt(live_cr3);
    if (!live_pml4) return -1;

    // Check for 2MB alignment on both virtual and physical addresses
    if ((virt & 0x1FFFFFULL) || (phys & 0x1FFFFFULL)) return -1;

    // Extract table indices
    unsigned i4 = (virt >> 39) & 0x1FF;
    unsigned i3 = (virt >> 30) & 0x1FF;
    unsigned i2 = (virt >> 21) & 0x1FF;

    // Get or create the Page Directory Pointer Table (PDPT)
    uint64_t *pdpt = get_or_create(live_pml4, i4);
    if (!pdpt) return -1;

    // Handle case where a 1GB huge page is already mapped here
    uint64_t ent3 = pdpt[i3];
    if ((ent3 & PFLAG_PRESENT) && (ent3 & (1ULL << 7))) {
        uint64_t *new_pd = alloc_pt_page();
        if (!new_pd) return -1;
        uint64_t new_pd_phys = (phys_offset == 0) ? (uint64_t)(uintptr_t)new_pd : ((uint64_t)(uintptr_t)new_pd - phys_offset);

        uint64_t base_phys  = ent3 & 0xFFFFFFFFC0000000ULL;
        uint64_t huge_flags = ent3 & 0xFFFULL;
        
        // Split the 1GB page into 512 2MB pages
        for (int j = 0; j < 512; j++)
            new_pd[j] = (base_phys + ((uint64_t)j << 21)) | huge_flags | (1ULL << 7);

        // Update PDPT entry to point to the new Page Directory
        pdpt[i3] = (new_pd_phys & PAGE_MASK) | PFLAG_PRESENT | PFLAG_WRITABLE;
        __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
    }

    // Get or create the Page Directory (PD)
    uint64_t *pd = get_or_create(pdpt, i3);
    if (!pd) return -1;

    // Check existing 2MB mapping to avoid overwriting with a mismatch
    uint64_t ent2 = pd[i2];
    if (ent2 & PFLAG_PRESENT) {
        uint64_t existing_phys = ent2 & PAGE_MASK_2M;
        uint64_t requested_phys = phys & PAGE_MASK_2M;
        if (existing_phys == requested_phys) return 0; // Already mapped to the same phys addr
        return -1; // Collision
    }

    // Insert the new 2MB page entry (Setting bit 7 for PS - Page Size)
    uint64_t entry = (phys & 0xFFFFFFFFFFE00000ULL) | (flags & 0xFFFULL) | PFLAG_PRESENT | (1ULL << 7);
    pd[i2] = entry;
    
    // Invalidate the TLB for the updated virtual address
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

uint64_t paging_create_process_pml4(void) {
    if (!pml4) {
        paging_init();
        if (!pml4) return 0;
    }

    uint64_t *new_pml4 = alloc_pt_page();
    if (!new_pml4) return 0;

    uint64_t v = (uint64_t)(uintptr_t)new_pml4;
    uint64_t new_phys = (phys_offset == 0) ? v : (v - phys_offset);

    if (!kernel_master_cr3) {
        uint64_t current_cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
        com_write_string(COM1_PORT, "[PAGING] WARNING: kernel_master_cr3 unset at PML4 create time, using current CR3 0x");
        com_write_hex64(COM1_PORT, current_cr3);
        com_write_string(COM1_PORT, " (may be wrong if called from user context)\n");
        kernel_master_cr3 = current_cr3;
    }

    uint64_t master_pml4_phys = kernel_master_cr3 & 0xFFFFFFFFFFFFF000ULL;
    uint64_t *master_pml4 = (uint64_t*)phys_to_virt(master_pml4_phys);
    if (!master_pml4) {
        phys_free_frame(new_phys);
        return 0;
    }

    int high_half_copied = 0;
    for (int i = 0; i < PT_ENTRIES; ++i) {
        uint64_t e = master_pml4[i];
        if (i >= 256) {
            new_pml4[i] = e;
            if (e & PFLAG_PRESENT) high_half_copied++;
        } else {
            if ((e & PFLAG_PRESENT) && !(e & PFLAG_USER)) {
                new_pml4[i] = e;
            } else {
                new_pml4[i] = 0;
            }
        }
    }

    if (kernel_debug_is_on()) {
        char tmpbuf[32];
        com_write_string(COM1_PORT, "[PAGING] Copied ");
        format_hex64(tmpbuf, high_half_copied);
        com_write_string(COM1_PORT, tmpbuf);
        com_write_string(COM1_PORT, " present high-half entries\n");
        com_write_string(COM1_PORT, "[PAGING] Created process PML4 at ");
        format_hex64(tmpbuf, new_phys);
        com_write_string(COM1_PORT, tmpbuf);
        com_write_string(COM1_PORT, "\n");
    }

    return new_phys;
}

static uint64_t *get_or_create_in_pml4(uint64_t *pml4_virt, unsigned idx4) {
    if (idx4 >= 256) return NULL;

    uint64_t ent = pml4_virt[idx4];
    if (ent & PFLAG_PRESENT) {
        if (!(ent & PFLAG_USER)) {
            uint64_t *next = alloc_pt_page();
            if (!next) return NULL;
            uint64_t v = (uint64_t)(uintptr_t)next;
            uint64_t next_phys = (phys_offset == 0) ? v : (v - phys_offset);
            pml4_virt[idx4] = (next_phys & PAGE_MASK) | (PFLAG_PRESENT | PFLAG_WRITABLE | PFLAG_USER);
            return next;
        }
        uint64_t phys = ent & PAGE_MASK;
        return (uint64_t *)phys_to_virt(phys);
    } else {
        uint64_t *next = alloc_pt_page();
        if (!next) return NULL;
        uint64_t v = (uint64_t)(uintptr_t)next;
        uint64_t next_phys = (phys_offset == 0) ? v : (v - phys_offset);
        pml4_virt[idx4] = (next_phys & PAGE_MASK) | (PFLAG_PRESENT | PFLAG_WRITABLE | PFLAG_USER);
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
    
        if ((flags & PFLAG_USER) && i4 >= 256) {
            return -1;
        }
        uint64_t *pdpt = get_or_create_in_pml4(pml4_virt, i4);
        if (!pdpt) return -1;
        if (flags & PFLAG_USER) pml4_virt[i4] |= PFLAG_USER;

        uint64_t *pd = get_or_create(pdpt, i3);
        if (!pd) return -1;
        if (flags & PFLAG_USER) pdpt[i3] |= PFLAG_USER;

        uint64_t *pt = get_or_create(pd, i2);
        if (!pt) return -1;
        if (flags & PFLAG_USER) pd[i2] |= PFLAG_USER;

        uint64_t entry = (paddr & PAGE_MASK) | (flags & 0xFFFULL) | PFLAG_PRESENT;
        pt[i1] = entry;
        if (flags & PFLAG_USER) pt[i1] |= PFLAG_USER;
    }
    return 0;
}

uint64_t paging_get_pte(uint64_t virt) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t cur_pml4_phys = cr3 & 0xFFFFFFFFFFFFF000ULL;
    uint64_t *current_pml4 = (uint64_t*)phys_to_virt(cur_pml4_phys);
    if (!current_pml4) return 0;

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t pml4_entry = current_pml4[pml4_idx];
    if (!(pml4_entry & PFLAG_PRESENT)) return 0;

    uint64_t pdpt_phys = pml4_entry & 0xFFFFFFFFFFFFF000ULL;
    uint64_t *pdpt = (uint64_t*)phys_to_virt(pdpt_phys);
    if (!pdpt) return 0;
    uint64_t pdpt_entry = pdpt[pdpt_idx];
    if (!(pdpt_entry & PFLAG_PRESENT)) return 0;
    if (pdpt_entry & (1ULL << 7)) return pdpt_entry;

    uint64_t pd_phys = pdpt_entry & 0xFFFFFFFFFFFFF000ULL;
    uint64_t *pd = (uint64_t*)phys_to_virt(pd_phys);
    if (!pd) return 0;
    uint64_t pd_entry = pd[pd_idx];
    if (!(pd_entry & PFLAG_PRESENT)) return 0;
    if (pd_entry & (1ULL << 7)) return pd_entry;

    uint64_t pt_phys = pd_entry & 0xFFFFFFFFFFFFF000ULL;
    uint64_t *pt = (uint64_t*)phys_to_virt(pt_phys);
    if (!pt) return 0;
    return pt[pt_idx];
}

int paging_set_pte(uint64_t virt, uint64_t pte) {
    if (!pml4) paging_init();
    if (!pml4) return -1;

    unsigned i4 = (virt >> 39) & 0x1FF;
    unsigned i3 = (virt >> 30) & 0x1FF;
    unsigned i2 = (virt >> 21) & 0x1FF;
    unsigned i1 = (virt >> 12) & 0x1FF;

    uint64_t *pdpt = get_or_create(pml4, i4);
    if (!pdpt) return -1;
    if (pte & PFLAG_USER) pml4[i4] |= PFLAG_USER;

    uint64_t *pd = get_or_create(pdpt, i3);
    if (!pd) return -1;
    if (pte & PFLAG_USER) pdpt[i3] |= PFLAG_USER;

    uint64_t *pt = get_or_create(pd, i2);
    if (!pt) return -1;
    if (pte & PFLAG_USER) pd[i2] |= PFLAG_USER;

    pt[i1] = pte;
    __asm__ volatile("invlpg (%0)" :: "r"(virt & ~0xFFFULL) : "memory");
    return 0;
}

uint64_t paging_get_flags(uint64_t virt) {
    uint64_t *current_pml4 = paging_get_pml4();
    if (!current_pml4) {
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        uint64_t cur_pml4_phys = cr3 & 0xFFFFFFFFFFFFF000ULL;
        current_pml4 = (uint64_t*)phys_to_virt(cur_pml4_phys);
        if (!current_pml4) return 0;
    }

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t pml4_entry = current_pml4[pml4_idx];
    if (!(pml4_entry & PFLAG_PRESENT)) return 0;

    uint64_t pdpt_phys = pml4_entry & 0xFFFFFFFFFFFFF000ULL;
    uint64_t *pdpt = (uint64_t*)phys_to_virt(pdpt_phys);
    if (!pdpt) return 0;
    uint64_t pdpt_entry = pdpt[pdpt_idx];
    if (!(pdpt_entry & PFLAG_PRESENT)) return 0;
    if (pdpt_entry & (1ULL << 7)) return pdpt_entry & 0xFFFULL;

    uint64_t pd_phys = pdpt_entry & 0xFFFFFFFFFFFFF000ULL;
    uint64_t *pd = (uint64_t*)phys_to_virt(pd_phys);
    if (!pd) return 0;
    uint64_t pd_entry = pd[pd_idx];
    if (!(pd_entry & PFLAG_PRESENT)) return 0;
    if (pd_entry & (1ULL << 7)) return pd_entry & 0xFFFULL;

    uint64_t pt_phys = pd_entry & 0xFFFFFFFFFFFFF000ULL;
    uint64_t *pt = (uint64_t*)phys_to_virt(pt_phys);
    if (!pt) return 0;
    uint64_t pt_entry = pt[pt_idx];
    if (!(pt_entry & PFLAG_PRESENT)) return 0;
    return pt_entry & 0xFFFULL;
}

uint64_t paging_virt_to_phys(uint64_t virt) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    uint64_t cur_pml4_phys = cr3 & 0xFFFFFFFFFFFFF000ULL;
    uint64_t *current_pml4 = (uint64_t*)phys_to_virt(cur_pml4_phys);
    if (!current_pml4) return 0;

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;
    uint64_t offset   = virt & 0xFFF;

    uint64_t pml4_entry = current_pml4[pml4_idx];
    if (!(pml4_entry & PFLAG_PRESENT)) return 0;

    uint64_t pdpt_phys = pml4_entry & 0xFFFFFFFFFFFFF000ULL;
    uint64_t *pdpt = (uint64_t*)phys_to_virt(pdpt_phys);
    if (!pdpt) return 0;

    uint64_t pdpt_entry = pdpt[pdpt_idx];
    if (!(pdpt_entry & PFLAG_PRESENT)) return 0;
    if (pdpt_entry & (1ULL << 7)) {
        uint64_t page_phys   = pdpt_entry & 0xFFFFFFFFC0000000ULL;
        uint64_t page_offset = virt & 0x3FFFFFFF;
        return page_phys | page_offset;
    }

    uint64_t pd_phys = pdpt_entry & 0xFFFFFFFFFFFFF000ULL;
    uint64_t *pd = (uint64_t*)phys_to_virt(pd_phys);
    if (!pd) return 0;

    uint64_t pd_entry = pd[pd_idx];
    if (!(pd_entry & PFLAG_PRESENT)) return 0;
    if (pd_entry & (1ULL << 7)) {
        uint64_t page_phys   = pd_entry & 0xFFFFFFFFFFE00000ULL;
        uint64_t page_offset = virt & 0x1FFFFF;
        return page_phys | page_offset;
    }

    uint64_t pt_phys = pd_entry & 0xFFFFFFFFFFFFF000ULL;
    uint64_t *pt = (uint64_t*)phys_to_virt(pt_phys);
    if (!pt) return 0;

    uint64_t pt_entry = pt[pt_idx];
    if (!(pt_entry & PFLAG_PRESENT)) return 0;

    return (pt_entry & 0xFFFFFFFFFFFFF000ULL) | offset;
}

static int is_canonical_high(uint64_t v) {
    return ((v >> 48) == 0xFFFFULL);
}

static uint64_t pick_ioremap_base(void) {
    if (!pml4) return 0;
    for (int idx = 510; idx >= 256; --idx) {
        if (idx == 256) continue;
        if (!(pml4[idx] & PFLAG_PRESENT)) {
            return 0xFFFF000000000000ULL | ((uint64_t)idx << 39);
        }
    }
    return 0;
}

static uint64_t pick_scratch_base(void) {
    if (!pml4) return 0;
    for (int idx = 509; idx >= 256; --idx) {
        if (idx == 256) continue;
        if (ioremap_base && ((ioremap_base >> 39) & 0x1FF) == (uint64_t)idx) continue;
        if (!(pml4[idx] & PFLAG_PRESENT)) {
            return 0xFFFF000000000000ULL | ((uint64_t)idx << 39);
        }
    }
    return 0;
}

uint64_t paging_get_scratch_base(void) {
    if (!scratch_base) {
        if (!pml4) {
            paging_init();
            if (!pml4) return 0;
        }
        scratch_base = pick_scratch_base();
        if (!scratch_base) return 0;
        scratch_base += 0x2000ULL;
    }
    return scratch_base;
}

void* ioremap(uint64_t phys_addr, uint64_t size) {
    ASSERT_BSP_ONLY();

    com_write_string(COM1_PORT, "[IOREMAP] phys=");
    com_printf(COM1_PORT, "0x%08x%08x size=0x%x\n",
               (uint32_t)(phys_addr >> 32), (uint32_t)(phys_addr & 0xFFFFFFFFu),
               (uint32_t)size);
    if (kernel_debug_is_on()) {
        com_write_string(COM1_PORT, "[IOREMAP] Called with phys=0x");
        com_printf(COM1_PORT, "%08x", (uint32_t)(phys_addr >> 32));
        com_printf(COM1_PORT, "%08x", (uint32_t)(phys_addr & 0xFFFFFFFF));
        com_printf(COM1_PORT, ", size=0x%x\n", (uint32_t)size);
    }

    if (!pml4) {
        if (kernel_debug_is_on())
            com_write_string(COM1_PORT, "[IOREMAP] PML4 is NULL, calling paging_init()...\n");
        paging_init();
        if (!pml4) {
            com_write_string(COM1_PORT, "[IOREMAP] ERROR: Failed to initialize paging\n");
            return NULL;
        }
    }

    uint64_t phys_base   = phys_addr & PAGE_MASK;
    uint64_t offset      = phys_addr & 0xFFF;
    uint64_t aligned_size = ((size + offset + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
    const uint64_t huge_sz = 2ULL * 1024 * 1024;

    if (kernel_debug_is_on()) {
        com_printf(COM1_PORT, "[IOREMAP] Aligned phys=0x%08x, offset=0x%x, size=0x%x\n",
                   (uint32_t)phys_base, (uint32_t)offset, (uint32_t)aligned_size);
    }

    if (ioremap_base == 0 || ioremap_next == 0 || !is_canonical_high(ioremap_base) || !is_canonical_high(ioremap_next)) {
        ioremap_base = pick_ioremap_base();
        if (ioremap_base == 0) {
            com_write_string(COM1_PORT, "[IOREMAP] ERROR: No free PML4 slot for ioremap\n");
            return NULL;
        }
        if (ioremap_base == 0xFFFF800000000000ULL) {
            com_write_string(COM1_PORT, "[IOREMAP] ERROR: ioremap_base collided with KHEAP_START; refusing\n");
            return NULL;
        }
        ioremap_next = ioremap_base + 0x10000ULL;
        if (kernel_debug_is_on()) {
            com_write_string(COM1_PORT, "[IOREMAP] Selected ioremap_base = ");
            char tmpbuf[32];
            format_hex64(tmpbuf, ioremap_base);
            com_write_string(COM1_PORT, tmpbuf);
            com_write_string(COM1_PORT, "\n");
        }
    }

    if (!is_canonical_high(ioremap_next)) {
        com_write_string(COM1_PORT, "[IOREMAP] WARNING: ioremap_next was non-canonical; reinitializing\n");
        ioremap_base = 0;
        ioremap_next = 0;
        return ioremap(phys_addr, size);
    }

    uint64_t virt_base = ioremap_next;
    uint64_t map_size  = aligned_size;
    if ((phys_base % huge_sz) == 0 && aligned_size >= huge_sz) {
        virt_base = (virt_base + (huge_sz - 1)) & ~(huge_sz - 1);
        map_size  = (aligned_size + huge_sz - 1) & ~(huge_sz - 1);
    }
    ioremap_next = virt_base + map_size;

    com_write_string(COM1_PORT, "[IOREMAP] virt_base=");
    com_printf(COM1_PORT, "0x%08x%08x pages=%u\n",
               (uint32_t)(virt_base >> 32), (uint32_t)(virt_base & 0xFFFFFFFFu),
               (uint32_t)(aligned_size / PAGE_SIZE));

    if (kernel_debug_is_on()) {
        com_write_string(COM1_PORT, "[IOREMAP] Allocated virtual range: 0x");
        com_printf(COM1_PORT, "%08x%08x - 0x",
                   (uint32_t)(virt_base >> 32), (uint32_t)(virt_base & 0xFFFFFFFF));
        com_printf(COM1_PORT, "%08x%08x\n",
                   (uint32_t)((virt_base + aligned_size - 1) >> 32),
                   (uint32_t)((virt_base + aligned_size - 1) & 0xFFFFFFFF));
    }

    uint64_t flags = PFLAG_WRITABLE | PFLAG_PWT | PFLAG_PCD;
    if (kernel_debug_is_on())
        com_printf(COM1_PORT, "[IOREMAP] Mapping with flags: 0x%x (W+PCD+PWT)\n", (uint32_t)flags);

    uint64_t mapped = 0;
    if ((phys_base % huge_sz) == 0 && (virt_base % huge_sz) == 0) {
        uint64_t huge_pages = (aligned_size + huge_sz - 1) / huge_sz;
        if (huge_pages) {
            com_write_string(COM1_PORT, "[IOREMAP] mapping huge pages count=");
            com_printf(COM1_PORT, "%u\n", (uint32_t)huge_pages);
            for (uint64_t i = 0; i < huge_pages; i++) {
                uint64_t vaddr = virt_base + (i * huge_sz);
                uint64_t paddr = phys_base + (i * huge_sz);
                com_write_string(COM1_PORT, "[IOREMAP] map2m v=");
                com_printf(COM1_PORT, "0x%08x%08x p=0x%08x%08x\n",
                           (uint32_t)(vaddr >> 32), (uint32_t)(vaddr & 0xFFFFFFFFu),
                           (uint32_t)(paddr >> 32), (uint32_t)(paddr & 0xFFFFFFFFu));
                int rc = paging_map_2m_page(vaddr, paddr, flags);
                com_write_string(COM1_PORT, "[IOREMAP] map2m rc=");
                com_printf(COM1_PORT, "%d\n", rc);
                if (rc != 0) {
                    com_write_string(COM1_PORT, "[IOREMAP] ERROR: paging_map_2m_page failed\n");
                    return NULL;
                }
            }
            mapped = huge_pages * huge_sz;
        }
    }

    if (mapped == 0) {
        uint64_t pages = aligned_size / PAGE_SIZE;
        com_write_string(COM1_PORT, "[IOREMAP] mapping 4k pages=");
        com_printf(COM1_PORT, "%u\n", (uint32_t)pages);
        for (uint64_t i = 0; i < pages; i++) {
            uint64_t vaddr = virt_base + (i * PAGE_SIZE);
            uint64_t paddr = phys_base + (i * PAGE_SIZE);
            if (paging_map_page(vaddr, paddr, flags) != 0) {
                com_write_string(COM1_PORT, "[IOREMAP] ERROR: paging_map_page failed\n");
                return NULL;
            }
        }
    }

    if (kernel_debug_is_on())
        com_write_string(COM1_PORT, "[IOREMAP] All pages mapped successfully\n");

    __asm__ volatile("mfence" ::: "memory");

    uint64_t result_virt = virt_base + offset;

    if (kernel_debug_is_on()) {
        com_write_string(COM1_PORT, "[IOREMAP] Returning virtual address: 0x");
        com_printf(COM1_PORT, "%08x%08x\n",
                   (uint32_t)(result_virt >> 32), (uint32_t)(result_virt & 0xFFFFFFFF));
        com_write_string(COM1_PORT, "[IOREMAP] Verifying mapping with virt_to_phys...\n");
        uint64_t verify_phys = paging_virt_to_phys(result_virt);
        if (verify_phys != phys_addr)
            com_write_string(COM1_PORT, "[IOREMAP] WARNING: virt_to_phys mismatch\n");
        else
            com_write_string(COM1_PORT, "[IOREMAP] Mapping verification OK\n");
    }

    return (void*)result_virt;
}

void* ioremap_guarded(uint64_t phys_addr, uint64_t size) {
    void *p = ioremap(phys_addr, size);
    if (!p) return NULL;
    ioremap_next += PAGE_SIZE;
    if (kernel_debug_is_on())
        com_write_string(COM1_PORT, "[IOREMAP] Guard page reserved after mapping\n");
    return p;
}

void paging_sync_kernel_mappings(uint64_t *target_pml4) {
    if (!target_pml4 || !kernel_master_cr3) return;
    uint64_t *master = (uint64_t *)phys_to_virt(kernel_master_cr3 & 0xFFFFFFFFFFFFF000ULL);
    if (!master) return;

    uint64_t cur_phys;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cur_phys));
    cur_phys &= 0xFFFFFFFFFFFFF000ULL;
    uint64_t *cur = (uint64_t *)phys_to_virt(cur_phys);

    for (int i = 256; i < 512; i++) {
        uint64_t e = master[i];
        if (!(e & PFLAG_PRESENT) && cur && cur != master)
            e = cur[i];
        if (e & PFLAG_PRESENT)
            target_pml4[i] = e;
    }
}

int paging_map_kernel_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    if (virt < 0xFFFF800000000000ULL)
        return paging_map_page(virt, phys, flags);
    if (!kernel_master_cr3)
        return paging_map_page(virt, phys, flags);

    uint64_t master_phys = kernel_master_cr3 & 0xFFFFFFFFFFFFF000ULL;
    uint64_t *master = (uint64_t *)phys_to_virt(master_phys);
    if (!master) return -1;

    unsigned i4 = (virt >> 39) & 0x1FF;
    unsigned i3 = (virt >> 30) & 0x1FF;
    unsigned i2 = (virt >> 21) & 0x1FF;
    unsigned i1 = (virt >> 12) & 0x1FF;

    uint64_t *pdpt = get_or_create(master, i4);
    if (!pdpt) return -1;
    uint64_t *pd = get_or_create(pdpt, i3);
    if (!pd) return -1;
    uint64_t *pt = get_or_create(pd, i2);
    if (!pt) return -1;

    pt[i1] = (phys & PAGE_MASK) | (flags & 0xFFFULL) | PFLAG_PRESENT;
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");

    uint64_t cur_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cur_cr3));
    cur_cr3 &= 0xFFFFFFFFFFFFF000ULL;
    if (cur_cr3 != master_phys) {
        uint64_t *cur_pml4 = (uint64_t *)phys_to_virt(cur_cr3);
        if (cur_pml4) {
            cur_pml4[i4] = master[i4];
            __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
        }
    }

    return 0;
}

void paging_free_process_pml4(uint64_t pml4_phys_addr) {
    if (!pml4_phys_addr) return;

    /* Safety: refuse to free the currently-active CR3. */
    uint64_t live_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(live_cr3));
    live_cr3 &= 0xFFFFFFFFFFFFF000ULL;
    if ((pml4_phys_addr & 0xFFFFFFFFFFFFF000ULL) == live_cr3) {
        com_write_string(COM1_PORT,
            "[PAGING] PANIC: paging_free_process_pml4 called on live CR3!\n");
        for (;;) __asm__ volatile("hlt");
    }

    /* Also refuse to free the kernel master CR3. */
    if (kernel_master_cr3 &&
        (pml4_phys_addr & 0xFFFFFFFFFFFFF000ULL) ==
        (kernel_master_cr3 & 0xFFFFFFFFFFFFF000ULL)) {
        com_write_string(COM1_PORT,
            "[PAGING] PANIC: paging_free_process_pml4 called on kernel master CR3!\n");
        for (;;) __asm__ volatile("hlt");
    }

    uint64_t *proc_pml4 = (uint64_t *)phys_to_virt(pml4_phys_addr & 0xFFFFFFFFFFFFF000ULL);
    if (!proc_pml4) {
        com_write_string(COM1_PORT,
            "[PAGING] paging_free_process_pml4: phys_to_virt returned NULL\n");
        return;
    }

    /* Walk low-half entries only (0–255). */
    for (unsigned i4 = 0; i4 < 256; i4++) {
        uint64_t e4 = proc_pml4[i4];
        if (!(e4 & PFLAG_PRESENT)) continue;

        uint64_t pdpt_phys = e4 & PAGE_MASK;
        uint64_t *pdpt = (uint64_t *)phys_to_virt(pdpt_phys);
        if (!pdpt) continue;

        for (unsigned i3 = 0; i3 < 512; i3++) {
            uint64_t e3 = pdpt[i3];
            if (!(e3 & PFLAG_PRESENT)) continue;

            /* 1 GiB huge page — no PT pages underneath, nothing to free. */
            if (e3 & (1ULL << 7)) continue;

            uint64_t pd_phys = e3 & PAGE_MASK;
            uint64_t *pd = (uint64_t *)phys_to_virt(pd_phys);
            if (!pd) continue;

            for (unsigned i2 = 0; i2 < 512; i2++) {
                uint64_t e2 = pd[i2];
                if (!(e2 & PFLAG_PRESENT)) continue;

                /* 2 MiB huge page — no PT page underneath, nothing to free. */
                if (e2 & (1ULL << 7)) continue;

                uint64_t pt_phys = e2 & PAGE_MASK;
                phys_free_frame(pt_phys);        /* free the PT page */
            }

            phys_free_frame(pd_phys);            /* free the PD page */
        }

        phys_free_frame(pdpt_phys);              /* free the PDPT page */
    }

    /* Finally free the PML4 page itself. */
    phys_free_frame(pml4_phys_addr & 0xFFFFFFFFFFFFF000ULL);

    if (kernel_debug_is_on()) {
        com_write_string(COM1_PORT, "[PAGING] Freed process PML4 at ");
        char tmp[32];
        format_hex64(tmp, pml4_phys_addr);
        com_write_string(COM1_PORT, tmp);
        com_write_string(COM1_PORT, "\n");
    }
}

uint64_t paging_get_phys_offset(void) {
    return phys_offset;
}