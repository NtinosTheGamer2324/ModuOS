// process_new.h - Clean POSIX-compliant process management
// This will replace the old process.h once complete

#ifndef MODUOS_KERNEL_PROCESS_NEW_H
#define MODUOS_KERNEL_PROCESS_NEW_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// container_of — recover the enclosing struct from a member pointer.
// Used by scheduler.c to get process_t* from rbtree_node_t* without keeping
// a back-pointer inside the node struct.
// ---------------------------------------------------------------------------

#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - __builtin_offsetof(type, member)))
#endif

// ---------------------------------------------------------------------------
// Embedded scheduler node
//
// Lives inside process_t as the field 'sched_node'.  Embedding it here
// means scheduler.c never calls kzalloc/kfree in the scheduling hot path
// and there is no external sched_nodes[MAX_PROCESSES] index array that can
// go stale after a red-black tree rotation.
//
// Rules for users of this struct:
//   - Do NOT store a 'process_t *' back-pointer inside the node.
//     Use container_of(node_ptr, process_t, sched_node) instead.
//   - After removal from the tree, left/right/parent are NULLed by
//     rbtree_remove().  Do not read them outside the scheduler.
//   - Zero-init this struct (memset to 0) when a process_t is first
//     allocated so sched_node_in_tree() returns false before first enqueue.
// ---------------------------------------------------------------------------

typedef struct rbtree_node {
    struct rbtree_node *left;
    struct rbtree_node *right;
    struct rbtree_node *parent;
    bool                is_red;
    uint64_t            vruntime;
} rbtree_node_t;

// ---------------------------------------------------------------------------
// Process states (POSIX-style)
// ---------------------------------------------------------------------------

typedef enum {
    PROCESS_STATE_UNUSED = 0,
    PROCESS_STATE_EMBRYO,
    PROCESS_STATE_RUNNABLE,
    PROCESS_STATE_READY = PROCESS_STATE_RUNNABLE,
    PROCESS_STATE_RUNNING,
    PROCESS_STATE_SLEEPING,
    PROCESS_STATE_BLOCKED = PROCESS_STATE_SLEEPING,
    PROCESS_STATE_ZOMBIE,
    PROCESS_STATE_TERMINATED = PROCESS_STATE_ZOMBIE,
} process_state_t;

// ---------------------------------------------------------------------------
// CPU context (registers saved during context switch)
// ---------------------------------------------------------------------------

typedef struct {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t rip;
    uint64_t rsp;
    uint64_t rflags;
} cpu_context_t;

// ---------------------------------------------------------------------------
// Process descriptor
// ---------------------------------------------------------------------------

#define PROCESS_NAME_MAX 64
#define PROCESS_MAX_FDS  256
#define PROCESS_MAX_PATH 256

typedef struct process {
    // Process identification
    uint32_t pid;
    uint32_t ppid;
    uint32_t pgid;
    uint32_t sid;
    uint32_t refcount;
    char     name[PROCESS_NAME_MAX];

    // User/Group IDs
    uint32_t uid;
    uint32_t gid;

    // State
    process_state_t state;
    int             exit_code;

    // Scheduling (CFS)
    //
    // sched_node is the embedded red-black tree node.  It replaces the old
    // sched_next/sched_prev linked-list fields and the separately heap-
    // allocated rbtree_node_t that was stored in sched_nodes[MAX_PROCESSES].
    //
    // Invariant: after process_create_with_args() returns, sched_node is
    // either zeroed (not in tree) or fully linked (in tree).  It is never
    // in a partially-linked state outside of a spinlock-protected section
    // inside scheduler.c.
    rbtree_node_t sched_node;   // embedded — DO NOT access outside scheduler.c

    uint64_t vruntime;
    uint64_t exec_start;
    int      nice;
    int      priority;
    uint32_t weight;
    uint64_t total_time;
    volatile int need_resched;

    // CPU context
    cpu_context_t context;
    uint8_t fpu_state[512] __attribute__((aligned(16)));

    // Memory management
    uint64_t cr3;
    uint64_t page_table;
    void    *kernel_stack;
    uint64_t user_stack_top;
    uint64_t user_stack_low;
    uint64_t user_stack_limit;

    // User mode entry
    uint64_t entry_point;
    uint64_t user_sp;
    uint64_t user_rip;
    uint64_t user_rsp;
    void    *user_stack;

    // Memory regions
    uint64_t user_image_base;
    uint64_t user_image_end;
    uint64_t user_heap_base;
    uint64_t user_heap_end;
    uint64_t user_heap_limit;
    uint64_t user_mmap_base;
    uint64_t user_mmap_end;
    uint64_t user_mmap_limit;

    // Flags
    int is_user;

    // Arguments
    int    argc;
    char **argv;
    char **envp;
    int    envc;

    // Filesystem
    char cwd[PROCESS_MAX_PATH];
    int  root_slot;
    int  current_slot;

    // File descriptors
    void *fd_table[PROCESS_MAX_FDS];

    // Parent-child relationships
    struct process *parent;
    uint32_t        parent_pid;
    struct process *children;
    struct process *sibling_next;
    struct process *sibling_prev;

    // Wait / sleep
    void    *wait_channel;
    uint64_t sleep_ticks;

    // Signals
    uint64_t pending_signals;
    uint64_t blocked_signals;

    // Controlling terminal
    int controlling_tty;

    // Effective/saved IDs
    uint32_t euid;
    uint32_t egid;
    uint32_t suid;
    uint32_t sgid;

    // Per-signal disposition (0=SIG_DFL, 1=SIG_IGN, else=user handler VA)
    uint64_t signal_handlers[64];

} process_t;

// ---------------------------------------------------------------------------
// Global process table
// ---------------------------------------------------------------------------

#define MAX_PROCESSES 256
extern process_t *process_table[MAX_PROCESSES];
extern uint32_t   next_pid;

// ---------------------------------------------------------------------------
// Process table management (process_table.c)
// ---------------------------------------------------------------------------

void       process_table_init(void);
process_t *process_alloc(void);
void       process_free(process_t *p);
process_t *process_find(uint32_t pid);

// ---------------------------------------------------------------------------
// Fork and exec
// ---------------------------------------------------------------------------

int do_fork(void);
int do_exec(const char *path, char **argv, char **envp);

// ---------------------------------------------------------------------------
// Exit and wait
// ---------------------------------------------------------------------------

typedef int pid_t;

void do_exit(int status);
int  do_wait(int *status);
int  do_waitpid(int32_t pid, int *status, int options);

// ---------------------------------------------------------------------------
// Scheduler (scheduler.c)
// ---------------------------------------------------------------------------

void     scheduler_init(void);
void     scheduler_add(process_t *p);
void     scheduler_remove(process_t *p);
void     schedule(void);
void     scheduler_tick(void);
int      should_reschedule(void);

// ---------------------------------------------------------------------------
// Context switching
// ---------------------------------------------------------------------------

void switch_to(process_t *prev, process_t *next);

// ---------------------------------------------------------------------------
// Current process
// ---------------------------------------------------------------------------

extern volatile process_t *current;

static inline process_t *get_current(void) {
    return (process_t *)current;
}

uint64_t process_get_current_cr3(void);

static inline process_t *process_get_current(void) {
    return (process_t *)current;
}

// ---------------------------------------------------------------------------
// Sleep / wakeup / signals
// ---------------------------------------------------------------------------

void sleep_on(void *channel);
void wakeup(void *channel);
int  send_signal(uint32_t pid, int sig);

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

void process_exit(int status);

// ---------------------------------------------------------------------------
// Process table management (process_table_compat.c)
// ---------------------------------------------------------------------------

uint32_t process_alloc_pid(void);
int      process_register(process_t *proc);
int      process_unregister(uint32_t pid);

// ---------------------------------------------------------------------------
// Process lifecycle (process.c)
// ---------------------------------------------------------------------------

void       process_destroy(process_t *proc);
void       process_free_user_memory(process_t *p);
process_t *process_create(const char *name, void (*entry_point)(void), int priority);
process_t *process_create_with_args(const char *name, void (*entry_point)(void),
                                    int priority, int argc, char **argv);
void       process_set_build_pml4(uint64_t *pml4_virt, uint64_t pml4_phys);
uint64_t  *process_get_build_pml4(void);

// ---------------------------------------------------------------------------
// FD injection
// ---------------------------------------------------------------------------

int   process_inject_fd(uint32_t pid, int fd, void *fd_obj);
void *process_get_fd(uint32_t pid, int fd);
int   process_close_fd(uint32_t pid, int fd);

// ---------------------------------------------------------------------------
// Signals / init
// ---------------------------------------------------------------------------

void check_signals(void);
void process_management_init(void);
void create_init_process(const char *path);
void process_wake(uint32_t pid);
void set_curproc(process_t *p);

#endif // MODUOS_KERNEL_PROCESS_NEW_H