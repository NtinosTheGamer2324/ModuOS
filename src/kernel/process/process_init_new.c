// process_init_new.c - Initialize the new process management system

#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/memory/kheap.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/COM/com.h"

#define KERNEL_STACK_SIZE 16384

// External functions
extern uint64_t read_cr3(void);
extern void amd64_syscall_set_kernel_stack(uint64_t stack_top);

// Idle process entry point
static void idle_process(void) {
    com_write_string(COM1_PORT, "[IDLE] Idle process running\n");
    for (;;) {
        __asm__ volatile("hlt");
    }
}

// Initialize process management subsystem
void process_management_init(void) {
    com_write_string(COM1_PORT, "[PROCESS] Initializing new process management system\n");
    
    // Initialize process table
    process_table_init();
    
    // Initialize scheduler
    scheduler_init();
    
    // Create idle process (PID 0)
    process_t *idle = process_alloc();
    if (!idle) {
        com_write_string(COM1_PORT, "[PROCESS] Failed to create idle process\n");
        return;
    }
    
    // Override PID to be 0
    idle->pid = 0;
    idle->ppid = 0;
    idle->pgid = 0;
    idle->sid = 0;
    strncpy(idle->name, "idle", PROCESS_NAME_MAX - 1);
    
    // Allocate kernel stack
    idle->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!idle->kernel_stack) {
        com_write_string(COM1_PORT, "[PROCESS] Failed to allocate idle stack\n");
        process_free(idle);
        return;
    }
    memset(idle->kernel_stack, 0, KERNEL_STACK_SIZE);
    
    // Set up context
    memset(&idle->context, 0, sizeof(cpu_context_t));
    idle->context.rip = (uint64_t)idle_process;
    idle->context.rsp = (uint64_t)idle->kernel_stack + KERNEL_STACK_SIZE - 16;
    idle->context.rflags = 0x202;  // IF=1
    
    // Use current CR3 (kernel page table)
    idle->cr3 = read_cr3();
    
    // Scheduler parameters
    idle->nice = 0;
    idle->weight = 1024;
    idle->vruntime = 0;
    idle->state = PROCESS_STATE_RUNNING;
    
    // Set as current process
    current = idle;
    
    // Set up kernel stack for interrupts/syscalls
    uint64_t kstack_top = (uint64_t)idle->kernel_stack + KERNEL_STACK_SIZE - 16;
    amd64_syscall_set_kernel_stack(kstack_top);
    
    com_write_string(COM1_PORT, "[PROCESS] Idle process created (PID 0)\n");
    com_write_string(COM1_PORT, "[PROCESS] Process management initialized\n");
}

// Create init process (PID 1) - first user process
void create_init_process(const char *path) {
    com_write_string(COM1_PORT, "[PROCESS] Creating init process\n");
    
    process_t *init = process_alloc();
    if (!init) {
        com_write_string(COM1_PORT, "[PROCESS] Failed to allocate init process\n");
        return;
    }
    
    // Set up init
    init->ppid = 0;
    init->pgid = init->pid;
    init->sid = init->pid;
    strncpy(init->name, "init", PROCESS_NAME_MAX - 1);
    
    // Allocate kernel stack
    init->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!init->kernel_stack) {
        com_write_string(COM1_PORT, "[PROCESS] Failed to allocate init stack\n");
        process_free(init);
        return;
    }
    memset(init->kernel_stack, 0, KERNEL_STACK_SIZE);
    
    // Default parameters
    init->nice = 0;
    init->weight = 1024;
    init->vruntime = 0;
    strncpy(init->cwd, "/", PROCESS_MAX_PATH - 1);
    
    // Set as child of idle
    init->parent = current;
    init->sibling_next = current->children;
    if (current->children) {
        current->children->sibling_prev = init;
    }
    current->children = init;
    
    // Make init the current process temporarily to load the program
    process_t *old_current = current;
    current = init;
    
    // Load program
    char *argv[] = { (char *)path, NULL };
    char *envp[] = { NULL };
    
    if (do_exec(path, argv, envp) != 0) {
        com_write_string(COM1_PORT, "[PROCESS] Failed to exec init\n");
        current = old_current;
        process_free(init);
        return;
    }
    
    // Add to scheduler
    init->state = PROCESS_STATE_RUNNABLE;
    scheduler_add(init);
    
    // Restore current
    current = old_current;
    
    com_write_string(COM1_PORT, "[PROCESS] Init process created (PID ");
    char buf[16];
    itoa(init->pid, buf, 10);
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, ")\n");
}
