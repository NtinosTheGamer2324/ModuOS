// signals_new.c - Basic signal handling (POSIX-style)

#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/COM/com.h"

// Signal numbers (subset of POSIX signals)
#define SIGKILL  9
#define SIGTERM 15
#define SIGCHLD 17

// Send a signal to a process
int send_signal(uint32_t pid, int sig) {
    process_t *p = process_find(pid);
    if (!p) {
        return -1;  // Process not found
    }
    
    com_write_string(COM1_PORT, "[SIGNAL] Sending signal ");
    char buf[16];
    itoa(sig, buf, 10);
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, " to PID ");
    itoa(pid, buf, 10);
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, "\n");
    
    // Set pending signal bit
    p->pending_signals |= (1ULL << sig);
    
    // If process is sleeping, wake it up
    if (p->state == PROCESS_STATE_SLEEPING) {
        p->state = PROCESS_STATE_RUNNABLE;
        scheduler_add(p);
    }
    
    return 0;
}

// Check for pending signals (called before returning to user mode)
void check_signals(void) {
    process_t *p = current;
    if (!p) return;
    
    uint64_t pending = p->pending_signals & ~p->blocked_signals;
    if (!pending) return;
    
    // Find highest priority signal
    for (int sig = 1; sig < 64; sig++) {
        if (pending & (1ULL << sig)) {
            // Clear pending bit
            p->pending_signals &= ~(1ULL << sig);
            
            // Handle signal
            switch (sig) {
                case SIGKILL:
                    com_write_string(COM1_PORT, "[SIGNAL] SIGKILL - terminating\n");
                    do_exit(128 + sig);
                    break;
                    
                case SIGTERM:
                    com_write_string(COM1_PORT, "[SIGNAL] SIGTERM - terminating\n");
                    do_exit(128 + sig);
                    break;
                    
                case SIGCHLD:
                    // Ignore for now (parent will check in wait)
                    break;
                    
                default:
                    // Default: terminate
                    com_write_string(COM1_PORT, "[SIGNAL] Signal ");
                    char buf[16];
                    itoa(sig, buf, 10);
                    com_write_string(COM1_PORT, buf);
                    com_write_string(COM1_PORT, " - terminating\n");
                    do_exit(128 + sig);
                    break;
            }
            
            break;  // Only handle one signal at a time
        }
    }
}
