// process.c - Process owns its arguments
#include "moduos/kernel/process/process.h"
#include "moduos/kernel/user_identity.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/macros.h"
#include "moduos/kernel/debug.h"
#include "moduos/kernel/spinlock.h"
#include "moduos/kernel/rwlock.h"
#include "moduos/kernel/percpu.h"
#include "moduos/arch/AMD64/cpu.h"
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
static process_t *current_process = NULL; /* legacy; BSP init uses per-CPU field */
static process_t *ready_queue_head = NULL;

static spinlock_t sched_lock __attribute__((aligned(64)));
static rwlock_t process_table_rwlock __attribute__((aligned(64)));  /* RWLock for process_table (read-heavy) */
static spinlock_t next_pid_lock __attribute__((aligned(64)));        /* Separate lock for PID allocation */
static uint32_t next_pid = 1;
static int g_resched_requested = 0;

extern void process_return_trampoline(void);

static char **copy_argv(int argc, char **argv) {
    if (argc <= 0 || !argv) return NULL;
    char **out = (char**)kzalloc(sizeof(char*) * (argc + 1));
    if (!out) return NULL;
    for (int i = 0; i < argc; i++) {
        const char *src = argv[i] ? argv[i] : "";
        size_t len = strlen(src) + 1;
        out[i] = (char*)kzalloc(len);
        if (!out[i]) {
            for (int j = 0; j < i; j++) { if (out[j]) kfree(out[j]); }
            kfree(out);
            return NULL;
        }
        memcpy(out[i], src, len);
    }
    out[argc] = NULL;
    return out;
}

static void free_argv(int argc, char **argv) {
    if (!argv) return;
    for (int i = 0; i < argc; i++) {
        if (argv[i]) kfree(argv[i]);
    }
    kfree(argv);
}

// NOTE: cpu-local storage is not fully initialized yet in current boot flow.
// Until SMP/per-CPU is brought up, use the legacy global pointer.
static inline process_t *get_curproc(void) {
    return current_process;
}

static inline void set_curproc(process_t *p) {
    current_process = p;
}

/* forward decls for helpers used early in this file */
static void free_argv(int argc, char **argv);

uint32_t process_alloc_pid(void) {
    // PID 0 is reserved for idle.
    for (;;) {
        spinlock_lock(&next_pid_lock);
        uint32_t pid = next_pid++;
        if (pid == 0 || pid >= MAX_PROCESSES) {
            next_pid = 1;
            pid = next_pid++;
        }
        if (pid < MAX_PROCESSES && process_table[pid] == NULL) { spinlock_unlock(&next_pid_lock); return pid; }
    }
}

int process_register(process_t *proc) {
    if (!proc) return -1;
    if (proc->pid >= MAX_PROCESSES) return -1;
    rwlock_write_lock(&process_table_rwlock);
    if (process_table[proc->pid] != NULL) {
        rwlock_write_unlock(&process_table_rwlock);
        return -1;
    }
    process_table[proc->pid] = proc;
    rwlock_write_unlock(&process_table_rwlock);
    scheduler_add_process(proc);
    return 0;
}

int process_unregister(uint32_t pid) {
    if (pid >= MAX_PROCESSES) return -1;
    rwlock_write_lock(&process_table_rwlock);
    if (!process_table[pid]) return -1;
    process_table[pid] = NULL;
    rwlock_write_unlock(&process_table_rwlock);
    return 0;
}

void process_destroy(process_t *proc) {
    if (!proc) return;
    // Only intended for zombie/non-running processes.
    (void)process_unregister(proc->pid);
    process_free_user_memory(proc);
    if (proc->kernel_stack) kfree(proc->kernel_stack);
    if (proc->argv) free_argv(proc->argc, proc->argv);
    if (proc->envp) free_argv(proc->envc, proc->envp);
    kfree(proc);
}

static int scheduler_enabled = 0;
#define NICE_0_LOAD 1024
#define SCHED_LATENCY_TICKS 20
#define SCHED_MIN_GRAN_TICKS 4

static uint64_t sched_clock_ticks = 0;
static uint64_t min_vruntime = 0;
static uint32_t total_weight = 0;
static uint32_t nr_running = 0;

static const uint32_t nice_to_weight_table[40] = {
    88761, 71755, 56483, 46273, 36291, 29154, 23254, 18705, 14949, 11916,
    9548, 7620, 6100, 4904, 3906, 3121, 2501, 1991, 1586, 1277,
    1024, 820, 655, 526, 423, 335, 272, 215, 172, 137,
    110, 87, 70, 56, 45, 36, 29, 23, 18, 15
};

static uint32_t nice_to_weight(int nice) {
    if (nice < -20) nice = -20;
    if (nice > 19) nice = 19;
    return nice_to_weight_table[nice + 20];
}
/* Linux-like: processes become zombies on exit and are reaped by parent wait.
 * We no longer auto-free processes immediately on exit.
 */
static process_t *process_to_reap = NULL; /* deprecated (kept for future) */

/* External context switch implemented in assembly */
extern void context_switch(cpu_state_t *old_state, cpu_state_t *new_state,
                           void *old_fpu_state, void *new_fpu_state);

/* Helper: top of kernel stack pointer */
static inline uint64_t stack_top(void *stack_base) {
    return (uint64_t)stack_base + KERNEL_STACK_SIZE;
}

static uint64_t *g_build_pml4 = NULL;
static uint64_t g_build_pml4_phys = 0;

void process_set_build_pml4(uint64_t *pml4_virt, uint64_t pml4_phys) {
    g_build_pml4 = pml4_virt;
    g_build_pml4_phys = pml4_phys;
}

/* forward declarations */
static void idle_entry(void);
static void idle_entry(void) {
    for (;;) {
        __asm__ volatile("hlt");
    }
}
extern void process_return_trampoline(void);

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

static void cfs_update_min_vruntime(void) {
    if (ready_queue_head) {
        min_vruntime = ready_queue_head->vruntime;
    }
}

static void cfs_enqueue_process(process_t *proc) {
    proc->next = NULL;
    if (!ready_queue_head) {
        ready_queue_head = proc;
    } else if (proc->vruntime < ready_queue_head->vruntime) {
        proc->next = ready_queue_head;
        ready_queue_head = proc;
    } else {
        process_t *cur = ready_queue_head;
        while (cur->next && cur->next->vruntime <= proc->vruntime) {
            cur = cur->next;
        }
        proc->next = cur->next;
        cur->next = proc;
    }
    nr_running++;
    total_weight += proc->weight;
    cfs_update_min_vruntime();
}

static void cfs_dequeue_process(process_t *proc) {
    if (!ready_queue_head || !proc) return;
    if (ready_queue_head == proc) {
        ready_queue_head = proc->next;
    } else {
        process_t *cur = ready_queue_head;
        while (cur->next) {
            if (cur->next == proc) {
                cur->next = proc->next;
                break;
            }
            cur = cur->next;
        }
    }
    proc->next = NULL;
    if (nr_running > 0) nr_running--;
    if (total_weight >= proc->weight) total_weight -= proc->weight;
    cfs_update_min_vruntime();
}

static void cfs_update_curr(process_t *cp, uint64_t delta_exec) {
    if (!cp || cp->pid == 0) return;
    if (delta_exec == 0) return;
    cp->sum_exec_runtime += delta_exec;
    uint64_t vruntime_delta = (delta_exec * NICE_0_LOAD) / (cp->weight ? cp->weight : NICE_0_LOAD);
    cp->vruntime += vruntime_delta;
}

static uint64_t cfs_calc_slice(process_t *cp) {
    if (!cp || total_weight == 0) return SCHED_MIN_GRAN_TICKS;
    uint64_t slice = (SCHED_LATENCY_TICKS * (uint64_t)cp->weight) / total_weight;
    if (slice < SCHED_MIN_GRAN_TICKS) slice = SCHED_MIN_GRAN_TICKS;
    return slice;
}

void scheduler_add_process(process_t *proc) {
    spinlock_lock(&sched_lock);
    if (!proc) { spinlock_unlock(&sched_lock); return; }
    if (proc->state == PROCESS_STATE_ZOMBIE || proc->state == PROCESS_STATE_TERMINATED) {
        spinlock_unlock(&sched_lock);
        return;
    }
    if (proc->weight == 0) proc->weight = nice_to_weight(proc->nice);
    if (proc->vruntime < min_vruntime) proc->vruntime = min_vruntime;
    cfs_enqueue_process(proc);
    proc->state = PROCESS_STATE_READY;
    debug_print_ready_queue();
    spinlock_unlock(&sched_lock);
}

void scheduler_remove_process(process_t *proc) {
    spinlock_lock(&sched_lock);
    if (!proc) { spinlock_unlock(&sched_lock); return; }
    cfs_dequeue_process(proc);
    debug_print_ready_queue();
    spinlock_unlock(&sched_lock);
}

static void free_user_range(uint64_t start, uint64_t end) {
    if (end <= start) return;
    start &= ~0xFFFULL;
    end = (end + 0xFFFULL) & ~0xFFFULL;

    for (uint64_t cur = start; cur < end; cur += 0x1000ULL) {
        uint64_t phys = paging_virt_to_phys(cur);
        if (phys != 0) {
            paging_unmap_page(cur);
            phys_ref_dec(phys & ~0xFFFULL);
        }
    }
}

void process_free_user_memory(process_t *p) {
    if (!p || !p->is_user) return;

    /* ELF image and user-space allocations: free only user half ranges.
     * We currently use a single global page table, so we must explicitly clean up
     * the ranges we hand out to ring3.
     */

    /* Program image mapped by ELF loader (precise range recorded at exec time). */
    if (p->user_image_base && p->user_image_end && p->user_image_end > p->user_image_base) {
        free_user_range(p->user_image_base, p->user_image_end);
    }

    /* User heap mappings created by sys_sbrk() */
    free_user_range(p->user_heap_base, p->user_heap_end);

    /* User mmap mappings created by sys_mmap() */
    free_user_range(p->user_mmap_base, p->user_mmap_end);

    /* User stack region (including any growth). */
    if (p->user_stack_top && p->user_stack_low && p->user_stack_top > p->user_stack_low) {
        free_user_range(p->user_stack_low, p->user_stack_top);
    } else if (p->user_stack) {
        uint64_t base = (uint64_t)(uintptr_t)p->user_stack;
        free_user_range(base, base + USER_STACK_SIZE);
    }
}

static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void write_cr3(uint64_t v) {
    paging_switch_cr3(v);
}

static int parent_waiting_for_child(process_t *parent, process_t *child) {
    if (!parent || !child) return 0;
    if (!parent->waiting) return 0;

    int32_t wpid = parent->wait_pid;
    if (wpid == -1) return 1;
    if (wpid > 0) return ((uint32_t)wpid == child->pid);
    if (wpid == 0) return (parent->pgid != 0 && child->pgid == parent->pgid);

    // wpid < -1 => specific pgid
    int32_t pg = -wpid;
    return (child->pgid == pg);
}

static void do_switch_and_reap(process_t *old, process_t *newp) {
    char buf[12];

    /* Sanity: never jump to NULL/low memory. */
    if (newp && newp->cpu_state.rip < 0x100000) {
        COM_LOG_ERROR(COM1_PORT, "Refusing to context_switch: suspicious RIP");
        for(;;) { __asm__ volatile("hlt"); }
    }

    if (kernel_debug_is_on()) com_write_string(COM1_PORT, "[SWITCH] Calling context_switch asm...\n");

    /* Prevent IRQs from firing while the stack/CR3/TSS are in transition. */
    __asm__ volatile("cli" ::: "memory");

    /* Save/restore CR3 per process (per-process address spaces). */
    uint64_t cur_cr3 = read_cr3();
    if (old) old->page_table = cur_cr3;

    if (newp && newp->page_table && newp->page_table != cur_cr3) {
        write_cr3(newp->page_table);
    }

    if (newp && newp->kernel_stack) {
        uint64_t top = (stack_top(newp->kernel_stack) - 16) & ~0xFULL;
        amd64_syscall_set_kernel_stack(top);
    }

    /* Restore interrupt flag once the stack/CR3 is stable. */
    __asm__ volatile("sti" ::: "memory");

    /* Lazy FPU switching: set TS depending on whether the next process owns the live FPU state. */
    fpu_lazy_on_context_switch(newp);

    context_switch(old ? &old->cpu_state : NULL, &newp->cpu_state,
                  old ? (void*)old->fpu_state : NULL, (void*)newp->fpu_state);

    /* THIS LINE EXECUTES IN THE NEW PROCESS CONTEXT */
    if (kernel_debug_is_on()) {
        com_write_string(COM1_PORT, "[SWITCH] Back from asm, now in PID ");
        itoa(get_curproc()->pid, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, "\n");
    }

    /* Auto-reap zombies to avoid PID table filling up.
     *
     * IMPORTANT: do NOT reap a zombie just because the parent isn't currently
     * waiting. The parent may call waitpid later; if we destroy the child early,
     * waitpid will block forever (no child exists to signal).
     *
     * We only reap automatically when the parent is gone (or has terminated).
     */
    if (old && old->state == PROCESS_STATE_ZOMBIE) {
        process_t *parent = process_get_by_pid(old->parent_pid);
        if (!parent || parent->state == PROCESS_STATE_ZOMBIE || parent->state == PROCESS_STATE_TERMINATED) {
            process_destroy(old);
            extern void VGA_ForceRedrawConsole(void);
            VGA_ForceRedrawConsole();
        }
    }

    if (kernel_debug_is_on()) com_write_string(COM1_PORT, "[SWITCH] do_switch_and_reap returning to caller\n");
}

void schedule(void) {
    if (!scheduler_enabled) return;

    process_t *old = get_curproc();
    process_t *newp = ready_queue_head ? ready_queue_head : process_table[0];
    if (!newp) return;

    /* Dequeue the next process from the ready queue if it's the head */
    if (ready_queue_head && newp == ready_queue_head) {
        cfs_dequeue_process(newp);
    }

    /* If old process is still running and not idle, re-enqueue it */
    if (old && old->state == PROCESS_STATE_RUNNING && old->pid != 0) {
        uint64_t delta = sched_clock_ticks - old->exec_start;
        cfs_update_curr(old, delta);
        old->exec_start = sched_clock_ticks;
        old->state = PROCESS_STATE_READY;
        cfs_enqueue_process(old);
    }

    /* NOW check if we're switching to the same process.
     * This must be done AFTER dequeuing/re-enqueuing to avoid getting stuck.
     * If old == newp, there's nothing else to run, so just return.
     */
    if (old == newp) return;

    newp->state = PROCESS_STATE_RUNNING;
    newp->exec_start = sched_clock_ticks;
    set_curproc(newp);

    if (newp && newp->kernel_stack) {
        uint64_t top = ((uint64_t)(uintptr_t)newp->kernel_stack + KERNEL_STACK_SIZE - 16) & ~0xFULL;
        amd64_syscall_set_kernel_stack(top);
    }

    do_switch_and_reap(old, newp);
    __asm__ volatile("sti");
}

void scheduler_tick(void) {
    if (!scheduler_enabled) return;
    sched_clock_ticks++;
    process_t *cp = get_curproc();
    if (!cp) return;

    if (cp->pid != 0) {
        uint64_t delta = sched_clock_ticks - cp->exec_start;
        cfs_update_curr(cp, delta);
        cp->exec_start = sched_clock_ticks;
        uint64_t slice = cfs_calc_slice(cp);
        if (delta >= slice) {
            scheduler_request_reschedule();
        }
    }
}

/* Lock-free fast path - no spinlock overhead! */
process_t* process_get_current(void) {
    return get_curproc();  // Already per-CPU, no lock needed
}

process_t* process_get_by_pid(uint32_t pid) {
    if (pid >= MAX_PROCESSES) return NULL;
    rwlock_read_lock(&process_table_rwlock);
    process_t *p = process_table[pid];
    rwlock_read_unlock(&process_table_rwlock);
    return p;
}

void process_exit(int exit_code) {
    /* If this process currently owns the FPU state, drop ownership. */
    fpu_lazy_on_process_exit(current_process);
    process_t *cp = get_curproc();
    if (!cp) return;

    cp->state = PROCESS_STATE_ZOMBIE;
    cp->exit_code = exit_code;

    /* Wake parent waiting in waitpid/waitx */
    process_t *parent = process_get_by_pid(cp->parent_pid);
    if (parent && parent->waiting) {
        int match = 0;
        int32_t wpid = parent->wait_pid;
        if (wpid == -1) match = 1;
        else if (wpid > 0) match = ((uint32_t)wpid == cp->pid);
        else if (wpid == 0) match = (parent->pgid != 0 && cp->pgid == parent->pgid);
        else { /* wpid < -1 */
            int32_t pg = -wpid;
            match = (cp->pgid == pg);
        }

        if (match) {
            parent->waiting = 0;
            parent->wait_result_pid = (int32_t)cp->pid;
            parent->wait_result_status = ((cp->exit_code & 0xFF) << 8);
            if (parent->state == PROCESS_STATE_BLOCKED) {
                parent->state = PROCESS_STATE_READY;
                scheduler_add_process(parent);
            }
        }
    }

    com_write_string(COM1_PORT, "[PROC] Process ");
    char pidbuf[12];
    itoa(cp->pid, pidbuf, 10);
    com_write_string(COM1_PORT, pidbuf);
    com_write_string(COM1_PORT, " exited with code ");
    itoa(exit_code, pidbuf, 10);
    com_write_string(COM1_PORT, pidbuf);
    com_write_string(COM1_PORT, "\n");

    /* Leave as zombie until parent waitpid reaps it. */

    /* Ensure the zombie isn't on the ready queue. */
    scheduler_remove_process(cp);

    /* Switch to next runnable process using the scheduler.
     * If this ever returns, halt.
     */
    schedule();

    for (;;) { __asm__ volatile("hlt"); }
}

void process_kill(uint32_t pid) {
    process_t *p = process_get_by_pid(pid);
    if (!p) return;

    if (p->state != PROCESS_STATE_ZOMBIE && p->state != PROCESS_STATE_TERMINATED) {
        p->state = PROCESS_STATE_TERMINATED;
        scheduler_remove_process(p);

        process_free_user_memory(p);
        if (p->kernel_stack) kfree(p->kernel_stack);
        if (p->argv) free_argv(p->argc, p->argv);
        if (p->envp) free_argv(p->envc, p->envp);

        process_table[pid] = NULL;
        kfree(p);
    }
}

void process_yield(void) {
    if (scheduler_take_reschedule()) {
        /* fall through to schedule() */
    }

    process_t *cp = get_curproc();
    if (!cp) {
        if (kernel_debug_is_on()) com_write_string(COM1_PORT, "[YIELD] Warning: no current process\n");
        return;
    }
    
    if (kernel_debug_is_on()) {
        com_write_string(COM1_PORT, "[YIELD] Process ");
        char buf[12];
        itoa(cp->pid, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, " (");
        com_write_string(COM1_PORT, cp->name);
        com_write_string(COM1_PORT, ") yielding (state=");
        itoa(cp->state, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, ")\n");
        debug_print_ready_queue();
    }
    schedule();

    if (kernel_debug_is_on()) {
        com_write_string(COM1_PORT, "[YIELD] Process ");
        char buf2[12];
        itoa(get_curproc()->pid, buf2, 10);
        com_write_string(COM1_PORT, buf2);
        com_write_string(COM1_PORT, " resumed after yield\n");
    }
}

void process_sleep(uint64_t milliseconds) {
    process_t *cp = get_curproc();
    if (!cp) return;
    cp->state = PROCESS_STATE_SLEEPING;
    cp->time_slice = milliseconds;
    scheduler_remove_process(cp);
    schedule();
}

void process_wake(uint32_t pid) {
    process_t *p = process_get_by_pid(pid);
    if (!p || p->state != PROCESS_STATE_SLEEPING) return;
    p->state = PROCESS_STATE_READY;
    scheduler_add_process(p);
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

    idle->nice = 0;
    idle->weight = nice_to_weight(0);
    idle->vruntime = 0;
    idle->sum_exec_runtime = 0;
    idle->exec_start = 0;

    process_table[0] = idle;
    current_process = idle;

    fpu_lazy_on_context_switch(NULL);

    COM_LOG_OK(COM1_PORT, "Process manager initialized");
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

    /* Early boot: allow userland helpers (userman/login) to run with UID 0
     * so they can register devfs nodes. Once user sessions are established,
     * SYS_SETUID can drop privileges.
     */
    proc->uid = current_process ? current_process->uid : 0;
    if (!current_process || uid_is_kernel(proc->uid)) {
        proc->uid = 0;
    }
    proc->gid = current_process ? current_process->gid : 0;

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

    /* If we're building a user process in a temporary CR3, the kernel stack mapping
     * must exist in both the process PML4 and the kernel PML4; otherwise IRQ entry
     * may switch to an unmapped RSP0 and fault.
     */
    if (g_build_pml4) {
        uint64_t ks_virt = (uint64_t)(uintptr_t)proc->kernel_stack;
        uint64_t ks_phys = paging_virt_to_phys(ks_virt);
        if (!ks_phys) {
            COM_LOG_ERROR(COM1_PORT, "Failed to resolve kernel stack physical address");
            if (proc->argv) free_argv(proc->argc, proc->argv);
            kfree(proc->kernel_stack);
            kfree(proc);
            return NULL;
        }
        int map_rc = paging_map_range_to_pml4(g_build_pml4, ks_virt, ks_phys, KERNEL_STACK_SIZE,
                                              PFLAG_PRESENT | PFLAG_WRITABLE);
        if (map_rc != 0) {
            COM_LOG_ERROR(COM1_PORT, "Failed to map kernel stack into process PML4");
            if (proc->argv) free_argv(proc->argc, proc->argv);
            kfree(proc->kernel_stack);
            kfree(proc);
            return NULL;
        }

        uint64_t *kernel_pml4 = (uint64_t*)phys_to_virt_kernel(kernel_cr3 & 0xFFFFFFFFFFFFF000ULL);
        if (!kernel_pml4) {
            COM_LOG_ERROR(COM1_PORT, "Failed to resolve kernel PML4");
            if (proc->argv) free_argv(proc->argc, proc->argv);
            kfree(proc->kernel_stack);
            kfree(proc);
            return NULL;
        }
        map_rc = paging_map_range_to_pml4(kernel_pml4, ks_virt, ks_phys, KERNEL_STACK_SIZE,
                                          PFLAG_PRESENT | PFLAG_WRITABLE);
        if (map_rc != 0) {
            COM_LOG_ERROR(COM1_PORT, "Failed to map kernel stack into kernel PML4");
            if (proc->argv) free_argv(proc->argc, proc->argv);
            kfree(proc->kernel_stack);
            kfree(proc);
            return NULL;
        }
    }

    memset(&proc->cpu_state, 0, sizeof(cpu_state_t));

    proc->is_user = 0;
    proc->user_rip = 0;
    proc->user_rsp = 0;

    uint64_t ep = (uint64_t)entry_point;
    if (ep >= 0x0000000000400000ULL && ep < 0x0000800000000000ULL) {
        proc->is_user = 1;
        proc->user_rip = ep;
        if (g_build_pml4_phys) {
            proc->page_table = g_build_pml4_phys;
        }

        /* Ensure new user processes start with a clean envp (RBX).
         * The user entry trampoline passes rbx as envp, so leaking kernel
         * values here can crash early in userland startup.
         */
        proc->cpu_state.rbx = 0;

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

        int map_rc;
        if (g_build_pml4) {
            map_rc = paging_map_range_to_pml4(g_build_pml4, user_stack_base, phys_base, USER_STACK_SIZE, PFLAG_PRESENT | PFLAG_WRITABLE | PFLAG_USER);
        } else {
            map_rc = paging_map_range(user_stack_base, phys_base, USER_STACK_SIZE, PFLAG_PRESENT | PFLAG_WRITABLE | PFLAG_USER);
        }
        if (map_rc != 0) {
            COM_LOG_ERROR(COM1_PORT, "Failed to map user stack");
            for (size_t p = 0; p < pages; p++) phys_free_frame(phys_base + p * PAGE_SIZE);
            if (proc->argv) free_argv(proc->argc, proc->argv);
            kfree(proc->kernel_stack);
            kfree(proc);
            return NULL;
        }

        proc->user_stack = (void*)(uintptr_t)user_stack_base;
        /* SysV ABI expects RSP % 16 == 8 on function entry (after call).
         * For _start (no call), set RSP so it looks like a normal entry.
         */
        proc->user_rsp = user_stack_top - 8;
        proc->user_stack_top = user_stack_top;
        proc->user_stack_low = user_stack_base;
        proc->user_stack_limit = user_stack_base;

        proc->user_heap_base = 0x0000005000000000ULL;
        proc->user_heap_end = proc->user_heap_base;
        proc->user_heap_limit = proc->user_heap_base + 64ULL * 1024ULL * 1024ULL;

        proc->user_mmap_base  = 0x0000006000000000ULL;
        proc->user_mmap_end   = proc->user_mmap_base;
        proc->user_mmap_limit = proc->user_mmap_base + 256ULL * 1024ULL * 1024ULL;

        if (proc->argc > 0 && proc->argv) {
            uint64_t sp = proc->user_rsp;
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
                goto argv_done;
            }

            /* Align to 16 bytes before laying out argv. */
            sp &= ~0xFULL;

            sp -= (uint64_t)(proc->argc + 1) * sizeof(uint64_t);
            uint64_t *user_argv = (uint64_t*)(uintptr_t)sp;
            for (int i = 0; i < proc->argc; i++) {
                user_argv[i] = user_str_ptrs[i];
            }
            user_argv[proc->argc] = 0;

            /* Ensure SysV ABI: RSP % 16 == 8 at user entry. */
            if ((sp & 0xFULL) == 0) {
                sp -= 8;
                *(uint64_t*)(uintptr_t)sp = 0;
            }
            proc->user_rsp = sp;

            proc->cpu_state.r12 = (uint64_t)proc->argc;
            proc->cpu_state.r13 = (uint64_t)(uintptr_t)user_argv;
        }
argv_done:
        proc->cpu_state.r14 = proc->user_rip;
        proc->cpu_state.r15 = proc->user_rsp;

        proc->cpu_state.rip = (uint64_t)(uintptr_t)amd64_enter_user_trampoline;
    } else {
        proc->cpu_state.rip = ep;
    }

    uint64_t top = (stack_top(proc->kernel_stack) - 16) & ~0xFULL;

    uint64_t initial_rsp = top - 8;
    uint64_t *ret_slot = (uint64_t *)initial_rsp;
    *ret_slot = (uint64_t)process_return_trampoline;

    proc->cpu_state.rsp = initial_rsp;
    proc->cpu_state.rbp = initial_rsp;
    proc->cpu_state.rflags = 0x202;

    memset(proc->fpu_state, 0, sizeof(proc->fpu_state));

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
        /* Ensure envp is a valid NULL pointer for user entry. */
        proc->cpu_state.rbx = 0;
    }

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
    proc->nice = 0;
    proc->weight = nice_to_weight(0);
    proc->vruntime = min_vruntime;
    proc->sum_exec_runtime = 0;
    proc->exec_start = sched_clock_ticks;

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

void scheduler_init(void) {
    COM_LOG_INFO(COM1_PORT, "Initializing scheduler");
    ready_queue_head = NULL;
    spinlock_init(&sched_lock);
    rwlock_init(&process_table_rwlock);
    spinlock_init(&next_pid_lock);
    scheduler_enabled = 1;
    total_weight = 0;
    nr_running = 0;
    min_vruntime = 0;
    sched_clock_ticks = 0;
    COM_LOG_OK(COM1_PORT, "Scheduler initialized");
}

void scheduler_request_reschedule(void) { g_resched_requested = 1; }

int scheduler_take_reschedule(void) { int v = g_resched_requested; g_resched_requested = 0; return v; }

