#include "moduos/drivers/power/ACPI.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/io/io.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/macros.h"
#include "moduos/kernel/memory/paging.h"  // Add this for ioremap()

/* Global ACPI tables */
static rsdp_descriptor_t* rsdp = NULL;
static rsdt_t* rsdt = NULL;
static xsdt_t* xsdt = NULL;
static fadt_t* fadt = NULL;
static madt_t* madt = NULL;

/* ACPI enabled flag */
static int acpi_enabled = 0;

/* Identity-mapped region limit (512MB in your case) */
#define IDENTITY_MAP_LIMIT 0x20000000ULL  // 512MB

/* Helper to safely access physical memory beyond identity-mapped region */
static void* map_acpi_memory(uint64_t phys_addr, size_t min_size) {
    /* If in low memory (identity-mapped), use directly */
    if (phys_addr < IDENTITY_MAP_LIMIT && (phys_addr + min_size) <= IDENTITY_MAP_LIMIT) {
        return (void*)(uintptr_t)phys_addr;
    }
    
    /* Otherwise, use ioremap to map it */
    COM_LOG(COM1_PORT, "Mapping high ACPI memory");
    com_printf(COM1_PORT, "[ACPI]:  Mapping ACPI region at 0x%x (size=%d)\n", (uint32_t)phys_addr, (int)min_size);
    void* mapped = ioremap(phys_addr, min_size);
    if (!mapped) {
        COM_LOG_ERROR(COM1_PORT, "Failed to map ACPI memory");
    }
    return mapped;
}

/* Checksum validation for ACPI tables */
static int acpi_checksum(void* ptr, size_t len) {
    if (!ptr || len == 0) return 0;
    uint8_t sum = 0;
    uint8_t* bytes = (uint8_t*)ptr;
    for (size_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + bytes[i]);
    }
    return sum == 0;
}

/* Find RSDP in memory - RSDP is always in low memory so no mapping needed */
static rsdp_descriptor_t* find_rsdp(void) {
    /* Search EBDA (Extended BIOS Data Area) */
    uint16_t ebda_segment = *((volatile uint16_t*)0x40E);
    uint32_t ebda_address = (uint32_t)ebda_segment * 16;

    if (ebda_address && ebda_address < 0x100000) {
        for (uint32_t addr = ebda_address; addr < ebda_address + 1024; addr += 16) {
            rsdp_descriptor_t* candidate = (rsdp_descriptor_t*)(uintptr_t)addr;
            if (memcmp(candidate->signature, "RSD PTR ", 8) == 0) {
                uint32_t len = 20;
                if (candidate->revision >= 2 && candidate->length >= 20) {
                    if (candidate->length > 1024) {
                        continue;
                    }
                    len = candidate->length;
                }

                if (acpi_checksum(candidate, len)) {
                    return candidate;
                }
            }
        }
    }

    /* Search main BIOS area (0xE0000 - 0xFFFFF) */
    for (uint32_t addr = 0xE0000; addr < 0x100000; addr += 16) {
        rsdp_descriptor_t* candidate = (rsdp_descriptor_t*)(uintptr_t)addr;
        if (memcmp(candidate->signature, "RSD PTR ", 8) == 0) {
            uint32_t len = 20;
            if (candidate->revision >= 2 && candidate->length >= 20) {
                if (candidate->length > 1024) {
                    continue;
                }
                len = candidate->length;
            }

            if (acpi_checksum(candidate, len)) {
                return candidate;
            }
        }
    }

    return NULL;
}

/* Find a specific ACPI table */
static void* find_acpi_table(const char* signature) {
    if (!rsdt && !xsdt) return NULL;

    if (xsdt) {
        uint32_t entries = 0;
        if (xsdt->header.length > sizeof(acpi_sdt_header_t)) {
            entries = (xsdt->header.length - sizeof(acpi_sdt_header_t)) / 8;
        }
        for (uint32_t i = 0; i < entries; i++) {
            uint64_t entry_addr = xsdt->entries[i];
            if (entry_addr == 0) continue;
            
            /* Map the table header first to read its length */
            acpi_sdt_header_t* header = (acpi_sdt_header_t*)map_acpi_memory(entry_addr, sizeof(acpi_sdt_header_t));
            if (!header) continue;
            
            if (memcmp(header->signature, signature, 4) == 0) {
                /* Found it - now map the full table */
                void* full_table = map_acpi_memory(entry_addr, header->length);
                if (full_table && acpi_checksum(full_table, header->length)) {
                    return full_table;
                }
            }
        }
    } else if (rsdt) {
        uint32_t entries = 0;
        if (rsdt->header.length > sizeof(acpi_sdt_header_t)) {
            entries = (rsdt->header.length - sizeof(acpi_sdt_header_t)) / 4;
        }
        for (uint32_t i = 0; i < entries; i++) {
            uint32_t entry_addr = rsdt->entries[i];
            if (entry_addr == 0) continue;
            
            /* Map the table header first to read its length */
            acpi_sdt_header_t* header = (acpi_sdt_header_t*)map_acpi_memory(entry_addr, sizeof(acpi_sdt_header_t));
            if (!header) continue;
            
            if (memcmp(header->signature, signature, 4) == 0) {
                /* Found it - now map the full table */
                void* full_table = map_acpi_memory(entry_addr, header->length);
                if (full_table && acpi_checksum(full_table, header->length)) {
                    return full_table;
                }
            }
        }
    }

    return NULL;
}

/* Initialize ACPI */
int acpi_init(void) {
    COM_LOG(COM1_PORT, "Initializing ACPI...");

    /* Find RSDP (always in low memory) */
    rsdp = find_rsdp();
    if (!rsdp) {
        COM_LOG_ERROR(COM1_PORT, "RSDP not found");
        return -1;
    }

    COM_LOG_OK(COM1_PORT, "RSDP found");
    com_printf(COM1_PORT, "  ACPI Revision: %d\n", rsdp->revision);

    /* Get RSDT or XSDT - these might be in high memory! */
    if (rsdp->revision >= 2 && rsdp->xsdt_address) {
        com_printf(COM1_PORT, "  XSDT at physical address: 0x%x\n", (uint32_t)rsdp->xsdt_address);
        
        /* Map XSDT header first to get its size */
        xsdt_t* xsdt_header = (xsdt_t*)map_acpi_memory(rsdp->xsdt_address, sizeof(acpi_sdt_header_t));
        if (xsdt_header && xsdt_header->header.length > 0) {
            /* Now map the full XSDT */
            xsdt = (xsdt_t*)map_acpi_memory(rsdp->xsdt_address, xsdt_header->header.length);
            if (xsdt && acpi_checksum(xsdt, xsdt->header.length)) {
                COM_LOG_OK(COM1_PORT, "XSDT found and validated");
            } else {
                COM_LOG_ERROR(COM1_PORT, "XSDT checksum failed or invalid");
                xsdt = NULL;
            }
        } else {
            COM_LOG_ERROR(COM1_PORT, "Failed to map XSDT");
            xsdt = NULL;
        }
    }

    if (!xsdt) {
        com_printf(COM1_PORT, "  RSDT at physical address: 0x%x\n", rsdp->rsdt_address);
        
        /* Map RSDT header first to get its size */
        rsdt_t* rsdt_header = (rsdt_t*)map_acpi_memory(rsdp->rsdt_address, sizeof(acpi_sdt_header_t));
        if (rsdt_header && rsdt_header->header.length > 0) {
            /* Now map the full RSDT */
            rsdt = (rsdt_t*)map_acpi_memory(rsdp->rsdt_address, rsdt_header->header.length);
            if (rsdt && acpi_checksum(rsdt, rsdt->header.length)) {
                COM_LOG_OK(COM1_PORT, "RSDT found and validated");
            } else {
                COM_LOG_ERROR(COM1_PORT, "RSDT checksum failed or invalid");
                return -1;
            }
        } else {
            COM_LOG_ERROR(COM1_PORT, "Failed to map RSDT");
            return -1;
        }
    }

    /* Find FADT (Fixed ACPI Description Table) */
    fadt = (fadt_t*)find_acpi_table("FACP");
    if (fadt) {
        COM_LOG_OK(COM1_PORT, "FADT found");
        /* Print FADT reset register info for debugging */
        if (fadt->header.length >= 129) {
            com_printf(COM1_PORT, "  FADT Length: %d, Reset Reg: 0x%x, Space: %d, Value: 0x%x\n",
                       fadt->header.length,
                       (uint32_t)fadt->reset_reg.address,
                       fadt->reset_reg.address_space,
                       fadt->reset_value);
        } else {
            com_printf(COM1_PORT, "  FADT Length: %d (too short for reset register)\n", fadt->header.length);
        }
    } else {
        COM_LOG_WARN(COM1_PORT, "FADT not found");
    }

    /* Find MADT (Multiple APIC Description Table) */
    madt = (madt_t*)find_acpi_table("APIC");
    if (madt) {
        COM_LOG_OK(COM1_PORT, "MADT found");
    } else {
        COM_LOG_WARN(COM1_PORT, "MADT not found");
    }

    acpi_enabled = 1;
    return 0;
}

/* Check if ACPI is available */
int acpi_is_available(void) {
    return acpi_enabled;
}

/* Get FADT */
fadt_t* acpi_get_fadt(void) {
    return fadt;
}

/* Get MADT */
madt_t* acpi_get_madt(void) {
    return madt;
}

/* ACPI power management - shutdown */
void acpi_shutdown(void) {
    if (!acpi_enabled) {
        COM_LOG_ERROR(COM1_PORT, "ACPI not enabled");
        return;
    }
    
    if (!fadt) {
        COM_LOG_ERROR(COM1_PORT, "FADT not available, cannot shutdown via ACPI");
        return;
    }

    /* Prefer X PM1 control blocks if present */
    uint32_t pm1a = 0;
    uint32_t pm1b = 0;

    /* Use 32-bit IO/Memory fields first, fall back to X_* GAS if available */
    if (fadt->pm1a_control_block) pm1a = fadt->pm1a_control_block;
    if (fadt->pm1b_control_block) pm1b = fadt->pm1b_control_block;

    if (!pm1a && fadt->header.length >= sizeof(fadt_t)) {
        /* Try the extended GAS fields (x_pm1a_control_block) if present */
        if (fadt->x_pm1a_control_block.address && fadt->x_pm1a_control_block.address_space == 1) {
            pm1a = (uint32_t)fadt->x_pm1a_control_block.address;
        }
        if (fadt->x_pm1b_control_block.address && fadt->x_pm1b_control_block.address_space == 1) {
            pm1b = (uint32_t)fadt->x_pm1b_control_block.address;
        }
    }

    if (!pm1a) {
        COM_LOG_ERROR(COM1_PORT, "PM1a Control Block not available");
        return;
    }

    COM_LOG(COM1_PORT, "Initiating ACPI shutdown...");

    /* For most systems, writing to PM1a with SLP_EN set triggers shutdown */
    /* This is a simplified approach - real implementation would parse DSDT */
    uint16_t slp_typa = 5;  /* Common value for S5 (soft off) */
    uint16_t pm1a_value = (slp_typa << 10) | (1 << 13);  /* SLP_TYP | SLP_EN */

    outw((uint16_t)pm1a, pm1a_value);

    if (pm1b) {
        outw((uint16_t)pm1b, pm1a_value);
    }

    /* If we reach here, ACPI shutdown failed */
    COM_LOG_ERROR(COM1_PORT, "ACPI shutdown failed");
}

/* ACPI reboot */
void acpi_reboot(void) {
    COM_LOG(COM1_PORT, "=== ACPI REBOOT REQUESTED ===");
    
    /* Try ACPI reset register first (most reliable if available) */
    if (acpi_enabled && fadt) {
        COM_LOG(COM1_PORT, "FADT available, checking reset register...");
        
        /* Check if reset register is available (ACPI 2.0+) */
        if (fadt->header.length >= 129) {
            COM_LOG(COM1_PORT, "FADT has reset register fields");
            VGA_Writef("Reset reg address: 0x%x\n", (uint32_t)fadt->reset_reg.address);
            VGA_Writef("Reset reg address_space: %d\n", fadt->reset_reg.address_space);
            VGA_Writef("Reset value: 0x%x\n", fadt->reset_value);
            
            if (fadt->reset_reg.address != 0) {
                COM_LOG(COM1_PORT, "Using ACPI Reset Register");
                uint8_t reset_value = fadt->reset_value;

                switch (fadt->reset_reg.address_space) {
                    case 0: /* System Memory */
                        COM_LOG(COM1_PORT, "Writing to system memory reset register");
                        {
                            volatile uint8_t* reset_addr = (volatile uint8_t*)map_acpi_memory(
                                fadt->reset_reg.address, 1);
                            if (reset_addr) {
                                *reset_addr = reset_value;
                            }
                        }
                        break;
                        
                    case 1: /* System I/O */
                        COM_LOG(COM1_PORT, "Writing to I/O port reset register");
                        VGA_Writef("I/O Port: 0x%x, Value: 0x%x\n", 
                                   (uint16_t)fadt->reset_reg.address, reset_value);
                        outb((uint16_t)fadt->reset_reg.address, reset_value);
                        break;
                        
                    case 2: /* PCI Configuration Space */
                        COM_LOG_WARN(COM1_PORT, "PCI reset not implemented");
                        goto fallback_reboot;
                        
                    default:
                        COM_LOG_WARN(COM1_PORT, "Unknown reset register address space");
                        VGA_Writef("Unknown address space: %d\n", fadt->reset_reg.address_space);
                        goto fallback_reboot;
                }

                /* Wait for reboot to occur */
                COM_LOG(COM1_PORT, "Waiting for ACPI reboot (5 second timeout)...");
                for (volatile int i = 0; i < 50000000; i++) {
                    __asm__ volatile("pause");
                }

                /* If we reach here, ACPI reboot failed */
                COM_LOG_WARN(COM1_PORT, "ACPI reboot timeout");
            } else {
                COM_LOG_WARN(COM1_PORT, "Reset register address is 0 (not supported)");
                VGA_Write("ACPI Reset Register not supported by firmware\n");
            }
        } else {
            COM_LOG_WARN(COM1_PORT, "FADT too short for reset register");
            VGA_Writef("FADT length=%d, need >=129\n", fadt->header.length);
        }
    } else {
        COM_LOG_WARN(COM1_PORT, "ACPI not available or FADT not found");
    }

fallback_reboot:
    /* Fallback 1: Try keyboard controller reset (8042) */
    COM_LOG(COM1_PORT, "=== FALLBACK: Keyboard controller reset ===");
    VGA_Write("Attempting keyboard controller reset...\n");
    
    /* Disable interrupts */
    __asm__ volatile("cli");
    
    /* Wait for keyboard controller to be ready */
    int timeout = 100000;
    while (timeout-- > 0) {
        if ((inb(0x64) & 0x02) == 0) break;
        __asm__ volatile("pause");
    }
    
    if (timeout <= 0) {
        COM_LOG_WARN(COM1_PORT, "Keyboard controller not ready for reset");
        VGA_Write("Keyboard controller timeout\n");
    } else {
        COM_LOG(COM1_PORT, "Sending keyboard controller reset command (0xFE)...");
        /* Send reset command */
        outb(0x64, 0xFE);
        
        /* Wait a bit */
        for (volatile int i = 0; i < 10000000; i++) {
            __asm__ volatile("pause");
        }
        COM_LOG_WARN(COM1_PORT, "Keyboard controller reset failed");
        VGA_Write("Keyboard controller reset failed\n");
    }

    /* Fallback 2: Triple fault method */
    COM_LOG(COM1_PORT, "=== FALLBACK: Triple fault method ===");
    VGA_Write("Attempting triple fault reset...\n");
    
    /* Load invalid IDT to cause triple fault */
    struct {
        uint16_t limit;
        uint32_t base;
    } __attribute__((packed)) idtr = {0, 0};
    
    __asm__ volatile(
        "lidt %0\n"
        "int $0x03\n"
        : : "m"(idtr)
    );

    /* Should never reach here */
    COM_LOG_ERROR(COM1_PORT, "ALL REBOOT METHODS FAILED!");
    VGA_Write("\\cr=== REBOOT FAILED - SYSTEM HALTED ===\\rr\n");
    while(1) {
        __asm__ volatile("hlt");
    }
}

/* Parse MADT entries */
void acpi_parse_madt(void) {
    if (!madt) {
        COM_LOG_ERROR(COM1_PORT, "MADT not available");
        return;
    }

    VGA_Write("\\cg===============================================================\\rr\n");
    VGA_Write("\\cg                    MADT (APIC) INFORMATION                  \\rr\n");
    VGA_Write("\\cg===============================================================\\rr\n\n");

    VGA_Writef("Local APIC Address: 0x%x\n", madt->local_apic_address);
    VGA_Writef("Flags: 0x%x\n\n", madt->flags);

    uint8_t* ptr = (uint8_t*)madt->entries;
    uint8_t* end = (uint8_t*)madt + madt->header.length;

    int cpu_count = 0;
    int ioapic_count = 0;

    while (ptr + sizeof(madt_entry_header_t) <= end) {
        madt_entry_header_t* entry = (madt_entry_header_t*)ptr;
        if (entry->length < sizeof(madt_entry_header_t)) {
            break;
        }
        if (ptr + entry->length > end) {
            break;
        }

        switch (entry->type) {
            case 0: /* Processor Local APIC */
            {
                if (entry->length < sizeof(madt_local_apic_t)) break;
                madt_local_apic_t* lapic = (madt_local_apic_t*)entry;
                if (lapic->flags & 1) {
                    VGA_Writef("\\cgCPU #%d:\\rr APIC ID=%d, Flags=0x%x\n",
                               lapic->processor_id, lapic->apic_id, lapic->flags);
                    cpu_count++;
                }
            }
            break;

            case 1: /* I/O APIC */
            {
                if (entry->length < sizeof(madt_ioapic_t)) break;
                madt_ioapic_t* ioapic = (madt_ioapic_t*)entry;
                VGA_Writef("\\cyI/O APIC #%d:\\rr Address=0x%x, GSI Base=%d\n",
                           ioapic->ioapic_id, ioapic->ioapic_address,
                           ioapic->global_system_interrupt_base);
                ioapic_count++;
            }
            break;

            case 2: /* Interrupt Source Override */
            {
                if (entry->length < sizeof(madt_interrupt_override_t)) break;
                madt_interrupt_override_t* iso = (madt_interrupt_override_t*)entry;
                VGA_Writef("\\cbISO:\\rr Bus=%d, IRQ=%d, GSI=%d, Flags=0x%x\n",
                           iso->bus_source, iso->irq_source,
                           iso->global_system_interrupt, iso->flags);
            }
            break;

            default:
                break;
        }

        ptr += entry->length;
    }

    VGA_Writef("\n\\cgTotal CPUs: %d\\rr\n", cpu_count);
    VGA_Writef("\\cgTotal I/O APICs: %d\\rr\n\n", ioapic_count);
}

/* List all ACPI tables */
void acpi_list_tables(void) {
    if (!rsdt && !xsdt) {
        COM_LOG_ERROR(COM1_PORT, "No ACPI tables available");
        return;
    }

    VGA_Write("\\cg===============================================================\\rr\n");
    VGA_Write("\\cg                    ACPI TABLES                              \\rr\n");
    VGA_Write("\\cg===============================================================\\rr\n\n");

    if (xsdt) {
        uint32_t entries = 0;
        if (xsdt->header.length > sizeof(acpi_sdt_header_t))
            entries = (xsdt->header.length - sizeof(acpi_sdt_header_t)) / 8;
        VGA_Writef("Using XSDT with %d entries:\n\n", entries);

        for (uint32_t i = 0; i < entries; i++) {
            uint64_t entry_addr = xsdt->entries[i];
            if (!entry_addr) continue;
            acpi_sdt_header_t* header = (acpi_sdt_header_t*)map_acpi_memory(entry_addr, sizeof(acpi_sdt_header_t));
            if (!header) continue;
            char sig[5] = {0};
            memcpy(sig, header->signature, 4);
            VGA_Writef("  [%d] \\cy%s\\rr - Length: %d bytes, Revision: %d\n",
                       i, sig, header->length, header->revision);
        }
    } else if (rsdt) {
        uint32_t entries = 0;
        if (rsdt->header.length > sizeof(acpi_sdt_header_t))
            entries = (rsdt->header.length - sizeof(acpi_sdt_header_t)) / 4;
        VGA_Writef("Using RSDT with %d entries:\n\n", entries);

        for (uint32_t i = 0; i < entries; i++) {
            uint32_t entry_addr = rsdt->entries[i];
            if (!entry_addr) continue;
            acpi_sdt_header_t* header = (acpi_sdt_header_t*)map_acpi_memory(entry_addr, sizeof(acpi_sdt_header_t));
            if (!header) continue;
            char sig[5] = {0};
            memcpy(sig, header->signature, 4);
            VGA_Writef("  [%d] \\cy%s\\rr - Length: %d bytes, Revision: %d\n",
                       i, sig, header->length, header->revision);
        }
    }

    VGA_Write("\n");
}

/* Get ACPI info for display */
void acpi_print_info(void) {
    if (!acpi_enabled) {
        VGA_Write("\\crACPI not initialized\\rr\n");
        return;
    }

    VGA_Write("\\cg===============================================================\\rr\n");
    VGA_Write("\\cg                    ACPI INFORMATION                         \\rr\n");
    VGA_Write("\\cg===============================================================\\rr\n\n");

    if (rsdp) {
        char oemid[7] = {0};
        memcpy(oemid, rsdp->oemid, 6);
        VGA_Writef("\\cyRSDP:\\rr\n");
        VGA_Writef("  OEM ID: %s\n", oemid);
        VGA_Writef("  Revision: %d\n", rsdp->revision);
        VGA_Writef("  RSDT Address: 0x%x\n", rsdp->rsdt_address);
        if (rsdp->revision >= 2) {
            VGA_Writef("  XSDT Address: 0x%x\n", (uint32_t)rsdp->xsdt_address);
        }
        VGA_Write("\n");
    }

    if (fadt) {
        VGA_Write("\\cyFADT:\\rr\n");
        VGA_Writef("  PM1a Control Block: 0x%x\n", fadt->pm1a_control_block);
        VGA_Writef("  PM1b Control Block: 0x%x\n", fadt->pm1b_control_block);
        VGA_Writef("  PM Timer Block: 0x%x\n", fadt->pm_timer_block);

        if (fadt->header.length >= 129) {
            VGA_Writef("  Reset Register: 0x%x (Space: %d)\n",
                       (uint32_t)fadt->reset_reg.address,
                       fadt->reset_reg.address_space);
            VGA_Writef("  Reset Value: 0x%x\n", fadt->reset_value);
        }
        VGA_Write("\n");
    }

    if (madt) {
        VGA_Write("\\cyMADT:\\rr Found (use 'acpi madt' for details)\n\n");
    }
}