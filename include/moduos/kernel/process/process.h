// process.h — compatibility shim
//
// The canonical process descriptor is now defined in process_new.h.
// This file re-exports everything old callers expect so they do not
// need to be updated individually.

#ifndef PROCESS_H
#define PROCESS_H

#include "moduos/kernel/process/process_new.h"

// Constants that process_new.h does not carry but old callers use.
#ifndef KERNEL_STACK_SIZE
#define KERNEL_STACK_SIZE 16384
#endif
#ifndef USER_STACK_SIZE
#define USER_STACK_SIZE 65536
#endif
#ifndef MAX_OPEN_FILES
#define MAX_OPEN_FILES 256   /* matches PROCESS_MAX_FDS in process_new.h */
#endif

// cpu_state_t is the same layout as cpu_context_t.
// Provide the alias so files that reference cpu_state_t still compile.
typedef cpu_context_t cpu_state_t;

// Legacy field alias — old callers use ->parent_pid; new struct has ->ppid.
// NOTE: do NOT use this macro in new code.
#ifndef parent_pid
#define parent_pid ppid
#endif

// Old scheduler API names — compat wrappers declared in scheduler_compat.c.
void scheduler_add_process(process_t *proc);
void scheduler_remove_process(process_t *proc);
void scheduler_request_reschedule(void);
int  scheduler_take_reschedule(void);
uint64_t scheduler_get_min_vruntime(void);
uint32_t scheduler_nice_to_weight(int nice);
uint64_t scheduler_get_clock_ticks(void);
void debug_print_ready_queue(void);

// Legacy process management entry points still implemented in process.c.
void process_init(void);
void process_wake(uint32_t pid);
void set_curproc(process_t *p);
void do_switch_and_reap(process_t *old, process_t *newp);

// Context switch asm stub — takes cpu_context_t* (== cpu_state_t*).
extern void context_switch(cpu_context_t *old_state, cpu_context_t *new_state,
                           void *old_fpu_state, void *new_fpu_state);

// Lazy FPU hooks.
void fpu_lazy_on_context_switch(process_t *next);
void fpu_lazy_on_process_exit(process_t *p);
void fpu_lazy_handle_nm(void);

#endif /* PROCESS_H */
