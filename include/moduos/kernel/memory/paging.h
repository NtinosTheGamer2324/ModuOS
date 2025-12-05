#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

/* Flags for mapping (lowest 12 bits used) */
#define PFLAG_PRESENT 0x1
#define PFLAG_WRITABLE 0x2
#define PFLAG_USER 0x4
#define PFLAG_PWT 0x8
#define PFLAG_PCD 0x10

/* Existing prototypes... */
void paging_init(void);
uint64_t *paging_get_pml4(void);
uint64_t paging_get_pml4_phys(void);
int paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
int paging_unmap_page(uint64_t virt);
int paging_map_range(uint64_t virt_base, uint64_t phys_base, uint64_t size, uint64_t flags);

/* NEW: create per-process PML4 copying kernel high-half entries.
 * Returns physical address of new PML4 (non-zero on success).
 * The returned PML4 page is identity-mapped in your early boot convention.
 */
uint64_t paging_create_process_pml4(void);

/* NEW: Map a virtual range (virt_base) -> phys_base into the given PML4 (virtual pointer).
 * pml4_virt must be the *virtual* pointer to the PML4 page (we assume identity mapping).
 * This mirrors paging_map_range but targets a provided PML4 page.
 */
int paging_map_range_to_pml4(uint64_t *pml4_virt, uint64_t virt_base, uint64_t phys_base, uint64_t size, uint64_t flags);
uint64_t paging_virt_to_phys(uint64_t virt);

#endif /* PAGING_H */
