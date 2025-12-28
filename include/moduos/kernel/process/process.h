#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include <stddef.h>

#define MAX_PROCESSES 256
#define PROCESS_NAME_MAX 64
#define MAX_OPEN_FILES 16
#define KERNEL_STACK_SIZE 8192
#define USER_STACK_SIZE 65536

typedef enum {
    PROCESS_STATE_READY = 0,
    PROCESS_STATE_RUNNING,
    PROCESS_STATE_BLOCKED,
    PROCESS_STATE_SLEEPING,
    PROCESS_STATE_ZOMBIE,
    PROCESS_STATE_TERMINATED
} process_state_t;

/*
 CPU state used by context_switch.
 Order and offsets MUST match context_switch.asm.
 We save callee-saved regs per SysV: r15, r14, r13, r12, rbx, rbp.
 Also save rip, rsp, and RFLAGS (critical for interrupt enable flag).
 Layout (bytes): r15(0), r14(8), r13(16), r12(24), rbx(32),
                rbp(40), rip(48), rsp(56), rflags(64)
 */
typedef struct {
    uint64_t r15;      // +0
    uint64_t r14;      // +8
    uint64_t r13;      // +16
    uint64_t r12;      // +24
    uint64_t rbx;      // +32
    uint64_t rbp;      // +40
    uint64_t rip;      // +48
    uint64_t rsp;      // +56
    uint64_t rflags;   // +64 - ADDED: Must preserve interrupt flag!
} cpu_state_t;

typedef struct process {
    uint32_t pid;
    uint32_t parent_pid;
    char name[PROCESS_NAME_MAX];

    /* User identity */
    uint32_t uid; /* 0 = mdman/root */
    uint32_t gid;

    process_state_t state;
    int exit_code;

    cpu_state_t cpu_state;

    /* FPU/SSE state (FXSAVE/FXRSTOR).
     * Must be 16-byte aligned for fxsave64/fxrstor64.
     */
    __attribute__((aligned(16))) uint8_t fpu_state[512];

    // Memory management - single global kernel page table for now
    uint64_t page_table;
    void *kernel_stack;
    void *user_stack;

    /* User-mode launch context (used by amd64_enter_user_trampoline) */
    uint64_t user_rip;
    uint64_t user_rsp;
    int is_user;

    /* User heap (sbrk/brk) */
    uint64_t user_heap_base;
    uint64_t user_heap_end;
    uint64_t user_heap_limit;

    /* User mmap region (used by userland dynamic linker) */
    uint64_t user_mmap_base;
    uint64_t user_mmap_end;
    uint64_t user_mmap_limit;

    // File descriptors
    void *fd_table[MAX_OPEN_FILES];

    // Timing
    uint64_t time_slice;
    uint64_t total_time;

    // Priority (0 = highest)
    int priority;
    
    // Arguments (Windows-style)
    int argc;
    char **argv;

    // Filesystem context
    char cwd[256];           // Current working directory
    int current_slot;        // Currently active filesystem slot (-1 = none)

    // Linked list for scheduler
    struct process *next;
} process_t;

/* Process management functions */
void process_init(void);
process_t* process_create(const char *name, void (*entry_point)(void), int priority);
process_t* process_create_with_args(const char *name, void (*entry_point)(void), int priority, int argc, char **argv);
void process_exit(int exit_code);
void process_kill(uint32_t pid);
process_t* process_get_current(void);
process_t* process_get_by_pid(uint32_t pid);
void process_yield(void);
void process_sleep(uint64_t milliseconds);
void process_wake(uint32_t pid);

/* Scheduler */
void scheduler_init(void);
void scheduler_add_process(process_t *proc);
void scheduler_remove_process(process_t *proc);
void schedule(void);
void scheduler_tick(void);
void scheduler_request_reschedule(void);
int  scheduler_take_reschedule(void);

/* Context switching (assembly) */
/* Context switching (assembly)
 * void context_switch(cpu_state_t *old_state, cpu_state_t *new_state,
 *                    void *old_fpu_state, void *new_fpu_state)
 */
extern void context_switch(cpu_state_t *old_state, cpu_state_t *new_state,
                           void *old_fpu_state, void *new_fpu_state);

/* Lazy FPU switching hooks */
void fpu_lazy_on_context_switch(struct process *next);
void fpu_lazy_on_process_exit(struct process *p);
void fpu_lazy_handle_nm(void);

#endif /* PROCESS_H */
