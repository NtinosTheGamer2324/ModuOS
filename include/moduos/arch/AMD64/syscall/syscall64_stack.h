#ifndef MODUOS_AMD64_SYSCALL64_STACK_H
#define MODUOS_AMD64_SYSCALL64_STACK_H

#include <stdint.h>

/*
 * Current CPU's kernel syscall stack pointer (top of current process kernel stack).
 * In SMP this must become per-CPU (via GS base).
 */
void amd64_syscall_set_kernel_stack(uint64_t rsp0);
uint64_t amd64_syscall_get_kernel_stack(void);

#endif
