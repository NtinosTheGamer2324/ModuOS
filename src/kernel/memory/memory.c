//memory.c
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/COM/com.h"

void memory_system_init(void *mb2)
{
    com_write_string(COM1_PORT, "[MEM] Starting memory system initialization...\n");
    
    com_write_string(COM1_PORT, "[MEM] Step 1: Parsing Multiboot2 info and initializing physical allocator...\n");
    memory_init(mb2);
    
    com_write_string(COM1_PORT, "[MEM] Step 2: Using bootloader's identity mapping...\n");
    early_identity_map();
    
    com_write_string(COM1_PORT, "[MEM] Step 3: Initializing paging system (copying bootloader mappings)...\n");
    paging_init();
    
    com_write_string(COM1_PORT, "[MEM] Step 4: Extending identity mapping to all RAM...\n");
    early_identity_map_all();
    
    /* ADD THESE DEBUG LINES */
    com_write_string(COM1_PORT, "[MEM] Step 4 complete! early_identity_map_all() returned successfully\n");
    com_write_string(COM1_PORT, "[MEM] Memory system initialized successfully!\n");
    com_write_string(COM1_PORT, "[MEM] Kernel heap is now available via kmalloc()\n");
    com_write_string(COM1_PORT, "[MEM] Returning from memory_system_init()...\n");
}