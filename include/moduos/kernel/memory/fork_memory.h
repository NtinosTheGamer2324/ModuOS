#ifndef FORK_MEMORY_H
#define FORK_MEMORY_H

#include <stdint.h>

/*
 * Copy all user-space memory from source CR3 to destination CR3.
 * Used by fork() to duplicate parent's address space.
 * 
 * Parameters:
 *   src_cr3 - Physical address of source page directory
 *   dst_cr3 - Physical address of destination page directory
 */
void copy_user_memory(uint64_t src_cr3, uint64_t dst_cr3);

#endif /* FORK_MEMORY_H */
