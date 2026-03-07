// signals_new.c - Basic signal handling (POSIX-style)

#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/COM/com.h"

extern char *itoa(int value, char *str, int base);

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
    
    /* Guard against UB: shifting a 64-bit value by >= 64 is undefined in C. */
    if (sig < 1 || sig >= 64) return -1;

    // Set pending signal bit
    p->pending_signals |= (1ULL << sig);
    
    /* If the process is sleeping, move it to the compat CFS queue so that
     * schedule() can pick it up.  scheduler_add() delegates to the same
     * compat queue, so this is consistent with the wakeup() path. */
    if (p->state == PROCESS_STATE_SLEEPING) {
        p->state = PROCESS_STATE_RUNNABLE;
        scheduler_add(p);   /* → scheduler_add_process() → compat CFS queue */
    }
    
    return 0;
}

// Set signal handler
uint64_t do_signal(int sig, uint64_t handler) {
    process_t *p = (process_t *)current;
    if (!p) return (uint64_t)-1;
    
    if (sig < 1 || sig >= 64) return (uint64_t)-1;
    
    // SIGKILL and SIGSTOP cannot be caught or ignored
    if (sig == 9 || sig == 19) return (uint64_t)-1;
    
    uint64_t old_handler = p->signal_handlers[sig];
    p->signal_handlers[sig] = handler;
    
    return old_handler;
}

// Check for pending signals (called before returning to user mode)
void check_signals(void) {
    process_t *p = (process_t *)current;
    if (!p) return;
    
    uint64_t pending = p->pending_signals & ~p->blocked_signals;
    if (!pending) return;
    
    // Find highest priority signal
    for (int sig = 1; sig < 64; sig++) {
        if (pending & (1ULL << sig)) {
            // Clear pending bit
            p->pending_signals &= ~(1ULL << sig);
            
            uint64_t handler = p->signal_handlers[sig];
            
            // SIG_IGN (1) - ignore signal
            if (handler == 1) {
                continue;
            }
            
            // SIG_DFL (0) - default action
            if (handler == 0) {
                switch (sig) {
                    case SIGKILL:
                        com_write_string(COM1_PORT, "[SIGNAL] SIGKILL - terminating\n");
                        do_exit(-(sig));
                        break;

                    case SIGTERM:
                        com_write_string(COM1_PORT, "[SIGNAL] SIGTERM - terminating\n");
                        do_exit(-(sig));
                        break;
                        
                    case SIGCHLD:
                        // Ignore by default
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
            } else {
                // User-defined handler - set up stack frame to call it
                // TODO: Implement user signal handler invocation
                // For now, just log
                com_write_string(COM1_PORT, "[SIGNAL] User handler at 0x");
                char buf[32];
                for (int i = 15; i >= 0; i--) {
                    uint8_t nibble = (handler >> (i * 4)) & 0xF;
                    buf[15 - i] = nibble < 10 ? '0' + nibble : 'A' + nibble - 10;
                }
                buf[16] = '\n';
                buf[17] = '\0';
                com_write_string(COM1_PORT, buf);
                
                // TODO: Invoke user handler by modifying saved context
                // For now, ignore
            }
            
            break;  // Only handle one signal at a time
        }
    }
}
