// SPDX-License-Identifier: GPL-2.0-only
//
// ModuOS Kernel (GPLv2)
// fork_impl.c - Linux-like fork() for ModuOS (full address-space copy)
// Included by syscall.c

#include "moduos/kernel/errno.h"
#include "moduos/kernel/process/process.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/COM/com.h"

extern volatile uint64_t g_syscall_entry_rbp;
extern void syscall_entry_return(void);

// From process.c (private); replicate constants
#ifndef KERNEL_STACK_SIZE
#define KERNEL_STACK_SIZE 8192
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

// Map a single user page into child address space using Copy-On-Write if writable.
static int cow_map_user_page(uint64_t child_cr3, uint64_t vaddr, uint64_t parent_phys, uint64_t parent_pte) {
    uint64_t *child_pml4 = (uint64_t*)phys_to_virt_kernel(child_cr3 & 0xFFFFFFFFFFFFF000ULL);
    if (!child_pml4) {
        return -ENOMEM;
    }

    // Preserve user bit; apply COW for writable pages.
    uint64_t map_flags = PFLAG_PRESENT | PFLAG_USER;
    uint64_t child_pte_flags = (parent_pte & 0xFFFULL);

    if (parent_pte & PFLAG_WRITABLE) {
        // Mark both parent and child as read-only + COW.
        uint64_t page = vaddr & ~0xFFFULL;
        uint64_t new_parent_pte = (parent_phys & 0xFFFFFFFFFFFFF000ULL) | (child_pte_flags & ~PFLAG_WRITABLE) | PFLAG_COW;
        paging_set_pte(page, new_parent_pte);

        map_flags = (map_flags & ~PFLAG_WRITABLE) | PFLAG_COW;
        // Increase refcount since child will share.
        phys_ref_inc(parent_phys & 0xFFFFFFFFFFFFF000ULL);
    } else {
        // Read-only page can be shared as-is.
        phys_ref_inc(parent_phys & 0xFFFFFFFFFFFFF000ULL);
    }

    // Map into child
    if (paging_map_range_to_pml4(child_pml4, vaddr, parent_phys & 0xFFFFFFFFFFFFF000ULL, 4096, (map_flags | (child_pte_flags & (PFLAG_PWT|PFLAG_PCD|PFLAG_COW)))) != 0) {
        // Undo refcount bump
        phys_ref_dec(parent_phys & 0xFFFFFFFFFFFFF000ULL);
        return -ENOMEM;
    }

    return 0;
}

// Very simple clone strategy: copy the recorded user ranges page-by-page.
static int clone_user_address_space(process_t *parent, process_t *child) {
    if (!parent || !child) return -EINVAL;

    uint64_t child_cr3 = paging_create_process_pml4();
    if (!child_cr3) return -ENOMEM;

    // Switch into parent CR3 (should already be active), use paging_virt_to_phys to query.
    // Copy ranges: image, heap, mmap, stack(low..top).

    struct range { uint64_t a,b; } ranges[4];
    int rn = 0;

    if (parent->user_image_base && parent->user_image_end && parent->user_image_end > parent->user_image_base)
        ranges[rn++] = (struct range){ parent->user_image_base, parent->user_image_end };
    if (parent->user_heap_base && parent->user_heap_end && parent->user_heap_end > parent->user_heap_base)
        ranges[rn++] = (struct range){ parent->user_heap_base, parent->user_heap_end };
    if (parent->user_mmap_base && parent->user_mmap_end && parent->user_mmap_end > parent->user_mmap_base)
        ranges[rn++] = (struct range){ parent->user_mmap_base, parent->user_mmap_end };
    if (parent->user_stack_low && parent->user_stack_top && parent->user_stack_top > parent->user_stack_low)
        ranges[rn++] = (struct range){ parent->user_stack_low, parent->user_stack_top };

    for (int i = 0; i < rn; i++) {
        uint64_t start = ranges[i].a & ~0xFFFULL;
        uint64_t end = (ranges[i].b + 0xFFFULL) & ~0xFFFULL;
        for (uint64_t v = start; v < end; v += 0x1000ULL) {
            uint64_t phys = paging_virt_to_phys(v);
            if (!phys) continue;
            uint64_t pte = paging_get_pte(v);
            if (!(pte & PFLAG_USER)) continue;
            int rc = cow_map_user_page(child_cr3, v, phys, pte);
            if (rc != 0) return rc;
        }
    }

    child->page_table = child_cr3;

    // Copy bookkeeping
    child->user_image_base = parent->user_image_base;
    child->user_image_end  = parent->user_image_end;

    child->user_heap_base  = parent->user_heap_base;
    child->user_heap_end   = parent->user_heap_end;
    child->user_heap_limit = parent->user_heap_limit;

    child->user_mmap_base  = parent->user_mmap_base;
    child->user_mmap_end   = parent->user_mmap_end;
    child->user_mmap_limit = parent->user_mmap_limit;

    child->user_stack = parent->user_stack;
    child->user_stack_top = parent->user_stack_top;
    child->user_stack_low = parent->user_stack_low;
    child->user_stack_limit = parent->user_stack_limit;

    child->user_rip = parent->user_rip;
    child->user_rsp = parent->user_rsp;

    return 0;
}

int sys_fork_impl(void) {
    process_t *parent = process_get_current();
    if (!parent) return -ESRCH;
    if (!parent->is_user) return -EACCES;

    // Allocate child process structure
    process_t *child = (process_t*)kzalloc(sizeof(process_t));
    if (!child) return -ENOMEM;

    // Allocate PID
    child->pid = process_alloc_pid();
    if (child->pid == 0 || child->pid >= MAX_PROCESSES) { kfree(child); return -EAGAIN; }
    child->parent_pid = parent->pid;
    child->pgid = parent->pgid;
    child->uid = parent->uid;
    child->gid = parent->gid;

    strncpy(child->name, parent->name, PROCESS_NAME_MAX - 1);
    child->state = PROCESS_STATE_READY;
    child->priority = parent->priority;

    // Inherit filesystem context
    child->current_slot = parent->current_slot;
    strncpy(child->cwd, parent->cwd, sizeof(child->cwd) - 1);

    // Clone environment
    child->envc = parent->envc;
    child->envp = dup_strv(parent->envp, parent->envc);
    if (parent->envc && !child->envp) { kfree(child); return -ENOMEM; }

    // Kernel stack: clone bytes
    child->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!child->kernel_stack) { fork_free_strv(child->envp, child->envc); kfree(child); return -ENOMEM; }
    memcpy(child->kernel_stack, parent->kernel_stack, KERNEL_STACK_SIZE);

    // Child is a user process too.
    child->is_user = 1;

    // Prepare the child's saved scheduler cpu_state so that when it is scheduled it
    // continues at the syscall epilogue (syscall_entry_return) and iretq's back to
    // userland using the already-copied syscall frame on its kernel stack.
    memset(&child->cpu_state, 0, sizeof(child->cpu_state));

    uint64_t parent_stack_base = (uint64_t)(uintptr_t)parent->kernel_stack;
    uint64_t child_stack_base  = (uint64_t)(uintptr_t)child->kernel_stack;

    // Translate parent's syscall frame base pointer (rbp inside syscall_entry)
    // into the child's copied kernel stack.
    uint64_t parent_rbp = g_syscall_entry_rbp;
    if (!parent_rbp || parent_rbp < parent_stack_base || parent_rbp >= parent_stack_base + KERNEL_STACK_SIZE) {
        // If we can't locate the frame, fail (fork must be called from syscall context).
        kfree(child->kernel_stack);
        fork_free_strv(child->envp, child->envc);
        kfree(child);
        return -EFAULT;
    }

    uint64_t rbp_off = parent_rbp - parent_stack_base;
    uint64_t child_rbp = child_stack_base + rbp_off;

    // At rbp (in syscall_entry), [rbp] is saved rax. Child returns 0.
    *(uint64_t*)(uintptr_t)child_rbp = 0;

    child->cpu_state.rip = (uint64_t)(uintptr_t)syscall_entry_return;
    child->cpu_state.rsp = child_rbp;
    child->cpu_state.rbp = child_rbp;
    child->cpu_state.rflags = 0x202;

    // Clone user address space into a new CR3
    int rc = clone_user_address_space(parent, child);
    if (rc != 0) {
        // leak cleanup minimal
        kfree(child->kernel_stack);
        fork_free_strv(child->envp, child->envc);
        kfree(child);
        return rc;
    }

    // Register child in process table and ready queue.
    if (process_register(child) != 0) {
        // Best-effort cleanup
        process_free_user_memory(child);
        if (child->kernel_stack) kfree(child->kernel_stack);
        fork_free_strv(child->envp, child->envc);
        kfree(child);
        return -EAGAIN;
    }

    // Parent returns child's PID.
    return (int)child->pid;
}
