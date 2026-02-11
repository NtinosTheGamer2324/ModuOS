#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

void pit_init(uint32_t frequency);
void timer_irq_handler(void);
uint64_t get_system_ticks(void);
uint32_t get_pit_frequency(void);

// Called by APIC timer ISR to drive the system timebase.
void timer_tick_from_apic(void);

// Enable/disable APIC-driven tick (disables PIT tick increment to avoid double counting).
void timer_set_apic_enabled(int enabled);

/* Helpers */
uint64_t ticks_to_ms(uint64_t ticks);
uint64_t ms_to_ticks(uint64_t ms);

void usb_tick(void);

#endif