#pragma once
#include <stdint.h>

// Read the ACPI PM timer (24-bit or 32-bit depending on HW) in raw ticks.
// Returns 0 if not available.
uint32_t acpi_pm_timer_read(void);

// Return the PM timer I/O port base, or 0 if not available.
uint16_t acpi_pm_timer_port(void);
