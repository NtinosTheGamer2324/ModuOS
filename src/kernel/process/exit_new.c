// exit_new.c - Process exit/termination

#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/spinlock.h"
#include "moduos/kernel/COM/com.h"

extern char *itoa(int value, char *str, int base);

// Coarse lock protecting all parent-child list manipulations.
// Both do_exit() and do_waitpid() must hold this before touching
// any sibling_next/sibling_prev/children/parent pointer.
// Declared non-static so wait_new.c can reference it via extern.
// Initialised once from process_management_init() — never lazily.
spinlock_t children_lock;

// Called once from process_management_init() before any process is created.
void exit_subsystem_init(void) {
    spinlock_init(&children_lock);
}

// Terminate the current process
void do_exit(int status) {
    process_t *p = (process_t *)current;
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
            p->fd_table[i] = NULL;
        }
    }

    // Reparent children to init (PID 1) under the children lock
    spinlock_lock(&children_lock);
    if (p->children) {
        process_t *init = process_find(1);
        if (init) {
            process_t *child = p->children;
            while (child) {
                process_t *next = child->sibling_next;

                child->parent = init;
                child->ppid = init->pid;

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
    spinlock_unlock(&children_lock);

    // Mark zombie BEFORE removing from scheduler.
    p->state = PROCESS_STATE_ZOMBIE;
    /* Encode exit status in POSIX wait-status format:
     *   Normal exit  (status >= 0): wstatus = (status & 0xFF) << 8
     *   Signal death (status < 0) : wstatus = (-status) & 0x7F  */
    if (status < 0)
        p->exit_code = (-status) & 0x7F;
    else
        p->exit_code = (status & 0xFF) << 8;

    // Remove from the run queue now that state is visibly ZOMBIE.
    scheduler_remove(p);

    // Wake up parent if it is sleeping in waitpid
    if (p->parent) {
        wakeup(p->parent);
    }

    // Yield the CPU - we will never run again
    schedule();

    for (;;) {
        __asm__ volatile("hlt");
    }
}
