#include "moduos/drivers/Drive/SATA/SATA.h"
#include "moduos/drivers/Drive/SATA/AHCI.h"
#include "moduos/drivers/Drive/SATA/satapi.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/macros.h"
#include "moduos/kernel/memory/memory.h"
#include <stddef.h>

// Global SATA subsystem state
static sata_info_t sata_info = {0};

// Last error per device
static int device_last_error[32] = {0};

// ===========================================================================
// Helper Functions
// ===========================================================================

static void sata_trim_string(char *str, int len) {
    // Trim trailing spaces from strings
    int i = len - 1;
    while (i >= 0 && (str[i] == ' ' || str[i] == '\0')) {
        str[i] = '\0';
        i--;
    }
}

static uint64_t sata_calculate_mb(uint64_t sectors, uint32_t sector_size) {
    return (sectors * sector_size) / (1024 * 1024);
}

static uint64_t sata_calculate_gb(uint64_t sectors, uint32_t sector_size) {
    return (sectors * sector_size) / (1024 * 1024 * 1024);
}

static void sata_copy_and_trim(char *dest, const char *src, int len) {
    for (int i = 0; i < len; i++) {
        dest[i] = src[i];
    }
    dest[len] = '\0';
    sata_trim_string(dest, len);
}

static sata_device_type_t sata_detect_device_type(ahci_port_info_t *ahci_port) {
    // Check AHCI type FIRST - this is authoritative
    if (ahci_port->type == AHCI_DEV_SATAPI) {
        return SATA_TYPE_OPTICAL;
    }
    
    if (ahci_port->type != AHCI_DEV_SATA) {
        return SATA_TYPE_UNKNOWN;
    }
    
    // For SATA HDD/SSD, check model string
    const char *model = ahci_port->model;
    for (int i = 0; model[i] != '\0'; i++) {
        if ((model[i] == 'S' || model[i] == 's') &&
            (model[i+1] == 'S' || model[i+1] == 's') &&
            (model[i+2] == 'D' || model[i+2] == 'd')) {
            return SATA_TYPE_SSD;
        }
    }
    
    return SATA_TYPE_HDD;
}

// ===========================================================================
// Initialization and Detection
// ===========================================================================

int sata_init(void) {
    COM_LOG_INFO(COM1_PORT, "Initializing SATA subsystem");
    
    // Reset state
    for (int i = 0; i < 32; i++) {
        sata_info.devices[i].present = 0;
        sata_info.devices[i].port = i;
        sata_info.devices[i].status = SATA_STATUS_NOT_PRESENT;
        device_last_error[i] = SATA_SUCCESS;
    }
    
    sata_info.device_count = 0;
    sata_info.initialized = 0;
    sata_info.ahci_available = 0;
    
    // Get AHCI controller
    ahci_controller_t *ahci = ahci_get_controller();
    
    if (!ahci || !ahci->pci_found) {
        COM_LOG_ERROR(COM1_PORT, "AHCI not available - SATA cannot initialize");
        return SATA_ERR_NO_AHCI;
    }
    
    sata_info.ahci_available = 1;
    
    // Scan all AHCI ports
    for (int i = 0; i < AHCI_MAX_PORTS; i++) {
        ahci_port_info_t *ahci_port = &ahci->ports[i];
        
        if (ahci_port->type == AHCI_DEV_NULL) {
            continue;
        }
        
        // Found a device!
        sata_device_t *dev = &sata_info.devices[i];
        dev->present = 1;
        dev->port = i;
        dev->type = sata_detect_device_type(ahci_port);
        dev->status = SATA_STATUS_READY;
        
        // Copy device information
        sata_copy_and_trim(dev->model, ahci_port->model, 40);
        sata_copy_and_trim(dev->serial, ahci_port->serial, 20);
        
        // Set capacity information
        dev->total_sectors = ahci_port->sector_count;
        dev->sector_size = ahci_port->sector_size;
        dev->capacity_mb = sata_calculate_mb(dev->total_sectors, dev->sector_size);
        dev->capacity_gb = sata_calculate_gb(dev->total_sectors, dev->sector_size);
        
        // Set feature flags (based on AHCI capabilities)
        dev->supports_48bit_lba = (dev->total_sectors > 0xFFFFFFF) ? 1 : 0;
        dev->supports_dma = 1;  // AHCI always uses DMA
        dev->supports_smart = 1; // Assume SMART support
        dev->supports_ncq = 0;   // NCQ not implemented yet
        dev->supports_trim = (dev->type == SATA_TYPE_SSD) ? 1 : 0;
        
        // Initialize statistics
        dev->reads_completed = 0;
        dev->writes_completed = 0;
        dev->errors = 0;
        
        sata_info.device_count++;
        
        com_write_string(COM1_PORT, "[SATA] Registered device on port ");
        char port_str[4];
        port_str[0] = '0' + i;
        port_str[1] = '\0';
        com_write_string(COM1_PORT, port_str);
        com_write_string(COM1_PORT, " - ");
        com_write_string(COM1_PORT, sata_get_type_string(dev->type));
        com_write_string(COM1_PORT, "\n");
    }
    
    sata_info.initialized = 1;
    
    com_write_string(COM1_PORT, "[SATA] Found ");
    char count_str[4];
    count_str[0] = '0' + sata_info.device_count;
    count_str[1] = ' ';
    count_str[2] = '\0';
    com_write_string(COM1_PORT, count_str);
    com_write_string(COM1_PORT, "SATA device(s)\n");
    
    // Initialize SATAPI for optical drives
    satapi_init();
    
    COM_LOG_OK(COM1_PORT, "SATA subsystem initialized");
    return SATA_SUCCESS;
}

void sata_shutdown(void) {
    if (!sata_info.initialized) {
        return;
    }
    
    COM_LOG_INFO(COM1_PORT, "Shutting down SATA subsystem");
    
    // Shutdown SATAPI
    satapi_shutdown();
    
    // Flush all devices
    for (int i = 0; i < 32; i++) {
        if (sata_info.devices[i].present) {
            sata_flush(i);
        }
    }
    
    ahci_shutdown();
    
    sata_info.initialized = 0;
    COM_LOG_OK(COM1_PORT, "SATA subsystem shut down");
}

int sata_rescan(void) {
    if (!sata_info.initialized) {
        return sata_init();
    }
    
    COM_LOG_INFO(COM1_PORT, "Rescanning SATA devices");
    
    // Re-probe AHCI ports
    ahci_probe_ports();
    
    // Re-initialize SATA
    return sata_init();
}

sata_info_t* sata_get_info(void) {
    return &sata_info;
}

// ===========================================================================
// Device Access
// ===========================================================================

sata_device_t* sata_get_device(uint8_t port) {
    if (port >= 32) {
        return NULL;
    }
    
    if (!sata_info.devices[port].present) {
        return NULL;
    }
    
    return &sata_info.devices[port];
}

sata_device_t* sata_get_first_device(void) {
    for (int i = 0; i < 32; i++) {
        if (sata_info.devices[i].present) {
            return &sata_info.devices[i];
        }
    }
    return NULL;
}

int sata_get_device_count(void) {
    return sata_info.device_count;
}

int sata_device_ready(uint8_t port) {
    if (!sata_info.initialized) {
        return 0;
    }
    
    if (port >= 32) {
        return 0;
    }
    
    sata_device_t *dev = &sata_info.devices[port];
    return (dev->present && dev->status == SATA_STATUS_READY);
}

// ===========================================================================
// Read/Write Operations
// ===========================================================================

int sata_read(uint8_t port, uint64_t lba, uint32_t count, void *buffer) {
    if (!sata_info.initialized) {
        return SATA_ERR_NOT_INIT;
    }
    
    if (port >= 32) {
        return SATA_ERR_INVALID_PORT;
    }
    
    sata_device_t *dev = &sata_info.devices[port];
    
    if (!dev->present) {
        device_last_error[port] = SATA_ERR_NO_DEVICE;
        return SATA_ERR_NO_DEVICE;
    }
    
    if (dev->status != SATA_STATUS_READY) {
        device_last_error[port] = SATA_ERR_NOT_READY;
        return SATA_ERR_NOT_READY;
    }
    
    if (lba >= dev->total_sectors) {
        device_last_error[port] = SATA_ERR_INVALID_LBA;
        return SATA_ERR_INVALID_LBA;
    }
    
    if (count == 0) {
        device_last_error[port] = SATA_ERR_INVALID_COUNT;
        return SATA_ERR_INVALID_COUNT;
    }
    
    if (buffer == NULL) {
        device_last_error[port] = SATA_ERR_NULL_BUFFER;
        return SATA_ERR_NULL_BUFFER;
    }
    
    // Perform AHCI read
    dev->status = SATA_STATUS_BUSY;
    
    int result = ahci_read_sectors(port, lba, count, buffer);
    
    if (result == 0) {
        dev->reads_completed++;
        dev->status = SATA_STATUS_READY;
        device_last_error[port] = SATA_SUCCESS;
        return SATA_SUCCESS;
    } else {
        dev->errors++;
        dev->status = SATA_STATUS_ERROR;
        device_last_error[port] = SATA_ERR_READ_FAILED;
        return SATA_ERR_READ_FAILED;
    }
}

int sata_write(uint8_t port, uint64_t lba, uint32_t count, const void *buffer) {
    if (!sata_info.initialized) {
        return SATA_ERR_NOT_INIT;
    }
    
    if (port >= 32) {
        return SATA_ERR_INVALID_PORT;
    }
    
    sata_device_t *dev = &sata_info.devices[port];
    
    if (!dev->present) {
        device_last_error[port] = SATA_ERR_NO_DEVICE;
        return SATA_ERR_NO_DEVICE;
    }
    
    if (dev->status != SATA_STATUS_READY) {
        device_last_error[port] = SATA_ERR_NOT_READY;
        return SATA_ERR_NOT_READY;
    }
    
    if (lba >= dev->total_sectors) {
        device_last_error[port] = SATA_ERR_INVALID_LBA;
        return SATA_ERR_INVALID_LBA;
    }
    
    if (count == 0) {
        device_last_error[port] = SATA_ERR_INVALID_COUNT;
        return SATA_ERR_INVALID_COUNT;
    }
    
    if (buffer == NULL) {
        device_last_error[port] = SATA_ERR_NULL_BUFFER;
        return SATA_ERR_NULL_BUFFER;
    }
    
    // Perform AHCI write
    dev->status = SATA_STATUS_BUSY;
    
    int result = ahci_write_sectors(port, lba, count, buffer);
    
    if (result == 0) {
        dev->writes_completed++;
        dev->status = SATA_STATUS_READY;
        device_last_error[port] = SATA_SUCCESS;
        return SATA_SUCCESS;
    } else {
        dev->errors++;
        dev->status = SATA_STATUS_ERROR;
        device_last_error[port] = SATA_ERR_WRITE_FAILED;
        return SATA_ERR_WRITE_FAILED;
    }
}

int sata_read_sector(uint8_t port, uint64_t lba, void *buffer) {
    return sata_read(port, lba, 1, buffer);
}

int sata_write_sector(uint8_t port, uint64_t lba, const void *buffer) {
    return sata_write(port, lba, 1, buffer);
}

// ===========================================================================
// Utility Functions
// ===========================================================================

const char* sata_get_type_string(sata_device_type_t type) {
    switch (type) {
        case SATA_TYPE_HDD: return "HDD";
        case SATA_TYPE_SSD: return "SSD";
        case SATA_TYPE_OPTICAL: return "Optical";
        case SATA_TYPE_UNKNOWN: return "Unknown";
        default: return "None";
    }
}

const char* sata_get_status_string(sata_device_status_t status) {
    switch (status) {
        case SATA_STATUS_READY: return "Ready";
        case SATA_STATUS_BUSY: return "Busy";
        case SATA_STATUS_ERROR: return "Error";
        default: return "Not Present";
    }
}

void sata_format_capacity(uint64_t capacity_mb, char *buffer, int buffer_size) {
    if (capacity_mb < 1024) {
        // Less than 1 GB, show in MB
        int pos = 0;
        uint64_t mb = capacity_mb;
        char temp[32];
        int i = 0;
        
        if (mb == 0) {
            buffer[pos++] = '0';
        } else {
            while (mb > 0 && i < 30) {
                temp[i++] = '0' + (mb % 10);
                mb /= 10;
            }
            while (i > 0) {
                buffer[pos++] = temp[--i];
            }
        }
        
        buffer[pos++] = ' ';
        buffer[pos++] = 'M';
        buffer[pos++] = 'B';
        buffer[pos] = '\0';
    } else {
        // Show in GB
        uint64_t gb = capacity_mb / 1024;
        uint64_t remainder = ((capacity_mb % 1024) * 10) / 1024;
        
        int pos = 0;
        char temp[32];
        int i = 0;
        
        if (gb == 0) {
            buffer[pos++] = '0';
        } else {
            while (gb > 0 && i < 30) {
                temp[i++] = '0' + (gb % 10);
                gb /= 10;
            }
            while (i > 0) {
                buffer[pos++] = temp[--i];
            }
        }
        
        buffer[pos++] = '.';
        buffer[pos++] = '0' + remainder;
        buffer[pos++] = ' ';
        buffer[pos++] = 'G';
        buffer[pos++] = 'B';
        buffer[pos] = '\0';
    }
}

void sata_dump_device_info(uint8_t port) {
    if (port >= 32) {
        com_write_string(COM1_PORT, "[SATA] Invalid port\n");
        return;
    }
    
    sata_device_t *dev = &sata_info.devices[port];
    
    if (!dev->present) {
        com_write_string(COM1_PORT, "[SATA] No device on port ");
        char p[4];
        p[0] = '0' + port;
        p[1] = '\0';
        com_write_string(COM1_PORT, p);
        com_write_string(COM1_PORT, "\n");
        return;
    }
    
    com_write_string(COM1_PORT, "\n=== SATA Device Information ===\n");
    com_write_string(COM1_PORT, "Port:         ");
    char p[4];
    p[0] = '0' + port;
    p[1] = '\0';
    com_write_string(COM1_PORT, p);
    com_write_string(COM1_PORT, "\n");
    
    com_write_string(COM1_PORT, "Type:         ");
    com_write_string(COM1_PORT, sata_get_type_string(dev->type));
    com_write_string(COM1_PORT, "\n");
    
    com_write_string(COM1_PORT, "Status:       ");
    com_write_string(COM1_PORT, sata_get_status_string(dev->status));
    com_write_string(COM1_PORT, "\n");
    
    com_write_string(COM1_PORT, "Model:        ");
    com_write_string(COM1_PORT, dev->model);
    com_write_string(COM1_PORT, "\n");
    
    com_write_string(COM1_PORT, "Serial:       ");
    com_write_string(COM1_PORT, dev->serial);
    com_write_string(COM1_PORT, "\n");
    
    char cap_buf[64];
    sata_format_capacity(dev->capacity_mb, cap_buf, 64);
    com_write_string(COM1_PORT, "Capacity:     ");
    com_write_string(COM1_PORT, cap_buf);
    com_write_string(COM1_PORT, "\n");
    
    com_write_string(COM1_PORT, "Sector Size:  ");
    char ss[8];
    ss[0] = '0' + (dev->sector_size / 100);
    ss[1] = '0' + ((dev->sector_size / 10) % 10);
    ss[2] = '0' + (dev->sector_size % 10);
    ss[3] = '\0';
    com_write_string(COM1_PORT, ss);
    com_write_string(COM1_PORT, " bytes\n");
    
    com_write_string(COM1_PORT, "Features:     ");
    if (dev->supports_48bit_lba) com_write_string(COM1_PORT, "48-bit-LBA ");
    if (dev->supports_dma) com_write_string(COM1_PORT, "DMA ");
    if (dev->supports_smart) com_write_string(COM1_PORT, "SMART ");
    if (dev->supports_ncq) com_write_string(COM1_PORT, "NCQ ");
    if (dev->supports_trim) com_write_string(COM1_PORT, "TRIM ");
    com_write_string(COM1_PORT, "\n");
    
    com_write_string(COM1_PORT, "===============================\n\n");
}

void sata_dump_all_devices(void) {
    com_write_string(COM1_PORT, "\n=== All SATA Devices ===\n");
    
    if (!sata_info.initialized) {
        com_write_string(COM1_PORT, "SATA subsystem not initialized\n");
        return;
    }
    
    if (sata_info.device_count == 0) {
        com_write_string(COM1_PORT, "No SATA devices found\n");
        return;
    }
    
    for (int i = 0; i < 32; i++) {
        if (sata_info.devices[i].present) {
            sata_dump_device_info(i);
        }
    }
}

// ===========================================================================
// Advanced Operations
// ===========================================================================

int sata_flush(uint8_t port) {
    // Placeholder for flush cache command
    // Would need to implement FLUSH CACHE EXT command via AHCI
    if (!sata_device_ready(port)) {
        return SATA_ERR_NOT_READY;
    }
    
    // For now, just return success
    return SATA_SUCCESS;
}

int sata_get_temperature(uint8_t port, uint8_t *temp_celsius) {
    // Placeholder for SMART temperature reading
    // Would need to implement SMART READ DATA command
    if (!sata_device_ready(port)) {
        return SATA_ERR_NOT_READY;
    }
    
    // Not implemented yet
    return SATA_ERR_HARDWARE;
}

int sata_verify(uint8_t port, uint64_t lba, uint32_t count) {
    // Placeholder for verify sectors command
    if (!sata_device_ready(port)) {
        return SATA_ERR_NOT_READY;
    }
    
    // Not implemented yet
    return SATA_ERR_HARDWARE;
}

// ===========================================================================
// Error Handling
// ===========================================================================

int sata_get_last_error(uint8_t port) {
    if (port >= 32) {
        return SATA_ERR_INVALID_PORT;
    }
    return device_last_error[port];
}

void sata_clear_error(uint8_t port) {
    if (port < 32) {
        device_last_error[port] = SATA_SUCCESS;
        if (sata_info.devices[port].present) {
            sata_info.devices[port].status = SATA_STATUS_READY;
        }
    }
}

const char* sata_get_error_string(int error_code) {
    switch (error_code) {
        case SATA_SUCCESS: return "Success";
        case SATA_ERR_NOT_INIT: return "SATA not initialized";
        case SATA_ERR_NO_DEVICE: return "No device present";
        case SATA_ERR_NOT_READY: return "Device not ready";
        case SATA_ERR_INVALID_PORT: return "Invalid port number";
        case SATA_ERR_INVALID_LBA: return "Invalid LBA";
        case SATA_ERR_INVALID_COUNT: return "Invalid sector count";
        case SATA_ERR_NULL_BUFFER: return "NULL buffer";
        case SATA_ERR_READ_FAILED: return "Read operation failed";
        case SATA_ERR_WRITE_FAILED: return "Write operation failed";
        case SATA_ERR_TIMEOUT: return "Operation timeout";
        case SATA_ERR_HARDWARE: return "Hardware error";
        case SATA_ERR_NO_AHCI: return "AHCI not available";
        default: return "Unknown error";
    }
}