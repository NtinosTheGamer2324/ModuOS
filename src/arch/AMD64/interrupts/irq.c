#include "moduos/arch/AMD64/interrupts/irq.h"
#include "moduos/kernel/spinlock.h"
#include "moduos/arch/AMD64/interrupts/idt.h"
#include "moduos/kernel/memory/string.h" // itoa
#include "moduos/arch/AMD64/interrupts/pic.h"
#include "moduos/arch/AMD64/interrupts/ioapic.h"
#include "moduos/kernel/COM/com.h"

extern void (*irq_stubs[16])(); // from isr.asm

static irq_handler_t irq_handlers[16] = { 0 };
static spinlock_t irq_handlers_lock;

// REMOVE timer_irq_handler from here - it's in timer.c now

static void irq_dbg_hex64(uint64_t v, char out[17]) {
    static const char *hx = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) {
        out[i] = hx[v & 0xF];
        v >>= 4;
    }
    out[16] = 0;
}

void irq_install_handler(int irq, irq_handler_t handler) {
    if (irq >= 0 && irq < 16) {
        irq_handlers[irq] = handler;

        // Debug: log handler address so we can catch truncated/unrelocated module pointers.
        char h[17];
        irq_dbg_hex64((uint64_t)(uintptr_t)handler, h);
        com_write_string(COM1_PORT, "[IRQ] install irq=");
        char ibuf[4];
        itoa(irq, ibuf, 10);
        com_write_string(COM1_PORT, ibuf);
        com_write_string(COM1_PORT, " handler=0x");
        com_write_string(COM1_PORT, h);
        com_write_string(COM1_PORT, "\n");
    }
}

void irq_uninstall_handler(int irq) {
    if (irq >= 0 && irq < 16)
        irq_handlers[irq] = 0;
}

void irq_dispatch(uint8_t irq) {
    static uint64_t dispatch_count = 0;
    dispatch_count++;
    if (irq == 0 && (dispatch_count % 1000) == 0) {
        com_write_string(COM1_PORT, "[IRQ-DISPATCH] IRQ 0 dispatched, count=");
        char buf[32];
        itoa((int)dispatch_count, buf, 10);
        com_write_string(COM1_PORT, buf);
        com_write_string(COM1_PORT, "\n");
    }
    
    if (irq < 16 && irq_handlers[irq]) {
        irq_handlers[irq]();
    }

    // Ack interrupt.
    // If IOAPIC is enabled, EOI must be sent to LAPIC; otherwise send PIC EOI.
    if (ioapic_is_enabled()) {
        ioapic_eoi();
    } else {
        pic_send_eoi(irq);
    }
}

void irq_init(void) {
    for (int i = 0; i < 16; i++) {
        idt_set_entry(32 + i, irq_stubs[i], 0x8E); // Interrupt gate, ring 0
    }
}