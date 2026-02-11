#pragma once
#include <stdint.h>

// Initialize LAPIC using MADT (ACPI). Returns 0 on success.
int apic_init_from_madt(void);

// Initialize LAPIC timer to generate periodic ticks at the requested Hz.
// Uses PIT ticks for calibration (PIT must already be running).
int apic_timer_init(uint32_t hz);

int apic_is_enabled(void);
int apic_timer_is_enabled(void);
