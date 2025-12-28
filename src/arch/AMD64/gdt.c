#include "moduos/arch/AMD64/gdt.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/debug.h"
#include "moduos/kernel/macros.h"
#include <stddef.h>

/*
 * Proper AMD64 GDT + TSS for ring3 + syscall/sysret.
 *
 * In long mode, base/limit of CS/DS are mostly ignored, but access rights/DPL still matter.
 * We still need valid descriptors for:
 *  - kernel code (DPL0)
 *  - kernel data (DPL0)
 *  - user code (DPL3)
 *  - user data (DPL3)
 *  - 64-bit TSS descriptor (for RSP0 stack switching)
 */

struct __attribute__((packed)) gdt_ptr {
    uint16_t limit;
    uint64_t base;
};

/* 8-byte segment descriptor */
static uint64_t gdt[8];

/* 64-bit TSS */
struct __attribute__((packed)) tss64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
};

static struct tss64 tss;

static uint64_t gdt_make_code_desc(uint8_t dpl) {
    /*
     * 64-bit code segment:
     *  P=1, DPL=dpl, S=1, type=0xA (exec/read)
     *  L=1 (64-bit), D=0
     */
    uint64_t desc = 0;
    desc |= (uint64_t)0xA << 40;          /* type */
    desc |= (uint64_t)1   << 44;          /* S */
    desc |= (uint64_t)(dpl & 3) << 45;    /* DPL */
    desc |= (uint64_t)1   << 47;          /* P */
    desc |= (uint64_t)1   << 53;          /* L */
    return desc;
}

static uint64_t gdt_make_data_desc(uint8_t dpl) {
    /*
     * Data segment:
     *  P=1, DPL=dpl, S=1, type=0x2 (read/write)
     */
    uint64_t desc = 0;
    desc |= (uint64_t)0x2 << 40;
    desc |= (uint64_t)1   << 44;
    desc |= (uint64_t)(dpl & 3) << 45;
    desc |= (uint64_t)1   << 47;
    return desc;
}

static void gdt_set_tss_desc(int idx, uint64_t base, uint32_t limit) {
    /* 16-byte TSS descriptor consumes two GDT slots */
    uint64_t low = 0;
    uint64_t high = 0;

    low |= (limit & 0xFFFFULL);
    low |= (base & 0xFFFFFFULL) << 16;

    low |= (uint64_t)0x9 << 40;          /* type = 64-bit available TSS */
    low |= (uint64_t)0   << 44;          /* S = 0 (system) */
    low |= (uint64_t)0   << 45;          /* DPL = 0 */
    low |= (uint64_t)1   << 47;          /* P */

    low |= (uint64_t)((limit >> 16) & 0xF) << 48;
    low |= (uint64_t)((base >> 24) & 0xFF) << 56;

    high |= (base >> 32) & 0xFFFFFFFFULL;

    gdt[idx] = low;
    gdt[idx + 1] = high;
}

void amd64_tss_set_rsp0(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}

void amd64_gdt_init(void) {
    /* Null */
    gdt[0] = 0;

    /* Kernel code/data */
    gdt[1] = gdt_make_code_desc(0);
    gdt[2] = gdt_make_data_desc(0);

    /* User data/code (layout chosen to satisfy SYSRET selector derivation)
     * USER_DS = 0x23 (index 4), USER_CS = 0x2B (index 5)
     */
    gdt[3] = gdt_make_data_desc(3);
    gdt[4] = gdt_make_data_desc(3); /* USER_DS (index 4) */
    gdt[5] = gdt_make_code_desc(3); /* USER_CS (index 5) */

    /* TSS */
    for (size_t i = 0; i < sizeof(tss); i++) ((uint8_t*)&tss)[i] = 0;
    tss.iopb_offset = (uint16_t)sizeof(tss);
    gdt_set_tss_desc(6, (uint64_t)(uintptr_t)&tss, (uint32_t)(sizeof(tss) - 1));

    struct gdt_ptr gp;
    gp.limit = (uint16_t)(sizeof(gdt) - 1);
    gp.base = (uint64_t)(uintptr_t)&gdt[0];

    __asm__ volatile ("lgdt %0" : : "m"(gp));

    /* Reload segment registers. CS reload requires far return/jump. */
    __asm__ volatile (
        "mov %0, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%ss\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        :
        : "i"(KERNEL_DS)
        : "ax", "memory"
    );

    /* Far return to reload CS */
    __asm__ volatile (
        "pushq %0\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        :
        : "i"(KERNEL_CS)
        : "rax", "memory"
    );

    /* Load TSS */
    __asm__ volatile ("ltr %0" : : "r"((uint16_t)TSS_SEL));

    COM_LOG_OK(COM1_PORT, "AMD64 GDT+TSS initialized");
}
