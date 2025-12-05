#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include <stddef.h>
#include "moduos/kernel/io/io.h"
#include "moduos/kernel/interrupts/irq.h"
#include "moduos/kernel/interrupts/pic.h"

/* Ports for primary/secondary channels */
#define ATA_PRIMARY_BASE    0x1F0
#define ATA_PRIMARY_CTRL    0x3F6
#define ATA_SECONDARY_BASE  0x170
#define ATA_SECONDARY_CTRL  0x376

/* Registers (offsets from base) */
#define REG_DATA     0
#define REG_ERROR    1
#define REG_SECCNT   2
#define REG_LBA_LO   3
#define REG_LBA_MID  4
#define REG_LBA_HI   5
#define REG_DRIVE    6
#define REG_STATUS   7
#define REG_COMMAND  7

/* Control register offsets (from ctrl port) */
#define REG_ALTSTATUS 0
#define REG_DEVCONTROL 0

/* Commands */
#define ATA_CMD_IDENTIFY     0xEC
#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_CMD_READ_PIO     0x20
#define ATA_CMD_READ_PIO_EXT 0x24
#define ATA_CMD_WRITE_PIO    0x30
#define ATA_CMD_CACHE_FLUSH  0xE7

#ifndef ATA_CMD_PACKET
#define ATA_CMD_PACKET 0xA0
#endif

/* Status bits */
#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF   0x20
#define ATA_SR_DSC  0x10
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

/* Drive selection */
#define ATA_DRIVE_MASTER 0x00
#define ATA_DRIVE_SLAVE  0x01

/* Simple drive descriptor */
typedef struct {
    int exists;
    int is_atapi;
    uint8_t channel; /* 0 = primary, 1 = secondary */
    uint8_t drive;   /* 0 = master, 1 = slave */
    char model[41];  /* ascii model string, null terminated */
} ata_drive_t;

typedef struct {
    uint8_t bootable;
    uint8_t type;
    uint32_t start_lba;
    uint32_t size_sectors;
} ata_partition_t;

/* Function declarations */
int ata_init();
int ata_read_sector_lba28(int drive_index, uint32_t lba, void* buffer);
int ata_write_sector_lba28(int drive_index, uint32_t lba, const void* buffer);
int ata_read_sectors(int drive_index, uint32_t lba, void* buffer, uint32_t sector_count);
void ata_print_drives(void);
const ata_drive_t* ata_get_drive(int drive_index);

#endif /* ATA_H */
