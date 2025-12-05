// idt.h
#ifndef IDT_H
#define IDT_H

#include <stdint.h>

#define IDT_ENTRIES 256

struct idt_entry {
    uint16_t offset_low;    // bits 0-15 of handler address
    uint16_t selector;      // code segment selector
    uint8_t  ist;           // interrupt stack table offset (0 for now)
    uint8_t  type_attr;     // type and attributes
    uint16_t offset_mid;    // bits 16-31 of handler address
    uint32_t offset_high;   // bits 32-63 of handler address
    uint32_t zero;          // reserved
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

extern struct idt_entry idt[IDT_ENTRIES];

void idt_set_entry(int vector, void (*isr)(), uint8_t flags);
void idt_load();

#endif
