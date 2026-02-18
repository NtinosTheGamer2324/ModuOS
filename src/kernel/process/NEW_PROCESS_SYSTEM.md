# New POSIX-Compliant Process Management System

## Overview
This is a complete rewrite of the ModuOS process management system, designed to be modular, clean, and POSIX-compliant.

## Architecture

### File Structure
```
src/kernel/process/
├── process_new.h           - Main header with data structures
├── process_table_new.c     - PID allocation and lookup
├── fork_new.c              - Process forking
├── exec_new.c              - Program execution
├── exit_new.c              - Process termination
├── wait_new.c              - Parent waits for child
├── scheduler_new.c         - CFS scheduler
├── context_switch_new.c    - CPU context switching
├── signals_new.c           - Signal handling
└── process_init_new.c      - Initialization
```

### Key Principles

1. **Separation of Concerns**
   - Each file handles ONE specific aspect
   - No monolithic files

2. **POSIX Compliance**
   - `fork()` creates child by copying parent
   - `exec()` replaces process image
   - `wait()` for parent-child synchronization
   - Proper signal handling

3. **Clean Data Structures**
   - `process_t` contains everything about a process
   - Separate CPU context from process metadata
   - Parent-child relationships via linked lists

4. **CFS Scheduler**
   - Fair scheduling based on vruntime
   - Nice value support (-20 to +19)
   - Preemption when time slice exhausted

## Process Lifecycle

```
EMBRYO → RUNNABLE → RUNNING → [SLEEPING] → ZOMBIE
          ↑            ↓
          └────────────┘
           (preempted)
```

### States
- **UNUSED**: Slot not allocated
- **EMBRYO**: Being created (fork/exec in progress)
- **RUNNABLE**: Ready to run (in scheduler queue)
- **RUNNING**: Currently executing
- **SLEEPING**: Waiting for event
- **ZOMBIE**: Terminated, waiting for parent to reap

## API Functions

### Process Creation
```c
int do_fork(void);                    // Fork current process
int do_exec(const char *path, 
            char **argv, 
            char **envp);             // Execute program
```

### Process Termination
```c
void do_exit(int status);             // Exit current process
int do_wait(int *status);             // Wait for any child
int do_waitpid(uint32_t pid, 
               int *status, 
               int options);          // Wait for specific child
```

### Scheduler
```c
void scheduler_init(void);            // Initialize scheduler
void scheduler_add(process_t *p);     // Add to run queue
void scheduler_remove(process_t *p);  // Remove from run queue
void schedule(void);                  // Pick next process
void scheduler_tick(void);            // Called from timer
```

### Sleep/Wakeup
```c
void sleep_on(void *channel);         // Sleep on event
void wakeup(void *channel);           // Wake sleeping processes
```

### Signals
```c
int send_signal(uint32_t pid, int sig); // Send signal
void check_signals(void);               // Handle pending signals
```

## Integration Steps

### 1. Update Timer Handler
In `timer.c`, call the new scheduler:
```c
void timer_handler(void) {
    scheduler_tick();  // New function
    schedule();        // New function
}
```

### 2. Update Syscall Handler
In `syscall.c`, wire up new syscalls:
```c
case SYS_FORK:   return do_fork();
case SYS_EXEC:   return do_exec(...);
case SYS_EXIT:   do_exit(arg1); break;
case SYS_WAIT:   return do_wait(...);
case SYS_KILL:   return send_signal(...);
```

### 3. Initialize at Boot
In `kernel.c`:
```c
process_management_init();           // Initialize subsystem
create_init_process("/sbin/init");   // Create init
schedule();                          // Start scheduling
```

### 4. Compilation
Add to Makefile:
```make
PROCESS_OBJS = \
    build/kernel/process/process_table_new.o \
    build/kernel/process/fork_new.o \
    build/kernel/process/exec_new.o \
    build/kernel/process/exit_new.o \
    build/kernel/process/wait_new.o \
    build/kernel/process/scheduler_new.o \
    build/kernel/process/context_switch_new.o \
    build/kernel/process/signals_new.o \
    build/kernel/process/process_init_new.o
```

## Missing Pieces (To Be Implemented)

### Memory Management Functions Needed
```c
uint64_t create_user_page_table(void);
uint64_t clone_page_table(uint64_t src);
void copy_user_memory(uint64_t src_cr3, uint64_t dst_cr3);
void map_user_page(uint64_t cr3, uint64_t vaddr, uint64_t paddr, int writable);
uint64_t alloc_physical_page(void);
```

### Assembly Functions Needed
```c
void context_switch_asm(cpu_context_t *old, cpu_context_t *new,
                        void *old_fpu, void *new_fpu);
```

This should be similar to existing `context_switch.asm` but use the new `cpu_context_t` structure.

## Benefits Over Old System

1. **Modularity**: Each file is < 300 lines, easy to understand
2. **POSIX Standard**: Familiar to Unix/Linux developers
3. **Testability**: Can test each component independently
4. **Maintainability**: Clear separation of concerns
5. **Correctness**: Proper parent-child relationships, no hacky workarounds

## Testing Plan

1. Test PID allocation/deallocation
2. Test scheduler adds/removes processes correctly
3. Test fork creates proper child
4. Test exec loads program correctly
5. Test wait/exit synchronization
6. Test signal delivery
7. Integration test: fork+exec+wait chain

## Migration Strategy

1. Keep old system intact
2. Build new system alongside (_new.c files)
3. Test new system thoroughly
4. Switch over in one commit
5. Remove old system

## Notes

- The assembly `context_switch.asm` needs minimal changes - just use `cpu_context_t` offsets
- File descriptor table is placeholder - expand when implementing file I/O
- Signal handling is basic - can be expanded with signal handlers, masks, etc.
- CFS uses simple linked list - can be optimized with red-black tree later
