#include "moduos/drivers/Drive/vDrive.h"
#include "moduos/drivers/Drive/ATA/ata.h"
#include "moduos/drivers/Drive/ATA/atapi.h"
#include "moduos/drivers/Drive/SATA/SATA.h"
#include "moduos/drivers/Drive/SATA/satapi.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/macros.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/drivers/graphics/VGA.h"
#include <stddef.h>

// MBR partition entry structure
typedef struct __attribute__((packed)) {
    uint8_t status;           // 0x80 = bootable, 0x00 = non-bootable
    uint8_t first_chs[3];     // First sector in CHS
    uint8_t partition_type;   // Partition type
    uint8_t last_chs[3];      // Last sector in CHS
    uint32_t first_lba;       // First sector in LBA
    uint32_t sector_count;    // Number of sectors
} mbr_partition_entry_t;

// MBR structure
typedef struct __attribute__((packed)) {
    uint8_t bootstrap[446];
    mbr_partition_entry_t partitions[4];
    uint16_t signature;       // Should be 0xAA55
} mbr_t;

// Global vDrive system state
static vdrive_system_t vdrive_system = {0};

// Last error per drive
static int drive_last_error[VDRIVE_MAX_DRIVES] = {0};

// ===========================================================================
// Helper Functions
// ===========================================================================

static void vdrive_copy_string(char *dest, const char *src, int max_len) {
    int i;
    for (i = 0; i < max_len && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

static void vdrive_trim_string(char *str, int len) {
    int i = len - 1;
    while (i >= 0 && (str[i] == ' ' || str[i] == '\0')) {
        str[i] = '\0';
        i--;
    }
}

static uint64_t vdrive_calc_mb(uint64_t sectors, uint32_t sector_size) {
    return (sectors * sector_size) / (1024 * 1024);
}

static uint64_t vdrive_calc_gb(uint64_t sectors, uint32_t sector_size) {
    return (sectors * sector_size) / (1024 * 1024 * 1024);
}

static int vdrive_string_contains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    
    int needle_len = 0;
    while (needle[needle_len]) needle_len++;
    
    for (int i = 0; haystack[i]; i++) {
        int match = 1;
        for (int j = 0; j < needle_len; j++) {
            if (haystack[i + j] != needle[j]) {
                match = 0;
                break;
            }
        }
        if (match) return 1;
    }
    return 0;
}

// Convert number to string
static void uint32_to_str(uint32_t num, char *buf) {
    if (num == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    
    char temp[16];
    int i = 0;
    while (num > 0) {
        temp[i++] = '0' + (num % 10);
        num /= 10;
    }
    
    int j = 0;
    while (i > 0) {
        buf[j++] = temp[--i];
    }
    buf[j] = '\0';
}

// Get partition type string
static const char* get_partition_type_string(uint8_t type) {
    switch (type) {
        case 0x00: return "Empty";
        case 0x01: return "FAT12";
        case 0x04: return "FAT16 <32M";
        case 0x05: return "Extended";
        case 0x06: return "FAT16";
        case 0x07: return "NTFS/exFAT";
        case 0x0B: return "FAT32";
        case 0x0C: return "FAT32 LBA";
        case 0x0E: return "FAT16 LBA";
        case 0x0F: return "Extended LBA";
        case 0x82: return "Linux swap";
        case 0x83: return "Linux";
        case 0x85: return "Linux ext";
        case 0x8E: return "Linux LVM";
        case 0xA5: return "FreeBSD";
        case 0xA6: return "OpenBSD";
        case 0xA8: return "macOS";
        case 0xAF: return "macOS HFS+";
        case 0xEE: return "GPT";
        case 0xEF: return "EFI System";
        default: return "Unknown";
    }
}

// ===========================================================================
// Drive Registration
// ===========================================================================

static int vdrive_register_ata_drive(int ata_index, const ata_drive_t *ata_drive) {
    if (vdrive_system.drive_count >= VDRIVE_MAX_DRIVES) {
        COM_LOG_WARN(COM1_PORT, "vDrive: Maximum drives reached, skipping ATA drive");
        return -1;
    }
    
    int vdrive_id = vdrive_system.drive_count;
    vdrive_t *vdrive = &vdrive_system.drives[vdrive_id];
    
    vdrive->present = 1;
    vdrive->vdrive_id = vdrive_id;
    vdrive->backend = VDRIVE_BACKEND_ATA;
    vdrive->backend_id = ata_index;
    
    if (ata_drive->is_atapi) {
        vdrive->type = VDRIVE_TYPE_ATA_ATAPI;
        vdrive->read_only = 1;
        vdrive->status = VDRIVE_STATUS_READY;
        vdrive->sector_size = 2048;  // ATAPI uses 2048-byte sectors
        
        com_write_string(COM1_PORT, "[vDrive] ATAPI drive marked as READY (will spin up on first read)\n");
    } else {
        vdrive->type = VDRIVE_TYPE_ATA_HDD;
        vdrive->read_only = 0;
        vdrive->status = VDRIVE_STATUS_READY;
        vdrive->sector_size = 512;
    }
    
    vdrive_copy_string(vdrive->model, ata_drive->model, 40);
    vdrive_trim_string(vdrive->model, 40);
    
    vdrive->serial[0] = '\0';
    vdrive->total_sectors = 0;
    vdrive->capacity_mb = 0;
    vdrive->capacity_gb = 0;
    vdrive->supports_lba48 = 0;
    vdrive->supports_dma = 0;
    
    vdrive->reads = 0;
    vdrive->writes = 0;
    vdrive->errors = 0;
    
    vdrive_system.drive_count++;
    vdrive_system.ata_count++;
    
    return vdrive_id;
}

void vdrive_debug_registration(void) {
    com_write_string(COM1_PORT, "\n=== vDrive Registration Debug ===\n");
    
    for (int i = 0; i < VDRIVE_MAX_DRIVES; i++) {
        if (vdrive_system.drives[i].present) {
            vdrive_t *d = &vdrive_system.drives[i];
            
            com_printf(COM1_PORT, "vDrive %d:\n", i);
            com_printf(COM1_PORT, "  Type: %s (%d)\n", 
                      vdrive_get_type_string(d->type), d->type);
            com_printf(COM1_PORT, "  Backend: %s (%d)\n", 
                      vdrive_get_backend_string(d->backend), d->backend);
            com_printf(COM1_PORT, "  Backend ID: %d\n", d->backend_id);
            com_printf(COM1_PORT, "  Model: %s\n", d->model);
            com_printf(COM1_PORT, "  Sector Size: %u bytes\n", d->sector_size);
            com_printf(COM1_PORT, "  Read Only: %s\n", d->read_only ? "Yes" : "No");
        }
    }
    
    com_write_string(COM1_PORT, "=================================\n\n");
}

static int vdrive_register_sata_drive(int sata_port, const sata_device_t *sata_drive) {
    if (vdrive_system.drive_count >= VDRIVE_MAX_DRIVES) {
        COM_LOG_WARN(COM1_PORT, "vDrive: Maximum drives reached, skipping SATA drive");
        return -1;
    }
    
    int vdrive_id = vdrive_system.drive_count;
    vdrive_t *vdrive = &vdrive_system.drives[vdrive_id];
    
    vdrive->present = 1;
    vdrive->vdrive_id = vdrive_id;
    vdrive->backend = VDRIVE_BACKEND_SATA;
    vdrive->backend_id = sata_port;
    vdrive->status = (sata_drive->status == SATA_STATUS_READY) ? 
                     VDRIVE_STATUS_READY : VDRIVE_STATUS_NOT_PRESENT;
    
    switch (sata_drive->type) {
        case SATA_TYPE_HDD:
            vdrive->type = VDRIVE_TYPE_SATA_HDD;
            vdrive->read_only = 0;
            vdrive->sector_size = 512;
            break;
        case SATA_TYPE_SSD:
            vdrive->type = VDRIVE_TYPE_SATA_SSD;
            vdrive->read_only = 0;
            vdrive->sector_size = 512;
            break;
        case SATA_TYPE_OPTICAL:
            vdrive->type = VDRIVE_TYPE_SATA_OPTICAL;
            vdrive->read_only = 1;
            vdrive->sector_size = 2048;  // SATAPI uses 2048-byte sectors
            // Get capacity from SATAPI if available
            satapi_device_t *satapi_dev = satapi_get_device(sata_port);
            if (satapi_dev && satapi_dev->present) {
                vdrive->total_sectors = satapi_dev->total_blocks;
                vdrive->capacity_mb = satapi_dev->capacity_mb;
                vdrive->capacity_gb = satapi_dev->capacity_mb / 1024;
            }
            break;
        default:
            vdrive->type = VDRIVE_TYPE_UNKNOWN;
            vdrive->read_only = 0;
            vdrive->sector_size = 512;
    }
    
    vdrive_copy_string(vdrive->model, sata_drive->model, 40);
    vdrive_copy_string(vdrive->serial, sata_drive->serial, 20);
    
    // For non-optical drives, use SATA info
    if (sata_drive->type != SATA_TYPE_OPTICAL) {
        vdrive->total_sectors = sata_drive->total_sectors;
        vdrive->capacity_mb = sata_drive->capacity_mb;
        vdrive->capacity_gb = sata_drive->capacity_gb;
    }
    
    vdrive->supports_lba48 = sata_drive->supports_48bit_lba;
    vdrive->supports_dma = sata_drive->supports_dma;
    
    vdrive->reads = 0;
    vdrive->writes = 0;
    vdrive->errors = 0;
    
    vdrive_system.drive_count++;
    vdrive_system.sata_count++;
    
    return vdrive_id;
}

// ===========================================================================
// Initialization
// ===========================================================================

int vdrive_init(void) {
    COM_LOG_INFO(COM1_PORT, "Initializing vDrive subsystem");
    
    for (int i = 0; i < VDRIVE_MAX_DRIVES; i++) {
        vdrive_system.drives[i].present = 0;
        vdrive_system.drives[i].vdrive_id = i;
        vdrive_system.drives[i].status = VDRIVE_STATUS_NOT_PRESENT;
        drive_last_error[i] = VDRIVE_SUCCESS;
    }
    
    vdrive_system.drive_count = 0;
    vdrive_system.ata_count = 0;
    vdrive_system.sata_count = 0;
    
    sata_info_t *sata_info = sata_get_info();
    if (sata_info && sata_info->initialized) {
        com_write_string(COM1_PORT, "[vDrive] Scanning SATA drives...\n");
        
        for (int i = 0; i < 32; i++) {
            sata_device_t *sata_dev = sata_get_device(i);
            if (sata_dev && sata_dev->present) {
                int vdrive_id = vdrive_register_sata_drive(i, sata_dev);
                if (vdrive_id >= 0) {
                    com_write_string(COM1_PORT, "[vDrive] Registered SATA drive as vDrive");
                    char id_str[4];
                    id_str[0] = '0' + vdrive_id;
                    id_str[1] = '\0';
                    com_write_string(COM1_PORT, id_str);
                    com_write_string(COM1_PORT, "\n");
                }
            }
        }
    }
    
    com_write_string(COM1_PORT, "[vDrive] Scanning ATA drives...\n");
    
    for (int i = 0; i < 4; i++) {
        const ata_drive_t *ata_drive = ata_get_drive(i);
        if (ata_drive && ata_drive->exists) {
            int vdrive_id = vdrive_register_ata_drive(i, ata_drive);
            if (vdrive_id >= 0) {
                com_write_string(COM1_PORT, "[vDrive] Registered ATA drive as vDrive");
                char id_str[4];
                id_str[0] = '0' + vdrive_id;
                id_str[1] = '\0';
                com_write_string(COM1_PORT, id_str);
                com_write_string(COM1_PORT, "\n");
            }
        }
    }
    
    com_write_string(COM1_PORT, "[vDrive] Total drives: ");
    char count[4];
    count[0] = '0' + vdrive_system.drive_count;
    count[1] = ' ';
    count[2] = '(';
    count[3] = '\0';
    com_write_string(COM1_PORT, count);
    
    count[0] = '0' + vdrive_system.sata_count;
    count[1] = ' ';
    count[2] = '\0';
    com_write_string(COM1_PORT, count);
    com_write_string(COM1_PORT, "SATA, ");
    
    count[0] = '0' + vdrive_system.ata_count;
    count[1] = ' ';
    count[2] = '\0';
    com_write_string(COM1_PORT, count);
    com_write_string(COM1_PORT, "ATA)\n");
    
    if (vdrive_system.drive_count == 0) {
        COM_LOG_WARN(COM1_PORT, "No drives detected!");
        vdrive_system.initialized = 1;
        return VDRIVE_ERR_NO_DRIVES;
    }
    
    vdrive_system.initialized = 1;
    COM_LOG_OK(COM1_PORT, "vDrive subsystem initialized");
    
    return VDRIVE_SUCCESS;
}

void vdrive_shutdown(void) {
    if (!vdrive_system.initialized) {
        return;
    }
    
    COM_LOG_INFO(COM1_PORT, "Shutting down vDrive subsystem");
    
    for (int i = 0; i < vdrive_system.drive_count; i++) {
        vdrive_flush(i);
    }
    
    vdrive_system.initialized = 0;
    COM_LOG_OK(COM1_PORT, "vDrive subsystem shut down");
}

int vdrive_rescan(void) {
    COM_LOG_INFO(COM1_PORT, "Rescanning drives");
    sata_rescan();
    return vdrive_init();
}

vdrive_system_t* vdrive_get_system_info(void) {
    return &vdrive_system;
}

// ===========================================================================
// Drive Access
// ===========================================================================

vdrive_t* vdrive_get(uint8_t vdrive_id) {
    if (vdrive_id >= VDRIVE_MAX_DRIVES) {
        return NULL;
    }
    
    if (!vdrive_system.drives[vdrive_id].present) {
        return NULL;
    }
    
    return &vdrive_system.drives[vdrive_id];
}

vdrive_t* vdrive_get_first(void) {
    for (int i = 0; i < VDRIVE_MAX_DRIVES; i++) {
        if (vdrive_system.drives[i].present) {
            return &vdrive_system.drives[i];
        }
    }
    return NULL;
}

int vdrive_get_count(void) {
    return vdrive_system.drive_count;
}

int vdrive_is_ready(uint8_t vdrive_id) {
    if (!vdrive_system.initialized) {
        return 0;
    }
    
    vdrive_t *drive = vdrive_get(vdrive_id);
    if (!drive) {
        return 0;
    }
    
    if (drive->type == VDRIVE_TYPE_ATA_ATAPI || 
        drive->type == VDRIVE_TYPE_SATA_OPTICAL) {
        return drive->present;
    }
    
    return (drive->status == VDRIVE_STATUS_READY);
}

vdrive_t* vdrive_find_by_model(const char *model_substring) {
    if (!model_substring) {
        return NULL;
    }
    
    for (int i = 0; i < VDRIVE_MAX_DRIVES; i++) {
        if (vdrive_system.drives[i].present) {
            if (vdrive_string_contains(vdrive_system.drives[i].model, model_substring)) {
                return &vdrive_system.drives[i];
            }
        }
    }
    
    return NULL;
}

// ===========================================================================
// Read/Write Operations
// ===========================================================================

int vdrive_read(uint8_t vdrive_id, uint64_t lba, uint32_t count, void *buffer) {
    if (!vdrive_system.initialized) {
        return VDRIVE_ERR_NOT_INIT;
    }
    
    vdrive_t *drive = vdrive_get(vdrive_id);
    if (!drive) {
        return VDRIVE_ERR_NO_DRIVE;
    }

    /*
     * Optical drives (ATAPI/SATAPI) often report non-READY status until the first
     * media access triggers spin-up. Treat "present" as readable for them.
     */
    int is_optical = (drive->type == VDRIVE_TYPE_ATA_ATAPI ||
                      drive->type == VDRIVE_TYPE_SATA_OPTICAL);

    if (!is_optical) {
        if (drive->status != VDRIVE_STATUS_READY) {
            drive_last_error[vdrive_id] = VDRIVE_ERR_NOT_READY;
            return VDRIVE_ERR_NOT_READY;
        }
    } else {
        if (!drive->present) {
            drive_last_error[vdrive_id] = VDRIVE_ERR_NO_DRIVE;
            return VDRIVE_ERR_NO_DRIVE;
        }
        /* If present but not marked READY yet, allow the read path to proceed. */
        if (drive->status != VDRIVE_STATUS_READY) {
            drive->status = VDRIVE_STATUS_READY;
        }
    }

    if (count == 0) {
        drive_last_error[vdrive_id] = VDRIVE_ERR_INVALID_COUNT;
        return VDRIVE_ERR_INVALID_COUNT;
    }
    
    if (buffer == NULL) {
        drive_last_error[vdrive_id] = VDRIVE_ERR_NULL_BUFFER;
        return VDRIVE_ERR_NULL_BUFFER;
    }
    
    int result = -1;
    drive->status = VDRIVE_STATUS_BUSY;
    
    // ========================================================================
    // CRITICAL: Route based on SPECIFIC drive type first, then backend
    // ========================================================================
    
    if (drive->type == VDRIVE_TYPE_SATA_OPTICAL) {
        // SATAPI - SATA optical drive with 2048-byte sectors
        
        if (drive->backend != VDRIVE_BACKEND_SATA) {
            com_write_string(COM1_PORT, "[vDrive] ERROR: SATAPI drive has wrong backend!\n");
            drive->status = VDRIVE_STATUS_ERROR;
            drive_last_error[vdrive_id] = VDRIVE_ERR_BACKEND;
            return VDRIVE_ERR_BACKEND;
        }
        
        // IMPORTANT: SATAPI uses 2048-byte sectors, but the LBA and count 
        // need to be converted if the caller is using 512-byte addressing
        // For simplicity, assume caller is using 2048-byte addressing
        result = satapi_read_blocks(drive->backend_id, (uint32_t)lba, count, buffer);
        result = (result == SATAPI_SUCCESS) ? 0 : -1;
    }
    else if (drive->type == VDRIVE_TYPE_ATA_ATAPI) {
        // ATAPI - ATA optical drive with 2048-byte sectors
        
        if (drive->backend != VDRIVE_BACKEND_ATA) {
            com_write_string(COM1_PORT, "[vDrive] ERROR: ATAPI drive has wrong backend!\n");
            drive->status = VDRIVE_STATUS_ERROR;
            drive_last_error[vdrive_id] = VDRIVE_ERR_BACKEND;
            return VDRIVE_ERR_BACKEND;
        }
        
        result = atapi_read_blocks_pio(drive->backend_id, (uint32_t)lba, count, buffer);
    }
    else if (drive->backend == VDRIVE_BACKEND_SATA) {
        // SATA hard drive or SSD with 512-byte sectors
        result = sata_read(drive->backend_id, lba, count, buffer);
        result = (result == SATA_SUCCESS) ? 0 : -1;
    }
    else if (drive->backend == VDRIVE_BACKEND_ATA) {
        // ATA hard drive with 512-byte sectors
        result = ata_read_sectors(drive->backend_id, (uint32_t)lba, buffer, count);
    }
    else {
        com_write_string(COM1_PORT, "[vDrive] ERROR: Unknown backend type!\n");
        drive->status = VDRIVE_STATUS_ERROR;
        drive_last_error[vdrive_id] = VDRIVE_ERR_BACKEND;
        return VDRIVE_ERR_BACKEND;
    }
    
    if (result == 0) {
        drive->reads++;
        drive->status = VDRIVE_STATUS_READY;
        drive_last_error[vdrive_id] = VDRIVE_SUCCESS;
        return VDRIVE_SUCCESS;
    } else {
        drive->errors++;
        drive->status = VDRIVE_STATUS_ERROR;
        drive_last_error[vdrive_id] = VDRIVE_ERR_READ_FAILED;
        return VDRIVE_ERR_READ_FAILED;
    }
}

int vdrive_write(uint8_t vdrive_id, uint64_t lba, uint32_t count, const void *buffer) {
    if (!vdrive_system.initialized) {
        return VDRIVE_ERR_NOT_INIT;
    }
    
    vdrive_t *drive = vdrive_get(vdrive_id);
    if (!drive) {
        return VDRIVE_ERR_NO_DRIVE;
    }
    
    if (drive->read_only) {
        drive_last_error[vdrive_id] = VDRIVE_ERR_READ_ONLY;
        return VDRIVE_ERR_READ_ONLY;
    }
    
    if (drive->status != VDRIVE_STATUS_READY) {
        drive_last_error[vdrive_id] = VDRIVE_ERR_NOT_READY;
        return VDRIVE_ERR_NOT_READY;
    }
    
    if (count == 0) {
        drive_last_error[vdrive_id] = VDRIVE_ERR_INVALID_COUNT;
        return VDRIVE_ERR_INVALID_COUNT;
    }
    
    if (buffer == NULL) {
        drive_last_error[vdrive_id] = VDRIVE_ERR_NULL_BUFFER;
        return VDRIVE_ERR_NULL_BUFFER;
    }
    
    int result = -1;
    drive->status = VDRIVE_STATUS_BUSY;
    
    if (drive->backend == VDRIVE_BACKEND_SATA) {
        result = sata_write(drive->backend_id, lba, count, buffer);
        result = (result == SATA_SUCCESS) ? 0 : -1;
    } else if (drive->backend == VDRIVE_BACKEND_ATA) {
        const uint8_t *buf = (const uint8_t*)buffer;
        for (uint32_t i = 0; i < count; i++) {
            result = ata_write_sector_lba28(drive->backend_id, (uint32_t)(lba + i), buf + (i * 512));
            if (result != 0) {
                break;
            }
        }
    } else {
        drive->status = VDRIVE_STATUS_ERROR;
        drive_last_error[vdrive_id] = VDRIVE_ERR_BACKEND;
        return VDRIVE_ERR_BACKEND;
    }
    
    if (result == 0) {
        drive->writes++;
        drive->status = VDRIVE_STATUS_READY;
        drive_last_error[vdrive_id] = VDRIVE_SUCCESS;
        return VDRIVE_SUCCESS;
    } else {
        drive->errors++;
        drive->status = VDRIVE_STATUS_ERROR;
        drive_last_error[vdrive_id] = VDRIVE_ERR_WRITE_FAILED;
        return VDRIVE_ERR_WRITE_FAILED;
    }
}

int vdrive_read_sector(uint8_t vdrive_id, uint64_t lba, void *buffer) {
    return vdrive_read(vdrive_id, lba, 1, buffer);
}

int vdrive_write_sector(uint8_t vdrive_id, uint64_t lba, const void *buffer) {
    return vdrive_write(vdrive_id, lba, 1, buffer);
}

// ===========================================================================
// Utility Functions
// ===========================================================================

const char* vdrive_get_type_string(vdrive_type_t type) {
    switch (type) {
        case VDRIVE_TYPE_ATA_HDD: return "ATA HDD";
        case VDRIVE_TYPE_ATA_ATAPI: return "ATA CD/DVD";
        case VDRIVE_TYPE_SATA_HDD: return "SATA HDD";
        case VDRIVE_TYPE_SATA_SSD: return "SATA SSD";
        case VDRIVE_TYPE_SATA_OPTICAL: return "SATA CD/DVD";
        case VDRIVE_TYPE_UNKNOWN: return "Unknown";
        default: return "None";
    }
}

const char* vdrive_get_backend_string(vdrive_backend_t backend) {
    switch (backend) {
        case VDRIVE_BACKEND_ATA: return "ATA";
        case VDRIVE_BACKEND_SATA: return "SATA";
        default: return "None";
    }
}

const char* vdrive_get_status_string(vdrive_status_t status) {
    switch (status) {
        case VDRIVE_STATUS_READY: return "Ready";
        case VDRIVE_STATUS_BUSY: return "Busy";
        case VDRIVE_STATUS_ERROR: return "Error";
        default: return "Not Present";
    }
}

void vdrive_format_capacity(uint64_t capacity_mb, char *buffer, int size) {
    sata_format_capacity(capacity_mb, buffer, size);
}

void vdrive_dump_info(uint8_t vdrive_id) {
    vdrive_t *drive = vdrive_get(vdrive_id);
    
    if (!drive) {
        com_write_string(COM1_PORT, "[vDrive] Invalid drive ID\n");
        return;
    }
    
    com_write_string(COM1_PORT, "\n=== vDrive Information ===\n");
    com_write_string(COM1_PORT, "Drive ID:     ");
    char id[4];
    id[0] = '0' + vdrive_id;
    id[1] = '\0';
    com_write_string(COM1_PORT, id);
    com_write_string(COM1_PORT, "\n");
    
    com_write_string(COM1_PORT, "Type:         ");
    com_write_string(COM1_PORT, vdrive_get_type_string(drive->type));
    com_write_string(COM1_PORT, "\n");
    
    com_write_string(COM1_PORT, "Backend:      ");
    com_write_string(COM1_PORT, vdrive_get_backend_string(drive->backend));
    com_write_string(COM1_PORT, " (ID: ");
    id[0] = '0' + drive->backend_id;
    com_write_string(COM1_PORT, id);
    com_write_string(COM1_PORT, ")\n");
    
    com_write_string(COM1_PORT, "Status:       ");
    com_write_string(COM1_PORT, vdrive_get_status_string(drive->status));
    com_write_string(COM1_PORT, "\n");
    
    com_write_string(COM1_PORT, "Model:        ");
    com_write_string(COM1_PORT, drive->model);
    com_write_string(COM1_PORT, "\n");
    
    if (drive->serial[0] != '\0') {
        com_write_string(COM1_PORT, "Serial:       ");
        com_write_string(COM1_PORT, drive->serial);
        com_write_string(COM1_PORT, "\n");
    }
    
    if (drive->capacity_mb > 0) {
        char cap[64];
        vdrive_format_capacity(drive->capacity_mb, cap, 64);
        com_write_string(COM1_PORT, "Capacity:     ");
        com_write_string(COM1_PORT, cap);
        com_write_string(COM1_PORT, "\n");
    }
    
    com_write_string(COM1_PORT, "==========================\n\n");
}

void vdrive_dump_all(void) {
    com_write_string(COM1_PORT, "\n=== All vDrives ===\n");
    
    if (!vdrive_system.initialized) {
        com_write_string(COM1_PORT, "vDrive not initialized\n");
        return;
    }
    
    if (vdrive_system.drive_count == 0) {
        com_write_string(COM1_PORT, "No drives present\n");
        return;
    }
    
    for (int i = 0; i < VDRIVE_MAX_DRIVES; i++) {
        if (vdrive_system.drives[i].present) {
            vdrive_dump_info(i);
        }
    }
}

void vdrive_print_table(void) {
    VGA_Write("\n=== Drive and Partition Table ===\n\n");
    
    for (int i = 0; i < VDRIVE_MAX_DRIVES; i++) {
        if (vdrive_system.drives[i].present) {
            vdrive_t *d = &vdrive_system.drives[i];
            
            // Print drive header
            VGA_Write("Drive ");
            char id[4];
            id[0] = '0' + i;
            id[1] = '\0';
            VGA_Write(id);
            VGA_Write(": ");
            VGA_Write(d->model);
            VGA_Write(" (");
            VGA_Write(vdrive_get_type_string(d->type));
            VGA_Write(" - ");
            VGA_Write(vdrive_get_backend_string(d->backend));
            VGA_Write(")\n");
            
            //Check for ALL optical drive types
            if (d->type == VDRIVE_TYPE_ATA_ATAPI || 
                d->type == VDRIVE_TYPE_SATA_OPTICAL ||
                d->read_only) {
                VGA_Write("  [Optical Drive - No Partitions]\n\n");
                continue;
            }
            
            // Also skip if sector size is not 512 (safety check)
            if (d->sector_size != 512) {
                VGA_Write("  [Non-standard sector size - Skipping partition scan]\n\n");
                continue;
            }
            
            // Try to read MBR
            mbr_t *mbr = (mbr_t*)kmalloc(512);
            if (!mbr) {
                VGA_Write("  [Memory allocation failed]\n\n");
                continue;
            }
            
            int read_result = vdrive_read_sector(i, 0, mbr);
            if (read_result != 0) {
                VGA_Write("  [Cannot read MBR - Error code: ");
                char err[8];
                err[0] = '0' + (-read_result);
                err[1] = ']';
                err[2] = '\n';
                err[3] = '\n';
                err[4] = '\0';
                VGA_Write(err);
                kfree(mbr);
                continue;
            }
            
            // Verify MBR signature
            if (mbr->signature != 0xAA55) {
                VGA_Write("  [Invalid MBR signature - No partition table]\n\n");
                kfree(mbr);
                continue;
            }
            
            // Print partition table header
            VGA_Write("  +------+------+------------+---------------+--------------+\n");
            VGA_Write("  | Part | Boot | Type       | Start LBA     | Size (MB)    |\n");
            VGA_Write("  +------+------+------------+---------------+--------------+\n");
            
            int found_partition = 0;
            for (int p = 0; p < 4; p++) {
                mbr_partition_entry_t *part = &mbr->partitions[p];
                
                if (part->partition_type == 0x00) {
                    continue; // Empty partition
                }
                
                found_partition = 1;
                
                VGA_Write("  |  ");
                char num[4];
                num[0] = '0' + (p + 1);
                num[1] = ' ';
                num[2] = ' ';
                num[3] = '\0';
                VGA_Write(num);
                VGA_Write(" | ");
                
                // Boot flag
                if (part->status == 0x80) {
                    VGA_Write(" Yes");
                } else {
                    VGA_Write("  No");
                }
                VGA_Write(" | ");
                
                // Partition type (pad to 10 chars)
                const char *type_str = get_partition_type_string(part->partition_type);
                VGA_Write(type_str);
                int type_len = 0;
                while (type_str[type_len]) type_len++;
                for (int pad = type_len; pad < 10; pad++) {
                    VGA_Write(" ");
                }
                VGA_Write(" | ");
                
                // Start LBA (pad to 13 chars)
                char lba_str[16];
                uint32_to_str(part->first_lba, lba_str);
                int lba_len = 0;
                while (lba_str[lba_len]) lba_len++;
                for (int pad = lba_len; pad < 13; pad++) {
                    VGA_Write(" ");
                }
                VGA_Write(lba_str);
                VGA_Write(" | ");
                
                // Size in MB (pad to 12 chars)
                uint64_t size_mb = ((uint64_t)part->sector_count * 512) / (1024 * 1024);
                char size_str[16];
                uint32_to_str((uint32_t)size_mb, size_str);
                int size_len = 0;
                while (size_str[size_len]) size_len++;
                for (int pad = size_len; pad < 12; pad++) {
                    VGA_Write(" ");
                }
                VGA_Write(size_str);
                VGA_Write(" |\n");
            }
            
            if (!found_partition) {
                VGA_Write("  |                    [No Partitions Found]                    |\n");
            }
            
            VGA_Write("  +------+------+------------+---------------+--------------+\n\n");
            
            kfree(mbr);
        }
    }
    
    VGA_Write("=================================\n\n");
}

// ===========================================================================
// Advanced Operations
// ===========================================================================

int vdrive_flush(uint8_t vdrive_id) {
    vdrive_t *drive = vdrive_get(vdrive_id);
    if (!drive || !vdrive_is_ready(vdrive_id)) {
        return VDRIVE_ERR_NOT_READY;
    }
    
    if (drive->backend == VDRIVE_BACKEND_SATA) {
        return sata_flush(drive->backend_id);
    }
    
    return VDRIVE_SUCCESS;
}

void vdrive_get_stats(uint8_t vdrive_id, uint64_t *reads, uint64_t *writes, uint64_t *errors) {
    vdrive_t *drive = vdrive_get(vdrive_id);
    if (!drive) {
        if (reads) *reads = 0;
        if (writes) *writes = 0;
        if (errors) *errors = 0;
        return;
    }
    
    if (reads) *reads = drive->reads;
    if (writes) *writes = drive->writes;
    if (errors) *errors = drive->errors;
}

void vdrive_reset_stats(uint8_t vdrive_id) {
    vdrive_t *drive = vdrive_get(vdrive_id);
    if (drive) {
        drive->reads = 0;
        drive->writes = 0;
        drive->errors = 0;
    }
}

// ===========================================================================
// Error Handling
// ===========================================================================

int vdrive_get_last_error(uint8_t vdrive_id) {
    if (vdrive_id >= VDRIVE_MAX_DRIVES) {
        return VDRIVE_ERR_INVALID_ID;
    }
    return drive_last_error[vdrive_id];
}

void vdrive_clear_error(uint8_t vdrive_id) {
    if (vdrive_id < VDRIVE_MAX_DRIVES) {
        drive_last_error[vdrive_id] = VDRIVE_SUCCESS;
        if (vdrive_system.drives[vdrive_id].present) {
            vdrive_system.drives[vdrive_id].status = VDRIVE_STATUS_READY;
        }
    }
}

const char* vdrive_get_error_string(int error_code) {
    switch (error_code) {
        case VDRIVE_SUCCESS: return "Success";
        case VDRIVE_ERR_NOT_INIT: return "vDrive not initialized";
        case VDRIVE_ERR_NO_DRIVE: return "No drive present";
        case VDRIVE_ERR_NOT_READY: return "Drive not ready";
        case VDRIVE_ERR_INVALID_ID: return "Invalid drive ID";
        case VDRIVE_ERR_INVALID_LBA: return "Invalid LBA";
        case VDRIVE_ERR_INVALID_COUNT: return "Invalid sector count";
        case VDRIVE_ERR_NULL_BUFFER: return "NULL buffer";
        case VDRIVE_ERR_READ_FAILED: return "Read failed";
        case VDRIVE_ERR_WRITE_FAILED: return "Write failed";
        case VDRIVE_ERR_READ_ONLY: return "Read-only drive";
        case VDRIVE_ERR_BACKEND: return "Backend error";
        case VDRIVE_ERR_NO_DRIVES: return "No drives found";
        default: return "Unknown error";
    }
}