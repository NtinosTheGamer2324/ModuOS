// scheduler.c - CFS (Completely Fair Scheduler) implementation
#include "moduos/kernel/process/process.h"
#include "moduos/kernel/spinlock.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/debug.h"
#include "moduos/kernel/memory/string.h"
#include <stdint.h>

/* COM logging macros */
#define COM_LOG_INFO(port, msg) com_write_string(port, "[INFO] " msg "\n")
#define COM_LOG_OK(port, msg) com_write_string(port, "[OK] " msg "\n")

/* CFS scheduler state */
static process_t *ready_queue_head = NULL;
static spinlock_t sched_lock __attribute__((aligned(64)));
static int scheduler_enabled = 0;
static int g_resched_requested = 0;

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

/* Debug helper to print ready queue */
void debug_print_ready_queue(void) {
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
    if (!cp) return;
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

/* Forward declarations from process.c */
extern void do_switch_and_reap(process_t *old, process_t *newp);
extern process_t *process_get_current(void);
extern void set_curproc(process_t *p);
extern void amd64_syscall_set_kernel_stack(uint64_t top);

/* Access to process table from process.c */
process_t **get_process_table(void); // Defined in process.c

void schedule(void) {
    if (!scheduler_enabled) return;

    process_t *old = process_get_current();
    process_t **process_table = get_process_table();

    /* If old process is still running, re-enqueue it FIRST (including idle/PID 0) */
    if (old && old->state == PROCESS_STATE_RUNNING) {
        uint64_t delta = sched_clock_ticks - old->exec_start;
        cfs_update_curr(old, delta);
        old->exec_start = sched_clock_ticks;
        old->state = PROCESS_STATE_READY;
        
        /* Ensure weight is set before enqueueing */
        if (old->weight == 0) old->weight = nice_to_weight(old->nice);
        
        cfs_enqueue_process(old);
    }

    /* NOW pick the next process (after old was re-enqueued) */
    process_t *newp = ready_queue_head ? ready_queue_head : process_table[0];
    if (!newp) return;

    /* Dequeue the next process from the ready queue if it's the head */
    if (ready_queue_head && newp == ready_queue_head) {
        cfs_dequeue_process(newp);
    }

    /* NOW check if we're switching to the same process. */
    if (old == newp) return;

    newp->state = PROCESS_STATE_RUNNING;
    newp->exec_start = sched_clock_ticks;
    set_curproc(newp);

    if (newp && newp->kernel_stack) {
        uint64_t top = ((uint64_t)(uintptr_t)newp->kernel_stack + KERNEL_STACK_SIZE - 16) & ~0xFULL;
        amd64_syscall_set_kernel_stack(top);
    }

    do_switch_and_reap(old, newp);
}

void scheduler_tick(void) {
    if (!scheduler_enabled) return;
    sched_clock_ticks++;
    process_t *cp = process_get_current();
    if (!cp) return;

    uint64_t delta = sched_clock_ticks - cp->exec_start;
    cfs_update_curr(cp, delta);
    cp->exec_start = sched_clock_ticks;
    uint64_t slice = cfs_calc_slice(cp);
    if (delta >= slice) {
        scheduler_request_reschedule();
    }
}

void scheduler_init(void) {
    COM_LOG_INFO(COM1_PORT, "Initializing scheduler");
    ready_queue_head = NULL;
    spinlock_init(&sched_lock);
    scheduler_enabled = 1;
    total_weight = 0;
    nr_running = 0;
    min_vruntime = 0;
    sched_clock_ticks = 0;
    COM_LOG_OK(COM1_PORT, "Scheduler initialized");
}

void scheduler_request_reschedule(void) { g_resched_requested = 1; }

int scheduler_take_reschedule(void) { int v = g_resched_requested; g_resched_requested = 0; return v; }

uint64_t scheduler_get_min_vruntime(void) { return min_vruntime; }

uint32_t scheduler_nice_to_weight(int nice) { return nice_to_weight(nice); }

uint64_t scheduler_get_clock_ticks(void) { return sched_clock_ticks; }
