#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>
#include <stddef.h>

/* RSDP (Root System Description Pointer) */
typedef struct {
    char signature[8];          /* "RSD PTR " */
    uint8_t checksum;
    char oemid[6];
    uint8_t revision;
    uint32_t rsdt_address;
    
    /* ACPI 2.0+ fields */
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed)) rsdp_descriptor_t;

/* ACPI SDT (System Description Table) Header */
typedef struct {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_header_t;

/* RSDT (Root System Description Table) */
typedef struct {
    acpi_sdt_header_t header;
    uint32_t entries[];
} __attribute__((packed)) rsdt_t;

/* XSDT (Extended System Description Table) */
typedef struct {
    acpi_sdt_header_t header;
    uint64_t entries[];
} __attribute__((packed)) xsdt_t;

/* Generic Address Structure */
typedef struct {
    uint8_t address_space;      /* 0=System Memory, 1=System I/O */
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
} __attribute__((packed)) acpi_gas_t;

/* FADT (Fixed ACPI Description Table) */
typedef struct {
    acpi_sdt_header_t header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    
    uint8_t reserved;
    uint8_t preferred_pm_profile;
    uint16_t sci_interrupt;
    uint32_t smi_command_port;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_req;
    uint8_t pstate_control;
    
    uint32_t pm1a_event_block;
    uint32_t pm1b_event_block;
    uint32_t pm1a_control_block;
    uint32_t pm1b_control_block;
    uint32_t pm2_control_block;
    uint32_t pm_timer_block;
    uint32_t gpe0_block;
    uint32_t gpe1_block;
    
    uint8_t pm1_event_length;
    uint8_t pm1_control_length;
    uint8_t pm2_control_length;
    uint8_t pm_timer_length;
    uint8_t gpe0_length;
    uint8_t gpe1_length;
    uint8_t gpe1_base;
    uint8_t cstate_control;
    
    uint16_t worst_c2_latency;
    uint16_t worst_c3_latency;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t day_alarm;
    uint8_t month_alarm;
    uint8_t century;
    
    uint16_t boot_arch_flags;
    uint8_t reserved2;
    uint32_t flags;
    
    /* ACPI 2.0+ fields */
    acpi_gas_t reset_reg;
    uint8_t reset_value;
    uint8_t reserved3[3];
    
    uint64_t x_firmware_control;
    uint64_t x_dsdt;
    
    acpi_gas_t x_pm1a_event_block;
    acpi_gas_t x_pm1b_event_block;
    acpi_gas_t x_pm1a_control_block;
    acpi_gas_t x_pm1b_control_block;
    acpi_gas_t x_pm2_control_block;
    acpi_gas_t x_pm_timer_block;
    acpi_gas_t x_gpe0_block;
    acpi_gas_t x_gpe1_block;
} __attribute__((packed)) fadt_t;

/* MADT (Multiple APIC Description Table) Entry Header */
typedef struct {
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) madt_entry_header_t;

/* MADT Entry Type 0: Processor Local APIC */
typedef struct {
    madt_entry_header_t header;
    uint8_t processor_id;
    uint8_t apic_id;
    uint32_t flags;
} __attribute__((packed)) madt_local_apic_t;

/* MADT Entry Type 1: I/O APIC */
typedef struct {
    madt_entry_header_t header;
    uint8_t ioapic_id;
    uint8_t reserved;
    uint32_t ioapic_address;
    uint32_t global_system_interrupt_base;
} __attribute__((packed)) madt_ioapic_t;

/* MADT Entry Type 2: Interrupt Source Override */
typedef struct {
    madt_entry_header_t header;
    uint8_t bus_source;
    uint8_t irq_source;
    uint32_t global_system_interrupt;
    uint16_t flags;
} __attribute__((packed)) madt_interrupt_override_t;

/* MADT (Multiple APIC Description Table) */
typedef struct {
    acpi_sdt_header_t header;
    uint32_t local_apic_address;
    uint32_t flags;
    uint8_t entries[];
} __attribute__((packed)) madt_t;

/* ACPI function declarations */

/* Initialize ACPI subsystem */
int acpi_init(void);

/* Check if ACPI is available */
int acpi_is_available(void);

/* Get ACPI tables */
fadt_t* acpi_get_fadt(void);
madt_t* acpi_get_madt(void);

/* Power management */
void acpi_shutdown(void);
void acpi_reboot(void);

/* Information and debugging */
void acpi_print_info(void);
void acpi_list_tables(void);
void acpi_parse_madt(void);
void acpi_print_info(void);

#endif /* ACPI_H */