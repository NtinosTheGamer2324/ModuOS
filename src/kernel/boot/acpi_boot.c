#include "moduos/kernel/acpi_boot.h"

static uint64_t g_rsdp_phys;

void acpi_boot_set_rsdp_phys(uint64_t phys) {
    g_rsdp_phys = phys;
}

uint64_t acpi_boot_get_rsdp_phys(void) {
    return g_rsdp_phys;
}
