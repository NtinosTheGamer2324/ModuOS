#ifndef MODUOS_ARCH_AMD64_APIC_H
#define MODUOS_ARCH_AMD64_APIC_H

#include <stdint.h>

#define APIC_REGION_SIZE        4096

/* --- Legacy PIC --- */
#define PIC1_DATA_PORT          0x21
#define PIC2_DATA_PORT          0xA1
#define PIC_MASK_ALL            0xFF

/* --- Local APIC --- */
#define LAPIC_REG_TPR           0x0080
#define LAPIC_REG_EOI           0x00B0
#define LAPIC_REG_SPURIOUS      0x00F0
#define LAPIC_SPURIOUS_ENABLE   0x0100

#define IA32_APIC_BASE_MSR      0x1B
#define IA32_APIC_BASE_MSR_EN   0x0800

/* --- I/O APIC --- */
#define IOAPIC_REG_INDEX        0x00
#define IOAPIC_REG_DATA         0x10
#define IOAPIC_RED_TABLE_BASE   0x10

#define IOAPIC_RED_DEL_FIXED    0x0000
#define IOAPIC_RED_TRIG_EDGE    0x0000
#define IOAPIC_RED_MASK         0x00010000

/* --- Core API --- */
void apic_init_system(void);
void disable_legacy_pic(void);
void apic_map_irq(uint32_t irq, uint8_t idt_vector, uint8_t target_cpu_id);
void apic_unmap_irq(uint32_t irq);

/* --- Internal Blocks --- */
void lapic_init(uint64_t phys_addr);
void lapic_eoi(void);
void ioapic_init(uint64_t phys_addr);
void ioapic_set_pin(uint32_t pin, uint32_t low_flags, uint32_t high_flags);

#endif
