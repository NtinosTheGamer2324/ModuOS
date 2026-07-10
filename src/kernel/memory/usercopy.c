#include "moduos/kernel/memory/usercopy.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/process/process_new.h"

static uint64_t pte_from_pml4(uint64_t pml4_phys, uint64_t vaddr) {
    uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (vaddr >> 30) & 0x1FF;
    uint64_t pd_idx   = (vaddr >> 21) & 0x1FF;
    uint64_t pt_idx   = (vaddr >> 12) & 0x1FF;

    uint64_t *pml4 = (uint64_t *)phys_to_virt_kernel(pml4_phys & ~0xFFFULL);
    if (!pml4) return 0;
    uint64_t pml4e = pml4[pml4_idx];
    if (!(pml4e & PFLAG_PRESENT)) return 0;

    uint64_t *pdpt = (uint64_t *)phys_to_virt_kernel(pml4e & ~0xFFFULL);
    if (!pdpt) return 0;
    uint64_t pdpte = pdpt[pdpt_idx];
    if (!(pdpte & PFLAG_PRESENT)) return 0;
    /* 1 GiB huge page */
    if (pdpte & (1ULL << 7)) return pdpte;

    uint64_t *pd = (uint64_t *)phys_to_virt_kernel(pdpte & ~0xFFFULL);
    if (!pd) return 0;
    uint64_t pde = pd[pd_idx];
    if (!(pde & PFLAG_PRESENT)) return 0;
    /* 2 MiB huge page */
    if (pde & (1ULL << 7)) return pde;

    uint64_t *pt = (uint64_t *)phys_to_virt_kernel(pde & ~0xFFFULL);
    if (!pt) return 0;
    return pt[pt_idx];
}

/*
 * Validate that every page in [addr, addr+n) is present and user-accessible
 * in the *current process's* page table (not the CR3 that happens to be
 * loaded right now, which may belong to a just-exited child).
 */
static int user_range_is_mapped(uint64_t addr, size_t n) {
    if (n == 0) return 1;
    if (addr >= 0x0000800000000000ULL) return 0;
    uint64_t end = addr + (uint64_t)n - 1;
    if (end < addr) return 0;  /* overflow */
    if (end >= 0x0000800000000000ULL) return 0;

    /* Prefer the authoritative page table from the process struct. */
    uint64_t pt_phys = 0;
    process_t *proc = process_get_current();
    if (proc && proc->page_table)
        pt_phys = proc->page_table & ~0xFFFULL;

    uint64_t start_page = addr & ~0xFFFULL;
    uint64_t end_page   = end  & ~0xFFFULL;

    for (uint64_t v = start_page; v <= end_page; v += 0x1000ULL) {
        uint64_t pte;
        if (pt_phys)
            pte = pte_from_pml4(pt_phys, v);
        else
            pte = paging_get_pte(v);   /* kernel-only fallback */

        if (!(pte & PFLAG_PRESENT)) return 0;
        if (!(pte & PFLAG_USER))    return 0;
    }

    return 1;
}

int usercopy_to_user(void *user_dst, const void *kernel_src, size_t n) {
    if (!user_dst || (!kernel_src && n)) return -1;
    if (!user_range_is_mapped((uint64_t)(uintptr_t)user_dst, n)) {
        extern void com_write_string(uint16_t, const char*);
        extern int  com_write_hex64(uint16_t, uint64_t);
        extern char *itoa(int, char*, int);

        com_write_string(0x3F8, "[USERCOPY] ERROR: user range not mapped\n");
        com_write_string(0x3F8, "[USERCOPY]   Address: 0x");
        com_write_hex64(0x3F8, (uint64_t)(uintptr_t)user_dst);

        char buf[32];
        com_write_string(0x3F8, "\n[USERCOPY]   Size: ");
        itoa((int)n, buf, 10);
        com_write_string(0x3F8, buf);

        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        com_write_string(0x3F8, "\n[USERCOPY]   Current CR3: 0x");
        com_write_hex64(0x3F8, cr3);

        process_t *proc = process_get_current();
        com_write_string(0x3F8, "\n[USERCOPY]   proc->page_table: 0x");
        com_write_hex64(0x3F8, proc ? proc->page_table : 0);

        uint64_t addr = (uint64_t)(uintptr_t)user_dst;
        uint64_t page = addr & ~0xFFFULL;
        uint64_t pte  = proc && proc->page_table
                        ? pte_from_pml4(proc->page_table & ~0xFFFULL, page)
                        : paging_get_pte(page);
        com_write_string(0x3F8, "\n[USERCOPY]   Page: 0x");
        com_write_hex64(0x3F8, page);
        com_write_string(0x3F8, " PTE: 0x");
        com_write_hex64(0x3F8, pte);
        com_write_string(0x3F8, "\n");
        return -2;
    }

    /*
     * The destination is in user address space.  If CR3 doesn't match the
     * current process's page table (can happen right after a child exits and
     * the scheduler hasn't fully restored the parent's CR3 yet), switch now.
     */
    process_t *proc = process_get_current();
    if (proc && proc->page_table) {
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        if ((cr3 & ~0xFFFULL) != (proc->page_table & ~0xFFFULL))
            __asm__ volatile("mov %0, %%cr3" :: "r"(proc->page_table) : "memory");
    }

    memcpy(user_dst, kernel_src, n);
    return 0;
}


#define USER_SPACE_LIMIT 0x00007FFFFFFFFFFFULL

int usercopy_from_user(void *kernel_dst, const void *user_src, size_t n) {
    
    uint64_t start = (uint64_t)(uintptr_t)user_src;

    if (!kernel_dst || (!user_src && n)) return -1;

    if (start >= USER_SPACE_LIMIT || (start + n) > USER_SPACE_LIMIT) {
        return -3; // Return an error if address is in kernel range
    }

    if (!user_range_is_mapped((uint64_t)(uintptr_t)user_src, n)) return -2;

    process_t *proc = process_get_current();
    if (proc && proc->page_table) {
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        if ((cr3 & ~0xFFFULL) != (proc->page_table & ~0xFFFULL))
            __asm__ volatile("mov %0, %%cr3" :: "r"(proc->page_table) : "memory");
    }

    memcpy(kernel_dst, user_src, n);
    return 0;
}

int usercopy_string_from_user(char *kernel_dst, const char *user_src, size_t max_len) {
    if (!kernel_dst || max_len == 0) return -1;
    kernel_dst[0] = 0;
    if (!user_src) return -1;

    process_t *proc = process_get_current();
    if (proc && proc->page_table) {
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        if ((cr3 & ~0xFFFULL) != (proc->page_table & ~0xFFFULL))
            __asm__ volatile("mov %0, %%cr3" :: "r"(proc->page_table) : "memory");
    }

    for (size_t i = 0; i + 1 < max_len; i++) {
        if (!user_range_is_mapped((uint64_t)(uintptr_t)(user_src + i), 1)) {
            kernel_dst[i] = 0;
            return -2;
        }
        char c = user_src[i];
        kernel_dst[i] = c;
        if (c == 0) return 0;
    }
    kernel_dst[max_len - 1] = 0;
    return 0;
}