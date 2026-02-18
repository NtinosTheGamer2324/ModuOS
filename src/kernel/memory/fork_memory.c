/*
 * fork_memory.c - Memory copying for fork()
 * Implements copy-on-write (COW) for process forking
 */

#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/COM/com.h"

#define PAGE_SIZE 4096
#define USER_SPACE_START 0x0000000000400000ULL
#define USER_SPACE_END   0x00007FFFFFFFF000ULL

/*
 * Copy all user-space pages from source to destination page directory.
 * This is called during fork() to duplicate the parent's memory.
 * 
 * For now: Simple copy (not COW). Each page is physically copied.
 * TODO: Implement true copy-on-write using PFLAG_COW
 */
void copy_user_memory(uint64_t src_cr3, uint64_t dst_cr3) {
    uint64_t scratch_base = paging_get_scratch_base();
    uint64_t *src_pml4 = (uint64_t *)phys_to_virt_kernel(src_cr3);
    uint64_t *dst_pml4 = (uint64_t *)phys_to_virt_kernel(dst_cr3);
    
    if (!src_pml4 || !dst_pml4 || !scratch_base) {
        com_write_string(COM1_PORT, "[FORK] copy_user_memory: invalid parameters\n");
        return;
    }
    
    /* Save current CR3 */
    uint64_t old_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(old_cr3));
    
    /* Switch to source address space to read pages */
    paging_switch_cr3(src_cr3);
    
    /* Walk through user space and copy mapped pages */
    for (uint64_t vaddr = USER_SPACE_START; vaddr < USER_SPACE_END; vaddr += PAGE_SIZE) {
        /* Check if page is mapped in source */
        uint64_t src_pte = paging_get_pte(vaddr);
        if (!(src_pte & PFLAG_PRESENT)) {
            continue;  /* Page not mapped, skip */
        }
        
        /* Get physical address of source page */
        uint64_t src_phys = paging_virt_to_phys(vaddr);
        if (!src_phys) {
            continue;
        }
        
        /* Allocate new physical page for child */
        uint64_t dst_phys = phys_alloc_frame();
        if (!dst_phys) {
            com_write_string(COM1_PORT, "[FORK] copy_user_memory: out of memory\n");
            break;
        }
        
        /* Copy page data using scratch mapping */
        void *src_ptr = phys_to_virt_kernel(src_phys);
        void *dst_ptr = phys_to_virt_kernel(dst_phys);
        
        if (src_ptr && dst_ptr) {
            memcpy(dst_ptr, src_ptr, PAGE_SIZE);
        }
        
        /* Map the new page in destination address space */
        paging_switch_cr3(dst_cr3);
        uint64_t flags = src_pte & 0xFFF;  /* Preserve flags */
        paging_map_page(vaddr, dst_phys, flags);
        paging_switch_cr3(src_cr3);
    }
    
    /* Restore original CR3 */
    paging_switch_cr3(old_cr3);
}
