#pragma once
#include <stdint.h>

/*
 * Minimal per-CPU state for SMP.
 *
 * We use GS base to point at a cpu_local_t for the current CPU.
 * Field order is intentionally 64-bit only so we can use fixed offsets in asm.
 */

typedef struct cpu_local {
    uint64_t self;            /* +0  : pointer to this struct */
    uint64_t cpu_num;         /* +8  : 0..n-1 */
    uint64_t apic_id;         /* +16 : LAPIC ID */
    uint64_t syscall_rsp0;    /* +24 : ring0 stack for SYSCALL entry */
    uint64_t current_process; /* +32 : process_t* (opaque here) */
    uint64_t resched;         /* +40 : reschedule requested flag */
    uint64_t user_rsp;        /* +48 : user RSP saved by SYSCALL */
    uint64_t user_rip;        /* +56 : user RIP saved by SYSCALL */
    uint64_t user_rflags;     /* +64 : user RFLAGS saved by SYSCALL */
} cpu_local_t;

/* Offsets for assembly (must match struct layout) */
#define CPU_LOCAL_OFF_SELF            0
#define CPU_LOCAL_OFF_CPU_NUM         8
#define CPU_LOCAL_OFF_APIC_ID         16
#define CPU_LOCAL_OFF_SYSCALL_RSP0    24
#define CPU_LOCAL_OFF_CURRENT_PROCESS 32
#define CPU_LOCAL_OFF_RESCHED         40
#define CPU_LOCAL_OFF_USER_RSP        48
#define CPU_LOCAL_OFF_USER_RIP        56
#define CPU_LOCAL_OFF_USER_RFLAGS     64

