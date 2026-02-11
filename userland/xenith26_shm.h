#pragma once

#include "libc.h"
#include "../include/moduos/kernel/xenith26_shm.h"
#include "../include/moduos/kernel/syscall/syscall_numbers.h"

/* Userland wrappers for Xenith26 shared buffers */

static inline int x26_shm_create_u(x26_shm_create_req_t *req) {
    return (int)syscall(SYS_X26_SHM_CREATE, (long)req, 0, 0);
}

static inline int x26_shm_map_u(x26_shm_map_req_t *req) {
    return (int)syscall(SYS_X26_SHM_MAP, (long)req, 0, 0);
}

static inline int x26_shm_unmap_u(uint32_t buf_id, void *addr) {
    return (int)syscall(SYS_X26_SHM_UNMAP, (long)buf_id, (long)addr, 0);
}

static inline int x26_shm_destroy_u(uint32_t buf_id) {
    return (int)syscall(SYS_X26_SHM_DESTROY, (long)buf_id, 0, 0);
}

/* Reserved user mapping base for Xenith26 shm (must match kernel). */
#define X26_SHM_BASE 0x0000007000000000ULL
