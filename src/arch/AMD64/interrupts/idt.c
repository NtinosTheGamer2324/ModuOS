// idt.c
#include "moduos/arch/AMD64/interrupts/idt.h"

struct idt_entry idt[IDT_ENTRIES];

static struct idt_ptr idtr;

void idt_set_entry(int vector, void (*isr)(), uint8_t flags)
{
    uint64_t isr_addr = (uint64_t)isr;

    idt[vector].offset_low = isr_addr & 0xFFFF;
    idt[vector].selector = 0x08;       // kernel code segment selector
    idt[vector].ist = 0;
    idt[vector].type_attr = flags;
    idt[vector].offset_mid = (isr_addr >> 16) & 0xFFFF;
    idt[vector].offset_high = (isr_addr >> 32) & 0xFFFFFFFF;
    idt[vector].zero = 0;
}

void idt_load()
{
    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint64_t)&idt;

    __asm__ volatile("lidt %0" : : "m"(idtr));
}
