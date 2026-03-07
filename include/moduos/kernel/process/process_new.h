// process_new.h - Clean POSIX-compliant process management
// This will replace the old process.h once complete

#ifndef MODUOS_KERNEL_PROCESS_NEW_H
#define MODUOS_KERNEL_PROCESS_NEW_H

#include <stdint.h>
#include <stddef.h>

// Process states (POSIX-style)
typedef enum {
    PROCESS_STATE_UNUSED = 0,    // Slot not allocated
    PROCESS_STATE_EMBRYO,         // Being created
    PROCESS_STATE_RUNNABLE,       // Ready to run
    PROCESS_STATE_READY = PROCESS_STATE_RUNNABLE,  // Alias
    PROCESS_STATE_RUNNING,        // Currently executing
    PROCESS_STATE_SLEEPING,       // Waiting for event
    PROCESS_STATE_BLOCKED = PROCESS_STATE_SLEEPING,  // Alias
    PROCESS_STATE_ZOMBIE,         // Terminated, waiting for parent
    PROCESS_STATE_TERMINATED = PROCESS_STATE_ZOMBIE,  // Alias for compatibility
} process_state_t;

// CPU context (registers saved during context switch)
typedef struct {
    // Callee-saved registers (preserved across function calls)
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbx;
    uint64_t rbp;

    // Program counter and stack pointer
    uint64_t rip;
    uint64_t rsp;

    // Flags register
    uint64_t rflags;
} cpu_context_t;

// Process descriptor (task_struct equivalent)
#define PROCESS_NAME_MAX 64
#define PROCESS_MAX_FDS  256
#define PROCESS_MAX_PATH 256

typedef struct process {
    // Process identification
    uint32_t pid;                 // Process ID
    uint32_t ppid;                // Parent process ID (compatibility - use parent pointer)
    uint32_t pgid;                // Process group ID
    uint32_t sid;                 // Session ID
    uint32_t refcount;            // Reference count
    char name[PROCESS_NAME_MAX];  // Process name

    // User/Group IDs (POSIX) — real IDs only here to preserve struct layout
    uint32_t uid;                 // Real user ID
    uint32_t gid;                 // Real group ID

    // State
    process_state_t state;
    int exit_code;                // Exit status (for zombies)

    // Scheduling (CFS)
    uint64_t vruntime;            // Virtual runtime (nanoseconds)
    uint64_t exec_start;          // Time slice start
    int nice;                     // Nice value (-20 to +19)
    int priority;                 // Alias for nice (compatibility)
    uint32_t weight;              // Scheduling weight
    uint64_t total_time;          // Total CPU time used
    volatile int need_resched;    // Set by IRQ when preemption needed
    struct process *sched_next;   // Red-black tree links (simplified as linked list for now)
    struct process *sched_prev;

    // CPU context
    cpu_context_t context;        // Saved registers
    void *fpu_state;              // FPU/SSE state (512 bytes)

    // Memory management
    uint64_t cr3;                 // Page table base (per-process address space)
    uint64_t page_table;          // Alias for cr3 (compatibility)
    void *kernel_stack;           // Kernel stack pointer
    uint64_t user_stack_top;      // User stack top
    uint64_t user_stack_low;      // User stack bottom
    uint64_t user_stack_limit;    // User stack limit (lowest valid address)

    // User mode entry point
    uint64_t entry_point;         // User RIP
    uint64_t user_sp;             // User RSP
    uint64_t user_rip;            // Alias for entry_point (compatibility)
    uint64_t user_rsp;            // Alias for user_sp (compatibility)
    void *user_stack;             // Alias for user stack (compatibility)

    // Memory regions
    uint64_t user_image_base;     // Program image start
    uint64_t user_image_end;      // Program image end
    uint64_t user_heap_base;      // Heap start
    uint64_t user_heap_end;       // Heap end
    uint64_t user_heap_limit;     // Heap limit
    uint64_t user_mmap_base;      // Mmap region start
    uint64_t user_mmap_end;       // Mmap region end
    uint64_t user_mmap_limit;     // Mmap region limit

    // Process flags
    int is_user;                  // 1 if user mode process, 0 if kernel

    // Arguments
    int argc;
    char **argv;
    char **envp;
    int envc;                     // Environment variable count

    // Working directory and filesystem
    char cwd[PROCESS_MAX_PATH];
    int root_slot;                // Root filesystem slot
    int current_slot;             // Current working directory slot

    // File descriptors
    void *fd_table[PROCESS_MAX_FDS];

    // Parent-child relationships
    struct process *parent;
    uint32_t parent_pid;          // Compatibility - parent PID
    struct process *children;     // Linked list of children
    struct process *sibling_next; // Next sibling
    struct process *sibling_prev; // Previous sibling

    // Wait queue (for sleep/wakeup)
    void *wait_channel;           // What we're sleeping on
    uint64_t sleep_ticks;         // Tick count for process_sleep

    // Signals
    uint64_t pending_signals;
    uint64_t blocked_signals;
    
    // POSIX Controlling Terminal
    int controlling_tty;          // FD of controlling terminal (-1 = none)

    // Effective/saved IDs — placed after the assembly-ABI-critical fields
    // (cr3, context, fpu_state) so their offsets are not disturbed.
    uint32_t euid;                // Effective user ID
    uint32_t egid;                // Effective group ID
    uint32_t suid;                // Saved set-user-ID
    uint32_t sgid;                // Saved set-group-ID

    // Per-signal disposition table (sigaction equivalent)
    // 0 = SIG_DFL, 1 = SIG_IGN, else = user handler VA
    uint64_t signal_handlers[64];

} process_t;

// Global process table
#define MAX_PROCESSES 256
extern process_t *process_table[MAX_PROCESSES];
extern uint32_t next_pid;

// Process table management (process_table.c)
void process_table_init(void);
process_t *process_alloc(void);
void process_free(process_t *p);
process_t *process_find(uint32_t pid);

// Fork and exec (fork.c, exec.c)
int do_fork(void);  // Returns child PID
int do_exec(const char *path, char **argv, char **envp);

// Exit and wait (exit.c, wait.c)
void do_exit(int status);
int do_wait(int *status);  // Returns pid_t
int do_waitpid(int32_t pid, int *status, int options);

// POSIX type
typedef int pid_t;

// Scheduler (scheduler.c)
void scheduler_init(void);
void scheduler_add(process_t *p);
void scheduler_remove(process_t *p);
void schedule(void);
void scheduler_tick(void);
int should_reschedule(void);

// Context switching (context_switch.c)
void switch_to(process_t *prev, process_t *next);

// Current process
extern volatile process_t *current;
static inline process_t *get_current(void) {
    return (process_t *)current;
}

uint64_t process_get_current_cr3(void);

static inline process_t *process_get_current(void) {
    return (process_t *)current;
}

// Forward declarations
void sleep_on(void *channel);
void wakeup(void *channel);
int send_signal(uint32_t pid, int sig);

// process_get_by_pid — implemented in process_table_compat.c (rwlock-protected).
process_t *process_get_by_pid(uint32_t pid);

static inline void process_sleep(uint64_t ms) {
    if (current) {
        current->sleep_ticks = ms;
        sleep_on((void *)(uintptr_t)current->pid);
    }
}

static inline void process_yield(void) {
    schedule();
}

static inline void process_kill(uint32_t pid) {
    send_signal(pid, 9);  // SIGKILL
}

// process_exit is in process_exit_stub.c (needs to be callable from assembly)
void process_exit(int status);

/* Process table management - implemented in process_table_compat.c */
uint32_t process_alloc_pid(void);
int process_register(process_t *proc);
int process_unregister(uint32_t pid);

/* Process lifecycle - implemented in process.c */
void process_destroy(process_t *proc);
void process_free_user_memory(process_t *p);
process_t *process_create(const char *name, void (*entry_point)(void), int priority);
process_t *process_create_with_args(const char *name, void (*entry_point)(void), int priority, int argc, char **argv);
void process_set_build_pml4(uint64_t *pml4_virt, uint64_t pml4_phys);
uint64_t *process_get_build_pml4(void);

/* FD injection - allows a TTY manager or supervisor to inject file descriptors
 * into another process before it starts executing. Typical use: inject FD 0
 * (stdin), FD 1 (stdout), FD 2 (stderr) backed by a TTY or pipe object.
 *
 * fd_obj  - opaque pointer to the backing object (tty_t *, pipe_t *, etc.)
 * Returns 0 on success, -1 on error (pid not found, fd out of range, slot occupied).
 */
int process_inject_fd(uint32_t pid, int fd, void *fd_obj);

/* Retrieve an injected/open FD from a process. Returns NULL if not found. */
void *process_get_fd(uint32_t pid, int fd);

/* Close (clear) an FD slot in a process. Returns 0 on success. */
int process_close_fd(uint32_t pid, int fd);

// Signals (signals.c)
void check_signals(void);

// Initialization (process_init_new.c)
void process_management_init(void);
void create_init_process(const char *path);

#endif // MODUOS_KERNEL_PROCESS_NEW_H

