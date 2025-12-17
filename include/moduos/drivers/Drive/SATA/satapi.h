#ifndef SATAPI_H
#define SATAPI_H

#include <stdint.h>

// SATAPI (SATA ATAPI) - CD/DVD drive support over AHCI/SATA
// This driver provides SCSI command interface over SATA for optical drives

// SATAPI logical block size (standard for CD/DVD)
#define SATAPI_SECTOR_SIZE 2048

// SCSI Commands
#define SCSI_CMD_TEST_UNIT_READY    0x00
#define SCSI_CMD_REQUEST_SENSE      0x03
#define SCSI_CMD_READ_10            0x28
#define SCSI_CMD_READ_CAPACITY_10   0x25
#define SCSI_CMD_READ_TOC           0x43
#define SCSI_CMD_GET_EVENT_STATUS   0x4A
#define SCSI_CMD_READ_DISC_INFO     0x51
#define SCSI_CMD_READ_12            0xA8

// ATAPI command (sent via ATA PACKET command)
#define ATA_CMD_PACKET              0xA0

// Sense key values
#define SENSE_NO_SENSE              0x00
#define SENSE_NOT_READY             0x02
#define SENSE_MEDIUM_ERROR          0x03
#define SENSE_ILLEGAL_REQUEST       0x05
#define SENSE_UNIT_ATTENTION        0x06

// Additional Sense Codes
#define ASC_NO_ADDITIONAL_INFO      0x00
#define ASC_MEDIUM_NOT_PRESENT      0x3A

// ===========================================================================
// SATAPI Device Information
// ===========================================================================

typedef struct {
    uint8_t port_num;           // AHCI port number
    uint8_t present;            // Device present flag
    uint8_t ready;              // Device ready (media present)
    
    // Capacity information
    uint32_t total_blocks;      // Total logical blocks (2048 bytes each)
    uint32_t block_size;        // Block size (usually 2048)
    uint64_t capacity_mb;       // Capacity in MB
    
    // Last sense data
    uint8_t sense_key;          // Last sense key
    uint8_t asc;                // Additional Sense Code
    uint8_t ascq;               // Additional Sense Code Qualifier
    
    // Statistics
    uint64_t reads_completed;
    uint64_t errors;
} satapi_device_t;

// ===========================================================================
// Initialization and Detection
// ===========================================================================

// Initialize SATAPI subsystem (must be called after AHCI init)
int satapi_init(void);

// Shutdown SATAPI subsystem
void satapi_shutdown(void);

// Check if a port has a SATAPI device
int satapi_is_device(uint8_t port_num);

// Get SATAPI device information
satapi_device_t* satapi_get_device(uint8_t port_num);

// ===========================================================================
// Device Operations
// ===========================================================================

// Test if unit is ready (media present and spun up)
int satapi_test_unit_ready(uint8_t port_num);

// Request sense data (retrieve error information)
int satapi_request_sense(uint8_t port_num, uint8_t *sense_data, int sense_len);

// Read capacity (get total blocks and block size)
int satapi_read_capacity(uint8_t port_num, uint32_t *total_blocks, uint32_t *block_size);

// Get last sense information
void satapi_get_sense(uint8_t port_num, uint8_t *sense_key, uint8_t *asc, uint8_t *ascq);

// ===========================================================================
// Read Operations
// ===========================================================================

// Read logical blocks from SATAPI device
// lba: Logical block address
// count: Number of blocks to read (2048 bytes each)
// buffer: Output buffer (must be at least count * 2048 bytes)
int satapi_read_blocks(uint8_t port_num, uint32_t lba, uint32_t count, void *buffer);

// Read single sector/block
int satapi_read_sector(uint8_t port_num, uint32_t lba, void *buffer);

// ===========================================================================
// Utility Functions
// ===========================================================================

// Check if media is present
int satapi_media_present(uint8_t port_num);

// Get sense key string
const char* satapi_get_sense_key_string(uint8_t sense_key);

// Get ASC string
const char* satapi_get_asc_string(uint8_t asc);

// Dump device information
void satapi_dump_device_info(uint8_t port_num);

// ===========================================================================
// Error Codes
// ===========================================================================

#define SATAPI_SUCCESS              0
#define SATAPI_ERR_NOT_INIT         -1
#define SATAPI_ERR_NO_DEVICE        -2
#define SATAPI_ERR_NOT_READY        -3
#define SATAPI_ERR_INVALID_PORT     -4
#define SATAPI_ERR_INVALID_LBA      -5
#define SATAPI_ERR_INVALID_COUNT    -6
#define SATAPI_ERR_NULL_BUFFER      -7
#define SATAPI_ERR_READ_FAILED      -8
#define SATAPI_ERR_TIMEOUT          -9
#define SATAPI_ERR_HARDWARE         -10
#define SATAPI_ERR_NO_MEDIA         -11
#define SATAPI_ERR_SENSE_FAILED     -12

#endif // SATAPI_H
