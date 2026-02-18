// context_switch_new.c - CPU context switching wrapper

#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/COM/com.h"

#include "moduos/kernel/memory/string.h"

// External assembly function
extern void context_switch_asm(cpu_context_t *old_ctx, cpu_context_t *new_ctx,
                                void *old_fpu, void *new_fpu);

// CR3 register access (non-inline so it can be linked)
uint64_t read_cr3(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

void write_cr3(uint64_t val) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(val) : "memory");
}

// External functions from arch-specific code
extern void amd64_syscall_set_kernel_stack(uint64_t stack_top);
extern void fpu_lazy_on_context_switch(process_t *next);

#define KERNEL_STACK_SIZE 16384

static inline uint64_t stack_top(void *stack_base) {
    return (uint64_t)stack_base + KERNEL_STACK_SIZE;
}

// High-level context switch
void switch_to(process_t *prev, process_t *next) {
    if (!next) {
        com_write_string(COM1_PORT, "[SWITCH] ERROR: next is NULL\n");
        return;
    }
    
    // Debug output
    static int switch_count = 0;
    switch_count++;
    if (switch_count <= 20) {  // Increased from 10 to 20
        com_write_string(COM1_PORT, "[SWITCH] Context switch #");
        char buf[16];
        itoa(switch_count, buf, 10);
        com_write_string(COM1_PORT, buf);
        if (prev) {
            com_write_string(COM1_PORT, " from PID ");
            itoa(prev->pid, buf, 10);
            com_write_string(COM1_PORT, buf);
        } else {
            com_write_string(COM1_PORT, " from NULL");
        }
        com_write_string(COM1_PORT, " to PID ");
        itoa(next->pid, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, " RIP=0x");
        snprintf(buf, sizeof(buf), "%llx", (unsigned long long)next->context.rip);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, "\n");
    }
    
    // Disable interrupts during critical transition
    __asm__ volatile("cli" ::: "memory");
    
    // Save current CR3 if we have a previous process
    uint64_t old_cr3 = read_cr3();
    if (prev) {
        prev->cr3 = old_cr3;
    }
    
    // Switch to new page table if different
    if (next->cr3 && next->cr3 != old_cr3) {
        write_cr3(next->cr3);
    }
    
    // Update TSS kernel stack pointer for syscalls/interrupts
    if (next->kernel_stack) {
        uint64_t kstack_top = (stack_top(next->kernel_stack) - 16) & ~0xFULL;
        amd64_syscall_set_kernel_stack(kstack_top);
    }
    
    // Lazy FPU switching
    fpu_lazy_on_context_switch(next);
    
    // Perform the actual register switch
    // This saves prev's registers and loads next's registers
    // Interrupts will be re-enabled by restoring next's rflags (which has IF=1)
    context_switch_asm(
        prev ? &prev->context : NULL,
        &next->context,
        prev ? prev->fpu_state : NULL,
        next->fpu_state
    );
    
    // When we return here, we're running as the 'next' process
    // (or back in 'prev' if we were scheduled again)
}
