#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>
#include <stddef.h>

/* Parse Multiboot2 information and initialize memory regions */
void memory_init(void *mb2_ptr);
void memory_system_init(void *mb2_ptr);

/* Initialize paging (should not be called manually) */
void paging_init(void);

/* Identity-map first 512MB of RAM */
void early_identity_map(void);
void early_identity_map_all();

/* Kernel heap alloc/free */
void *kmalloc(size_t size);
void *kmalloc_aligned(size_t size, size_t alignment);
void *kzalloc(size_t size);
void kfree(void *ptr);
void kheap_stats(void);

#endif
