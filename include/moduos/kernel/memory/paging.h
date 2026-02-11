#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

/* Flags for mapping (lowest 12 bits used) */
#define PFLAG_PRESENT 0x1
#define PFLAG_WRITABLE 0x2
#define PFLAG_USER 0x4
#define PFLAG_PWT 0x8
#define PFLAG_PCD 0x10

/* Software-only flag stored in available PTE bits (bit 9). */
#define PFLAG_COW 0x200

/* amd64 PTE NX bit is handled elsewhere; for now we only need the USER bit */


/* Existing prototypes... */
void paging_init(void);

/* Configure the physmap direct-map offset used by phys_to_virt_kernel(). */
void paging_set_phys_offset(uint64_t offset);
uint64_t *paging_get_pml4(void);
uint64_t paging_get_pml4_phys(void);

/* Switch CR3 and update internal pml4 pointers (used for per-process address spaces). */
void paging_switch_cr3(uint64_t new_cr3_phys);
int paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
int paging_unmap_page(uint64_t virt);
int paging_map_range(uint64_t virt_base, uint64_t phys_base, uint64_t size, uint64_t flags);

// Fast mapping using 2MB huge pages (PS bit). virt/phys must be 2MB-aligned.
int paging_map_2m_page(uint64_t virt, uint64_t phys, uint64_t flags);
int paging_map_2m_range(uint64_t virt_base, uint64_t phys_base, uint64_t size, uint64_t flags);


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

/* Return the low 12 bits of the PTE for virt in the currently active address space.
 * Returns 0 if unmapped/not present.
 */
uint64_t paging_get_flags(uint64_t virt);

/* Get/set the raw PTE for virt in the current address space.
 * Returns 0 if unmapped.
 */
uint64_t paging_get_pte(uint64_t virt);
int paging_set_pte(uint64_t virt, uint64_t pte);

void *phys_to_virt_kernel(uint64_t phys);

/* A small reserved kernel-only scratch mapping area (2 pages) intended for
 * short-lived temporary mappings (e.g., fork page copying). The returned base
 * is a canonical high-half address; callers may use base and base+PAGE_SIZE.
 */
uint64_t paging_get_scratch_base(void);

/* Map physical I/O memory (MMIO) to virtual address space
 * Returns virtual address on success, 0 on failure
 */
void* ioremap(uint64_t phys_addr, uint64_t size);

// Like ioremap(), but reserves one extra unmapped guard page after the mapping.
// If callers write past the end of the mapped region they will fault immediately.
void* ioremap_guarded(uint64_t phys_addr, uint64_t size);

#endif /* PAGING_H */
