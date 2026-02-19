#include "moduos/arch/AMD64/interrupts/timer.h"
#include "moduos/arch/AMD64/interrupts/irq.h"
#include "moduos/arch/AMD64/interrupts/pic.h"
#include "moduos/kernel/io/io.h"
// #include "moduos/kernel/process/process.h"  // OLD - temporarily disabled
#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/memory/string.h"
#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43

static volatile uint64_t system_ticks = 0;
static volatile int in_timer_handler = 0;
static volatile uint32_t pit_hz = 100;
static volatile int apic_tick_enabled = 0;

uint32_t get_pit_frequency(void) {
    return pit_hz;
}

uint64_t ticks_to_ms(uint64_t ticks) {
    uint32_t hz = pit_hz ? pit_hz : 100;
    return (ticks * 1000ULL) / (uint64_t)hz;
}

uint64_t ms_to_ticks(uint64_t ms) {
    uint32_t hz = pit_hz ? pit_hz : 100;
    return (ms * (uint64_t)hz + 999ULL) / 1000ULL;
}

void pit_init(uint32_t frequency)
{
    if (frequency == 0) frequency = 100;
    pit_hz = frequency;
    uint32_t divisor = 1193182 / frequency;
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, divisor & 0xFF);
    outb(PIT_CHANNEL0, (divisor >> 8) & 0xFF);
    irq_install_handler(0, timer_irq_handler);
}

void timer_irq_handler(void) {
    static uint64_t handler_count = 0;
    handler_count++;
    
    if (handler_count == 1 || (handler_count % 5000) == 0) {
        com_write_string(COM1_PORT, "[TIMER-IRQ] handler called, count=");
        char buf[32];
        itoa((int)handler_count, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, "\n");
    }
    
    if (in_timer_handler) {
        // EOI is handled by irq_dispatch() (PIC or LAPIC depending on mode).
        return;
    }

    in_timer_handler = 1;
    if (!apic_tick_enabled) {
        system_ticks++;
    }

    // EOI is handled by irq_dispatch(); do not send PIC EOI here (breaks IOAPIC mode).
    in_timer_handler = 0;

    // Use new scheduler
    scheduler_tick();
    
    // Check if reschedule is needed and do it
    // SAFETY: Do NOT call schedule() from IRQ context!
    // The scheduler will be called from the main loop or other safe points
    // Calling schedule() from here causes triple faults due to context corruption
    // if (should_reschedule()) {
    //     schedule();
    // }
}

uint64_t get_system_ticks(void) {
    return system_ticks;
}

void timer_set_apic_enabled(int enabled) {
    apic_tick_enabled = enabled ? 1 : 0;
}

void timer_tick_from_apic(void) {
    system_ticks++;
    scheduler_tick();
}