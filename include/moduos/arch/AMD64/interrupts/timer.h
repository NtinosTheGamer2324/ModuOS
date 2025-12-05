#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

void pit_init(uint32_t frequency);
void timer_irq_handler(void);  // ADD THIS
uint64_t get_system_ticks(void);  // ADD THIS

#endif