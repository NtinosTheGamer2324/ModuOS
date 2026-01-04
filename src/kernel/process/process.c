// process.c - Process owns its arguments
#include "moduos/kernel/process/process.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/macros.h"
#include "moduos/kernel/debug.h"
#include "moduos/arch/AMD64/syscall/syscall64_stack.h"
#include <stdint.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE 0x1000
#endif
#ifndef HIGHER_HALF_OFFSET
#define HIGHER_HALF_OFFSET 0xFFFF800000000000ULL
#endif

#define PHYS_TO_VIRT(addr) ((uint64_t)(addr) + HIGHER_HALF_OFFSET)
#define VIRT_TO_PHYS(addr) ((uint64_t)(addr) - HIGHER_HALF_OFFSET)

static process_t *process_table[MAX_PROCESSES];
static process_t *current_process = NULL;
static process_t *ready_queue_head = NULL;
static uint32_t next_pid = 1;
static int scheduler_enabled = 0;
static process_t *process_to_reap = NULL;

/* External context switch implemented in assembly */
extern void context_switch(cpu_state_t *old_state, cpu_state_t *new_state,
                           void *old_fpu_state, void *new_fpu_state);

/* Helper: top of kernel stack pointer */
static inline uint64_t stack_top(void *stack_base) {
    return (uint64_t)stack_base + KERNEL_STACK_SIZE;
}

/* forward declarations */
static void idle_entry(void);
void process_exit(int exit_code);

/* Debug helper to print ready queue */
static void debug_print_ready_queue(void) {
    if (!kernel_debug_is_on()) return;
    com_write_string(COM1_PORT, "[SCHED-DEBUG] Ready queue: ");
    if (!ready_queue_head) {
        com_write_string(COM1_PORT, "EMPTY\n");
        return;
    }
    
    process_t *p = ready_queue_head;
    while (p) {
        com_write_string(COM1_PORT, "PID ");
        char buf[12];
        itoa(p->pid, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, "(");
        com_write_string(COM1_PORT, p->name);
        com_write_string(COM1_PORT, ")");
        if (p->next) com_write_string(COM1_PORT, " -> ");
        p = p->next;
    }
    com_write_string(COM1_PORT, "\n");
}

void __attribute__((noreturn)) process_return_trampoline(void) {
    process_exit(0);
    for (;;) { __asm__ volatile("hlt"); }
}

void process_init(void) {
    COM_LOG_INFO(COM1_PORT, "Initializing process manager");

    for (int i = 0; i < MAX_PROCESSES; i++) process_table[i] = NULL;

    process_t *idle = (process_t*)kmalloc(sizeof(process_t));
    if (!idle) {
        COM_LOG_ERROR(COM1_PORT, "Failed to create idle process");
        return;
    }
    memset(idle, 0, sizeof(process_t));
    idle->pid = 0;
    idle->parent_pid = 0;
    strncpy(idle->name, "idle", PROCESS_NAME_MAX - 1);
    idle->state = PROCESS_STATE_RUNNING;
    idle->priority = 255;
    idle->argc = 0;
    idle->argv = NULL;
    memset(idle->fpu_state, 0, sizeof(idle->fpu_state));

    /* Default filesystem context:
     * userland syscalls resolve normal / paths against proc->current_slot.
     * If this is -1/uninitialized, early user programs like /Apps/login will fail to open files.
     */
    extern int boot_drive_slot;
    idle->current_slot = boot_drive_slot;
    strncpy(idle->cwd, "/", sizeof(idle->cwd) - 1);
    idle->cwd[sizeof(idle->cwd) - 1] = 0;

    idle->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!idle->kernel_stack) {
        COM_LOG_ERROR(COM1_PORT, "Failed to allocate idle kernel stack");
        kfree(idle);
        return;
    }
    memset(idle->kernel_stack, 0, KERNEL_STACK_SIZE);

    memset(&idle->cpu_state, 0, sizeof(cpu_state_t));
    idle->cpu_state.rip = (uint64_t)idle_entry;
    uint64_t top = (stack_top(idle->kernel_stack) - 16) & ~0xFULL;
    amd64_syscall_set_kernel_stack(top);
    idle->cpu_state.rsp = top;
    idle->cpu_state.rbp = top;
    idle->cpu_state.rflags = 0x202;

    process_table[0] = idle;
    current_process = idle;

    /* Lazy FPU switching: start with TS=1 so first FPU use traps and sets owner. */
    fpu_lazy_on_context_switch(NULL);

    COM_LOG_OK(COM1_PORT, "Process manager initialized");
}

static void idle_entry(void) {
    com_write_string(COM1_PORT, "[IDLE] Idle process started\n");
    for (;;) {
        if (scheduler_enabled) {
            schedule();
        }
        __asm__ volatile("hlt");
    }
}

void scheduler_init(void) {
    COM_LOG_INFO(COM1_PORT, "Initializing scheduler");
    ready_queue_head = NULL;
    scheduler_enabled = 1;
    COM_LOG_OK(COM1_PORT, "Scheduler initialized");
}

/* Helper to deep copy argv */
static char **copy_argv(int argc, char **argv) {
    if (argc <= 0 || !argv) {
        return NULL;
    }
    
    // Allocate new argv array
    char **new_argv = (char **)kmalloc((argc + 1) * sizeof(char *));
    if (!new_argv) {
        com_write_string(COM1_PORT, "[PROC] Failed to allocate argv array\n");
        return NULL;
    }
    
    // Copy each string
    for (int i = 0; i < argc; i++) {
        if (!argv[i]) {
            new_argv[i] = NULL;
            continue;
        }
        
        size_t len = strlen(argv[i]);
        new_argv[i] = (char *)kmalloc(len + 1);
        if (!new_argv[i]) {
            com_write_string(COM1_PORT, "[PROC] Failed to allocate argv string\n");
            // Free previously allocated strings
            for (int j = 0; j < i; j++) {
                if (new_argv[j]) kfree(new_argv[j]);
            }
            kfree(new_argv);
            return NULL;
        }
        
        memcpy(new_argv[i], argv[i], len + 1);
    }
    
    new_argv[argc] = NULL;
    return new_argv;
}

/* Helper to free argv */
static void free_argv(int argc, char **argv) {
    if (!argv) return;
    
    for (int i = 0; i < argc; i++) {
        if (argv[i]) {
            kfree(argv[i]);
        }
    }
    kfree(argv);
}

process_t* process_create(const char *name, void (*entry_point)(void), int priority) {
    return process_create_with_args(name, entry_point, priority, 0, NULL);
}

extern void amd64_enter_user_trampoline(void);

process_t* process_create_with_args(const char *name, void (*entry_point)(void), int priority, int argc, char **argv) {
    uint32_t pid = next_pid++;
    if (pid >= MAX_PROCESSES) {
        COM_LOG_ERROR(COM1_PORT, "Process table full");
        return NULL;
    }

    process_t *proc = (process_t*)kzalloc(sizeof(process_t));
    if (!proc) {
        COM_LOG_ERROR(COM1_PORT, "Failed to allocate process structure");
        return NULL;
    }

    proc->pid = pid;
    proc->parent_pid = current_process ? current_process->pid : 0;
    strncpy(proc->name, name, PROCESS_NAME_MAX - 1);
    proc->state = PROCESS_STATE_READY;
    proc->priority = priority;

    /* Inherit identity from parent (default root for PID 0/boot processes) */
    proc->uid = current_process ? current_process->uid : 0;
    proc->gid = current_process ? current_process->gid : 0;
    
    // Deep copy arguments - process now owns them
    if (argc > 0 && argv) {
        proc->argv = copy_argv(argc, argv);
        if (!proc->argv) {
            COM_LOG_ERROR(COM1_PORT, "Failed to copy arguments");
            kfree(proc);
            return NULL;
        }
        proc->argc = argc;
        
        com_write_string(COM1_PORT, "[PROC] Copied ");
        char buf[12];
        itoa(argc, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, " arguments for process\n");
    } else {
        proc->argc = 0;
        proc->argv = NULL;
    }

    proc->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!proc->kernel_stack) {
        COM_LOG_ERROR(COM1_PORT, "Failed to allocate kernel stack");
        if (proc->argv) free_argv(proc->argc, proc->argv);
        kfree(proc);
        return NULL;
    }
    memset(proc->kernel_stack, 0, KERNEL_STACK_SIZE);

    uint64_t kernel_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(kernel_cr3));
    proc->page_table = kernel_cr3;

    memset(&proc->cpu_state, 0, sizeof(cpu_state_t));

    proc->is_user = 0;
    proc->user_rip = 0;
    proc->user_rsp = 0;

    /* If entry point is in typical userland range (>=0x400000), launch it in ring3.
     * (Kernel code is around 0x0010xxxx in this build.)
     */
    uint64_t ep = (uint64_t)entry_point;
    if (ep >= 0x0000000000400000ULL && ep < 0x0000800000000000ULL) {
        proc->is_user = 1;
        proc->user_rip = ep;

        /* Map a user stack near top of canonical low half. */
        const uint64_t user_stack_top = 0x00007FFFFFF00000ULL;
        const uint64_t user_stack_base = user_stack_top - USER_STACK_SIZE;

        size_t pages = USER_STACK_SIZE / PAGE_SIZE;
        uint64_t phys_base = phys_alloc_contiguous(pages);
        if (!phys_base) {
            COM_LOG_ERROR(COM1_PORT, "Failed to allocate user stack");
            if (proc->argv) free_argv(proc->argc, proc->argv);
            kfree(proc->kernel_stack);
            kfree(proc);
            return NULL;
        }

        if (paging_map_range(user_stack_base, phys_base, USER_STACK_SIZE, PFLAG_PRESENT | PFLAG_WRITABLE | PFLAG_USER) != 0) {
            COM_LOG_ERROR(COM1_PORT, "Failed to map user stack");
            for (size_t p = 0; p < pages; p++) phys_free_frame(phys_base + p * PAGE_SIZE);
            if (proc->argv) free_argv(proc->argc, proc->argv);
            kfree(proc->kernel_stack);
            kfree(proc);
            return NULL;
        }

        proc->user_stack = (void*)(uintptr_t)user_stack_base;
        proc->user_rsp = user_stack_top - 16;

        /* Initialize user heap region (simple sbrk). */
        proc->user_heap_base = 0x0000005000000000ULL;
        proc->user_heap_end = proc->user_heap_base;
        proc->user_heap_limit = proc->user_heap_base + 64ULL * 1024ULL * 1024ULL; /* 64 MiB per process */

        /* Initialize user mmap region (for dl/ld.so). Keep far from heap/stack. */
        proc->user_mmap_base  = 0x0000006000000000ULL;
        proc->user_mmap_end   = proc->user_mmap_base;
        proc->user_mmap_limit = proc->user_mmap_base + 256ULL * 1024ULL * 1024ULL; /* 256 MiB */

        /*
         * Copy argv strings into USER memory.
         * Previously we passed kernel pointers to user mode (0xffff8000...), which causes
         * user #PF when apps dereference argv. We instead build argv on the user stack.
         */
        if (proc->argc > 0 && proc->argv) {
            uint64_t sp = proc->user_rsp;

            /* Copy strings from high to low */
            uint64_t user_str_ptrs[64];
            if (proc->argc > 64) {
                COM_LOG_ERROR(COM1_PORT, "Too many argv items for user stack copy");
                proc->argc = 64;
            }

            int argv_ok = 1;
            for (int i = proc->argc - 1; i >= 0; i--) {
                const char *s = proc->argv[i] ? proc->argv[i] : "";
                size_t len = strlen(s) + 1;
                if (sp < user_stack_base + len + 64) {
                    COM_LOG_ERROR(COM1_PORT, "argv does not fit on user stack");
                    argv_ok = 0;
                    break;
                }
                sp -= len;
                memcpy((void*)(uintptr_t)sp, s, len);
                user_str_ptrs[i] = sp;
            }

            if (!argv_ok) {
                proc->argc = 0;
                proc->cpu_state.r12 = 0;
                proc->cpu_state.r13 = 0;
                /* leave proc->user_rsp as initially set */
                goto argv_done;
            }

            /* Align before placing the argv pointer table */
            sp &= ~0xFULL;

            /* argv pointers array (argc+1) */
            sp -= (uint64_t)(proc->argc + 1) * sizeof(uint64_t);
            uint64_t *user_argv = (uint64_t*)(uintptr_t)sp;
            for (int i = 0; i < proc->argc; i++) {
                user_argv[i] = user_str_ptrs[i];
            }
            user_argv[proc->argc] = 0;

            /*
             * Ensure SysV AMD64 stack alignment for userland.
             * On function entry, GCC expects: (%rsp + 8) % 16 == 0  (i.e. rsp % 16 == 8)
             * because a CALL would have pushed an 8-byte return address.
             * We enter via iretq, so we synthesize that return address and (if needed)
             * add an extra 8-byte pad so the invariant always holds.
             */
            if (((sp - 8) & 0xFULL) != 8) {
                sp -= 8;
                *(uint64_t*)(uintptr_t)sp = 0; /* pad */
            }
            sp -= 8;
            *(uint64_t*)(uintptr_t)sp = 0; /* fake return address */

            proc->user_rsp = sp;

            /* Pass user-mode argc/argv via r12/r13 (callee-saved, restored by context_switch). */
            proc->cpu_state.r12 = (uint64_t)proc->argc;
            proc->cpu_state.r13 = (uint64_t)(uintptr_t)user_argv;
        }
argv_done:

        /* Use r14/r15 to pass user RIP/RSP to the trampoline via context_switch restore. */
        proc->cpu_state.r14 = proc->user_rip;
        proc->cpu_state.r15 = proc->user_rsp;

        proc->cpu_state.rip = (uint64_t)(uintptr_t)amd64_enter_user_trampoline;
    } else {
        proc->cpu_state.rip = ep;
    }

    uint64_t top = (stack_top(proc->kernel_stack) - 16) & ~0xFULL;
    /* Keep syscall/interrupt RSP0 in sync (single CPU). */
    amd64_syscall_set_kernel_stack(top);

    uint64_t initial_rsp = top - 8;
    uint64_t *ret_slot = (uint64_t *)initial_rsp;
    *ret_slot = (uint64_t)process_return_trampoline;

    proc->cpu_state.rsp = initial_rsp;
    proc->cpu_state.rbp = initial_rsp;
    proc->cpu_state.rflags = 0x202;

    /* Initialize FPU state image for this process.
     * Zero is acceptable; fxrstor will load a clean state.
     */
    memset(proc->fpu_state, 0, sizeof(proc->fpu_state));
    
    /*
     * For kernel processes, we keep argc/argv in r12/r13 as kernel pointers.
     * For user processes, we already built argv on the user stack and set r12/r13 above.
     */
    if (!proc->is_user) {
        if (proc->argc > 0 && proc->argv) {
            proc->cpu_state.r12 = (uint64_t)proc->argc;
            proc->cpu_state.r13 = (uint64_t)proc->argv;
        } else {
            proc->cpu_state.r12 = 0;
            proc->cpu_state.r13 = 0;
        }
    } else {
        if (proc->argc <= 0) {
            proc->cpu_state.r12 = 0;
            proc->cpu_state.r13 = 0;
        }
    }

    // Inherit filesystem context from parent/current process so relative paths work in userland.
    if (current_process) {
        proc->current_slot = current_process->current_slot;
        strncpy(proc->cwd, current_process->cwd, sizeof(proc->cwd) - 1);
        proc->cwd[sizeof(proc->cwd) - 1] = 0;
    } else {
        proc->current_slot = -1;
        proc->cwd[0] = 0;
    }

    proc->time_slice = 0;
    proc->total_time = 0;

    com_write_string(COM1_PORT, "[PROC] Created process: ");
    com_write_string(COM1_PORT, name);
    com_write_string(COM1_PORT, " (PID ");
    char pidbuf[12];
    itoa(pid, pidbuf, 10);
    com_write_string(COM1_PORT, pidbuf);
    com_write_string(COM1_PORT, ")\n");

    process_table[pid] = proc;
    scheduler_add_process(proc);

    return proc;
}

void scheduler_add_process(process_t *proc) {
    if (!proc) return;
    
    if (kernel_debug_is_on()) {
        com_write_string(COM1_PORT, "[SCHED] Adding PID ");
        char buf[12];
        itoa(proc->pid, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, " (");
        com_write_string(COM1_PORT, proc->name);
        com_write_string(COM1_PORT, ") to ready queue\n");
    }
    
    proc->next = NULL;

    if (!ready_queue_head) {
        ready_queue_head = proc;
        debug_print_ready_queue();
        return;
    }

    if (proc->priority < ready_queue_head->priority) {
        proc->next = ready_queue_head;
        ready_queue_head = proc;
        debug_print_ready_queue();
        return;
    }

    process_t *cur = ready_queue_head;
    while (cur->next && cur->next->priority <= proc->priority) {
        cur = cur->next;
    }
    proc->next = cur->next;
    cur->next = proc;
    debug_print_ready_queue();
}

void scheduler_remove_process(process_t *proc) {
    if (!proc || !ready_queue_head) return;
    
    if (kernel_debug_is_on()) {
        com_write_string(COM1_PORT, "[SCHED] Removing PID ");
        char buf[12];
        itoa(proc->pid, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, " from ready queue\n");
    }
    
    if (ready_queue_head == proc) {
        ready_queue_head = proc->next;
        debug_print_ready_queue();
        return;
    }
    process_t *cur = ready_queue_head;
    while (cur->next) {
        if (cur->next == proc) {
            cur->next = proc->next;
            debug_print_ready_queue();
            return;
        }
        cur = cur->next;
    }
}

static void do_switch_and_reap(process_t *old, process_t *newp) {
    char buf[12];

    /* Sanity: never jump to NULL/low memory. */
    if (newp && newp->cpu_state.rip < 0x100000) {
        COM_LOG_ERROR(COM1_PORT, "Refusing to context_switch: suspicious RIP");
        for(;;) { __asm__ volatile("hlt"); }
    }

    if (kernel_debug_is_on()) com_write_string(COM1_PORT, "[SWITCH] Calling context_switch asm...\n");
    /* Lazy FPU switching: set TS depending on whether the next process owns the live FPU state. */
    fpu_lazy_on_context_switch(newp);

    context_switch(old ? &old->cpu_state : NULL, &newp->cpu_state,
                  old ? (void*)old->fpu_state : NULL, (void*)newp->fpu_state);

    /* THIS LINE EXECUTES IN THE NEW PROCESS CONTEXT */
    if (kernel_debug_is_on()) {
        com_write_string(COM1_PORT, "[SWITCH] Back from asm, now in PID ");
        itoa(current_process->pid, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, "\n");
    }

    if (process_to_reap) {
        process_t *dead = process_to_reap;
        process_to_reap = NULL;

        if (dead->pid != 0) {
            com_write_string(COM1_PORT, "[REAP] Reaping process PID ");
            itoa(dead->pid, buf, 10);
            com_write_string(COM1_PORT, buf);
            com_write_string(COM1_PORT, "\n");
            
            process_table[dead->pid] = NULL;
            if (dead->kernel_stack) kfree(dead->kernel_stack);
            
            // Free process-owned arguments
            if (dead->argv) {
                free_argv(dead->argc, dead->argv);
            }
            
            kfree(dead);
        }
    }
    
    if (kernel_debug_is_on()) com_write_string(COM1_PORT, "[SWITCH] do_switch_and_reap returning to caller\n");
}

void schedule(void) {
    if (!scheduler_enabled) return;

    char buf[12];

    process_t *old = current_process;
    process_t *newp = ready_queue_head ? ready_queue_head : process_table[0];

    if (!newp) {
        com_write_string(COM1_PORT, "[SCHED-ERROR] No process to schedule!\n");
        return;
    }
    
    if (old == newp) {
        return;
    }

    if (kernel_debug_is_on()) {
        com_write_string(COM1_PORT, "[SCHED] Switching from PID ");
        char buf[12];
        itoa(old->pid, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, " (");
        com_write_string(COM1_PORT, old->name);
        com_write_string(COM1_PORT, ", state=");
        itoa(old->state, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, ") to PID ");
        itoa(newp->pid, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, " (");
        com_write_string(COM1_PORT, newp->name);
        com_write_string(COM1_PORT, ")\n");
    }

    /* Dequeue the new process if it came from ready queue */
    if (ready_queue_head && newp == ready_queue_head) {
        ready_queue_head = newp->next;
        newp->next = NULL;
        if (kernel_debug_is_on()) com_write_string(COM1_PORT, "[SCHED] Dequeued new process from ready queue\n");
        debug_print_ready_queue();
    }

    /* Re-enqueue old process if it's still RUNNING and NOT idle */
    if (old && old->state == PROCESS_STATE_RUNNING && old->pid != 0) {
        if (kernel_debug_is_on()) {
            com_write_string(COM1_PORT, "[SCHED] Re-queueing old process PID ");
            itoa(old->pid, buf, 10);
            com_write_string(COM1_PORT, buf);
            com_write_string(COM1_PORT, "\n");
        }
        old->state = PROCESS_STATE_READY;
        scheduler_add_process(old);
    } else if (old && old->pid == 0) {
        if (kernel_debug_is_on()) com_write_string(COM1_PORT, "[SCHED] Not re-queueing idle process\n");
    } else if (old && old->state != PROCESS_STATE_RUNNING) {
        if (kernel_debug_is_on()) {
            com_write_string(COM1_PORT, "[SCHED] Not re-queueing (state != RUNNING): state=");
            itoa(old->state, buf, 10);
            com_write_string(COM1_PORT, buf);
            com_write_string(COM1_PORT, "\n");
        }
    }

    newp->state = PROCESS_STATE_RUNNING;
    current_process = newp;

    /* Update syscall/interrupt RSP0 to the new process kernel stack (single CPU). */
    if (newp && newp->kernel_stack) {
        uint64_t top = ((uint64_t)(uintptr_t)newp->kernel_stack + KERNEL_STACK_SIZE - 16) & ~0xFULL;
        amd64_syscall_set_kernel_stack(top);
    }
    
    if (kernel_debug_is_on()) com_write_string(COM1_PORT, "[SCHED] About to context switch...\n");
    do_switch_and_reap(old, newp);
    
    // CRITICAL: Ensure interrupts are enabled after context switch
    __asm__ volatile("sti");
    
    if (kernel_debug_is_on()) {
        com_write_string(COM1_PORT, "[SCHED] Returned from context switch (now running PID ");
        itoa(current_process->pid, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, ")\n");
    }
}

static volatile int g_resched_requested = 0;

void scheduler_request_reschedule(void) { g_resched_requested = 1; }
int scheduler_take_reschedule(void) { int v = g_resched_requested; g_resched_requested = 0; return v; }

void scheduler_tick(void) {
    if (!scheduler_enabled) return;
    if (!current_process) return;

    current_process->total_time++;
    if (current_process->total_time % 10 == 0) {
        schedule();
    }
}

process_t* process_get_current(void) {
    return current_process;
}

process_t* process_get_by_pid(uint32_t pid) {
    if (pid >= MAX_PROCESSES) return NULL;
    return process_table[pid];
}

void process_exit(int exit_code) {
    /* If this process currently owns the FPU state, drop ownership. */
    fpu_lazy_on_process_exit(current_process);
    if (!current_process) return;

    current_process->state = PROCESS_STATE_ZOMBIE;
    current_process->exit_code = exit_code;

    com_write_string(COM1_PORT, "[PROC] Process ");
    char pidbuf[12];
    itoa(current_process->pid, pidbuf, 10);
    com_write_string(COM1_PORT, pidbuf);
    com_write_string(COM1_PORT, " exited with code ");
    itoa(exit_code, pidbuf, 10);
    com_write_string(COM1_PORT, pidbuf);
    com_write_string(COM1_PORT, "\n");

    process_to_reap = current_process;

    com_write_string(COM1_PORT, "[EXIT] Looking for next process to run...\n");
    debug_print_ready_queue();
    
    process_t *target = ready_queue_head ? ready_queue_head : process_table[0];
    if (!target) {
        COM_LOG_ERROR(COM1_PORT, "process_exit: no target to switch to (no idle?)");
        for (;;) { __asm__ volatile("hlt"); }
    }

    com_write_string(COM1_PORT, "[EXIT] Target process: PID ");
    itoa(target->pid, pidbuf, 10);
    com_write_string(COM1_PORT, pidbuf);
    com_write_string(COM1_PORT, " (");
    com_write_string(COM1_PORT, target->name);
    com_write_string(COM1_PORT, ")\n");

    if (ready_queue_head && target == ready_queue_head) {
        ready_queue_head = target->next;
        target->next = NULL;
        com_write_string(COM1_PORT, "[EXIT] Dequeued target from ready queue\n");
        debug_print_ready_queue();
    }

    target->state = PROCESS_STATE_RUNNING;
    process_t *old = current_process;
    current_process = target;

    com_write_string(COM1_PORT, "[EXIT] Switching to target...\n");
    do_switch_and_reap(old, target);
    
    // CRITICAL: Ensure interrupts are enabled (should never reach here in zombie)
    __asm__ volatile("sti");

    for (;;) { __asm__ volatile("hlt"); }
}

void process_kill(uint32_t pid) {
    process_t *p = process_get_by_pid(pid);
    if (!p) return;

    if (p->state != PROCESS_STATE_ZOMBIE && p->state != PROCESS_STATE_TERMINATED) {
        p->state = PROCESS_STATE_TERMINATED;
        scheduler_remove_process(p);

        if (p->kernel_stack) kfree(p->kernel_stack);
        if (p->argv) free_argv(p->argc, p->argv);
        
        process_table[pid] = NULL;
        kfree(p);
    }
}

void process_yield(void) {
    if (scheduler_take_reschedule()) {
        /* fall through to schedule() */
    }

    if (!current_process) {
        if (kernel_debug_is_on()) com_write_string(COM1_PORT, "[YIELD] Warning: no current process\n");
        return;
    }
    
    if (kernel_debug_is_on()) {
        com_write_string(COM1_PORT, "[YIELD] Process ");
        char buf[12];
        itoa(current_process->pid, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, " (");
        com_write_string(COM1_PORT, current_process->name);
        com_write_string(COM1_PORT, ") yielding (state=");
        itoa(current_process->state, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, ")\n");
        debug_print_ready_queue();
    }
    schedule();

    if (kernel_debug_is_on()) {
        com_write_string(COM1_PORT, "[YIELD] Process ");
        char buf2[12];
        itoa(current_process->pid, buf2, 10);
        com_write_string(COM1_PORT, buf2);
        com_write_string(COM1_PORT, " resumed after yield\n");
    }
}

void process_sleep(uint64_t milliseconds) {
    if (!current_process) return;
    current_process->state = PROCESS_STATE_SLEEPING;
    current_process->time_slice = milliseconds;
    scheduler_remove_process(current_process);
    schedule();
}

void process_wake(uint32_t pid) {
    process_t *p = process_get_by_pid(pid);
    if (!p || p->state != PROCESS_STATE_SLEEPING) return;
    p->state = PROCESS_STATE_READY;
    scheduler_add_process(p);
}