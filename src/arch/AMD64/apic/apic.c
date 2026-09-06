#include "moduos/arch/AMD64/apic/apic.h"
#include "moduos/drivers/power/ACPI.h"
#include "moduos/kernel/io/io.h"
#include <stddef.h>

static uint32_t parse_madt_ioapic_phys(madt_t *madt) {
    uint8_t *ptr = madt->entries;
    uint8_t *end = (uint8_t *)madt + madt->header.length;

    while (ptr < end) {
        madt_entry_header_t *entry = (madt_entry_header_t *)ptr;
        if (entry->length == 0) break;
        
        if (entry->type == 1) {
            return ((madt_ioapic_t *)entry)->ioapic_address;
        }
        ptr += entry->length;
    }
    return 0;
}

void apic_init_system(void) {
    disable_legacy_pic();

    madt_t *madt = acpi_get_madt();
    if (!madt) return;

    uint32_t ioapic_phys = parse_madt_ioapic_phys(madt);
    if (!ioapic_phys) return;

    lapic_init(madt->local_apic_address);
    ioapic_init(ioapic_phys);
}

/* --- Mapping APIs --- */

void apic_map_irq(uint32_t irq, uint8_t idt_vector, uint8_t target_cpu_id) {
    uint32_t low_flags  = idt_vector | IOAPIC_RED_DEL_FIXED | IOAPIC_RED_TRIG_EDGE;
    uint32_t high_flags = (uint32_t)target_cpu_id << 24;

    ioapic_set_pin(irq, low_flags, high_flags);
}

void apic_unmap_irq(uint32_t irq) {
    uint32_t low_flags  = IOAPIC_RED_MASK;
    uint32_t high_flags = 0;

    ioapic_set_pin(irq, low_flags, high_flags);
}

void disable_legacy_pic(void) {
    outb(PIC1_DATA_PORT, PIC_MASK_ALL);
    outb(PIC2_DATA_PORT, PIC_MASK_ALL);
}
