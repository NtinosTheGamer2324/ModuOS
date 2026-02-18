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
#define PROCESS_MAX_FDS 256
#define PROCESS_MAX_PATH 256

typedef struct process {
    // Process identification
    uint32_t pid;                 // Process ID
    uint32_t ppid;                // Parent process ID (compatibility - use parent pointer)
    uint32_t pgid;                // Process group ID
    uint32_t sid;                 // Session ID
    char name[PROCESS_NAME_MAX];  // Process name
    
    // User/Group IDs (POSIX)
    uint32_t uid;                 // User ID
    uint32_t gid;                 // Group ID
    
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
    struct process *sched_next;   // Red-black tree links (simplified as linked list for now)
    struct process *sched_prev;
    
    // CPU context
    cpu_context_t context;        // Saved registers
    cpu_context_t cpu_state;      // Alias for context (compatibility)
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
    
    // File descriptors (will be expanded later)
    void *fd_table[PROCESS_MAX_FDS];
    
    // Parent-child relationships
    struct process *parent;
    uint32_t parent_pid;          // Compatibility - parent PID
    struct process *children;     // Linked list of children
    struct process *sibling_next; // Next sibling
    struct process *sibling_prev; // Previous sibling
    
    // Wait queue (for sleep/wakeup)
    void *wait_channel;           // What we're sleeping on
    
    // Signals (basic support)
    uint64_t pending_signals;
    uint64_t blocked_signals;
    
    // Xenith26 shared memory maps (compatibility)
    struct {
        int used;
        uint32_t buf_id;
        uint64_t vaddr;
        uint64_t size_bytes;
    } x26_maps[16];
    
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
int do_waitpid(uint32_t pid, int *status, int options);

// POSIX type
typedef int pid_t;

// Scheduler (scheduler.c)
void scheduler_init(void);
void scheduler_add(process_t *p);
void scheduler_remove(process_t *p);
void schedule(void);  // Pick next process to run
void scheduler_tick(void);  // Called from timer IRQ
int should_reschedule(void);  // Check if we need to switch

// Context switching (context_switch.c)
void switch_to(process_t *prev, process_t *next);

// Current process
extern process_t *current;
static inline process_t *get_current(void) {
    return current;
}

// Compatibility with old code
static inline process_t *process_get_current(void) {
    return current;
}

// Forward declarations
void sleep_on(void *channel);
int send_signal(uint32_t pid, int sig);

// Compatibility aliases
static inline process_t *process_get_by_pid(uint32_t pid) {
    return process_find(pid);
}

static inline void process_sleep(uint64_t ms) {
    // Sleep by setting vruntime (simplified)
    if (current) {
        current->vruntime = ms;  // Reuse vruntime for sleep timeout
        sleep_on((void*)(uintptr_t)ms);
    }
}

static inline void process_yield(void) {
    schedule();
}

static inline void process_kill(uint32_t pid) {
    // Send termination signal
    send_signal(pid, 9);  // SIGKILL
}

// More compatibility stubs for old code
// process_exit is in process_exit_stub.c (needs to be callable from assembly)
void process_exit(int status);  // Declaration only

static inline process_t *process_create(const char *name, void (*entry)(void), int priority) {
    // Old-style process creation - not supported in new system
    // Return NULL to indicate failure
    (void)name; (void)entry; (void)priority;
    return NULL;
}

static inline process_t *process_create_with_args(const char *name, void (*entry)(void), int argc, char **argv, int priority) {
    // Old-style process creation - not supported
    (void)name; (void)entry; (void)argc; (void)argv; (void)priority;
    return NULL;
}

static inline void process_destroy(process_t *p) {
    // In new system, use do_exit() from within the process
    (void)p;
}

static inline uint32_t process_alloc_pid(void) {
    process_t *p = process_alloc();
    if (!p) return 0;
    uint32_t pid = p->pid;
    process_free(p);
    return pid;
}

static inline int process_register(process_t *p) {
    // In new system, processes are registered when allocated
    (void)p;
    return 0;  // Success
}

static inline void process_free_user_memory(process_t *p) {
    // Free user memory (simplified - would need proper implementation)
    (void)p;
}

static inline void process_set_build_pml4(uint64_t pml4) {
    // Old build system compatibility - ignore
    (void)pml4;
}

// Sleep/wakeup (scheduler.c) - already declared above
void wakeup(void *channel);

// Signals (signals.c) - already declared above
void check_signals(void);

// Initialization (process_init_new.c)
void process_management_init(void);
void create_init_process(const char *path);

#endif // MODUOS_KERNEL_PROCESS_NEW_H
