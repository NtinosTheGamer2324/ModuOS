// irq.h
#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>

typedef void (*irq_handler_t)(void);

// Register/unregister a C-level IRQ handler
void irq_install_handler(int irq, irq_handler_t handler);
void irq_uninstall_handler(int irq);

// Called from assembly stubs
void irq_dispatch(uint8_t irq);

// Initialize all IRQs (set in IDT)
void irq_init(void);

#endif
