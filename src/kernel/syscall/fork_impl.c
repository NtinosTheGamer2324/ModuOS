// SPDX-License-Identifier: GPL-2.0-only
//
// ModuOS Kernel (GPLv2)
// fork_impl.c - Linux-like fork() for ModuOS (full address-space copy)
// Included by syscall.c

#include "moduos/kernel/errno.h"
// #include "moduos/kernel/process/process.h"  // OLD
#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/spinlock.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/COM/com.h"

extern volatile uint64_t g_syscall_entry_rbp;
extern void syscall_entry_return(void);

// From process.c (private); replicate constants
#ifndef KERNEL_STACK_SIZE
#define KERNEL_STACK_SIZE 16384
#endif

static char **dup_strv(char **src, int n) {
    if (n <= 0 || !src) return NULL;
    char **dst = (char**)kmalloc((n + 1) * sizeof(char*));
    if (!dst) return NULL;
    memset(dst, 0, (n + 1) * sizeof(char*));
    for (int i = 0; i < n; i++) {
        const char *s = src[i] ? src[i] : "";
        size_t len = strlen(s) + 1;
        dst[i] = (char*)kmalloc(len);
        if (!dst[i]) {
            for (int j = 0; j < i; j++) if (dst[j]) kfree(dst[j]);
            kfree(dst);
            return NULL;
        }
        memcpy(dst[i], s, len);
    }
    dst[n] = NULL;
    return dst;
}

static void fork_free_strv(char **v, int n) {
    if (!v) return;
    for (int i = 0; i < n; i++) if (v[i]) kfree(v[i]);
    kfree(v);
}

extern uint64_t phys_alloc_frame(void);
extern void     phys_free_frame(uint64_t phys);

// Clone the parent's user address space into a fresh PML4 by physically
// copying each mapped user page.  This allocates fresh physical frames for each
// user page, ensuring the child has an independent copy that won't corrupt the
// parent when process_free_user_memory() is called.
static int clone_user_address_space(process_t *parent, process_t *child) {
    if (!parent || !child) return -EINVAL;

    uint64_t child_cr3 = paging_create_process_pml4();
    if (!child_cr3) return -ENOMEM;
    
    com_write_string(COM1_PORT, "[FORK] Created new PML4 for child: CR3=0x");
    com_write_hex64(COM1_PORT, child_cr3);
    com_write_string(COM1_PORT, "\n");

    uint64_t *child_pml4 = (uint64_t *)phys_to_virt_kernel(child_cr3 & ~0xFFFULL);
    if (!child_pml4) return -ENOMEM;

    // Scratch VA used to temporarily map the new child physical page so we
    // can copy into it while still running on the parent's CR3.
    uint64_t scratch = paging_get_scratch_base();

    /* Resolve the current process PML4 and the kernel PML4 once for the loop. */
    uint64_t *kernel_pml4 = paging_get_pml4();
    uint64_t *cur_pml4 = NULL;
    unsigned  scratch_i4 = scratch ? ((scratch >> 39) & 0x1FF) : 0;
    if (scratch && kernel_pml4) {
        uint64_t cr3_phys;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_phys));
        cr3_phys &= 0xFFFFFFFFFFFFF000ULL;
        uint64_t *p = (uint64_t *)phys_to_virt_kernel(cr3_phys);
        if (p != kernel_pml4) cur_pml4 = p;
    }

    struct range { uint64_t a, b; } ranges[4];
    int rn = 0;

    if (parent->user_image_base && parent->user_image_end > parent->user_image_base)
        ranges[rn++] = (struct range){ parent->user_image_base, parent->user_image_end };
    if (parent->user_heap_base && parent->user_heap_end > parent->user_heap_base)
        ranges[rn++] = (struct range){ parent->user_heap_base, parent->user_heap_end };
    if (parent->user_mmap_base && parent->user_mmap_end > parent->user_mmap_base)
        ranges[rn++] = (struct range){ parent->user_mmap_base, parent->user_mmap_end };
    if (parent->user_stack_low && parent->user_stack_top > parent->user_stack_low) {
        ranges[rn++] = (struct range){ parent->user_stack_low, parent->user_stack_top };
        com_write_string(COM1_PORT, "[FORK] Copying stack range 0x");
        com_write_hex64(COM1_PORT, parent->user_stack_low);
        com_write_string(COM1_PORT, " - 0x");
        com_write_hex64(COM1_PORT, parent->user_stack_top);
        com_write_string(COM1_PORT, "\n");
    }

    for (int i = 0; i < rn; i++) {
        uint64_t start = ranges[i].a & ~0xFFFULL;
        uint64_t end   = (ranges[i].b + 0xFFFULL) & ~0xFFFULL;
        for (uint64_t v = start; v < end; v += 0x1000ULL) {
            uint64_t parent_phys = paging_virt_to_phys(v);
            if (!parent_phys) {
                com_write_string(COM1_PORT, "[FORK] Skipping unmapped page at 0x");
                com_write_hex64(COM1_PORT, v);
                com_write_string(COM1_PORT, "\n");
                continue;
            }
            uint64_t pte = paging_get_pte(v);
            if (!(pte & PFLAG_USER)) continue;

            // Allocate a fresh physical page for the child.
            uint64_t child_phys = phys_alloc_frame();
            if (!child_phys) return -ENOMEM;

            // Map scratch VA → child_phys in the kernel PML4, then propagate the
            // PML4 slot into the current process CR3 before dereferencing scratch.
            paging_map_page(scratch, child_phys, PFLAG_PRESENT | PFLAG_WRITABLE);
            if (cur_pml4 && kernel_pml4)
                cur_pml4[scratch_i4] = kernel_pml4[scratch_i4];

            // Copy parent page content (virtual address v is readable from here).
            memcpy((void *)(uintptr_t)scratch, (void *)(uintptr_t)v, 4096);

            // Unmap scratch to avoid stale TLB entries.
            paging_unmap_page(scratch);

            // Map child_phys into child's address space with the same flags.
            uint64_t flags = (pte & 0xFFFULL) | PFLAG_PRESENT | PFLAG_USER | PFLAG_WRITABLE;
            if (paging_map_range_to_pml4(child_pml4, v, child_phys, 4096, flags) != 0) {
                phys_free_frame(child_phys);
                return -ENOMEM;
            }
        }
    }

    com_write_string(COM1_PORT, "[FORK] Setting child CR3=0x");
    com_write_hex64(COM1_PORT, child_cr3);
    com_write_string(COM1_PORT, "\n");
    
    child->cr3 = child_cr3;
    child->page_table = child_cr3;  // CRITICAL: Initialize page_table too!

    child->user_image_base  = parent->user_image_base;
    child->user_image_end   = parent->user_image_end;
    child->user_heap_base   = parent->user_heap_base;
    child->user_heap_end    = parent->user_heap_end;
    child->user_heap_limit  = parent->user_heap_limit;
    child->user_mmap_base   = parent->user_mmap_base;
    child->user_mmap_end    = parent->user_mmap_end;
    child->user_mmap_limit  = parent->user_mmap_limit;
    child->user_stack       = parent->user_stack;
    child->user_stack_top   = parent->user_stack_top;
    child->user_stack_low   = parent->user_stack_low;
    child->user_stack_limit = parent->user_stack_limit;
    child->user_rip         = parent->user_rip;
    child->user_rsp         = parent->user_rsp;

    return 0;
}

extern void scheduler_add_process(process_t *p);

int sys_fork_impl(void) {
    process_t *parent = process_get_current();
    if (!parent) return -ESRCH;
    if (!parent->is_user) return -EACCES;

    // process_alloc() allocates the struct, assigns a PID, registers the slot
    // in process_table[], and allocates fpu_state.  It does NOT allocate a
    // kernel stack — we do that here because we need to copy the parent's.
    process_t *child = process_alloc();
    if (!child) return -ENOMEM;

    // Copy identity from parent
    child->parent_pid = parent->pid;
    child->ppid       = parent->pid;
    child->pgid       = parent->pgid;
    child->sid        = parent->sid;
    child->uid        = parent->uid;
    child->gid        = parent->gid;
    child->euid       = parent->euid;
    child->egid       = parent->egid;
    child->suid       = parent->suid;
    child->sgid       = parent->sgid;
    child->is_user    = 1;
    child->priority   = parent->priority;
    child->nice       = parent->nice;
    child->weight     = parent->weight;
    strncpy(child->name, parent->name, PROCESS_NAME_MAX - 1);

    // Inherit filesystem context
    child->current_slot = parent->current_slot;
    strncpy(child->cwd, parent->cwd, sizeof(child->cwd) - 1);

    // Clone environment
    child->envc = parent->envc;
    child->envp = dup_strv(parent->envp, parent->envc);
    if (parent->envc && !child->envp) { process_free(child); return -ENOMEM; }

    // Copy FPU state (process_alloc() already allocated fpu_state)
    if (parent->fpu_state && child->fpu_state)
        memcpy(child->fpu_state, parent->fpu_state, 512);

    // Kernel stack: copy parent's stack byte-for-byte so the saved frames
    // and on-stack variables are valid in the child's context.
    child->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!child->kernel_stack) { process_free(child); return -ENOMEM; }
    memcpy(child->kernel_stack, parent->kernel_stack, KERNEL_STACK_SIZE);

    // Translate parent's syscall frame base pointer into the child's stack.
    uint64_t parent_stack_base = (uint64_t)(uintptr_t)parent->kernel_stack;
    uint64_t child_stack_base  = (uint64_t)(uintptr_t)child->kernel_stack;

    uint64_t parent_rbp = g_syscall_entry_rbp;
    if (!parent_rbp ||
        parent_rbp < parent_stack_base ||
        parent_rbp >= parent_stack_base + KERNEL_STACK_SIZE) {
        kfree(child->kernel_stack);
        child->kernel_stack = NULL;
        process_free(child);
        return -EFAULT;
    }

    uint64_t rbp_off   = parent_rbp - parent_stack_base;
    uint64_t child_rbp = child_stack_base + rbp_off;

    // Patch saved rax on the child's copied stack → child returns 0 from fork().
    *(uint64_t *)(uintptr_t)child_rbp = 0;

    // When context_switch_asm jumps to context.rip (syscall_entry_return) with
    // RSP = child_rbp, the epilogue pops all saved registers and iretq's back
    // to userland using the hardware frame already on the copied stack.
    memset(&child->context, 0, sizeof(child->context));
    child->context.rip    = (uint64_t)(uintptr_t)syscall_entry_return;
    child->context.rsp    = child_rbp;
    child->context.rbp    = child_rbp;
    child->context.rflags = 0x202;  /* IF=1 */
    
    com_write_string(COM1_PORT, "[FORK] Child context: RIP=0x");
    com_write_hex64(COM1_PORT, child->context.rip);
    com_write_string(COM1_PORT, " RSP=0x");
    com_write_hex64(COM1_PORT, child->context.rsp);
    com_write_string(COM1_PORT, " current=0x");
    extern volatile process_t *current;
    com_write_hex64(COM1_PORT, (uint64_t)current);
    com_write_string(COM1_PORT, "\n");

    // Give the child a fair starting vruntime
    extern uint64_t scheduler_get_min_vruntime(void);
    child->vruntime = scheduler_get_min_vruntime();

    // Clone user address space (COW where writable)
    int rc = clone_user_address_space(parent, child);
    if (rc != 0) {
        kfree(child->kernel_stack);
        child->kernel_stack = NULL;
        process_free(child);
        return rc;
    }

    // Inherit parent's open file descriptors (POSIX fork semantics).
    extern void fd_clone_for_fork(int parent_pid, int child_pid);
    fd_clone_for_fork((int)parent->pid, (int)child->pid);

    // Link child into parent's children list under children_lock so that
    // do_waitpid() sees the child immediately after fork() returns.
    extern spinlock_t children_lock;
    spinlock_lock(&children_lock);
    child->parent       = parent;
    child->sibling_next = parent->children;
    child->sibling_prev = NULL;
    if (parent->children)
        parent->children->sibling_prev = child;
    parent->children = child;
    spinlock_unlock(&children_lock);

    // Set child state and add to the scheduler run queue.
    child->state = PROCESS_STATE_RUNNABLE;
    scheduler_add_process(child);

    com_write_string(COM1_PORT, "[FORK] Forked PID ");
    char buf[16];
    itoa((int)child->pid, buf, 10);
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, " from PID ");
    itoa((int)parent->pid, buf, 10);
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, " - Parent CR3=0x");
    com_write_hex64(COM1_PORT, parent->page_table ? parent->page_table : parent->cr3);
    com_write_string(COM1_PORT, " Child CR3=0x");
    com_write_hex64(COM1_PORT, child->cr3);
    com_write_string(COM1_PORT, "\n");

    // Parent returns child's PID.
    return (int)child->pid;
}
