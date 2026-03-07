// process_table.c - Process table, PID allocation, and per-PID lookup

#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/memory/kheap.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/rwlock.h"
#include "moduos/kernel/spinlock.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

process_t *process_table[MAX_PROCESSES];
volatile process_t *current = NULL;

static spinlock_t ptable_lock __attribute__((aligned(64)));
uint32_t next_pid = 1;   // PID 0 reserved for idle; extern-declared in process_new.h

// ---------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------

void process_table_init(void) {
    spinlock_init(&ptable_lock);
    for (int i = 0; i < MAX_PROCESSES; i++)
        process_table[i] = NULL;
    next_pid = 1;
    current  = NULL;
}

// process_table_compat_init() was a separate call in process_init_new.c;
// keep as a no-op so the call site compiles unchanged.
void process_table_compat_init(void) { /* merged into process_table_init() */ }

// ---------------------------------------------------------------------------
// PID allocation
// ---------------------------------------------------------------------------

// Scan for a free slot starting at next_pid, wrapping once.
// (legacy code disabled)

uint32_t process_alloc_pid(void) {
    spinlock_lock(&ptable_lock);
    for (uint32_t count = 0; count < MAX_PROCESSES - 1; count++) {
        uint32_t pid = next_pid;
        next_pid = (next_pid + 1 < MAX_PROCESSES) ? next_pid + 1 : 1;
        if (pid == 0) continue;
        if (process_table[pid] == NULL) {
            spinlock_unlock(&ptable_lock);
            return pid;
        }
    }
    spinlock_unlock(&ptable_lock);
    return 0;   // table full
}

// ---------------------------------------------------------------------------
// process_alloc / process_free / process_find
// ---------------------------------------------------------------------------

process_t *process_alloc(void) {
    spinlock_lock(&ptable_lock);

    uint32_t pid = 0;
    for (uint32_t i = next_pid; i < MAX_PROCESSES; i++) {
        if (process_table[i] == NULL) { pid = i; break; }
    }
    if (pid == 0) {
        for (uint32_t i = 1; i < next_pid; i++) {
            if (process_table[i] == NULL) { pid = i; break; }
        }
    }
    if (pid == 0) { spinlock_unlock(&ptable_lock); return NULL; }

    process_t *p = (process_t *)kmalloc(sizeof(process_t));
    if (!p) { spinlock_unlock(&ptable_lock); return NULL; }
    
    extern int com_write_string(uint16_t, const char*);
    extern int com_write_hex64(uint16_t, uint64_t);
    com_write_string(0x3F8, "[PROC_ALLOC] Allocated process struct at 0x");
    com_write_hex64(0x3F8, (uint64_t)p);
    com_write_string(0x3F8, "\n");
    
    memset(p, 0, sizeof(process_t));

    p->fpu_state = kmalloc(512);   // FXSAVE area
    if (!p->fpu_state) { kfree(p); spinlock_unlock(&ptable_lock); return NULL; }
    memset(p->fpu_state, 0, 512);

    p->pid      = pid;
    p->refcount = 1;
    p->state    = PROCESS_STATE_EMBRYO;

    process_table[pid] = p;
    next_pid = (pid + 1 < MAX_PROCESSES) ? pid + 1 : 1;

    spinlock_unlock(&ptable_lock);
    return p;
}

void process_free(process_t *p) {
    if (!p) return;

    spinlock_lock(&ptable_lock);
    if (p->pid < MAX_PROCESSES && process_table[p->pid] == p)
        process_table[p->pid] = NULL;
    spinlock_unlock(&ptable_lock);

    if (p->fpu_state)   kfree(p->fpu_state);
    if (p->kernel_stack) kfree(p->kernel_stack);

    if (p->argv) {
        for (int i = 0; i < p->argc; i++)
            if (p->argv[i]) kfree(p->argv[i]);
        kfree(p->argv);
    }
    if (p->envp) {
        for (int i = 0; p->envp[i]; i++)
            kfree(p->envp[i]);
        kfree(p->envp);
    }

    kfree(p);
}

// O(1) PID lookup.
process_t *process_find(uint32_t pid) {
    if (pid >= MAX_PROCESSES) return NULL;
    spinlock_lock(&ptable_lock);
    process_t *p = process_table[pid];
    spinlock_unlock(&ptable_lock);
    return p;
}

// ---------------------------------------------------------------------------
// Registration (used by the old process_create_with_args path in process.c)
// ---------------------------------------------------------------------------

int process_register(process_t *proc) {
    if (!proc || proc->pid == 0 || proc->pid >= MAX_PROCESSES) return -1;
    spinlock_lock(&ptable_lock);
    if (process_table[proc->pid] != NULL) {
        spinlock_unlock(&ptable_lock);
        return -1;
    }
    process_table[proc->pid] = proc;
    spinlock_unlock(&ptable_lock);
    return 0;
}

int process_unregister(uint32_t pid) {
    if (pid >= MAX_PROCESSES) return -1;
    spinlock_lock(&ptable_lock);
    if (!process_table[pid]) { spinlock_unlock(&ptable_lock); return -1; }
    process_table[pid] = NULL;
    spinlock_unlock(&ptable_lock);
    return 0;
}

// process_get_by_pid is a consistent alias for process_find used by legacy callers.
process_t *process_get_by_pid(uint32_t pid) {
    return process_find(pid);
}

process_t **get_process_table(void) {
    return (process_t **)process_table;
}

/* Initialize process subsystem: table and scheduler */
void process_subsystem_init(void) {
    process_table_init();
    /* scheduler_init is defined in scheduler.c */
    extern void scheduler_init(void);
    scheduler_init();
}
