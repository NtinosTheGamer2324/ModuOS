#include "moduos/arch/AMD64/interrupts/apic.h"
#include "moduos/arch/AMD64/interrupts/idt.h"
#include "moduos/arch/AMD64/interrupts/timer.h"
#include "moduos/drivers/power/ACPI.h"
#include "moduos/drivers/power/acpi_pm_timer.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/memory/paging.h"
#include <stdint.h>

// Minimal Local APIC (xAPIC) support (stage 1): enable LAPIC + periodic LAPIC timer.

#define LAPIC_REG_ID        0x020
#define LAPIC_REG_EOI       0x0B0
#define LAPIC_REG_SVR       0x0F0
#define LAPIC_REG_LVT_TIMER 0x320
#define LAPIC_REG_TMRINIT   0x380
#define LAPIC_REG_TMRCURR   0x390
#define LAPIC_REG_TMRDIV    0x3E0

#define LAPIC_SVR_ENABLE    (1u << 8)
#define LAPIC_LVT_MASK      (1u << 16)
#define LAPIC_LVT_PERIODIC  (1u << 17)

// Use a vector that doesn't collide with exceptions/IRQs (IRQs use 0x20..0x2F).
#define LAPIC_TIMER_VECTOR  0x40
#define LAPIC_SPURIOUS_VECTOR 0xFF

static volatile uint32_t *g_lapic;
static int g_apic_enabled;
static int g_apic_timer_enabled;

static inline uint32_t lapic_read(uint32_t reg) {
    return g_lapic[reg / 4];
}

static inline void lapic_write(uint32_t reg, uint32_t v) {
    g_lapic[reg / 4] = v;
    // read-after-write to flush posted writes on some systems
    (void)g_lapic[LAPIC_REG_ID / 4];
}

static void lapic_eoi(void) {
    if (g_lapic) lapic_write(LAPIC_REG_EOI, 0);
}

// ISR stubs are implemented in asm (apic_isr.asm) and call these C handlers.
void apic_timer_irq_handler_c(void) {
    lapic_eoi();
    timer_tick_from_apic();
}

void apic_spurious_irq_handler_c(void) {
    /* Per Intel SDM §10.9: spurious interrupts must NOT generate an EOI.
     * Sending one corrupts the LAPIC ISR state machine. */
    (void)0;
}

int apic_is_enabled(void) { return g_apic_enabled; }
int apic_timer_is_enabled(void) { return g_apic_timer_enabled; }

int apic_init_from_madt(void) {
    madt_t *madt = acpi_get_madt();
    if (!madt) return -1;

    uint64_t lapic_phys = (uint64_t)madt->local_apic_address;
    if (!lapic_phys) return -1;

    g_lapic = (volatile uint32_t*)phys_to_virt_kernel(lapic_phys);
    if (!g_lapic) return -1;

    // Install IDT entries we may trigger.
    extern void apic_timer_stub(void);
    extern void apic_spurious_stub(void);
    idt_set_entry(LAPIC_TIMER_VECTOR, apic_timer_stub, 0x8E);
    idt_set_entry(LAPIC_SPURIOUS_VECTOR, apic_spurious_stub, 0x8E);

    // Enable LAPIC via SVR.
    uint32_t svr = lapic_read(LAPIC_REG_SVR);
    svr = (svr & ~0xFFu) | LAPIC_SPURIOUS_VECTOR;
    svr |= LAPIC_SVR_ENABLE;
    lapic_write(LAPIC_REG_SVR, svr);

    g_apic_enabled = 1;
    com_write_string(COM1_PORT, "[APIC] LAPIC enabled\n");
    return 0;
}

// Calibrate LAPIC timer using ACPI PM timer as reference (preferred).
int apic_timer_init(uint32_t hz) {
    if (!g_apic_enabled || !g_lapic) return -1;
    if (hz == 0) hz = 1000;

    // Divide by 16 (common choice).
    lapic_write(LAPIC_REG_TMRDIV, 0x3);

    // Mask timer while calibrating.
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASK | LAPIC_TIMER_VECTOR);

    // One-shot downcounter from max.
    lapic_write(LAPIC_REG_TMRINIT, 0xFFFFFFFFu);
    uint32_t start = lapic_read(LAPIC_REG_TMRCURR);

    // Wait ~100ms using ACPI PM timer (3.579545 MHz). This does not depend on interrupts.
    uint16_t pm_port = acpi_pm_timer_port();
    if (!pm_port) {
        com_write_string(COM1_PORT, "[APIC] No ACPI PM timer; cannot calibrate\n");
        return -1;
    }

    const uint32_t pm_freq = 3579545u;
    const uint32_t delta = (pm_freq / 10u); // ~100ms

    uint32_t t0 = acpi_pm_timer_read() & 0x00FFFFFFu;
    while (((acpi_pm_timer_read() & 0x00FFFFFFu) - t0) < delta) {
        // busy wait
    }

    uint32_t end = lapic_read(LAPIC_REG_TMRCURR);
    uint32_t elapsed = start - end;
    if (elapsed == 0) {
        com_write_string(COM1_PORT, "[APIC] LAPIC timer calibration failed (elapsed=0)\n");
        return -1;
    }

    // elapsed counts per 100ms
    uint64_t counts_per_ms = (uint64_t)elapsed / 100ULL;
    if (counts_per_ms == 0) counts_per_ms = 1;

    uint64_t counts_per_tick = (counts_per_ms * 1000ULL) / (uint64_t)hz;
    if (counts_per_tick == 0) counts_per_tick = 1;

    // Program periodic timer.
    uint32_t lvt = LAPIC_TIMER_VECTOR | LAPIC_LVT_PERIODIC;
    lapic_write(LAPIC_REG_LVT_TIMER, lvt);
    lapic_write(LAPIC_REG_TMRINIT, (uint32_t)counts_per_tick);

    g_apic_timer_enabled = 1;
    timer_set_apic_enabled(1);

    com_write_string(COM1_PORT, "[APIC] LAPIC timer enabled\n");
    return 0;
}
