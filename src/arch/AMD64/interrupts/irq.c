#include "moduos/arch/AMD64/interrupts/irq.h"
#include "moduos/arch/AMD64/interrupts/idt.h"
#include "moduos/arch/AMD64/interrupts/pic.h"

extern void (*irq_stubs[16])(); // from isr.asm

static irq_handler_t irq_handlers[16] = { 0 };

// REMOVE timer_irq_handler from here - it's in timer.c now

void irq_install_handler(int irq, irq_handler_t handler) {
    if (irq >= 0 && irq < 16)
        irq_handlers[irq] = handler;
}

void irq_uninstall_handler(int irq) {
    if (irq >= 0 && irq < 16)
        irq_handlers[irq] = 0;
}

void irq_dispatch(uint8_t irq) {
    if (irq < 16 && irq_handlers[irq])
        irq_handlers[irq]();
    else
        pic_send_eoi(irq);  // Send EOI even if no handler
}

void irq_init(void) {
    for (int i = 0; i < 16; i++) {
        idt_set_entry(32 + i, irq_stubs[i], 0x8E); // Interrupt gate, ring 0
    }
}