#include "moduos/drivers/power/acpi_pm_timer.h"
#include "moduos/drivers/power/ACPI.h"
#include "moduos/kernel/io/io.h"

uint16_t acpi_pm_timer_port(void) {
    fadt_t *f = acpi_get_fadt();
    if (!f) return 0;
    // Prefer x_pm_timer_block when available and in System I/O space.
    if (f->header.length >= offsetof(fadt_t, x_pm_timer_block) + sizeof(f->x_pm_timer_block)) {
        if (f->x_pm_timer_block.address_space == 1 && f->x_pm_timer_block.address) {
            return (uint16_t)f->x_pm_timer_block.address;
        }
    }
    if (f->pm_timer_block) return (uint16_t)f->pm_timer_block;
    return 0;
}

uint32_t acpi_pm_timer_read(void) {
    uint16_t port = acpi_pm_timer_port();
    if (!port) return 0;
    // PM timer is read via 32-bit I/O, lower 24 bits are valid on most systems.
    return inl(port);
}
