// fork_new.c - POSIX fork() implementation

#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/memory/kheap.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/COM/com.h"

#define KERNEL_STACK_SIZE 16384

// Memory management includes
#include "moduos/kernel/memory/fork_memory.h"

// Fork: create a copy of the current process
int do_fork(void) {
    process_t *parent = current;
    if (!parent) {
        com_write_string(COM1_PORT, "[FORK] No current process\n");
        return -1;
    }
    
    com_write_string(COM1_PORT, "[FORK] Forking PID ");
    char buf[16];
    itoa(parent->pid, buf, 10);
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, "\n");
    
    // Allocate new process structure
    process_t *child = process_alloc();
    if (!child) {
        com_write_string(COM1_PORT, "[FORK] Failed to allocate process\n");
        return -1;
    }
    
    // Copy most fields from parent
    child->ppid = parent->pid;
    child->pgid = parent->pgid;
    child->sid = parent->sid;
    strncpy(child->name, parent->name, PROCESS_NAME_MAX - 1);
    child->nice = parent->nice;
    child->weight = parent->weight;
    
    // Copy working directory and filesystem info
    strncpy(child->cwd, parent->cwd, PROCESS_MAX_PATH - 1);
    child->root_slot = parent->root_slot;
    
    // Allocate kernel stack for child
    child->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!child->kernel_stack) {
        com_write_string(COM1_PORT, "[FORK] Failed to allocate kernel stack\n");
        process_free(child);
        return -1;
    }
    memset(child->kernel_stack, 0, KERNEL_STACK_SIZE);
    
    // Create new page directory for child (copies kernel mappings)
    child->cr3 = paging_create_process_pml4();
    if (!child->cr3) {
        com_write_string(COM1_PORT, "[FORK] Failed to create page table\n");
        kfree(child->kernel_stack);
        process_free(child);
        return -1;
    }
    
    // Copy user memory contents
    copy_user_memory(parent->cr3, child->cr3);
    
    // Copy CPU context (child will return from fork with different value)
    memcpy(&child->context, &parent->context, sizeof(cpu_context_t));
    
    // Copy FPU state
    memcpy(child->fpu_state, parent->fpu_state, 512);
    
    // Child gets return value of 0, parent gets child PID
    // We'll set this in the context's rax register
    // (This is a simplified approach - real implementation would handle this in assembly)
    
    // Copy file descriptors (shallow copy for now)
    for (int i = 0; i < PROCESS_MAX_FDS; i++) {
        child->fd_table[i] = parent->fd_table[i];
    }
    
    // Copy argv
    child->argc = parent->argc;
    if (parent->argv) {
        child->argv = kmalloc(sizeof(char *) * (parent->argc + 1));
        for (int i = 0; i < parent->argc; i++) {
            if (parent->argv[i]) {
                size_t len = strlen(parent->argv[i]);
                child->argv[i] = kmalloc(len + 1);
                strncpy(child->argv[i], parent->argv[i], len);
                child->argv[i][len] = 0;
            }
        }
        child->argv[parent->argc] = NULL;
    }
    
    // Copy envp
    if (parent->envp) {
        int env_count = 0;
        while (parent->envp[env_count]) env_count++;
        
        child->envp = kmalloc(sizeof(char *) * (env_count + 1));
        for (int i = 0; i < env_count; i++) {
            size_t len = strlen(parent->envp[i]);
            child->envp[i] = kmalloc(len + 1);
            strncpy(child->envp[i], parent->envp[i], len);
            child->envp[i][len] = 0;
        }
        child->envp[env_count] = NULL;
    }
    
    // Set up parent-child relationship
    child->parent = parent;
    child->sibling_next = parent->children;
    if (parent->children) {
        parent->children->sibling_prev = child;
    }
    parent->children = child;
    
    // Initialize scheduler fields
    child->vruntime = 0;  // Will be set by scheduler_add
    child->exec_start = 0;
    
    // Add to scheduler run queue
    child->state = PROCESS_STATE_RUNNABLE;
    scheduler_add(child);
    
    com_write_string(COM1_PORT, "[FORK] Created child PID ");
    itoa(child->pid, buf, 10);
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, "\n");
    
    // Return child PID to parent
    return child->pid;
}
