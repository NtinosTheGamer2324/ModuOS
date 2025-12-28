#include "moduos/drivers/Drive/ATA/ata.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/drivers/Time/RTC.h"
#include "moduos/kernel/memory/string.h"
#include <stdint.h>

/* ATAPI logical block size for data CDs (ISO9660): 2048 bytes */
#define ATAPI_SECTOR_SIZE 2048U

/* polling constants */
#define ATAPI_DRQ_POLL_LOOPS 500000  
#define ATAPI_PACKET_POLL_LOOPS 500000
#define ATAPI_ATTEMPTS 5
#define ATAPI_SPINUP_WAIT_SECONDS 1 /* wait 1s between spinup retries */

/* ATA registers offsets are defined in ata.h (REG_*, ATA_PRIMARY_BASE, etc.) */

/* small I/O delay via altstatus read */
static inline void atapi_io_delay(uint16_t ctrl_port) {
    (void)inb(ctrl_port + REG_ALTSTATUS);
}

/* map drive_index -> base & ctrl ports and drive select value (0xA0 master / 0xB0 slave) */
static void atapi_get_ports(int drive_index, uint16_t *base, uint16_t *ctrl, uint8_t *drive_sel) {
    int channel = (drive_index / 2); /* 0 primary, 1 secondary */
    int drv = (drive_index % 2);     /* 0 master, 1 slave */
    *base = (channel == 0) ? ATA_PRIMARY_BASE : ATA_SECONDARY_BASE;
    *ctrl = (channel == 0) ? ATA_PRIMARY_CTRL : ATA_SECONDARY_CTRL;
    *drive_sel = (drv == 0) ? 0xA0 : 0xB0;
}

/* wait until not busy and optionally require DRQ set (wait_drq=1).
   timeout_loops specifies how many polls. Returns 0 success, -1 timeout. */
static int atapi_poll_status(uint16_t base, uint16_t ctrl, int wait_drq, uint32_t timeout_loops) {
    uint32_t loops = 0;
    while (loops++ < timeout_loops) {
        uint8_t s = inb(base + REG_STATUS);
        if (!(s & ATA_SR_BSY)) {
            if (wait_drq) {
                if (s & ATA_SR_DRQ) return 0;
            } else {
                return 0;
            }
        }
        atapi_io_delay(ctrl);
    }
    return -1;
}

/* Send 12-byte ATAPI packet (as 6 words) after preparing registers.
   expected_bytes is transfer size in bytes (used to program SECCNT/LBA regs).
   Returns 0 on success (DRQ accepted), negative on error. */
static int atapi_send_packet(uint16_t base, uint16_t ctrl, uint8_t drive_sel, uint8_t *packet12, uint32_t expected_bytes) {
    /* select drive/head */
    outb(base + REG_DRIVE, drive_sel);
    for (int i=0;i<4;i++) atapi_io_delay(ctrl);

    /* Set byte count limit in CYLINDER registers (LBA_MID/HI)
     * NOT in sector count! ATAPI uses these to know max bytes per DRQ block.
     * Typical value is 0xF000 (or the actual transfer size if smaller) */
    uint16_t byte_count = (expected_bytes > 0xF000) ? 0xF000 : (uint16_t)expected_bytes;
    
    outb(base + REG_SECCNT, 0);      /* Features register = 0 for PIO */
    outb(base + REG_LBA_LO, 0);      /* Reserved, must be 0 */
    outb(base + REG_LBA_MID, (uint8_t)(byte_count & 0xFF));       /* Byte count LOW */
    outb(base + REG_LBA_HI, (uint8_t)((byte_count >> 8) & 0xFF)); /* Byte count HIGH */

    /* Issue PACKET command */
    outb(base + REG_COMMAND, ATA_CMD_PACKET);
    
    /* CRITICAL FIX #2: Read status register to acknowledge/clear the interrupt */
    (void)inb(base + REG_STATUS);

    /* Small delay for device to process */
    for (int i=0;i<15;i++) atapi_io_delay(ctrl);
    
    /* Wait for DRQ to accept the packet */
    if (atapi_poll_status(base, ctrl, 1, ATAPI_PACKET_POLL_LOOPS) != 0) {
        return -2;
    }

    /* Write the 12-byte packet as 6 words */
    for (int i = 0; i < 6; ++i) {
        uint16_t w = (uint16_t)(packet12[i*2] | (packet12[i*2 + 1] << 8));
        outw(base + REG_DATA, w);
    }
    
    /* Small delay after packet transmission */
    for (int i=0;i<4;i++) atapi_io_delay(ctrl);
    
    return 0;
}


/* Helper: REQUEST SENSE (6 or 12? we'll use 6 byte REQUEST SENSE = 0x03),
   allocate 18 bytes of sense data into buf (buf must be >= 18). */
static int atapi_request_sense(uint16_t base, uint16_t ctrl, uint8_t drive_sel, uint8_t *buf, int buf_len) {
    if (buf_len < 18) return -1;
    uint8_t packet[12];
    memset(packet, 0, sizeof(packet));
    packet[0] = 0x03; /* REQUEST SENSE */
    packet[4] = 18;   /* allocation length */

    if (atapi_send_packet(base, ctrl, drive_sel, packet, 18) != 0) {
        return -2;
    }

    /* wait for DRQ, then read 18 bytes (as words) */
    if (atapi_poll_status(base, ctrl, 1, ATAPI_DRQ_POLL_LOOPS) != 0) {
        return -3;
    }

    /* read 18 bytes -> 9 words via insw */
    insw(base + REG_DATA, buf, 9);
    return 0;
}

/* High-level: TEST UNIT READY -- returns 0 if ready, negative on error.
   It will poll quickly a few times; caller can choose to sleep between attempts. */
static int atapi_test_unit_ready_simple(uint16_t base, uint16_t ctrl, uint8_t drive_sel) {
    uint8_t packet[12];
    memset(packet, 0, sizeof(packet));
    packet[0] = 0x00; /* TEST UNIT READY */

    /* send packet (expected_bytes = 0) */
    if (atapi_send_packet(base, ctrl, drive_sel, packet, 0) != 0) {
        return -1;
    }

    /* After TEST UNIT READY many devices will simply clear BSY; wait a short time */
    if (atapi_poll_status(base, ctrl, 0, ATAPI_PACKET_POLL_LOOPS / 10) != 0) {
        return -2;
    }

    /* Inspect status for ERR bit; if set, request sense */
    uint8_t st = inb(base + REG_STATUS);
    if (st & ATA_SR_ERR) return -3;
    return 0;
}

/* Public: read `count` logical blocks (2048-bytes each) from ATAPI drive.
   drive_index 0..3, lba is logical block number on CD, out must be count*2048 bytes.
   Returns 0 success, negative error. */
int atapi_read_blocks_pio(int drive_index, uint32_t lba, uint32_t count, void* out) {
    if (!out) return -1;
    if (count == 0) return 0;
    if (drive_index < 0 || drive_index > 3) return -1;

    uint16_t base, ctrl;
    uint8_t drive_sel;
    atapi_get_ports(drive_index, &base, &ctrl, &drive_sel);

    /* prepare SCSI READ(10) packet (12 bytes) */
    uint8_t packet[12];
    memset(packet, 0, sizeof(packet));
    packet[0] = 0x28; /* READ(10) */
    /* LBA is big-endian in SCSI */
    packet[2] = (uint8_t)((lba >> 24) & 0xFF);
    packet[3] = (uint8_t)((lba >> 16) & 0xFF);
    packet[4] = (uint8_t)((lba >> 8) & 0xFF);
    packet[5] = (uint8_t)(lba & 0xFF);
    /* transfer length (blocks) 16-bit big-endian in bytes 7..8 */
    packet[7] = (uint8_t)((count >> 8) & 0xFF);
    packet[8] = (uint8_t)(count & 0xFF);

    /* total expected bytes */
    uint32_t total_bytes = count * ATAPI_SECTOR_SIZE;
    uint8_t *dst = (uint8_t*)out;

    /* Try multiple attempts to handle spin-up */
    for (int attempt = 0; attempt < ATAPI_ATTEMPTS; ++attempt) {
        /* TEST UNIT READY first */
        int tur = atapi_test_unit_ready_simple(base, ctrl, drive_sel);
        if (tur != 0) {
            /* Not ready: wait a second to let drive spin up, retry */
            com_printf(COM1_PORT, "ATAPI: drive %d not ready (attempt %d) tur=%d - waiting\n", drive_index, attempt+1, tur);
            rtc_wait_seconds(ATAPI_SPINUP_WAIT_SECONDS);
            continue;
        }

        /* send the READ(10) packet */
        int s = atapi_send_packet(base, ctrl, drive_sel, packet, total_bytes);
        if (s != 0) {
            com_printf(COM1_PORT, "ATAPI: PACKET not accepted on drive %d (attempt %d)\n", drive_index, attempt+1);
            rtc_wait_seconds(ATAPI_SPINUP_WAIT_SECONDS);
            continue;
        }

        /* read blocks one-by-one as device DRQs them */
        uint32_t blocks_left = count;
        uint32_t block_idx = 0;
        int failed = 0;

        while (blocks_left > 0) {
            if (atapi_poll_status(base, ctrl, 1, ATAPI_DRQ_POLL_LOOPS) != 0) {
                com_printf(COM1_PORT, "ATAPI: data DRQ timeout drive %d (block %u)\n", drive_index, block_idx);
                failed = 1;
                break;
            }

            /* read one logical block (2048 bytes) using insw (2048/2 = 1024 words) */
            insw(base + REG_DATA, dst + block_idx * ATAPI_SECTOR_SIZE, ATAPI_SECTOR_SIZE / 2);

            ++block_idx;
            --blocks_left;
            /* small altstatus delay to deglitch */
            atapi_io_delay(ctrl);
        }

        if (!failed) {
            /* Success */
            return 0;
        }

        /* If failed, request sense to log and then retry after a wait */
        uint8_t sense[18];
        if (atapi_request_sense(base, ctrl, drive_sel, sense, sizeof(sense)) == 0) {
            com_printf(COM1_PORT, "ATAPI: REQUEST SENSE (drive %d): key=%02x asc=%02x ascq=%02x\n",
                       drive_index, sense[2] & 0x0F, sense[12], sense[13]);
        } else {
            com_printf(COM1_PORT, "ATAPI: REQUEST SENSE failed (drive %d)\n", drive_index);
        }
        rtc_wait_seconds(ATAPI_SPINUP_WAIT_SECONDS);
    }

    com_printf(COM1_PORT, "ATAPI: read failed permanently (drive %d, lba=%u, count=%u)\n", drive_index, lba, count);
    return -3;
}

/* Convenience: read single sector (2048 bytes) */
int atapi_read_sector(int drive_index, uint32_t lba, void* out) {
    return atapi_read_blocks_pio(drive_index, lba, 1, out);
}
