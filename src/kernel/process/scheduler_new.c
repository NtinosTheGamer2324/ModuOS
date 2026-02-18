// scheduler_new.c - CFS (Completely Fair Scheduler) implementation

#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/spinlock.h"
#include "moduos/kernel/COM/com.h"

// Compatibility macros
#define spinlock_acquire spinlock_lock
#define spinlock_release spinlock_unlock
#include "moduos/kernel/memory/string.h"

// Scheduler state
static process_t *runqueue_head = NULL;  // Red-black tree root (simplified to linked list)
static process_t *runqueue_tail = NULL;
static uint64_t min_vruntime = 0;
static int scheduler_enabled = 0;
static spinlock_t sched_lock;

// CFS constants
#define SCHED_LATENCY_NS    6000000ULL   // 6ms target latency
#define MIN_GRANULARITY_NS  750000ULL    // 0.75ms minimum time slice
#define NICE_0_WEIGHT       1024

// Nice value to weight conversion (from Linux kernel)
static const uint32_t nice_to_weight_table[40] = {
    /* -20 */ 88761, 71755, 56483, 46273, 36291,
    /* -15 */ 29154, 23254, 18705, 14949, 11916,
    /* -10 */ 9548,  7620,  6100,  4904,  3906,
    /*  -5 */ 3121,  2501,  1991,  1586,  1277,
    /*   0 */ 1024,  820,   655,   526,   423,
    /*   5 */ 335,   272,   215,   172,   137,
    /*  10 */ 110,   87,    70,    56,    45,
    /*  15 */ 36,    29,    23,    18,    15,
};

static uint32_t nice_to_weight(int nice) {
    if (nice < -20) nice = -20;
    if (nice > 19) nice = 19;
    return nice_to_weight_table[nice + 20];
}

void scheduler_init(void) {
    spinlock_init(&sched_lock);
    runqueue_head = NULL;
    runqueue_tail = NULL;
    min_vruntime = 0;
    scheduler_enabled = 1;
    
    com_write_string(COM1_PORT, "[SCHED] CFS scheduler initialized\n");
}

// Add process to run queue (sorted by vruntime)
void scheduler_add(process_t *p) {
    if (!p) return;
    
    spinlock_acquire(&sched_lock);
    
    // Set weight if not set
    if (p->weight == 0) {
        p->weight = nice_to_weight(p->nice);
    }
    
    // New process gets min_vruntime to avoid starvation
    if (p->vruntime == 0) {
        p->vruntime = min_vruntime;
    }
    
    // Insert sorted by vruntime (simple linked list for now)
    if (!runqueue_head) {
        runqueue_head = runqueue_tail = p;
        p->sched_next = p->sched_prev = NULL;
    } else {
        // Insert at correct position
        process_t *curr = runqueue_head;
        while (curr && curr->vruntime < p->vruntime) {
            curr = curr->sched_next;
        }
        
        if (!curr) {
            // Insert at tail
            runqueue_tail->sched_next = p;
            p->sched_prev = runqueue_tail;
            p->sched_next = NULL;
            runqueue_tail = p;
        } else if (curr == runqueue_head) {
            // Insert at head
            p->sched_next = runqueue_head;
            p->sched_prev = NULL;
            runqueue_head->sched_prev = p;
            runqueue_head = p;
        } else {
            // Insert in middle
            p->sched_next = curr;
            p->sched_prev = curr->sched_prev;
            curr->sched_prev->sched_next = p;
            curr->sched_prev = p;
        }
    }
    
    p->state = PROCESS_STATE_RUNNABLE;
    
    spinlock_release(&sched_lock);
}

// Remove process from run queue
void scheduler_remove(process_t *p) {
    if (!p) return;
    
    spinlock_acquire(&sched_lock);
    
    if (p->sched_prev) {
        p->sched_prev->sched_next = p->sched_next;
    } else if (p == runqueue_head) {
        runqueue_head = p->sched_next;
    }
    
    if (p->sched_next) {
        p->sched_next->sched_prev = p->sched_prev;
    } else if (p == runqueue_tail) {
        runqueue_tail = p->sched_prev;
    }
    
    p->sched_next = p->sched_prev = NULL;
    
    spinlock_release(&sched_lock);
}

// Pick next process to run
static process_t *pick_next_task(void) {
    // CFS: pick process with minimum vruntime
    return runqueue_head;
}

// Update process runtime and vruntime
static void update_curr(process_t *p, uint64_t delta_ns) {
    if (!p) return;
    
    // Calculate vruntime increment based on weight
    // vruntime += delta * (NICE_0_WEIGHT / weight)
    uint64_t vruntime_delta = (delta_ns * NICE_0_WEIGHT) / p->weight;
    p->vruntime += vruntime_delta;
    
    // Update min_vruntime
    if (runqueue_head && runqueue_head->vruntime < min_vruntime) {
        min_vruntime = runqueue_head->vruntime;
    }
}

// Main scheduler function - pick next process to run
void schedule(void) {
    if (!scheduler_enabled) {
        com_write_string(COM1_PORT, "[SCHED] Scheduler not enabled\n");
        return;
    }
    
    spinlock_acquire(&sched_lock);
    
    process_t *prev = current;
    process_t *next = NULL;
    
    com_write_string(COM1_PORT, "[SCHED] schedule() called, current PID=");
    if (prev) {
        char buf[16];
        itoa(prev->pid, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, " state=");
        itoa(prev->state, buf, 10);
        com_write_string(COM1_PORT, buf);
    } else {
        com_write_string(COM1_PORT, "NULL");
    }
    com_write_string(COM1_PORT, "\n");
    
    // If current process is still runnable, add it back to queue
    if (prev && prev->state == PROCESS_STATE_RUNNING) {
        prev->state = PROCESS_STATE_RUNNABLE;
        scheduler_add(prev);
        com_write_string(COM1_PORT, "[SCHED] Added prev process back to queue\n");
    }
    
    // Pick next task
    next = pick_next_task();
    
    com_write_string(COM1_PORT, "[SCHED] pick_next_task returned ");
    if (next) {
        char buf[16];
        com_write_string(COM1_PORT, "PID=");
        itoa(next->pid, buf, 10);
        com_write_string(COM1_PORT, buf);
    } else {
        com_write_string(COM1_PORT, "NULL");
    }
    com_write_string(COM1_PORT, "\n");
    
    if (!next) {
        // No runnable tasks, switch to idle (PID 0)
        com_write_string(COM1_PORT, "[SCHED] No runnable tasks, finding idle (PID 0)\n");
        next = process_find(0);
    }
    
    if (next) {
        scheduler_remove(next);
        next->state = PROCESS_STATE_RUNNING;
        next->exec_start = 0;  // Will be set by scheduler_tick
        current = next;
        
        com_write_string(COM1_PORT, "[SCHED] Selected next PID=");
        char buf[16];
        itoa(next->pid, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, "\n");
    }
    
    spinlock_release(&sched_lock);
    
    // Context switch if needed
    if (prev != next && next) {
        com_write_string(COM1_PORT, "[SCHED] Context switching from ");
        if (prev) {
            char buf[16];
            itoa(prev->pid, buf, 10);
            com_write_string(COM1_PORT, buf);
        } else {
            com_write_string(COM1_PORT, "NULL");
        }
        com_write_string(COM1_PORT, " to ");
        char buf[16];
        itoa(next->pid, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, "\n");
        
        extern void switch_to(process_t *prev, process_t *next);
        switch_to(prev, next);
    } else {
        com_write_string(COM1_PORT, "[SCHED] No context switch needed (prev == next)\n");
    }
}

// Called from timer IRQ
void scheduler_tick(void) {
    if (!scheduler_enabled || !current) return;
    
    // Update current process runtime
    // In real implementation, would measure actual time
    uint64_t delta_ns = 1000000;  // 1ms tick approximation
    
    spinlock_acquire(&sched_lock);
    update_curr(current, delta_ns);
    spinlock_release(&sched_lock);
    
    // Check if we should preempt
    // Simple rule: if current has run for its fair share, reschedule
    if (runqueue_head && current->vruntime > runqueue_head->vruntime + MIN_GRANULARITY_NS) {
        schedule();
    }
    
    // Wake up sleeping processes (simplified)
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = process_table[i];
        if (p && p->state == PROCESS_STATE_SLEEPING) {
            // For now, just wake after some time
            // Real implementation would check wait_channel and events
            // This is a placeholder - proper sleep/wakeup in separate functions
        }
    }
}

// Sleep on a channel
void sleep_on(void *channel) {
    if (!current) return;
    
    spinlock_acquire(&sched_lock);
    current->wait_channel = channel;
    current->state = PROCESS_STATE_SLEEPING;
    scheduler_remove(current);
    spinlock_release(&sched_lock);
    
    schedule();  // Give up CPU
}

// Wake up all processes sleeping on channel
void wakeup(void *channel) {
    spinlock_acquire(&sched_lock);
    
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = process_table[i];
        if (p && p->state == PROCESS_STATE_SLEEPING && p->wait_channel == channel) {
            p->wait_channel = NULL;
            p->state = PROCESS_STATE_RUNNABLE;
            scheduler_add(p);
        }
    }
    
    spinlock_release(&sched_lock);
}

// Check if we should reschedule (called from timer interrupt)
int should_reschedule(void) {
    if (!scheduler_enabled || !current || !runqueue_head) return 0;
    
    // If current process is not running anymore, we must reschedule
    if (current->state != PROCESS_STATE_RUNNING) return 1;
    
    // Check if there's a process with lower vruntime (higher priority)
    if (runqueue_head->vruntime + MIN_GRANULARITY_NS < current->vruntime) {
        return 1;  // Preempt if difference is significant
    }
    
    return 0;
}
