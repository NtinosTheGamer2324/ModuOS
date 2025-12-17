/*
 * satapi.c
 *
 * SATAPI (SATA ATAPI) driver for CD/DVD drives over AHCI/SATA
 * 
 * This driver provides SCSI command support for SATA optical drives.
 * It uses the AHCI driver to send ATAPI PACKET commands with SCSI payloads.
 *
 * Features:
 *  - TEST UNIT READY with automatic spin-up wait
 *  - REQUEST SENSE for error reporting
 *  - READ CAPACITY to get disc size
 *  - READ(10) for reading 2048-byte logical blocks
 *  - Automatic retry on media not ready
 */

#include "moduos/drivers/Drive/SATA/satapi.h"
#include "moduos/drivers/Drive/SATA/AHCI.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/macros.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/paging.h"
#include <stddef.h>

// SATAPI devices (one per AHCI port)
static satapi_device_t satapi_devices[AHCI_MAX_PORTS];
static uint8_t satapi_initialized = 0;

// Retry settings
#define SATAPI_RETRY_COUNT 5
#define SATAPI_SPINUP_DELAY_MS 1000

// Timeout settings (in milliseconds)
#define SATAPI_TIMEOUT_MS 5000

// ===========================================================================
// Helper Functions
// ===========================================================================

static void satapi_usleep(uint32_t us) {
    for (volatile uint32_t i = 0; i < us * 100; i++) {
        __asm__ volatile("pause");
    }
}

static void satapi_msleep(uint32_t ms) {
    satapi_usleep(ms * 1000);
}

// ===========================================================================
// Low-Level ATAPI Command Execution
// ===========================================================================

/**
 * Send an ATAPI PACKET command with SCSI payload
 * 
 * @param port_num AHCI port number
 * @param scsi_cmd SCSI command packet (12 bytes)
 * @param buffer Data buffer (for read commands)
 * @param buffer_len Length of data buffer
 * @param is_write 1 for write, 0 for read
 * @return 0 on success, negative on error
 */
static int satapi_send_packet(uint8_t port_num, uint8_t *scsi_cmd, 
                               void *buffer, uint32_t buffer_len, int is_write) {
    ahci_controller_t *ctrl = ahci_get_controller();
    if (!ctrl || port_num >= AHCI_MAX_PORTS) {
        return SATAPI_ERR_INVALID_PORT;
    }
    
    ahci_port_info_t *port_info = &ctrl->ports[port_num];
    hba_port_t *port = port_info->port;
    
    if (!port || port_info->type != AHCI_DEV_SATAPI) {
        return SATAPI_ERR_NO_DEVICE;
    }
    
    // Clear pending interrupts
    port->is = (uint32_t)-1;
    
    // Find free command slot
    int slot = ahci_find_cmdslot(port);
    if (slot == -1) {
        COM_LOG_ERROR(COM1_PORT, "No free command slot");
        return SATAPI_ERR_HARDWARE;
    }
    
    hba_cmd_header_t *cmdheader = &port_info->cmd_list[slot];
    cmdheader->cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmdheader->w = is_write ? 1 : 0;
    cmdheader->a = 1;  // ATAPI bit
    cmdheader->prdtl = (buffer && buffer_len > 0) ? 1 : 0;
    
    hba_cmd_table_t *cmdtbl = port_info->cmd_tables[slot];
    
    // Zero out the command table
    for (int i = 0; i < 64; i++) {
        ((uint32_t*)cmdtbl)[i] = 0;
    }
    
    // Setup PRDT if we have data to transfer
    if (buffer && buffer_len > 0) {
        uint64_t buf_phys = paging_virt_to_phys((uint64_t)buffer);
        cmdtbl->prdt_entry[0].dba = (uint32_t)buf_phys;
        cmdtbl->prdt_entry[0].dbau = (uint32_t)(buf_phys >> 32);
        cmdtbl->prdt_entry[0].dbc = buffer_len - 1;  // 0-based
        cmdtbl->prdt_entry[0].i = 0;  // No interrupt
    }
    
    // Setup command FIS for PACKET command
    fis_reg_h2d_t *cmdfis = (fis_reg_h2d_t*)(&cmdtbl->cfis);
    
    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->c = 1;  // Command bit
    cmdfis->command = ATA_CMD_PACKET;
    cmdfis->featurel = 0;  // PIO mode
    cmdfis->featureh = 0;
    
    // Set byte count limit (for DRQ data blocks)
    uint16_t byte_count = (buffer_len > 0xFE00) ? 0xFE00 : (uint16_t)buffer_len;
    cmdfis->lba1 = (uint8_t)(byte_count & 0xFF);        // Byte count low
    cmdfis->lba2 = (uint8_t)((byte_count >> 8) & 0xFF); // Byte count high
    
    cmdfis->device = 0;
    cmdfis->lba0 = 0;
    cmdfis->lba3 = 0;
    cmdfis->lba4 = 0;
    cmdfis->lba5 = 0;
    cmdfis->countl = 0;
    cmdfis->counth = 0;
    
    // Copy SCSI command to ACMD area (12 bytes)
    for (int i = 0; i < 12; i++) {
        cmdtbl->acmd[i] = scsi_cmd[i];
    }
    
    // Wait for port to be ready
    int spin = 0;
    while ((port->tfd & (0x80 | 0x08)) && spin < 1000) {
        satapi_usleep(1000);
        spin++;
    }
    
    if (spin == 1000) {
        COM_LOG_ERROR(COM1_PORT, "SATAPI: Port hung");
        return SATAPI_ERR_TIMEOUT;
    }
    
    // Issue command
    port->ci = 1 << slot;
    
    // Wait for completion
    spin = 0;
    while (spin < SATAPI_TIMEOUT_MS) {
        if ((port->ci & (1 << slot)) == 0) {
            // Command completed
            return SATAPI_SUCCESS;
        }
        if (port->is & HBA_PxIS_TFES) {
            COM_LOG_ERROR(COM1_PORT, "SATAPI: Task file error");
            return SATAPI_ERR_HARDWARE;
        }
        satapi_usleep(1000);
        spin++;
    }
    
    COM_LOG_ERROR(COM1_PORT, "SATAPI: Command timeout");
    return SATAPI_ERR_TIMEOUT;
}

// ===========================================================================
// SCSI Commands
// ===========================================================================

int satapi_test_unit_ready(uint8_t port_num) {
    uint8_t cmd[12] = {0};
    cmd[0] = SCSI_CMD_TEST_UNIT_READY;
    
    int result = satapi_send_packet(port_num, cmd, NULL, 0, 0);
    
    if (result == SATAPI_SUCCESS) {
        satapi_devices[port_num].ready = 1;
    } else {
        satapi_devices[port_num].ready = 0;
        // Request sense to get the actual reason
        uint8_t sense_data[18];
        if (satapi_request_sense(port_num, sense_data, 18) == SATAPI_SUCCESS) {
            // Just store the sense data, don't treat as error
            com_printf(COM1_PORT, "[SATAPI]   Sense: key=0x%02x (", satapi_devices[port_num].sense_key);
            com_write_string(COM1_PORT, satapi_get_sense_key_string(satapi_devices[port_num].sense_key));
            com_write_string(COM1_PORT, "), asc=0x");
            com_printf(COM1_PORT, "%02x\n", satapi_devices[port_num].asc);
        }
    }
    
    return result;
}

int satapi_request_sense(uint8_t port_num, uint8_t *sense_data, int sense_len) {
    if (!sense_data || sense_len < 18) {
        return SATAPI_ERR_NULL_BUFFER;
    }
    
    uint8_t cmd[12] = {0};
    cmd[0] = SCSI_CMD_REQUEST_SENSE;
    cmd[4] = 18;  // Allocation length
    
    int result = satapi_send_packet(port_num, cmd, sense_data, 18, 0);
    
    if (result == SATAPI_SUCCESS) {
        // Parse sense data
        satapi_devices[port_num].sense_key = sense_data[2] & 0x0F;
        satapi_devices[port_num].asc = sense_data[12];
        satapi_devices[port_num].ascq = sense_data[13];
    }
    
    return result;
}

int satapi_read_capacity(uint8_t port_num, uint32_t *total_blocks, uint32_t *block_size) {
    uint8_t cmd[12] = {0};
    cmd[0] = SCSI_CMD_READ_CAPACITY_10;
    
    uint8_t capacity_data[8];
    int result = satapi_send_packet(port_num, cmd, capacity_data, 8, 0);
    
    if (result == SATAPI_SUCCESS) {
        // Parse capacity data (big-endian)
        uint32_t last_lba = ((uint32_t)capacity_data[0] << 24) |
                           ((uint32_t)capacity_data[1] << 16) |
                           ((uint32_t)capacity_data[2] << 8) |
                           ((uint32_t)capacity_data[3]);
        
        uint32_t blk_size = ((uint32_t)capacity_data[4] << 24) |
                           ((uint32_t)capacity_data[5] << 16) |
                           ((uint32_t)capacity_data[6] << 8) |
                           ((uint32_t)capacity_data[7]);
        
        if (total_blocks) {
            *total_blocks = last_lba + 1;  // Last LBA + 1 = total blocks
        }
        if (block_size) {
            *block_size = blk_size;
        }
        
        // Update device info
        satapi_devices[port_num].total_blocks = last_lba + 1;
        satapi_devices[port_num].block_size = blk_size;
        satapi_devices[port_num].capacity_mb = 
            ((uint64_t)(last_lba + 1) * blk_size) / (1024 * 1024);
    }
    
    return result;
}

int satapi_read_blocks(uint8_t port_num, uint32_t lba, uint32_t count, void *buffer) {
    if (!buffer) {
        return SATAPI_ERR_NULL_BUFFER;
    }
    
    if (count == 0) {
        return SATAPI_ERR_INVALID_COUNT;
    }
    
    if (port_num >= AHCI_MAX_PORTS) {
        return SATAPI_ERR_INVALID_PORT;
    }
    
    if (!satapi_devices[port_num].present) {
        return SATAPI_ERR_NO_DEVICE;
    }
    
    // Try multiple times in case drive needs to spin up
    for (int attempt = 0; attempt < SATAPI_RETRY_COUNT; attempt++) {
        // Check if unit is ready
        int ready = satapi_test_unit_ready(port_num);
        if (ready != SATAPI_SUCCESS) {
            if (attempt < SATAPI_RETRY_COUNT - 1) {
                COM_LOG_WARN(COM1_PORT, "SATAPI: Drive not ready, waiting for spin-up");
                satapi_msleep(SATAPI_SPINUP_DELAY_MS);
                continue;
            } else {
                // Request sense to get error details
                uint8_t sense_data[18];
                if (satapi_request_sense(port_num, sense_data, 18) == SATAPI_SUCCESS) {
                    com_printf(COM1_PORT, "[SATAPI] Sense: key=0x%02x, asc=0x%02x, ascq=0x%02x\n",
                              satapi_devices[port_num].sense_key,
                              satapi_devices[port_num].asc,
                              satapi_devices[port_num].ascq);
                }
                return SATAPI_ERR_NOT_READY;
            }
        }
        
        // Build READ(10) command
        uint8_t cmd[12] = {0};
        cmd[0] = SCSI_CMD_READ_10;
        
        // LBA (big-endian)
        cmd[2] = (uint8_t)((lba >> 24) & 0xFF);
        cmd[3] = (uint8_t)((lba >> 16) & 0xFF);
        cmd[4] = (uint8_t)((lba >> 8) & 0xFF);
        cmd[5] = (uint8_t)(lba & 0xFF);
        
        // Transfer length (big-endian, 16-bit)
        cmd[7] = (uint8_t)((count >> 8) & 0xFF);
        cmd[8] = (uint8_t)(count & 0xFF);
        
        uint32_t transfer_size = count * SATAPI_SECTOR_SIZE;
        int result = satapi_send_packet(port_num, cmd, buffer, transfer_size, 0);
        
        if (result == SATAPI_SUCCESS) {
            satapi_devices[port_num].reads_completed++;
            return SATAPI_SUCCESS;
        } else {
            satapi_devices[port_num].errors++;
            
            if (attempt < SATAPI_RETRY_COUNT - 1) {
                COM_LOG_WARN(COM1_PORT, "SATAPI: Read failed, retrying");
                satapi_msleep(100);  // Short delay before retry
            }
        }
    }
    
    return SATAPI_ERR_READ_FAILED;
}

int satapi_read_sector(uint8_t port_num, uint32_t lba, void *buffer) {
    return satapi_read_blocks(port_num, lba, 1, buffer);
}

// ===========================================================================
// Utility Functions
// ===========================================================================

int satapi_media_present(uint8_t port_num) {
    if (port_num >= AHCI_MAX_PORTS) {
        return 0;
    }
    
    if (!satapi_devices[port_num].present) {
        return 0;
    }
    
    // Test unit ready
    int result = satapi_test_unit_ready(port_num);
    if (result == SATAPI_SUCCESS) {
        return 1;
    }
    
    // Check sense data
    uint8_t sense_data[18];
    if (satapi_request_sense(port_num, sense_data, 18) == SATAPI_SUCCESS) {
        // Media not present has ASC 0x3A
        if (satapi_devices[port_num].asc == ASC_MEDIUM_NOT_PRESENT) {
            return 0;
        }
    }
    
    return 0;
}

void satapi_get_sense(uint8_t port_num, uint8_t *sense_key, uint8_t *asc, uint8_t *ascq) {
    if (port_num >= AHCI_MAX_PORTS) {
        return;
    }
    
    if (sense_key) *sense_key = satapi_devices[port_num].sense_key;
    if (asc) *asc = satapi_devices[port_num].asc;
    if (ascq) *ascq = satapi_devices[port_num].ascq;
}

const char* satapi_get_sense_key_string(uint8_t sense_key) {
    switch (sense_key) {
        case SENSE_NO_SENSE: return "No Sense";
        case SENSE_NOT_READY: return "Not Ready";
        case SENSE_MEDIUM_ERROR: return "Medium Error";
        case SENSE_ILLEGAL_REQUEST: return "Illegal Request";
        case SENSE_UNIT_ATTENTION: return "Unit Attention";
        default: return "Unknown";
    }
}

const char* satapi_get_asc_string(uint8_t asc) {
    switch (asc) {
        case ASC_NO_ADDITIONAL_INFO: return "No Additional Info";
        case ASC_MEDIUM_NOT_PRESENT: return "Medium Not Present";
        default: return "Unknown ASC";
    }
}

void satapi_dump_device_info(uint8_t port_num) {
    if (port_num >= AHCI_MAX_PORTS) {
        com_write_string(COM1_PORT, "[SATAPI] Invalid port number\n");
        return;
    }
    
    satapi_device_t *dev = &satapi_devices[port_num];
    
    if (!dev->present) {
        com_write_string(COM1_PORT, "[SATAPI] No device on port\n");
        return;
    }
    
    com_write_string(COM1_PORT, "\n=== SATAPI Device Information ===\n");
    com_printf(COM1_PORT, "Port:          %d\n", port_num);
    com_printf(COM1_PORT, "Present:       %s\n", dev->present ? "Yes" : "No");
    com_printf(COM1_PORT, "Ready:         %s\n", dev->ready ? "Yes" : "No");
    
    if (dev->total_blocks > 0) {
        com_printf(COM1_PORT, "Total Blocks:  %u\n", dev->total_blocks);
        com_printf(COM1_PORT, "Block Size:    %u bytes\n", dev->block_size);
        com_printf(COM1_PORT, "Capacity:      %llu MB\n", dev->capacity_mb);
    }
    
    com_printf(COM1_PORT, "Reads:         %llu\n", dev->reads_completed);
    com_printf(COM1_PORT, "Errors:        %llu\n", dev->errors);
    com_write_string(COM1_PORT, "=================================\n\n");
}

// ===========================================================================
// Initialization
// ===========================================================================

int satapi_init(void) {
    COM_LOG_INFO(COM1_PORT, "Initializing SATAPI driver");
    
    // Clear device structures
    for (int i = 0; i < AHCI_MAX_PORTS; i++) {
        satapi_devices[i].port_num = i;
        satapi_devices[i].present = 0;
        satapi_devices[i].ready = 0;
        satapi_devices[i].total_blocks = 0;
        satapi_devices[i].block_size = SATAPI_SECTOR_SIZE;
        satapi_devices[i].capacity_mb = 0;
        satapi_devices[i].sense_key = 0;
        satapi_devices[i].asc = 0;
        satapi_devices[i].ascq = 0;
        satapi_devices[i].reads_completed = 0;
        satapi_devices[i].errors = 0;
    }
    
    // Scan AHCI ports for SATAPI devices
    ahci_controller_t *ctrl = ahci_get_controller();
    if (!ctrl) {
        COM_LOG_ERROR(COM1_PORT, "AHCI controller not initialized");
        return SATAPI_ERR_NOT_INIT;
    }
    
    int device_count = 0;
    for (int i = 0; i < AHCI_MAX_PORTS; i++) {
        if (ctrl->ports[i].type == AHCI_DEV_SATAPI) {
            satapi_devices[i].present = 1;
            device_count++;
            
            com_printf(COM1_PORT, "[SATAPI] Found SATAPI device on port %d\n", i);
            
            // Try to test if drive is ready (don't fail if no media)
            int ready_result = satapi_test_unit_ready(i);
            
            if (ready_result == SATAPI_SUCCESS) {
                uint32_t blocks, block_size;
                if (satapi_read_capacity(i, &blocks, &block_size) == SATAPI_SUCCESS) {
                    com_printf(COM1_PORT, "[SATAPI]   Media present: %u blocks x %u bytes\n", 
                              blocks, block_size);
                }
            } else {
                com_write_string(COM1_PORT, "[SATAPI]   No media present (this is normal)\n");
            }
        }
    }
    
    if (device_count == 0) {
        COM_LOG_INFO(COM1_PORT, "No SATAPI devices found");
    } else {
        com_printf(COM1_PORT, "[SATAPI] Initialized %d device(s)\n", device_count);
    }
    
    satapi_initialized = 1;
    return SATAPI_SUCCESS;
}

void satapi_shutdown(void) {
    satapi_initialized = 0;
    COM_LOG_INFO(COM1_PORT, "SATAPI driver shut down");
}

int satapi_is_device(uint8_t port_num) {
    if (port_num >= AHCI_MAX_PORTS) {
        return 0;
    }
    return satapi_devices[port_num].present;
}

satapi_device_t* satapi_get_device(uint8_t port_num) {
    if (port_num >= AHCI_MAX_PORTS) {
        return NULL;
    }
    
    if (!satapi_devices[port_num].present) {
        return NULL;
    }
    
    return &satapi_devices[port_num];
}
