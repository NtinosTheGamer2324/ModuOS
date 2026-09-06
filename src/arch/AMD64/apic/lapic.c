#include <stddef.h>
#include "moduos/arch/AMD64/apic/apic.h"
#include "moduos/kernel/memory/paging.h"

static volatile uint32_t *g_lapic = NULL;

static inline void lapic_write(uint32_t reg, uint32_t val) {
    g_lapic[reg / 4] = val;
}

void lapic_init(uint64_t phys_addr) {
    g_lapic = (volatile uint32_t *)ioremap(phys_addr, APIC_REGION_SIZE);
    if (!g_lapic) return;

    uint32_t low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(IA32_APIC_BASE_MSR));
    low |= IA32_APIC_BASE_MSR_EN;
    asm volatile("wrmsr" : : "a"(low), "d"(high), "c"(IA32_APIC_BASE_MSR));

    lapic_write(LAPIC_REG_TPR, 0x00);
    lapic_write(LAPIC_REG_SPURIOUS, 0xFF | LAPIC_SPURIOUS_ENABLE);
    lapic_write(LAPIC_REG_EOI, 0x00);
}

void lapic_eoi(void) {
    if (g_lapic) {
        lapic_write(LAPIC_REG_EOI, 0x00);
    }
}
