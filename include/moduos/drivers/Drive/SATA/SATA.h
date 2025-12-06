#ifndef SATA_H
#define SATA_H

#include <stdint.h>

// SATA driver provides a unified interface for SATA drives
// It wraps AHCI operations with a simpler, more user-friendly API

// SATA Device Types
typedef enum {
    SATA_TYPE_NONE = 0,
    SATA_TYPE_HDD = 1,      // Hard Disk Drive
    SATA_TYPE_SSD = 2,      // Solid State Drive
    SATA_TYPE_OPTICAL = 3,  // CD/DVD Drive
    SATA_TYPE_UNKNOWN = 4
} sata_device_type_t;

// SATA Device Status
typedef enum {
    SATA_STATUS_NOT_PRESENT = 0,
    SATA_STATUS_READY = 1,
    SATA_STATUS_ERROR = 2,
    SATA_STATUS_BUSY = 3
} sata_device_status_t;

// SATA Device Information
typedef struct {
    uint8_t port;                    // AHCI port number
    uint8_t present;                 // Device present flag
    sata_device_type_t type;         // Device type
    sata_device_status_t status;     // Current status
    
    // Device identification
    char model[41];                  // Model string (40 chars + null)
    char serial[21];                 // Serial number (20 chars + null)
    char firmware[9];                // Firmware revision (8 chars + null)
    
    // Capacity information
    uint64_t total_sectors;          // Total number of sectors
    uint32_t sector_size;            // Bytes per sector (usually 512)
    uint64_t capacity_mb;            // Capacity in megabytes
    uint64_t capacity_gb;            // Capacity in gigabytes
    
    // Features
    uint8_t supports_48bit_lba;      // Supports 48-bit LBA
    uint8_t supports_dma;            // Supports DMA
    uint8_t supports_smart;          // Supports SMART
    uint8_t supports_ncq;            // Supports Native Command Queuing
    uint8_t supports_trim;           // Supports TRIM (SSD)
    
    // Performance tracking
    uint64_t reads_completed;        // Total read operations
    uint64_t writes_completed;       // Total write operations
    uint64_t errors;                 // Total errors encountered
} sata_device_t;

// SATA Subsystem Information
typedef struct {
    uint8_t initialized;             // Is SATA subsystem initialized?
    uint8_t ahci_available;          // Is AHCI controller available?
    uint8_t device_count;            // Number of detected devices
    sata_device_t devices[32];       // Array of SATA devices (max 32)
} sata_info_t;

// ===========================================================================
// Initialization and Detection
// ===========================================================================

// Initialize SATA subsystem
int sata_init(void);

// Shutdown SATA subsystem
void sata_shutdown(void);

// Rescan for SATA devices
int sata_rescan(void);

// Get SATA subsystem information
sata_info_t* sata_get_info(void);

// ===========================================================================
// Device Access
// ===========================================================================

// Get device by port number
sata_device_t* sata_get_device(uint8_t port);

// Get first available device
sata_device_t* sata_get_first_device(void);

// Get device count
int sata_get_device_count(void);

// Check if device is present and ready
int sata_device_ready(uint8_t port);

// ===========================================================================
// Read/Write Operations
// ===========================================================================

// Read sectors from device (blocking)
// Returns 0 on success, negative on error
int sata_read(uint8_t port, uint64_t lba, uint32_t count, void *buffer);

// Write sectors to device (blocking)
// Returns 0 on success, negative on error
int sata_write(uint8_t port, uint64_t lba, uint32_t count, const void *buffer);

// Read single sector
int sata_read_sector(uint8_t port, uint64_t lba, void *buffer);

// Write single sector
int sata_write_sector(uint8_t port, uint64_t lba, const void *buffer);

// ===========================================================================
// Utility Functions
// ===========================================================================

// Get device type string
const char* sata_get_type_string(sata_device_type_t type);

// Get device status string
const char* sata_get_status_string(sata_device_status_t status);

// Format capacity as string (e.g., "120.5 GB")
void sata_format_capacity(uint64_t capacity_mb, char *buffer, int buffer_size);

// Dump device information to COM port
void sata_dump_device_info(uint8_t port);

// Dump all devices information
void sata_dump_all_devices(void);

// ===========================================================================
// Advanced Operations
// ===========================================================================

// Flush write cache
int sata_flush(uint8_t port);

// Get device temperature (if supported via SMART)
int sata_get_temperature(uint8_t port, uint8_t *temp_celsius);

// Verify sectors
int sata_verify(uint8_t port, uint64_t lba, uint32_t count);

// ===========================================================================
// Error Handling
// ===========================================================================

// Get last error for device
int sata_get_last_error(uint8_t port);

// Clear error status
void sata_clear_error(uint8_t port);

// Get error string
const char* sata_get_error_string(int error_code);

// ===========================================================================
// Error Codes
// ===========================================================================

#define SATA_SUCCESS            0
#define SATA_ERR_NOT_INIT       -1
#define SATA_ERR_NO_DEVICE      -2
#define SATA_ERR_NOT_READY      -3
#define SATA_ERR_INVALID_PORT   -4
#define SATA_ERR_INVALID_LBA    -5
#define SATA_ERR_INVALID_COUNT  -6
#define SATA_ERR_NULL_BUFFER    -7
#define SATA_ERR_READ_FAILED    -8
#define SATA_ERR_WRITE_FAILED   -9
#define SATA_ERR_TIMEOUT        -10
#define SATA_ERR_HARDWARE       -11
#define SATA_ERR_NO_AHCI        -12

#endif // SATA_H