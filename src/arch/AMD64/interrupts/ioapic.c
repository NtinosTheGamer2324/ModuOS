#include "moduos/arch/AMD64/interrupts/ioapic.h"
#include "moduos/drivers/power/ACPI.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/COM/com.h"
#include <stdint.h>

// Minimal IOAPIC support (single IOAPIC, legacy IRQ0-15) with MADT overrides.

#define IOAPIC_REGSEL 0x00
#define IOAPIC_WIN    0x10

#define IOAPIC_REG_ID     0x00
#define IOAPIC_REG_VER    0x01
#define IOAPIC_REG_ARB    0x02
#define IOAPIC_REG_REDTBL (0x10)

// Redirection entry flags
#define IOAPIC_REDIR_MASKED      (1ULL << 16)
#define IOAPIC_REDIR_TRIGGER_LVL (1ULL << 15)
#define IOAPIC_REDIR_POL_LOW     (1ULL << 13)
#define IOAPIC_REDIR_DEST_PHYS   (0ULL << 11)
#define IOAPIC_REDIR_DELIV_FIXED (0ULL << 8)

// ISO flags bits (ACPI spec)
#define ISO_POLARITY_MASK 0x3
#define ISO_TRIGGER_MASK  0xC

static volatile uint32_t *g_ioapic;
static uint32_t g_ioapic_gsi_base;
static uint8_t g_ioapic_max_redir;
static int g_ioapic_enabled;

// local apic mmio for EOI
static volatile uint32_t *g_lapic;

static inline void ioapic_write(uint32_t reg, uint32_t v) {
    g_ioapic[IOAPIC_REGSEL/4] = reg;
    g_ioapic[IOAPIC_WIN/4] = v;
}

static inline uint32_t ioapic_read(uint32_t reg) {
    g_ioapic[IOAPIC_REGSEL/4] = reg;
    return g_ioapic[IOAPIC_WIN/4];
}

static inline void lapic_write(uint32_t reg, uint32_t v) {
    g_lapic[reg/4] = v;
    (void)g_lapic[0x20/4];
}

int ioapic_is_enabled(void) { return g_ioapic_enabled; }

void ioapic_eoi(void) {
    if (g_lapic) {
        lapic_write(0x0B0, 0);
    }
}

static uint16_t find_iso_flags(uint8_t irq) {
    madt_t *m = acpi_get_madt();
    if (!m) return 0;
    uint8_t *ptr = (uint8_t*)m->entries;
    uint8_t *end = (uint8_t*)m + m->header.length;
    while (ptr + sizeof(madt_entry_header_t) <= end) {
        madt_entry_header_t *h = (madt_entry_header_t*)ptr;
        if (h->length < sizeof(madt_entry_header_t)) break;
        if (h->type == 2 && h->length >= sizeof(madt_interrupt_override_t)) {
            madt_interrupt_override_t *iso = (madt_interrupt_override_t*)ptr;
            if (iso->bus_source == 0 && iso->irq_source == irq) {
                return iso->flags;
            }
        }
        ptr += h->length;
    }
    return 0;
}

static uint32_t find_gsi_for_irq(uint8_t irq) {
    madt_t *m = acpi_get_madt();
    if (!m) return irq;
    uint8_t *ptr = (uint8_t*)m->entries;
    uint8_t *end = (uint8_t*)m + m->header.length;
    while (ptr + sizeof(madt_entry_header_t) <= end) {
        madt_entry_header_t *h = (madt_entry_header_t*)ptr;
        if (h->length < sizeof(madt_entry_header_t)) break;
        if (h->type == 2 && h->length >= sizeof(madt_interrupt_override_t)) {
            madt_interrupt_override_t *iso = (madt_interrupt_override_t*)ptr;
            if (iso->bus_source == 0 && iso->irq_source == irq) {
                return iso->global_system_interrupt;
            }
        }
        ptr += h->length;
    }
    return irq;
}

int ioapic_route_irq(uint8_t irq, uint8_t vector, uint8_t dest_apic_id, uint16_t iso_flags) {
    if (!g_ioapic || !g_ioapic_enabled) return -1;

    uint32_t gsi = find_gsi_for_irq(irq);
    if (gsi < g_ioapic_gsi_base) return -1;
    uint32_t pin = gsi - g_ioapic_gsi_base;
    if (pin > g_ioapic_max_redir) return -1;

    uint64_t redir = 0;
    redir |= (uint64_t)vector;
    redir |= IOAPIC_REDIR_DELIV_FIXED | IOAPIC_REDIR_DEST_PHYS;

    uint16_t pol = iso_flags & ISO_POLARITY_MASK;
    uint16_t trg = iso_flags & ISO_TRIGGER_MASK;

    // polarity: 1=high, 3=low (0/2 means conforming -> treat as high)
    if (pol == 3) redir |= IOAPIC_REDIR_POL_LOW;

    // trigger: 0=conforming, 4=edge, 0xC=level (treat conforming as edge)
    if (trg == 0xC) redir |= IOAPIC_REDIR_TRIGGER_LVL;

    // unmask
    redir &= ~IOAPIC_REDIR_MASKED;

    uint32_t reg = IOAPIC_REG_REDTBL + (pin * 2);
    ioapic_write(reg + 0, (uint32_t)(redir & 0xFFFFFFFFu));
    ioapic_write(reg + 1, (uint32_t)((redir >> 32) & 0xFFu) | ((uint32_t)dest_apic_id << 24));

    return 0;
}

int ioapic_mask_irq(uint8_t irq) {
    if (!g_ioapic || !g_ioapic_enabled) return -1;

    uint32_t gsi = find_gsi_for_irq(irq);
    if (gsi < g_ioapic_gsi_base) return -1;
    uint32_t pin = gsi - g_ioapic_gsi_base;
    if (pin > g_ioapic_max_redir) return -1;

    uint32_t reg = IOAPIC_REG_REDTBL + (pin * 2);
    uint32_t lo = ioapic_read(reg);
    lo |= (uint32_t)IOAPIC_REDIR_MASKED;
    ioapic_write(reg, lo);
    return 0;
}

int ioapic_init_from_madt(void) {
    madt_t *m = acpi_get_madt();
    if (!m) return -1;

    // Map LAPIC for EOI
    if (m->local_apic_address) {
        g_lapic = (volatile uint32_t*)phys_to_virt_kernel((uint64_t)m->local_apic_address);
    }

    // Find first IOAPIC entry
    madt_ioapic_t *io = NULL;
    uint8_t *ptr = (uint8_t*)m->entries;
    uint8_t *end = (uint8_t*)m + m->header.length;
    while (ptr + sizeof(madt_entry_header_t) <= end) {
        madt_entry_header_t *h = (madt_entry_header_t*)ptr;
        if (h->length < sizeof(madt_entry_header_t)) break;
        if (h->type == 1 && h->length >= sizeof(madt_ioapic_t)) {
            io = (madt_ioapic_t*)ptr;
            break;
        }
        ptr += h->length;
    }
    if (!io) return -1;

    g_ioapic = (volatile uint32_t*)phys_to_virt_kernel((uint64_t)io->ioapic_address);
    g_ioapic_gsi_base = io->global_system_interrupt_base;

    uint32_t ver = ioapic_read(IOAPIC_REG_VER);
    g_ioapic_max_redir = (uint8_t)((ver >> 16) & 0xFFu);

    g_ioapic_enabled = 1;

    com_write_string(COM1_PORT, "[IOAPIC] enabled\n");

    // Route IRQ0-15 to vectors 0x20-0x2F on BSP APIC ID 0 (QEMU). Later we can read BSP APIC ID.
    for (uint8_t irq = 0; irq < 16; irq++) {
        uint16_t flags = find_iso_flags(irq);
        (void)ioapic_route_irq(irq, (uint8_t)(0x20 + irq), 0, flags);
    }

    return 0;
}
