#include <stddef.h>
#include "moduos/arch/AMD64/apic/apic.h"
#include "moduos/kernel/memory/paging.h"

static volatile uint32_t *g_ioapic = NULL;

void ioapic_init(uint64_t phys_addr) {
    g_ioapic = (volatile uint32_t *)ioremap(phys_addr, APIC_REGION_SIZE);
}

void ioapic_set_pin(uint32_t pin, uint32_t low_flags, uint32_t high_flags) {
    if (!g_ioapic) return;

    uint32_t reg_low  = IOAPIC_RED_TABLE_BASE + (2 * pin);
    uint32_t reg_high = reg_low + 1;

    g_ioapic[IOAPIC_REG_INDEX / 4] = reg_low;
    g_ioapic[IOAPIC_REG_DATA / 4]  = low_flags;

    g_ioapic[IOAPIC_REG_INDEX / 4] = reg_high;
    g_ioapic[IOAPIC_REG_DATA / 4]  = high_flags;
}
