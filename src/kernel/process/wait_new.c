// wait_new.c - POSIX wait/waitpid implementation

#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/spinlock.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/errno.h"
#include "moduos/kernel/debug.h"

extern char *itoa(int value, char *str, int base);

// Defined in exit_new.c - protects sibling/children list traversal
extern spinlock_t children_lock;

// Wait for any child to exit
int do_wait(int *status) {
    return do_waitpid(-1, status, 0);
}

// Wait for a specific child (pid > 0), any child (pid == -1),
// or any child in the current process group (pid == 0).
int do_waitpid(int32_t pid, int *status, int options) {
    process_t *parent = process_get_current();
    
    // Debug: show what current is
    // TEMPORARILY DISABLED TO AVOID FAULT
    // com_write_string(COM1_PORT, "[WAIT] do_waitpid called, current=0x");
    // com_write_hex64(COM1_PORT, (uint64_t)parent);
    // com_write_string(COM1_PORT, "\n");
    if (!parent) {
        com_write_string(COM1_PORT, "[WAIT] ERROR: current is NULL!\n");
        return -1;
    }
    
    // Safety check: verify parent pointer is valid (not a tiny value)
    if ((uint64_t)parent < 0xFFFF800000000000ULL) {
        com_write_string(COM1_PORT, "[WAIT] ERROR: current pointer invalid: 0x");
        com_write_hex64(COM1_PORT, (uint64_t)parent);
        com_write_string(COM1_PORT, " (looks like PID instead of pointer!)\n");
        return -ESRCH;
    }

    char buf[16];

    while (1) {
        spinlock_lock(&children_lock);

        if (!parent->children) {
            spinlock_unlock(&children_lock);
            if (kernel_debug_is_on())
                com_write_string(COM1_PORT, "[WAIT] No children\n");
            return -ECHILD;
        }

        /* For pid > 0: verify at least one child with that PID exists before
         * sleeping, otherwise return ECHILD immediately. */
        if (pid > 0) {
            int found_match = 0;
            process_t *c = parent->children;
            while (c) {
                if (c->pid == (uint32_t)pid) { found_match = 1; break; }
                c = c->sibling_next;
            }
            if (!found_match) {
                spinlock_unlock(&children_lock);
                return -ECHILD;
            }
        }

        process_t *child = parent->children;
        process_t *found = NULL;

        while (child) {
            int match = (pid == -1) ||
                        (pid > 0  && (uint32_t)pid == child->pid) ||
                        (pid == 0 && child->pgid == parent->pgid) ||
                        (pid < -1 && child->pgid == (uint32_t)(-pid));
            if (match && child->state == PROCESS_STATE_ZOMBIE) {
                found = child;
                break;
            }
            child = child->sibling_next;
        }

        if (found) {
            if (kernel_debug_is_on()) {
                com_write_string(COM1_PORT, "[WAIT] Reaping zombie PID ");
                itoa((int)found->pid, buf, 10);
                com_write_string(COM1_PORT, buf);
                com_write_string(COM1_PORT, " exit=");
                itoa(found->exit_code, buf, 10);
                com_write_string(COM1_PORT, buf);
                com_write_string(COM1_PORT, "\n");
            }

            uint32_t child_pid = found->pid;
            int exit_code = found->exit_code;

            // Unlink from parent's children list.
            if (found->sibling_prev)
                found->sibling_prev->sibling_next = found->sibling_next;
            else
                parent->children = found->sibling_next;
            if (found->sibling_next)
                found->sibling_next->sibling_prev = found->sibling_prev;

            spinlock_unlock(&children_lock);

            extern void process_destroy(process_t *p);
            extern void process_free_user_memory(process_t *p);
            extern void paging_switch_cr3(uint64_t new_cr3_phys);

            // Switch to the child's CR3 so that paging_virt_to_phys() and
            // paging_unmap_page() operate on the child's address space, not the
            // parent's.  Without this, free_user_range() would unmap the parent's
            // pages at the same virtual addresses, corrupting the parent.
            
            // Get the ACTUAL current CR3 - this is what we need to restore to
            uint64_t actual_current_cr3;
            __asm__ volatile("mov %%cr3, %0" : "=r"(actual_current_cr3));
            
            com_write_string(COM1_PORT, "[WAIT] Parent PID=");
            char pbuf[16];
            itoa((int)parent->pid, pbuf, 10);
            com_write_string(COM1_PORT, pbuf);
            com_write_string(COM1_PORT, " page_table=0x");
            com_write_hex64(COM1_PORT, parent->page_table);
            com_write_string(COM1_PORT, " cr3=0x");
            com_write_hex64(COM1_PORT, parent->cr3);
            com_write_string(COM1_PORT, "\n");
            com_write_string(COM1_PORT, "[WAIT] Current CR3=0x");
            com_write_hex64(COM1_PORT, actual_current_cr3);
            com_write_string(COM1_PORT, "\n");
            
            uint64_t child_cr3 = found->page_table ? found->page_table : found->cr3;
            com_write_string(COM1_PORT, "[WAIT] Child CR3=0x");
            com_write_hex64(COM1_PORT, child_cr3);
            com_write_string(COM1_PORT, "\n");
            
            if (child_cr3 && child_cr3 != actual_current_cr3) {
                com_write_string(COM1_PORT, "[WAIT] Switching to child CR3\n");
                paging_switch_cr3(child_cr3);
            }

            process_free_user_memory(found);

            // Restore the parent's address space before returning to it.
            if (child_cr3 && child_cr3 != actual_current_cr3) {
                com_write_string(COM1_PORT, "[WAIT] Restoring parent CR3\n");
                paging_switch_cr3(actual_current_cr3);
            }
            
            uint64_t verify_cr3;
            __asm__ volatile("mov %%cr3, %0" : "=r"(verify_cr3));
            com_write_string(COM1_PORT, "[WAIT] After restore CR3=0x");
            com_write_hex64(COM1_PORT, verify_cr3);
            com_write_string(COM1_PORT, "\n");

            process_destroy(found);

            if (kernel_debug_is_on()) {
                com_write_string(COM1_PORT, "[WAIT] Reap done, returning child PID ");
                itoa((int)child_pid, buf, 10);
                com_write_string(COM1_PORT, buf);
                com_write_string(COM1_PORT, "\n");
                com_write_string(COM1_PORT, "[WAIT] About to write exit_code to status pointer\n");
            }
            if (status) *status = exit_code;
            if (kernel_debug_is_on())
                com_write_string(COM1_PORT, "[WAIT] Exit code written, about to return\n");
            return (int)child_pid;
        }

        spinlock_unlock(&children_lock);

        if (options & 1) {  // WNOHANG
            return 0;
        }

        // Sleep until a child exits — process_exit() calls wakeup(parent).
        sleep_on(parent);
    }
}
