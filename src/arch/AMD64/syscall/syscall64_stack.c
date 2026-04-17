#include "moduos/arch/AMD64/syscall/syscall64_stack.h"
#include "moduos/arch/AMD64/gdt.h"
#include "moduos/arch/AMD64/cpu.h"
#include "moduos/kernel/percpu.h"
#include "moduos/kernel/COM/com.h"
#include <stdint.h>

/*
 * The TSS RSP0 field is the authoritative kernel-stack pointer for privilege
 * transitions.  Both the hardware (interrupt/exception entry) and
 * syscall64_entry (which reads it directly from g_tss) use this value.
 *
 * The GS-slot copy (cpu_local->syscall_rsp0) is kept in sync as a
 * convenience for any future SMP path that prefers not to indirect through
 * g_tss, but it is not load-bearing: syscall64_entry no longer reads from
 * it.  If cpu_local_get() returns NULL (GS not yet initialised) we simply
 * skip the GS update — the TSS copy is sufficient.
 */

static uint64_t g_syscall_rsp0;

void amd64_syscall_set_kernel_stack(uint64_t rsp0) {
    if ((rsp0 >> 48) != 0xFFFF || rsp0 < 0xFFFF800000000000ULL)
        return;

    g_syscall_rsp0 = rsp0;

    /* TSS RSP0 — used by hardware and by syscall64_entry directly. */
    amd64_tss_set_rsp0(rsp0);

    /* GS-slot copy — best-effort; not relied upon for stack switching. */
    cpu_local_t *local = cpu_local_get();
    if (local)
        local->syscall_rsp0 = rsp0;

    com_write_string(COM1_PORT, "[TSS] Set RSP0=0x");
    char buf[17];
    /* Print as a single 16-digit hex value. */
    static const char hex[] = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) {
        buf[i] = hex[rsp0 & 0xF];
        rsp0 >>= 4;
    }
    buf[16] = '\0';
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, "\n");
}

uint64_t amd64_syscall_get_kernel_stack(void) {
    return g_syscall_rsp0;
}