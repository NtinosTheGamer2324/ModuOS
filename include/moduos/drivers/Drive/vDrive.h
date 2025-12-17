#ifndef VDRIVE_H
#define VDRIVE_H

#include <stdint.h>

// vDrive: Virtual Drive abstraction layer
// Provides a unified interface for both ATA (legacy IDE) and SATA drives
// Automatically detects and manages all available storage devices

// Maximum number of virtual drives (ATA + SATA combined)
#define VDRIVE_MAX_DRIVES 32

// Drive types
typedef enum {
    VDRIVE_TYPE_NONE = 0,
    VDRIVE_TYPE_ATA_HDD = 1,      // ATA Hard Drive
    VDRIVE_TYPE_ATA_ATAPI = 2,    // ATA CD/DVD Drive
    VDRIVE_TYPE_SATA_HDD = 3,     // SATA Hard Drive
    VDRIVE_TYPE_SATA_SSD = 4,     // SATA Solid State Drive
    VDRIVE_TYPE_SATA_OPTICAL = 5, // SATA Optical Drive
    VDRIVE_TYPE_UNKNOWN = 6
} vdrive_type_t;

// Drive backend (which subsystem handles this drive)
typedef enum {
    VDRIVE_BACKEND_NONE = 0,
    VDRIVE_BACKEND_ATA = 1,
    VDRIVE_BACKEND_SATA = 2
} vdrive_backend_t;

// Drive status
typedef enum {
    VDRIVE_STATUS_NOT_PRESENT = 0,
    VDRIVE_STATUS_READY = 1,
    VDRIVE_STATUS_ERROR = 2,
    VDRIVE_STATUS_BUSY = 3
} vdrive_status_t;

// Virtual Drive descriptor
typedef struct {
    uint8_t present;              // Is this drive present?
    uint8_t vdrive_id;            // Virtual drive ID (0-31)
    
    vdrive_type_t type;           // Drive type
    vdrive_backend_t backend;     // Which subsystem handles this
    vdrive_status_t status;       // Current status
    
    // Backend-specific identifiers
    uint8_t backend_id;           // ATA drive index or SATA port number
    
    // Drive information
    char model[41];               // Model string
    char serial[21];              // Serial number (SATA only)
    
    // Capacity
    uint64_t total_sectors;       // Total sectors
    uint32_t sector_size;         // Bytes per sector (usually 512)
    uint64_t capacity_mb;         // Capacity in MB
    uint64_t capacity_gb;         // Capacity in GB
    
    // Features
    uint8_t supports_lba48;       // Supports 48-bit LBA
    uint8_t supports_dma;         // Supports DMA
    uint8_t read_only;            // Read-only drive (optical)
    
    // Statistics
    uint64_t reads;               // Total read operations
    uint64_t writes;              // Total write operations
    uint64_t errors;              // Total errors
} vdrive_t;

// vDrive subsystem info
typedef struct {
    uint8_t initialized;
    uint8_t drive_count;
    uint8_t ata_count;
    uint8_t sata_count;
    vdrive_t drives[VDRIVE_MAX_DRIVES];
} vdrive_system_t;

// ===========================================================================
// Initialization
// ===========================================================================

// Initialize vDrive subsystem (auto-detects ATA and SATA drives)
int vdrive_init(void);

// Shutdown vDrive subsystem
void vdrive_shutdown(void);

// Rescan for drives
int vdrive_rescan(void);

// Get system information
vdrive_system_t* vdrive_get_system_info(void);

// ===========================================================================
// Drive Access
// ===========================================================================

// Get drive by virtual ID
vdrive_t* vdrive_get(uint8_t vdrive_id);

// Get first available drive
vdrive_t* vdrive_get_first(void);

// Get drive count
int vdrive_get_count(void);

// Check if drive is ready
int vdrive_is_ready(uint8_t vdrive_id);

// Find drive by model string (partial match)
vdrive_t* vdrive_find_by_model(const char *model_substring);

// ===========================================================================
// Read/Write Operations
// ===========================================================================

// Read sectors (unified interface for both ATA and SATA)
int vdrive_read(uint8_t vdrive_id, uint64_t lba, uint32_t count, void *buffer);

// Write sectors (unified interface for both ATA and SATA)
int vdrive_write(uint8_t vdrive_id, uint64_t lba, uint32_t count, const void *buffer);

// Read single sector
int vdrive_read_sector(uint8_t vdrive_id, uint64_t lba, void *buffer);

// Write single sector
int vdrive_write_sector(uint8_t vdrive_id, uint64_t lba, const void *buffer);

// ===========================================================================
// Utility Functions
// ===========================================================================

// Get type string
const char* vdrive_get_type_string(vdrive_type_t type);

// Get backend string
const char* vdrive_get_backend_string(vdrive_backend_t backend);

// Get status string
const char* vdrive_get_status_string(vdrive_status_t status);

// Format capacity string
void vdrive_format_capacity(uint64_t capacity_mb, char *buffer, int size);

// Dump drive information
void vdrive_dump_info(uint8_t vdrive_id);

// Dump all drives
void vdrive_dump_all(void);

// Print drive table (formatted for display)
void vdrive_print_table(void);

// ===========================================================================
// Advanced Operations
// ===========================================================================

// Flush write cache
int vdrive_flush(uint8_t vdrive_id);

// Get drive statistics
void vdrive_get_stats(uint8_t vdrive_id, uint64_t *reads, uint64_t *writes, uint64_t *errors);

// Reset drive statistics
void vdrive_reset_stats(uint8_t vdrive_id);

// ===========================================================================
// Error Handling
// ===========================================================================

// Get last error for drive
int vdrive_get_last_error(uint8_t vdrive_id);

// Clear error
void vdrive_clear_error(uint8_t vdrive_id);

// Get error string
const char* vdrive_get_error_string(int error_code);

//debug
void vdrive_debug_registration(void);

// ===========================================================================
// Error Codes
// ===========================================================================

#define VDRIVE_SUCCESS           0
#define VDRIVE_ERR_NOT_INIT      -1
#define VDRIVE_ERR_NO_DRIVE      -2
#define VDRIVE_ERR_NOT_READY     -3
#define VDRIVE_ERR_INVALID_ID    -4
#define VDRIVE_ERR_INVALID_LBA   -5
#define VDRIVE_ERR_INVALID_COUNT -6
#define VDRIVE_ERR_NULL_BUFFER   -7
#define VDRIVE_ERR_READ_FAILED   -8
#define VDRIVE_ERR_WRITE_FAILED  -9
#define VDRIVE_ERR_READ_ONLY     -10
#define VDRIVE_ERR_BACKEND       -11
#define VDRIVE_ERR_NO_DRIVES     -12

#endif // VDRIVE_H