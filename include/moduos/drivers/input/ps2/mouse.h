#pragma once

#include <stdint.h>

// Initialize PS/2 mouse (port2) and install IRQ12 handler.
int ps2_mouse_init(void);

// IRQ12 handler
void ps2_mouse_irq_handler(void);
