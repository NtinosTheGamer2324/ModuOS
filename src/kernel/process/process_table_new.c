// process_table_new.c - Process table management and PID allocation

#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/memory/kheap.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/spinlock.h"

// Compatibility macros
#define spinlock_acquire spinlock_lock
#define spinlock_release spinlock_unlock

// Global process table
process_t *process_table[MAX_PROCESSES];
uint32_t next_pid = 1;  // PID 0 reserved for idle
process_t *current = NULL;

// Lock for process table access
static spinlock_t ptable_lock;

void process_table_init(void) {
    spinlock_init(&ptable_lock);
    
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_table[i] = NULL;
    }
    
    next_pid = 1;
    current = NULL;
}

// Allocate a new process slot
process_t *process_alloc(void) {
    spinlock_acquire(&ptable_lock);
    
    // Find free slot
    int slot = -1;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i] == NULL) {
            slot = i;
            break;
        }
    }
    
    if (slot == -1) {
        spinlock_release(&ptable_lock);
        return NULL;  // No free slots
    }
    
    // Allocate process structure
    process_t *p = (process_t *)kmalloc(sizeof(process_t));
    if (!p) {
        spinlock_release(&ptable_lock);
        return NULL;
    }
    
    // Zero initialize
    memset(p, 0, sizeof(process_t));
    
    // Allocate FPU state
    p->fpu_state = kmalloc(512);  // FXSAVE area
    if (!p->fpu_state) {
        kfree(p);
        spinlock_release(&ptable_lock);
        return NULL;
    }
    memset(p->fpu_state, 0, 512);
    
    // Assign PID
    p->pid = next_pid++;
    p->state = PROCESS_STATE_EMBRYO;
    
    // Insert into table
    process_table[slot] = p;
    
    spinlock_release(&ptable_lock);
    return p;
}

// Free a process slot
void process_free(process_t *p) {
    if (!p) return;
    
    spinlock_acquire(&ptable_lock);
    
    // Remove from table
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i] == p) {
            process_table[i] = NULL;
            break;
        }
    }
    
    spinlock_release(&ptable_lock);
    
    // Free resources
    if (p->fpu_state) kfree(p->fpu_state);
    if (p->kernel_stack) kfree(p->kernel_stack);
    
    // Free argv
    if (p->argv) {
        for (int i = 0; i < p->argc; i++) {
            if (p->argv[i]) kfree(p->argv[i]);
        }
        kfree(p->argv);
    }
    
    // Free envp
    if (p->envp) {
        for (int i = 0; p->envp[i] != NULL; i++) {
            kfree(p->envp[i]);
        }
        kfree(p->envp);
    }
    
    kfree(p);
}

// Find process by PID
process_t *process_find(uint32_t pid) {
    spinlock_acquire(&ptable_lock);
    
    process_t *p = NULL;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i] && process_table[i]->pid == pid) {
            p = process_table[i];
            break;
        }
    }
    
    spinlock_release(&ptable_lock);
    return p;
}
