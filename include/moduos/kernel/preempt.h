#pragma once

// Preemption points for safe rescheduling

#ifdef __cplusplus
extern "C" {
#endif

// Check for pending reschedule and do it if safe
void preempt_check_and_schedule(void);

// Called from syscall return path
void preempt_on_syscall_return(void);

// Called from exception/interrupt return path
void preempt_on_interrupt_return(void);

// Explicit yield - safe preemption point
void yield_cpu(void);

#ifdef __cplusplus
}
#endif
