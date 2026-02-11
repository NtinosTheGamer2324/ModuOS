#pragma once

#include <stdint.h>
#include <stddef.h>

/* Kernel/user shared structs for Xenith26 shared buffers.
 *
 * XRGB8888 only for now.
 */

typedef struct __attribute__((packed)) {
    uint16_t w;
    uint16_t h;
    uint16_t fmt; /* must be XRGB8888 */
    uint16_t flags;
    uint64_t preferred_addr; /* user virtual address to map at (page-aligned) */

    /* outputs (kernel fills) */
    uint32_t buf_id;
    uint32_t stride;
    uint64_t mapped_addr;
    uint64_t size_bytes;
} x26_shm_create_req_t;

typedef struct __attribute__((packed)) {
    uint32_t buf_id;
    uint32_t flags; /* bit0=RW (owner only), server always RO */
    uint64_t preferred_addr; /* page-aligned */

    /* outputs */
    uint64_t mapped_addr;
    uint64_t size_bytes;
} x26_shm_map_req_t;

/* Kernel API */
int x26_shm_create(x26_shm_create_req_t *user_req);
int x26_shm_map(x26_shm_map_req_t *user_req);
int x26_shm_unmap(uint32_t buf_id, uint64_t addr);
int x26_shm_destroy(uint32_t buf_id);

void x26_shm_process_cleanup(uint32_t pid);

/* Forward-declared process type is process_t (from process.h). */
struct process;
typedef struct process process_t;
void x26_shm_process_unmap_all(process_t *p);
