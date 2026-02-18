# New Process System - Integration Checklist

## ✅ COMPLETED

### Core Files Created (15 files)
- [x] `process_new.h` - Main header with all data structures
- [x] `process_table_new.c` - PID allocation and management
- [x] `scheduler_new.c` - CFS scheduler implementation
- [x] `context_switch_new.c` - High-level context switch wrapper
- [x] `context_switch_new.asm` - Low-level assembly context switch
- [x] `fork_new.c` - POSIX fork() implementation
- [x] `exec_new.c` - POSIX exec() implementation
- [x] `exit_new.c` - Process termination
- [x] `wait_new.c` - Parent-child synchronization
- [x] `signals_new.c` - Signal handling framework
- [x] `process_init_new.c` - Initialization code
- [x] `fork_memory.c` - Memory copying for fork()
- [x] `fork_memory.h` - Memory function declarations
- [x] `NEW_PROCESS_SYSTEM.md` - Architecture documentation
- [x] `INTEGRATION_CHECKLIST.md` - This file

### Memory Functions
- [x] `copy_user_memory()` implemented in `fork_memory.c`
- [x] Uses existing `paging_create_process_pml4()`
- [x] No missing memory functions!

## 🔄 TODO: Integration Steps

### Step 1: Add Files to Makefile

Add these to your kernel Makefile:

```makefile
# New process system
KERNEL_OBJS += build/kernel/process/process_table_new.o
KERNEL_OBJS += build/kernel/process/scheduler_new.o
KERNEL_OBJS += build/kernel/process/context_switch_new.o
KERNEL_OBJS += build/kernel/process/fork_new.o
KERNEL_OBJS += build/kernel/process/exec_new.o
KERNEL_OBJS += build/kernel/process/exit_new.o
KERNEL_OBJS += build/kernel/process/wait_new.o
KERNEL_OBJS += build/kernel/process/signals_new.o
KERNEL_OBJS += build/kernel/process/process_init_new.o

# Memory support for fork
KERNEL_OBJS += build/kernel/memory/fork_memory.o

# Assembly context switch
ASM_OBJS += build/arch/AMD64/syscall/context_switch_new.o
```

### Step 2: Update Syscall Handler

In `src/kernel/syscall/syscall.c`, add new syscalls:

```c
#include "moduos/kernel/process/process_new.h"

// In syscall_handler():
case SYS_FORK:
    return do_fork();
    
case SYS_EXEC:
    return do_exec((const char *)arg1, (char **)arg2, (char **)arg3);
    
case SYS_WAIT:
    return do_wait((int *)arg1);
    
case SYS_EXIT:
    do_exit((int)arg1);
    return 0;  // Never reached
    
case SYS_KILL:
    return do_kill((pid_t)arg1, (int)arg2);
```

### Step 3: Update Syscall Numbers

In `include/moduos/kernel/syscall/syscall_numbers.h`:

```c
#define SYS_FORK   2
#define SYS_EXEC   11
#define SYS_WAIT   61
#define SYS_EXIT   60
#define SYS_KILL   62
```

### Step 4: Replace Timer IRQ Handler

In `src/arch/AMD64/interrupts/timer.c`:

```c
#include "moduos/kernel/process/process_new.h"

void timer_handler(interrupt_frame_t *frame) {
    (void)frame;
    
    // Acknowledge PIT
    // ... existing code ...
    
    // Call new scheduler tick
    scheduler_tick();
    
    // Trigger reschedule if needed
    if (should_reschedule()) {
        schedule();
    }
}
```

### Step 5: Initialize in Kernel Main

In `src/kernel/kernel.c` or `src/kernel/mdinit.c`:

```c
#include "moduos/kernel/process/process_new.h"

void kernel_main(void) {
    // ... existing initialization ...
    
    // Initialize new process system
    process_management_init();
    
    // Create init process (PID 1)
    create_init_process("/ModuOS/System64/automan.sqr");
    
    // Start scheduling
    com_write_string(COM1_PORT, "[KERNEL] Starting scheduler...\n");
    schedule();  // Will switch to init process
    
    // Idle loop (only PID 0 reaches here)
    for (;;) {
        __asm__ volatile("hlt");
    }
}
```

### Step 6: Testing Plan

1. **Test Process Table**
   - Boot and verify PID 0 and PID 1 are created
   - Check `/proc` or debug output

2. **Test Scheduler**
   - Create multiple processes
   - Verify they get CPU time
   - Check CFS vruntime values

3. **Test Fork**
   - Implement test program that calls fork()
   - Verify parent and child both run
   - Check memory is properly copied

4. **Test Exec**
   - Call exec() after fork()
   - Verify new program loads and runs

5. **Test Wait**
   - Parent waits for child
   - Verify zombie cleanup

6. **Test Signals**
   - Send SIGTERM to process
   - Verify signal handler runs

## 📝 Migration from Old System

Once new system is working:

1. Rename old files:
   - `process.c` → `process_old.c.bak`
   - `scheduler.c` → `scheduler_old.c.bak`
   - etc.

2. Rename new files:
   - `process_new.h` → `process.h`
   - `*_new.c` → `*.c`
   - etc.

3. Remove old files from Makefile

4. Update all includes throughout codebase

## 🐛 Common Issues

1. **"Undefined reference" errors**
   - Make sure all `.o` files are in Makefile
   - Check function names match between .c and .h

2. **Triple fault on boot**
   - Check stack alignment in context_switch_new.asm
   - Verify CR3 values are physical addresses

3. **Processes don't run**
   - Add debug output in schedule()
   - Check scheduler_enabled flag
   - Verify timer IRQ is calling scheduler_tick()

4. **Fork fails**
   - Check memory allocation
   - Verify copy_user_memory() works
   - Test with simple programs first

## 📚 References

- POSIX Process Creation: https://pubs.opengroup.org/onlinepubs/9699919799/
- Linux Process Model: https://www.kernel.org/doc/html/latest/
- xv6 Source Code: https://github.com/mit-pdos/xv6-public

## ✅ Success Criteria

- [ ] System boots without errors
- [ ] PID 0 (idle) and PID 1 (init) created
- [ ] Scheduler switches between processes
- [ ] fork() creates working child process
- [ ] exec() loads and runs programs
- [ ] wait() properly handles zombies
- [ ] Processes can be killed with signals
- [ ] Timer continues running during process switches
