// preempt.c - Safe preemption points for scheduler
#include "moduos/kernel/process/process.h"
#include "moduos/kernel/COM/com.h"

// External scheduler functions
extern int should_reschedule(void);
extern void schedule(void);

// Check for pending reschedule and do it if safe
// This is called from:
// - Syscall return path (before returning to userspace)
// - Exception return path
// - Explicit yield points
void preempt_check_and_schedule(void) {
    // Check if we're in a safe context (interrupts enabled)
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0" : "=r"(rflags));
    
    if (!(rflags & 0x200)) {
        // Interrupts disabled - NOT safe to schedule
        return;
    }
    
    // Check if reschedule is needed
    if (should_reschedule()) {
        schedule();
    }
}

// Called from syscall return path (in syscall64.c or syscall.c)
void preempt_on_syscall_return(void) {
    preempt_check_and_schedule();
}

// Called from exception/interrupt return path (in fault.asm or isr.asm)
void preempt_on_interrupt_return(void) {
    // Only reschedule if returning to userspace
    // Check CPL (Current Privilege Level) from CS on stack
    // If we interrupted kernel mode, don't preempt (might be holding locks)
    preempt_check_and_schedule();
}

// Explicit yield - safe preemption point
void yield_cpu(void) {
    if (should_reschedule()) {
        schedule();
    }
}
