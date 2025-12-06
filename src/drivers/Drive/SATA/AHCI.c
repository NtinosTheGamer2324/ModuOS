#include "moduos/drivers/Drive/SATA/AHCI.h"
#include "moduos/drivers/PCI/pci.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/macros.h"
#include <stdint.h>
#include <stddef.h>

static ahci_controller_t ahci_controller;

// Memory allocation helper for DMA buffers
static void* ahci_alloc_dma(size_t size) {
    // Allocate aligned memory for DMA
    // In a real implementation, you'd want physically contiguous memory
    void *ptr = kmalloc(size);
    if (ptr) {
        // Zero the memory
        uint8_t *p = (uint8_t*)ptr;
        for (size_t i = 0; i < size; i++) {
            p[i] = 0;
        }
    }
    return ptr;
}

// ===========================================================================
// Port Management Functions
// ===========================================================================

void ahci_stop_cmd(hba_port_t *port) {
    // Clear ST (bit 0)
    port->cmd &= ~HBA_PxCMD_ST;
    
    // Clear FRE (bit 4)
    port->cmd &= ~HBA_PxCMD_FRE;
    
    // Wait until FR (bit 14), CR (bit 15) are cleared
    while (1) {
        if (port->cmd & HBA_PxCMD_FR)
            continue;
        if (port->cmd & HBA_PxCMD_CR)
            continue;
        break;
    }
}

void ahci_start_cmd(hba_port_t *port) {
    // Wait until CR (bit 15) is cleared
    while (port->cmd & HBA_PxCMD_CR);
    
    // Set FRE (bit 4) and ST (bit 0)
    port->cmd |= HBA_PxCMD_FRE;
    port->cmd |= HBA_PxCMD_ST;
}

int ahci_port_rebase(hba_port_t *port, int portno) {
    ahci_stop_cmd(port);
    
    // Allocate command list (1K aligned)
    void *clb = ahci_alloc_dma(1024);
    if (!clb) {
        COM_LOG_ERROR(COM1_PORT, "Failed to allocate command list");
        return -1;
    }
    
    // Get physical address for DMA
    uint64_t clb_phys = paging_virt_to_phys((uint64_t)clb);
    
    port->clb = (uint32_t)clb_phys;
    port->clbu = (uint32_t)(clb_phys >> 32);
    
    ahci_controller.ports[portno].cmd_list = (hba_cmd_header_t*)clb;
    
    // Allocate FIS (256 byte aligned)
    void *fb = ahci_alloc_dma(256);
    if (!fb) {
        COM_LOG_ERROR(COM1_PORT, "Failed to allocate FIS");
        return -1;
    }
    
    uint64_t fb_phys = paging_virt_to_phys((uint64_t)fb);
    
    port->fb = (uint32_t)fb_phys;
    port->fbu = (uint32_t)(fb_phys >> 32);
    
    ahci_controller.ports[portno].fis = (hba_fis_t*)fb;
    
    // Allocate command tables (256 bytes each, 32 slots)
    hba_cmd_header_t *cmdheader = (hba_cmd_header_t*)clb;
    for (int i = 0; i < 32; i++) {
        cmdheader[i].prdtl = 8;  // 8 PRDT entries per command table
        
        // Allocate command table (256 + 8*16 bytes for PRDT entries)
        void *ctba = ahci_alloc_dma(256 + 8 * 16);
        if (!ctba) {
            COM_LOG_ERROR(COM1_PORT, "Failed to allocate command table");
            return -1;
        }
        
        uint64_t ctba_phys = paging_virt_to_phys((uint64_t)ctba);
        
        cmdheader[i].ctba = (uint32_t)ctba_phys;
        cmdheader[i].ctbau = (uint32_t)(ctba_phys >> 32);
        
        ahci_controller.ports[portno].cmd_tables[i] = (hba_cmd_table_t*)ctba;
    }
    
    ahci_start_cmd(port);
    return 0;
}

int ahci_find_cmdslot(hba_port_t *port) {
    // Find a free command slot
    uint32_t slots = (port->sact | port->ci);
    for (int i = 0; i < 32; i++) {
        if ((slots & 1) == 0) {
            return i;
        }
        slots >>= 1;
    }
    return -1;
}

// ===========================================================================
// Device Type Detection
// ===========================================================================

ahci_device_type_t ahci_check_type(hba_port_t *port) {
    uint32_t ssts = port->ssts;
    
    uint8_t ipm = (ssts >> 8) & 0x0F;
    uint8_t det = ssts & 0x0F;
    
    // Check if device is present
    if (det != HBA_PxSSTS_DET_PRESENT)
        return AHCI_DEV_NULL;
    if (ipm != 0x01)
        return AHCI_DEV_NULL;
    
    // Check signature
    switch (port->sig) {
        case SATA_SIG_ATAPI:
            return AHCI_DEV_SATAPI;
        case SATA_SIG_SEMB:
            return AHCI_DEV_SEMB;
        case SATA_SIG_PM:
            return AHCI_DEV_PM;
        default:
            return AHCI_DEV_SATA;
    }
}

const char* ahci_get_device_type_string(ahci_device_type_t type) {
    switch (type) {
        case AHCI_DEV_SATA: return "SATA";
        case AHCI_DEV_SATAPI: return "SATAPI";
        case AHCI_DEV_SEMB: return "SEMB";
        case AHCI_DEV_PM: return "PM";
        default: return "NULL";
    }
}

// ===========================================================================
// Device Identification
// ===========================================================================

int ahci_identify_device(uint8_t port_num) {
    if (port_num >= AHCI_MAX_PORTS)
        return -1;
    
    ahci_port_info_t *port_info = &ahci_controller.ports[port_num];
    hba_port_t *port = port_info->port;
    
    if (!port || port_info->type == AHCI_DEV_NULL)
        return -1;
    
    // Clear pending interrupts
    port->is = (uint32_t)-1;
    
    int slot = ahci_find_cmdslot(port);
    if (slot == -1) {
        COM_LOG_ERROR(COM1_PORT, "No free command slot");
        return -1;
    }
    
    hba_cmd_header_t *cmdheader = &port_info->cmd_list[slot];
    cmdheader->cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmdheader->w = 0;  // Read from device
    cmdheader->prdtl = 1;
    
    hba_cmd_table_t *cmdtbl = port_info->cmd_tables[slot];
    
    // Allocate buffer for IDENTIFY data (512 bytes)
    uint16_t *identify_buf = (uint16_t*)ahci_alloc_dma(512);
    if (!identify_buf) {
        COM_LOG_ERROR(COM1_PORT, "Failed to allocate identify buffer");
        return -1;
    }
    
    uint64_t identify_phys = paging_virt_to_phys((uint64_t)identify_buf);
    
    // Setup PRDT
    cmdtbl->prdt_entry[0].dba = (uint32_t)identify_phys;
    cmdtbl->prdt_entry[0].dbau = (uint32_t)(identify_phys >> 32);
    cmdtbl->prdt_entry[0].dbc = 511;  // 512 bytes (0-based)
    cmdtbl->prdt_entry[0].i = 1;
    
    // Setup command FIS
    fis_reg_h2d_t *cmdfis = (fis_reg_h2d_t*)(&cmdtbl->cfis);
    
    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->c = 1;  // Command
    cmdfis->command = ATA_CMD_IDENTIFY;
    
    cmdfis->lba0 = 0;
    cmdfis->lba1 = 0;
    cmdfis->lba2 = 0;
    cmdfis->device = 0;
    cmdfis->lba3 = 0;
    cmdfis->lba4 = 0;
    cmdfis->lba5 = 0;
    cmdfis->countl = 0;
    cmdfis->counth = 0;
    
    // Wait for port to be ready
    int spin = 0;
    while ((port->tfd & (0x80 | 0x08)) && spin < 1000000) {
        spin++;
    }
    
    if (spin == 1000000) {
        COM_LOG_ERROR(COM1_PORT, "Port hung");
        kfree(identify_buf);
        return -1;
    }
    
    // Issue command
    port->ci = 1 << slot;
    
    // Wait for completion
    while (1) {
        if ((port->ci & (1 << slot)) == 0)
            break;
        if (port->is & HBA_PxIS_TFES) {
            COM_LOG_ERROR(COM1_PORT, "IDENTIFY command failed");
            kfree(identify_buf);
            return -1;
        }
    }
    
    // Parse IDENTIFY data
    // Model number (words 27-46)
    for (int i = 0; i < 20; i++) {
        uint16_t word = identify_buf[27 + i];
        port_info->model[i * 2] = (word >> 8) & 0xFF;
        port_info->model[i * 2 + 1] = word & 0xFF;
    }
    port_info->model[40] = '\0';
    
    // Serial number (words 10-19)
    for (int i = 0; i < 10; i++) {
        uint16_t word = identify_buf[10 + i];
        port_info->serial[i * 2] = (word >> 8) & 0xFF;
        port_info->serial[i * 2 + 1] = word & 0xFF;
    }
    port_info->serial[20] = '\0';
    
    // Sector count (words 60-61 for 28-bit, 100-103 for 48-bit)
    if (identify_buf[83] & (1 << 10)) {
        // 48-bit addressing
        port_info->sector_count = 
            ((uint64_t)identify_buf[100]) |
            ((uint64_t)identify_buf[101] << 16) |
            ((uint64_t)identify_buf[102] << 32) |
            ((uint64_t)identify_buf[103] << 48);
    } else {
        // 28-bit addressing
        port_info->sector_count = 
            ((uint32_t)identify_buf[60]) |
            ((uint32_t)identify_buf[61] << 16);
    }
    
    port_info->sector_size = 512;  // Standard sector size
    
    kfree(identify_buf);
    return 0;
}

// ===========================================================================
// Read/Write Operations
// ===========================================================================

int ahci_read_sectors(uint8_t port_num, uint64_t start_lba, uint32_t count, void *buffer) {
    if (port_num >= AHCI_MAX_PORTS || count == 0 || !buffer)
        return -1;
    
    ahci_port_info_t *port_info = &ahci_controller.ports[port_num];
    hba_port_t *port = port_info->port;
    
    if (!port || port_info->type != AHCI_DEV_SATA)
        return -1;
    
    // Clear pending interrupts
    port->is = (uint32_t)-1;
    
    int slot = ahci_find_cmdslot(port);
    if (slot == -1) {
        COM_LOG_ERROR(COM1_PORT, "No free command slot");
        return -1;
    }
    
    hba_cmd_header_t *cmdheader = &port_info->cmd_list[slot];
    cmdheader->cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmdheader->w = 0;  // Read from device
    cmdheader->prdtl = (uint16_t)((count - 1) / 16 + 1);  // PRDT entries
    
    hba_cmd_table_t *cmdtbl = port_info->cmd_tables[slot];
    
    // Setup PRDT entries
    uint32_t bytes_remaining = count * 512;
    uint8_t *buf_ptr = (uint8_t*)buffer;
    
    for (int i = 0; i < cmdheader->prdtl - 1; i++) {
        uint64_t buf_phys = paging_virt_to_phys((uint64_t)buf_ptr);
        cmdtbl->prdt_entry[i].dba = (uint32_t)buf_phys;
        cmdtbl->prdt_entry[i].dbau = (uint32_t)(buf_phys >> 32);
        cmdtbl->prdt_entry[i].dbc = 8192 - 1;  // 8K bytes (0-based)
        cmdtbl->prdt_entry[i].i = 0;
        buf_ptr += 8192;
        bytes_remaining -= 8192;
    }
    
    // Last entry
    uint64_t buf_phys = paging_virt_to_phys((uint64_t)buf_ptr);
    cmdtbl->prdt_entry[cmdheader->prdtl - 1].dba = (uint32_t)buf_phys;
    cmdtbl->prdt_entry[cmdheader->prdtl - 1].dbau = (uint32_t)(buf_phys >> 32);
    cmdtbl->prdt_entry[cmdheader->prdtl - 1].dbc = bytes_remaining - 1;
    cmdtbl->prdt_entry[cmdheader->prdtl - 1].i = 1;  // Interrupt on completion
    
    // Setup command FIS
    fis_reg_h2d_t *cmdfis = (fis_reg_h2d_t*)(&cmdtbl->cfis);
    
    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->c = 1;  // Command
    cmdfis->command = ATA_CMD_READ_DMA_EX;
    
    cmdfis->lba0 = (uint8_t)start_lba;
    cmdfis->lba1 = (uint8_t)(start_lba >> 8);
    cmdfis->lba2 = (uint8_t)(start_lba >> 16);
    cmdfis->device = 1 << 6;  // LBA mode
    
    cmdfis->lba3 = (uint8_t)(start_lba >> 24);
    cmdfis->lba4 = (uint8_t)(start_lba >> 32);
    cmdfis->lba5 = (uint8_t)(start_lba >> 40);
    
    cmdfis->countl = (uint8_t)count;
    cmdfis->counth = (uint8_t)(count >> 8);
    
    // Wait for port to be ready
    int spin = 0;
    while ((port->tfd & (0x80 | 0x08)) && spin < 1000000) {
        spin++;
    }
    
    if (spin == 1000000) {
        COM_LOG_ERROR(COM1_PORT, "Port hung");
        return -1;
    }
    
    // Issue command
    port->ci = 1 << slot;
    
    // Wait for completion
    while (1) {
        if ((port->ci & (1 << slot)) == 0)
            break;
        if (port->is & HBA_PxIS_TFES) {
            COM_LOG_ERROR(COM1_PORT, "Read command failed");
            return -1;
        }
    }
    
    return 0;
}

int ahci_write_sectors(uint8_t port_num, uint64_t start_lba, uint32_t count, const void *buffer) {
    if (port_num >= AHCI_MAX_PORTS || count == 0 || !buffer)
        return -1;
    
    ahci_port_info_t *port_info = &ahci_controller.ports[port_num];
    hba_port_t *port = port_info->port;
    
    if (!port || port_info->type != AHCI_DEV_SATA)
        return -1;
    
    // Clear pending interrupts
    port->is = (uint32_t)-1;
    
    int slot = ahci_find_cmdslot(port);
    if (slot == -1) {
        COM_LOG_ERROR(COM1_PORT, "No free command slot");
        return -1;
    }
    
    hba_cmd_header_t *cmdheader = &port_info->cmd_list[slot];
    cmdheader->cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmdheader->w = 1;  // Write to device
    cmdheader->prdtl = (uint16_t)((count - 1) / 16 + 1);
    
    hba_cmd_table_t *cmdtbl = port_info->cmd_tables[slot];
    
    // Setup PRDT entries
    uint32_t bytes_remaining = count * 512;
    const uint8_t *buf_ptr = (const uint8_t*)buffer;
    
    for (int i = 0; i < cmdheader->prdtl - 1; i++) {
        uint64_t buf_phys = paging_virt_to_phys((uint64_t)buf_ptr);
        cmdtbl->prdt_entry[i].dba = (uint32_t)buf_phys;
        cmdtbl->prdt_entry[i].dbau = (uint32_t)(buf_phys >> 32);
        cmdtbl->prdt_entry[i].dbc = 8192 - 1;
        cmdtbl->prdt_entry[i].i = 0;
        buf_ptr += 8192;
        bytes_remaining -= 8192;
    }
    
    // Last entry
    uint64_t buf_phys = paging_virt_to_phys((uint64_t)buf_ptr);
    cmdtbl->prdt_entry[cmdheader->prdtl - 1].dba = (uint32_t)buf_phys;
    cmdtbl->prdt_entry[cmdheader->prdtl - 1].dbau = (uint32_t)(buf_phys >> 32);
    cmdtbl->prdt_entry[cmdheader->prdtl - 1].dbc = bytes_remaining - 1;
    cmdtbl->prdt_entry[cmdheader->prdtl - 1].i = 1;
    
    // Setup command FIS
    fis_reg_h2d_t *cmdfis = (fis_reg_h2d_t*)(&cmdtbl->cfis);
    
    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->c = 1;
    cmdfis->command = ATA_CMD_WRITE_DMA_EX;
    
    cmdfis->lba0 = (uint8_t)start_lba;
    cmdfis->lba1 = (uint8_t)(start_lba >> 8);
    cmdfis->lba2 = (uint8_t)(start_lba >> 16);
    cmdfis->device = 1 << 6;
    
    cmdfis->lba3 = (uint8_t)(start_lba >> 24);
    cmdfis->lba4 = (uint8_t)(start_lba >> 32);
    cmdfis->lba5 = (uint8_t)(start_lba >> 40);
    
    cmdfis->countl = (uint8_t)count;
    cmdfis->counth = (uint8_t)(count >> 8);
    
    // Wait for port to be ready
    int spin = 0;
    while ((port->tfd & (0x80 | 0x08)) && spin < 1000000) {
        spin++;
    }
    
    if (spin == 1000000) {
        COM_LOG_ERROR(COM1_PORT, "Port hung");
        return -1;
    }
    
    // Issue command
    port->ci = 1 << slot;
    
    // Wait for completion
    while (1) {
        if ((port->ci & (1 << slot)) == 0)
            break;
        if (port->is & HBA_PxIS_TFES) {
            COM_LOG_ERROR(COM1_PORT, "Write command failed");
            return -1;
        }
    }
    
    return 0;
}

// ===========================================================================
// Port Probing
// ===========================================================================

int ahci_probe_ports(void) {
    uint32_t pi = ahci_controller.abar->pi;
    int port_count = 0;
    
    for (int i = 0; i < 32; i++) {
        if (pi & 1) {
            ahci_device_type_t type = ahci_check_type(&((hba_port_t*)((uintptr_t)ahci_controller.abar + 0x100 + (i * 0x80)))[0]);
            
            if (type != AHCI_DEV_NULL) {
                ahci_controller.ports[i].type = type;
                ahci_controller.ports[i].port_num = i;
                ahci_controller.ports[i].port = (hba_port_t*)((uintptr_t)ahci_controller.abar + 0x100 + (i * 0x80));
                
                com_write_string(COM1_PORT, "[AHCI] Found ");
                com_write_string(COM1_PORT, ahci_get_device_type_string(type));
                com_write_string(COM1_PORT, " drive on port ");
                char port_str[4];
                port_str[0] = '0' + i;
                port_str[1] = '\0';
                com_write_string(COM1_PORT, port_str);
                com_write_string(COM1_PORT, "\n");
                
                // Rebase port
                if (ahci_port_rebase(ahci_controller.ports[i].port, i) == 0) {
                    // Try to identify device
                    if (ahci_identify_device(i) == 0) {
                        com_write_string(COM1_PORT, "[AHCI]   Model: ");
                        com_write_string(COM1_PORT, ahci_controller.ports[i].model);
                        com_write_string(COM1_PORT, "\n");
                    }
                }
                
                port_count++;
            }
        }
        pi >>= 1;
    }
    
    ahci_controller.port_count = port_count;
    return port_count;
}

// ===========================================================================
// Initialization
// ===========================================================================

int ahci_init(void) {
    COM_LOG_INFO(COM1_PORT, "Initializing AHCI driver");
    
    // Find AHCI controller via PCI
    pci_device_t *pci_dev = pci_find_class(AHCI_CLASS_STORAGE, AHCI_SUBCLASS_SATA);
    
    if (!pci_dev) {
        COM_LOG_WARN(COM1_PORT, "No AHCI controller found");
        ahci_controller.pci_found = 0;
        return -1;
    }
    
    ahci_controller.pci_found = 1;
    
    com_write_string(COM1_PORT, "[AHCI] Found controller: ");
    com_write_string(COM1_PORT, pci_vendor_name(pci_dev->vendor_id));
    com_write_string(COM1_PORT, "\n");
    
    // Enable PCI bus mastering and memory space
    pci_enable_bus_mastering(pci_dev);
    pci_enable_memory_space(pci_dev);
    
    // Get ABAR (BAR5)
    uint32_t abar_phys = pci_read_bar(pci_dev, 5);
    if (abar_phys == 0) {
        COM_LOG_ERROR(COM1_PORT, "Invalid ABAR");
        return -1;
    }
    
    com_write_string(COM1_PORT, "[AHCI] ABAR at 0x");
    char hex[9];
    for (int i = 0; i < 8; i++) {
        int nibble = (abar_phys >> ((7-i)*4)) & 0xF;
        hex[i] = nibble < 10 ? '0' + nibble : 'a' + (nibble - 10);
    }
    hex[8] = '\0';
    com_write_string(COM1_PORT, hex);
    com_write_string(COM1_PORT, "\n");
    
    // **CRITICAL FIX**: Map the AHCI MMIO region into virtual memory
    // ABAR is typically 8KB in size, but we'll map 4KB (one page) which is sufficient
    // Map it with cache-disable and write-through flags for MMIO
    com_write_string(COM1_PORT, "[AHCI] Mapping ABAR into virtual memory...\n");
    
    uint64_t abar_virt = abar_phys; // Use identity mapping
    int map_result = paging_map_page(abar_virt, abar_phys, 
                                     PFLAG_PRESENT | PFLAG_WRITABLE | PFLAG_PCD | PFLAG_PWT);
    
    if (map_result != 0) {
        COM_LOG_ERROR(COM1_PORT, "Failed to map ABAR");
        return -1;
    }
    
    // Map additional pages if needed (AHCI HBA is typically up to 8KB)
    paging_map_page(abar_virt + 0x1000, abar_phys + 0x1000, 
                   PFLAG_PRESENT | PFLAG_WRITABLE | PFLAG_PCD | PFLAG_PWT);
    
    ahci_controller.abar = (hba_mem_t*)(uintptr_t)abar_virt;
    
    com_write_string(COM1_PORT, "[AHCI] ABAR mapped successfully\n");
    
    // Enable AHCI mode
    ahci_controller.abar->ghc |= HBA_GHC_AHCI_ENABLE;
    
    // Probe ports
    int ports = ahci_probe_ports();
    
    if (ports == 0) {
        COM_LOG_WARN(COM1_PORT, "No AHCI drives detected");
        return -1;
    }
    
    com_write_string(COM1_PORT, "[AHCI] Detected ");
    char port_count[4];
    port_count[0] = '0' + ports;
    port_count[1] = ' ';
    port_count[2] = '\0';
    com_write_string(COM1_PORT, port_count);
    com_write_string(COM1_PORT, "drive(s)\n");
    
    COM_LOG_OK(COM1_PORT, "AHCI driver initialized successfully");
    return 0;
}

void ahci_shutdown(void) {
    // Stop all ports
    for (int i = 0; i < AHCI_MAX_PORTS; i++) {
        if (ahci_controller.ports[i].type != AHCI_DEV_NULL) {
            ahci_stop_cmd(ahci_controller.ports[i].port);
        }
    }
}

ahci_controller_t* ahci_get_controller(void) {
    return &ahci_controller;
}