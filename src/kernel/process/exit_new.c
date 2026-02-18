// exit_new.c - Process exit/termination

#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/COM/com.h"

// Terminate the current process
void do_exit(int status) {
    process_t *p = current;
    if (!p) return;
    
    com_write_string(COM1_PORT, "[EXIT] PID ");
    char buf[16];
    itoa(p->pid, buf, 10);
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, " exiting with status ");
    itoa(status, buf, 10);
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, "\n");
    
    // Close all file descriptors
    for (int i = 0; i < PROCESS_MAX_FDS; i++) {
        if (p->fd_table[i]) {
            // Would close FD here
            p->fd_table[i] = NULL;
        }
    }
    
    // Reparent children to init (PID 1)
    if (p->children) {
        process_t *init = process_find(1);
        if (init) {
            process_t *child = p->children;
            while (child) {
                process_t *next = child->sibling_next;
                
                child->parent = init;
                child->ppid = init->pid;
                
                // Add to init's children list
                child->sibling_next = init->children;
                child->sibling_prev = NULL;
                if (init->children) {
                    init->children->sibling_prev = child;
                }
                init->children = child;
                
                child = next;
            }
        }
        p->children = NULL;
    }
    
    // Remove from scheduler
    scheduler_remove(p);
    
    // Mark as zombie
    p->state = PROCESS_STATE_ZOMBIE;
    p->exit_code = status;
    
    // Wake up parent if it's waiting
    if (p->parent) {
        wakeup(p->parent);
    }
    
    // Reschedule - we'll never run again
    schedule();
    
    // Should never reach here
    for (;;) {
        __asm__ volatile("hlt");
    }
}
