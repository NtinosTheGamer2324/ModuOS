// wait_new.c - POSIX wait/waitpid implementation

#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/COM/com.h"

// Wait for any child to exit
int do_wait(int *status) {
    return do_waitpid(-1, status, 0);
}

// Wait for specific child (or any if pid=-1)
int do_waitpid(uint32_t pid, int *status, int options) {
    process_t *parent = current;
    if (!parent) {
        return -1;
    }
    
    com_write_string(COM1_PORT, "[WAIT] PID ");
    char buf[16];
    itoa(parent->pid, buf, 10);
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, " waiting for child");
    if (pid != (uint32_t)-1) {
        com_write_string(COM1_PORT, " PID ");
        itoa(pid, buf, 10);
        com_write_string(COM1_PORT, buf);
    }
    com_write_string(COM1_PORT, "\n");
    
    // Check if parent has any children
    if (!parent->children) {
        com_write_string(COM1_PORT, "[WAIT] No children\n");
        return -1;  // ECHILD: no children
    }
    
    // Look for zombie child
    while (1) {
        process_t *child = parent->children;
        process_t *found = NULL;
        
        while (child) {
            // Check if this child matches the criteria
            if (pid == (uint32_t)-1 || child->pid == pid) {
                if (child->state == PROCESS_STATE_ZOMBIE) {
                    found = child;
                    break;
                }
            }
            child = child->sibling_next;
        }
        
        if (found) {
            // Found a zombie child
            com_write_string(COM1_PORT, "[WAIT] Found zombie child PID ");
            itoa(found->pid, buf, 10);
            com_write_string(COM1_PORT, buf);
            com_write_string(COM1_PORT, "\n");
            
            uint32_t child_pid = found->pid;
            int exit_code = found->exit_code;
            
            // Remove from parent's children list
            if (found->sibling_prev) {
                found->sibling_prev->sibling_next = found->sibling_next;
            } else {
                parent->children = found->sibling_next;
            }
            
            if (found->sibling_next) {
                found->sibling_next->sibling_prev = found->sibling_prev;
            }
            
            // Free the zombie process
            process_free(found);
            
            // Return exit status
            if (status) {
                *status = exit_code;
            }
            
            return child_pid;
        }
        
        // No zombie found yet
        if (options & 1) {  // WNOHANG
            return 0;
        }
        
        // Sleep until a child exits
        // Use parent process itself as the wait channel
        sleep_on(parent);
    }
}
