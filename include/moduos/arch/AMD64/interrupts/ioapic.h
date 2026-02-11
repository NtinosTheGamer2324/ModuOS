#pragma once
#include <stdint.h>

// Initialize IOAPIC routing using ACPI MADT. Returns 0 on success.
int ioapic_init_from_madt(void);

// Route legacy IRQ (0-15) to an IDT vector on the given APIC ID.
// Flags: polarity/trigger from MADT ISO when applicable.
int ioapic_route_irq(uint8_t irq, uint8_t vector, uint8_t dest_apic_id, uint16_t iso_flags);

// Mask legacy IRQ.
int ioapic_mask_irq(uint8_t irq);

int ioapic_is_enabled(void);

// Acknowledge end-of-interrupt for IOAPIC-delivered interrupts (LAPIC EOI).
void ioapic_eoi(void);
