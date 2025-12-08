#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/COM/com.h"

void memory_system_init(void *mb2)
{
    com_write_string(COM1_PORT, "[MEM] Starting memory system initialization...\n");
    
    /* Step 1: Parse Multiboot2 and initialize physical allocator FIRST
     * This must happen before any paging operations that need allocation
     */
    com_write_string(COM1_PORT, "[MEM] Step 1: Parsing Multiboot2 info and initializing physical allocator...\n");
    memory_init(mb2);
    
    /* At this point, we're still using the bootloader's page tables
     * and physical memory allocator is ready */
    
    /* Step 2: Do a minimal early identity map (just acknowledge bootloader's mapping)
     * This doesn't allocate new page tables, just uses existing ones from bootloader
     */
    com_write_string(COM1_PORT, "[MEM] Step 2: Using bootloader's identity mapping...\n");
    early_identity_map();
    
    /* Step 3: NOW initialize our own paging system
     * phys_alloc_frame() will work because phys_init() was called
     * The allocated frames are in the identity-mapped region
     * paging_init() will COPY bootloader's mappings to avoid losing them
     */
    com_write_string(COM1_PORT, "[MEM] Step 3: Initializing paging system (copying bootloader mappings)...\n");
    paging_init();
    
    /* Step 4: Extend identity mapping to cover all physical memory
     * Now we have our own page tables and can map everything
     * This maps ONLY regions that aren't already mapped
     */
    com_write_string(COM1_PORT, "[MEM] Step 4: Extending identity mapping to all RAM...\n");
    early_identity_map_all();
    
    com_write_string(COM1_PORT, "[MEM] Memory system initialized successfully!\n");
    com_write_string(COM1_PORT, "[MEM] Kernel heap is now available via kmalloc()\n");
}