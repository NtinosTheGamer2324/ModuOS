// process.c - Process owns its arguments
#include "moduos/kernel/process/process.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/macros.h"
#include "moduos/kernel/debug.h"
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
extern void context_switch(cpu_state_t *old_state, cpu_state_t *new_state);

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
    idle->cpu_state.rsp = top;
    idle->cpu_state.rbp = top;
    idle->cpu_state.rflags = 0x202;

    process_table[0] = idle;
    current_process = idle;

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
    proc->cpu_state.rip = (uint64_t)entry_point;

    uint64_t top = (stack_top(proc->kernel_stack) - 16) & ~0xFULL;
    uint64_t initial_rsp = top - 8;
    uint64_t *ret_slot = (uint64_t *)initial_rsp;
    *ret_slot = (uint64_t)process_return_trampoline;

    proc->cpu_state.rsp = initial_rsp;
    proc->cpu_state.rbp = initial_rsp;
    proc->cpu_state.rflags = 0x202;
    
    // Store argc and argv in callee-saved registers for context switch
    if (proc->argc > 0 && proc->argv) {
        proc->cpu_state.r12 = (uint64_t)proc->argc;
        proc->cpu_state.r13 = (uint64_t)proc->argv;
        
        com_write_string(COM1_PORT, "[PROC] Set up args: argc=");
        char buf[12];
        itoa(proc->argc, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, ", argv=0x");
        for (int j = 15; j >= 0; j--) {
            uint64_t addr = (uint64_t)proc->argv;
            uint8_t nibble = (addr >> (j * 4)) & 0xF;
            char hex = nibble < 10 ? '0' + nibble : 'a' + (nibble - 10);
            com_write_byte(COM1_PORT, hex);
        }
        com_write_string(COM1_PORT, "\n");
    } else {
        proc->cpu_state.r12 = 0;
        proc->cpu_state.r13 = 0;
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
    if (kernel_debug_is_on()) com_write_string(COM1_PORT, "[SWITCH] Calling context_switch asm...\n");
    context_switch(old ? &old->cpu_state : NULL, &newp->cpu_state);

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