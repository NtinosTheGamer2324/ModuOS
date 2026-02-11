#pragma once
#include <stdint.h>

// Boot-provided ACPI RSDP physical address from Multiboot2 ACPI tags.
// 0 means not provided.
void acpi_boot_set_rsdp_phys(uint64_t phys);
uint64_t acpi_boot_get_rsdp_phys(void);
