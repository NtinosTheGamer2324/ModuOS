#ifndef PHYS_H
#define PHYS_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096ULL

/* Called by memory_init: pass total memory and pointer to usable regions array.
 * usable_regions is an array of pairs {addr, len} laid out as uint64_t[2*count].
 */
void phys_init(uint64_t total_mem, const void *usable_regions, size_t region_count);

/* Basic physical frame alloc/free (returns physical address or 0 on OOM) */
uint64_t phys_alloc_frame(void);
void phys_free_frame(uint64_t paddr);

/* Allocate contiguous frames (returns physical base addr) */
uint64_t phys_alloc_contiguous(size_t nframes);

/* Reserve physical range (mark as used) */
void phys_reserve_range(uint64_t pstart, uint64_t plen);

/* Get total frames available (for debug) */
uint64_t phys_total_frames(void);

uint64_t phys_count_free_frames(void);
void phys_get_stats(uint64_t *total_frames, uint64_t *free_frames, uint64_t *used_frames);

#endif /* PHYS_H */
