// context_switch_new.c - CPU context switching wrapper
//
// This file bridges the new process_new.h world and the legacy context_switch
// assembly stub. It intentionally includes ONLY process_new.h so that the
// new process_t definition (with field 'context' of type cpu_context_t) is
// used throughout. cpu_state_t is forward-declared locally as an opaque alias
// because the asm stub's register layout is identical to cpu_context_t.

#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/memory/string.h"

// cpu_state_t has the same layout as cpu_context_t (9 uint64_t fields).
// Forward-declare it as an opaque type so we can call the legacy asm stub
// without pulling in process.h (which would redefine process_t and friends).
typedef cpu_context_t cpu_state_t;

// Use the new context_switch_asm which matches cpu_context_t offsets exactly.
extern void context_switch_asm(cpu_context_t *old_ctx, cpu_context_t *new_ctx,
                                void *old_fpu, void *new_fpu, uint64_t new_cr3);

// CR3 register access.
// read_cr3 is non-static so process_init_new.c can link against it.
// write_cr3 remains static (only used locally).
uint64_t read_cr3(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

static void write_cr3(uint64_t val) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(val) : "memory");
}

// External functions from arch-specific code
extern void amd64_syscall_set_kernel_stack(uint64_t stack_top);
extern void fpu_lazy_on_context_switch(process_t *next);

// Use the same stack size as process.h (8192). The local #define previously
// conflicted with process.h's definition; since we no longer include process.h,
// define it here to match the value used by the rest of the kernel.
#define KERNEL_STACK_SIZE 16384

static inline uint64_t stack_top(void *stack_base) {
    return (uint64_t)stack_base + KERNEL_STACK_SIZE;
}

// High-level context switch
void switch_to(process_t *prev, process_t *next) {
    if (!next) {
        return;
    }
    
    // Note: Interrupts must remain ENABLED so timer can preempt user processes
    // The scheduler lock protects the critical sections
    
    // Save current CR3 into the outgoing process's page_table field.
    uint64_t old_cr3 = read_cr3();
    if (prev) {
        prev->page_table = old_cr3;
    }
    
    // DEBUG: Log switches involving PID 2, and check if we're corrupting it
    extern process_t *process_get_by_pid(uint32_t);
    process_t *pid2 = process_get_by_pid(2);
    static uint64_t last_pid2_page_table = 0;
    static uint64_t pid2_addr_logged = 0;
    if (pid2 && !pid2_addr_logged) {
        extern int com_write_string(uint16_t, const char*);
        extern int com_write_hex64(uint16_t, uint64_t);
        com_write_string(0x3F8, "[DEBUG] PID 2 process struct is at 0x");
        com_write_hex64(0x3F8, (uint64_t)pid2);
        com_write_string(0x3F8, "\n");
        pid2_addr_logged = 1;
    }
    
    if ((prev && (prev->pid == 2 || prev->pid == 4)) || (next->pid == 2 || next->pid == 4) || 
        (pid2 && pid2->page_table != last_pid2_page_table)) {
        extern int com_write_string(uint16_t, const char*);
        extern int com_write_hex64(uint16_t, uint64_t);
        extern char *itoa(int, char*, int);
        char buf[16];
        com_write_string(0x3F8, "[SWITCH] ");
        if (prev) {
            itoa((int)prev->pid, buf, 10);
            com_write_string(0x3F8, buf);
        } else {
            com_write_string(0x3F8, "NULL");
        }
        com_write_string(0x3F8, " -> ");
        itoa((int)next->pid, buf, 10);
        com_write_string(0x3F8, buf);
        com_write_string(0x3F8, " | old_cr3=0x");
        com_write_hex64(0x3F8, old_cr3);
        if (prev) {
            com_write_string(0x3F8, " prev->page_table=0x");
            com_write_hex64(0x3F8, prev->page_table);
        }
        com_write_string(0x3F8, " next->page_table=0x");
        com_write_hex64(0x3F8, next->page_table);
        
        if (pid2) {
            com_write_string(0x3F8, " | PID2.page_table=0x");
            com_write_hex64(0x3F8, pid2->page_table);
            if (pid2->page_table != last_pid2_page_table) {
                com_write_string(0x3F8, " **CHANGED**");
                last_pid2_page_table = pid2->page_table;
            }
        }
        com_write_string(0x3F8, "\n");
    }

    // Update TSS kernel stack pointer for the incoming process.
    if (next->kernel_stack) {
        uint64_t base = (uint64_t)(uintptr_t)next->kernel_stack;
        if (base >= 0xFFFF800000000000ULL && ((base >> 48) == 0xFFFF)) {
            uint64_t kstack_top = (stack_top(next->kernel_stack) - 16) & ~0xFULL;
            amd64_syscall_set_kernel_stack(kstack_top);
        }
    }

    // Switch address space if the incoming process has a different PML4.
    // Use paging_switch_cr3() rather than a raw CR3 write so that paging.c's
    // global pml4 pointer stays in sync with the CPU's active address space.
    // Prefer page_table (kept up-to-date by switch_to), fall back to cr3
    // (set at fork/exec time) so newly-forked processes get their own PML4
    // on first schedule even before page_table has been saved by a prior switch.
    extern void paging_switch_cr3(uint64_t);
    // Prepare CR3 for the switch (but don't switch yet - let assembly do it)
    uint64_t next_cr3 = next->page_table ? next->page_table : next->cr3;
    if (!next_cr3 || next_cr3 == old_cr3) {
        next_cr3 = 0;  // Signal to assembly: don't switch CR3
    }

    // Lazy FPU: set CR0.TS so the next FP instruction traps.
    // Do this BEFORE CR3 switch to avoid running code in wrong address space
    fpu_lazy_on_context_switch(next);

    // context_switch_asm stub expects cpu_state_t* which is aliased to
    // cpu_context_t* above - same layout, safe cast. The 'context' field
    // in process_new.h's process_t is the canonical save area.
    // Pass next_cr3 so assembly can switch CR3 right before jumping
    context_switch_asm(
        prev ? &prev->context : NULL,
        &next->context,
        prev ? prev->fpu_state : NULL,
        next->fpu_state,
        next_cr3
    );
}
