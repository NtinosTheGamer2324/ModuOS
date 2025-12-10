#include "moduos/drivers/Drive/ATA/ata.h"
#include "moduos/kernel/io/io.h"
#include "moduos/kernel/interrupts/irq.h"
#include "moduos/kernel/interrupts/pic.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/drivers/graphics/VGA.h"
#include <stdint.h>
#include <stddef.h>

/* Internal state */
static ata_drive_t ata_drives[4];

/* Pending request per channel (0 = primary, 1 = secondary) */
typedef struct {
    volatile int active;      /* 1 = request in-flight */
    volatile int completed;   /* 1 = handler completed successfully, -1 = error */
    void *buffer;             /* destination buffer (512 bytes) */
    int drive_index;          /* 0..3 */
} ata_pending_t;

static ata_pending_t pending[2];

/* Map drive_index (0..3) -> channel/base/ctrl/irq/drive */
static inline uint16_t ata_base_for(int drive_index) {
    return (drive_index < 2) ? ATA_PRIMARY_BASE : ATA_SECONDARY_BASE;
}
static inline uint16_t ata_ctrl_for(int drive_index) {
    return (drive_index < 2) ? ATA_PRIMARY_CTRL : ATA_SECONDARY_CTRL;
}
static inline int ata_irq_for_channel(int channel) {
    return (channel == 0) ? 14 : 15; /* primary IRQ14, secondary IRQ15 */
}
static inline uint8_t ata_drive_bit_for(int drive_index) {
    return (drive_index & 1);
}

/* I/O helpers */
static inline uint8_t read_status(uint16_t base) {
    return inb(base + REG_STATUS);
}
static inline void write_command(uint16_t base, uint8_t cmd) {
    outb(base + REG_COMMAND, cmd);
}
static inline void write_control(uint16_t ctrl, uint8_t val) {
    outb(ctrl + REG_DEVCONTROL, val);
}
static inline void select_drive(uint16_t base, uint8_t drive, uint8_t lba_high4) {
    uint8_t val = 0xE0 | (drive << 4) | (lba_high4 & 0x0F);
    outb(base + REG_DRIVE, val);
}

/* Poll utilities (used for IDENTIFY and some waits) */
static int ata_wait_not_busy(uint16_t base) {
    for (int i = 0; i < 100000; ++i) {
        uint8_t s = read_status(base);
        if (!(s & ATA_SR_BSY)) return 0;
    }
    return -1;
}
static int ata_wait_drq(uint16_t base) {
    for (int i = 0; i < 100000; ++i) {
        uint8_t s = read_status(base);
        if (s & ATA_SR_ERR) return -2;
        if (s & ATA_SR_DRQ) return 0;
        if (s & ATA_SR_BSY) continue;
    }
    return -1;
}

/* Wrapper to read words from data port */
static void ata_insw(uint16_t base, void* buf, int word_count) {
    insw(base + REG_DATA, buf, word_count);
}

static void ata_decode_identify_string(char *out, const uint16_t *words, int word_count) {
    int j = 0;
    for (int i = 0; i < word_count; ++i) {
        uint16_t w = words[i];
        /* ATA stores each word with high byte first then low byte in string fields */
        out[j++] = (char)(w >> 8);
        out[j++] = (char)(w & 0xFF);
    }
    out[j] = '\0';

    /* Trim trailing spaces */
    while (j > 0 && out[j-1] == ' ') { out[--j] = '\0'; }

    /* Trim leading spaces by shifting left */
    int start = 0;
    while (out[start] == ' ') ++start;
    if (start > 0) {
        int k = 0;
        while (out[start]) out[k++] = out[start++];
        out[k] = '\0';
    }
}

/* IDENTIFY (same approach as before) */
static int ata_identify_drive(int drive_index) {
    uint16_t base = ata_base_for(drive_index);
    uint8_t drive = ata_drive_bit_for(drive_index);

    select_drive(base, drive, 0);
    io_wait();

    /* clear registers as per spec */
    outb(base + REG_SECCNT, 0);
    outb(base + REG_LBA_LO, 0);
    outb(base + REG_LBA_MID, 0);
    outb(base + REG_LBA_HI, 0);

    /* Try ATA IDENTIFY first (0xEC) */
    write_command(base, ATA_CMD_IDENTIFY);
    io_wait();

    uint8_t status = inb(base + REG_STATUS);
    if (status == 0 || status == 0xFF) {
        /* No device present (0x00) or floating bus (0xFF) */
        return -1;
    }

    /* If LBA_MID/LBA_HI are non-zero the device may be ATAPI. Many guides use
       that to determine ATAPI; we'll handle ATAPI by issuing IDENTIFY PACKET. */
    uint8_t lba_mid = inb(base + REG_LBA_MID);
    uint8_t lba_hi  = inb(base + REG_LBA_HI);
    if (lba_mid != 0 || lba_hi != 0) {
        /* Likely ATAPI. Try IDENTIFY PACKET (0xA1) to get model string. */
        write_command(base, ATA_CMD_IDENTIFY_PACKET);
        io_wait();

        /* If DRQ never appears we still mark exists/is_atapi, but no model data. */
        if (ata_wait_drq(base) < 0) {
            ata_drives[drive_index].exists = 1;
            ata_drives[drive_index].is_atapi = 1;
            ata_drives[drive_index].channel = (drive_index < 2) ? 0 : 1;
            ata_drives[drive_index].drive = drive;
            ata_drives[drive_index].model[0] = '\0';
            return 1; /* device exists, ATAPI but no IDENTIFY data */
        }

        uint16_t ident[256];
        ata_insw(base, ident, 256);

        char model_tmp[41];
        ata_decode_identify_string(model_tmp, &ident[27], 20); /* words 27..46 => 20 words */
        memset(ata_drives[drive_index].model, 0, sizeof(ata_drives[drive_index].model));
        for (int i = 0; i < 40 && model_tmp[i]; ++i) {
            ata_drives[drive_index].model[i] = model_tmp[i];
        }
        ata_drives[drive_index].model[40] = '\0';

        ata_drives[drive_index].exists = 1;
        ata_drives[drive_index].is_atapi = 1;
        ata_drives[drive_index].channel = (drive_index < 2) ? 0 : 1;
        ata_drives[drive_index].drive = drive;
        return 1;
    }

    /* Otherwise it's likely ATA: wait for DRQ and read identify */
    if (ata_wait_drq(base) < 0) {
        /* No DRQ -> treat as device not identified or error */
        return -3;
    }

    uint16_t ident[256];
    ata_insw(base, ident, 256);

    char model_tmp[41];
    ata_decode_identify_string(model_tmp, &ident[27], 20); /* model at words 27-46 */

    memset(ata_drives[drive_index].model, 0, sizeof(ata_drives[drive_index].model));
    for (int i = 0; i < 40 && model_tmp[i]; ++i) {
        ata_drives[drive_index].model[i] = model_tmp[i];
    }
    ata_drives[drive_index].model[40] = '\0';

    ata_drives[drive_index].exists = 1;
    ata_drives[drive_index].is_atapi = 0;
    ata_drives[drive_index].channel = (drive_index < 2) ? 0 : 1;
    ata_drives[drive_index].drive = drive;
    return 0;
}

/* --- IRQ handlers --- */

/* Common handler invoked by per-channel wrappers.
 * channel: 0 = primary, 1 = secondary
 * irq    : real IRQ number (14 or 15) to send EOI
 *
 * The handler:
 *  - checks if there's a pending request on that channel
 *  - if none: send EOI and return
 *  - if pending and DRQ: performs ata_insw(...) for 256 words (512 bytes)
 *  - sets pending.completed=1 on success or -1 on error
 *  - marks pending.active = 0 (no longer in-flight)
 *  - sends PIC EOI before returning
 */
static void ata_irq_handler_common(int channel, int irq) {
    uint16_t base = (channel == 0) ? ATA_PRIMARY_BASE : ATA_SECONDARY_BASE;

    if (!pending[channel].active) {
        /* spurious or unexpected interrupt; ack and return */
        pic_send_eoi(irq);
        return;
    }

    /* read status */
    uint8_t status = inb(base + REG_STATUS);

    if (status & ATA_SR_ERR) {
        pending[channel].completed = -1;
        pending[channel].active = 0;
        pic_send_eoi(irq);
        return;
    }

    if (status & ATA_SR_DRQ) {
        /* transfer 256 words -> 512 bytes into the provided buffer */
        ata_insw(base, pending[channel].buffer, 256);
        pending[channel].completed = 1;
        pending[channel].active = 0;
        pic_send_eoi(irq);
        return;
    }

    /* Not DRQ and not ERR: still may be busy; but to avoid deadlock,
     * mark error after a small fallback. */
    pending[channel].completed = -1;
    pending[channel].active = 0;
    pic_send_eoi(irq);
    return;
}

/* Per-channel wrappers installed with irq_install_handler */
static void ata_irq_primary(void)  { ata_irq_handler_common(0, 14); }
static void ata_irq_secondary(void){ ata_irq_handler_common(1, 15); }

/* Public API: initialization */
int ata_init(void) {
    /* Quick check: if primary and secondary status registers both return 0xFF,
     * the ATA controller likely doesn't exist - skip initialization entirely */
    uint8_t prim_status = inb(ATA_PRIMARY_BASE + REG_STATUS);
    uint8_t sec_status = inb(ATA_SECONDARY_BASE + REG_STATUS);
    
    if (prim_status == 0xFF && sec_status == 0xFF) {
        /* Floating bus - no ATA controller present */
        return -2;
    }

    for (int i = 0; i < 4; ++i) {
        ata_drives[i].exists = 0;
        ata_drives[i].is_atapi = 0;
        ata_drives[i].channel = (i < 2) ? 0 : 1;
        ata_drives[i].drive = (i & 1);
        ata_drives[i].model[0] = '\0';
    }

    for (int ch = 0; ch < 2; ++ch) {
        pending[ch].active = 0;
        pending[ch].completed = 0;
        pending[ch].buffer = NULL;
        pending[ch].drive_index = -1;
    }

    /* Install IRQ handlers */
    irq_install_handler(14, ata_irq_primary);
    irq_install_handler(15, ata_irq_secondary);

    /* Probe drives */
    int found = 0;
    int controller_dead = 1;

    for (int i = 0; i < 4; ++i) {
        int r = ata_identify_drive(i);
        if (r >= 0) {
            found++;
            controller_dead = 0;
        }
    }

    if (controller_dead) {
        return -2; /* nothing responded at all */
    }
    if (found == 0) {
        return -1; /* controller alive, but no drives */
    }
    return 0; /* success */
}

const ata_drive_t* ata_get_drive(int drive_index) {
    if (drive_index < 0 || drive_index >= 4) return NULL;
    return &ata_drives[drive_index];
}

/* Read single 512-byte sector (LBA28). Now IRQ-driven.
 * Behavior:
 *  - fills 'buffer' (512 bytes)
 *  - returns 0 on success, negative on error/timeouts
 */
int ata_read_sector_lba28(int drive_index, uint32_t lba, void* buffer) {
    if (!buffer) return -1;
    if (drive_index < 0 || drive_index >= 4) return -2;
    if (!ata_drives[drive_index].exists) return -3;
    if (ata_drives[drive_index].is_atapi) return -4;
    if (lba > 0x0FFFFFFF) return -5;

    uint16_t base = ata_base_for(drive_index);
    uint16_t ctrl = ata_ctrl_for(drive_index);
    uint8_t drive = ata_drive_bit_for(drive_index);
    int channel = (drive_index < 2) ? 0 : 1;
    int irq = ata_irq_for_channel(channel);

    /* Check that there's no in-flight request on this channel */
    if (pending[channel].active) return -6; /* busy */

    /* Setup pending request (publish to handler) */
    pending[channel].buffer = buffer;
    pending[channel].drive_index = drive_index;
    pending[channel].completed = 0;
    pending[channel].active = 1;

    /* Select drive and top 4 bits of LBA */
    select_drive(base, drive, (uint8_t)((lba >> 24) & 0x0F));
    io_wait();

    /* Set up registers: sector count = 1 */
    outb(base + REG_SECCNT, 1);
    outb(base + REG_LBA_LO, (uint8_t)(lba & 0xFF));
    outb(base + REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(base + REG_LBA_HI, (uint8_t)((lba >> 16) & 0xFF));

    /* Issue READ PIO command - controller will IRQ when ready (DRQ) */
    write_command(base, ATA_CMD_READ_PIO);

    /* Busy-wait for completion with timeout (handler will set pending[].completed) */
    const unsigned long timeout_limit = 1000000UL;
    unsigned long t = 0;
    while (pending[channel].completed == 0 && t++ < timeout_limit) {
        /* Optionally halt CPU here if you have a sleep/hlt primitive:
         * asm volatile("hlt");
         */
        /* small io_wait to avoid hammering too hard */
        io_wait();
    }

    if (pending[channel].completed == 1) {
        return 0; /* success - buffer filled by IRQ handler */
    } else if (pending[channel].completed == -1) {
        return -10; /* error during transfer */
    } else {
        /* timeout - try to clear state */
        pending[channel].active = 0;
        pending[channel].completed = 0;
        return -11; /* timeout */
    }
}

/* Read multiple sectors (LBA28) into buffer */
int ata_read_sectors(int drive_index, uint32_t lba, void* buffer, uint32_t sector_count) {
    if (!buffer) return -1;
    if (drive_index < 0 || drive_index >= 4) return -2;
    if (!ata_drives[drive_index].exists) return -3;
    if (ata_drives[drive_index].is_atapi) return -4;
    if (lba + sector_count - 1 > 0x0FFFFFFF) return -5;

    uint8_t* ptr = (uint8_t*)buffer;
    for (uint32_t i = 0; i < sector_count; ++i) {
        int r = ata_read_sector_lba28(drive_index, lba + i, ptr + i * 512);
        if (r < 0) return r; // propagate error
    }
    return 0;
}

int ata_write_sector_lba28(int drive_index, uint32_t lba, const void* buffer) {
    if (!buffer) return -1;
    if (drive_index < 0 || drive_index >= 4) return -2;
    if (!ata_drives[drive_index].exists) return -3;
    if (ata_drives[drive_index].is_atapi) return -4;
    if (lba > 0x0FFFFFFF) return -5;

    uint16_t base = ata_base_for(drive_index);
    uint8_t drive = ata_drive_bit_for(drive_index);

    /* Select drive + top 4 LBA bits */
    select_drive(base, drive, (uint8_t)((lba >> 24) & 0x0F));
    io_wait();

    /* Program registers for 1 sector */
    outb(base + REG_SECCNT, 1);
    outb(base + REG_LBA_LO,  (uint8_t)(lba & 0xFF));
    outb(base + REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(base + REG_LBA_HI,  (uint8_t)((lba >> 16) & 0xFF));

    /* Issue WRITE PIO */
    write_command(base, ATA_CMD_WRITE_PIO);

    /* Wait for drive to be ready for data */
    if (ata_wait_drq(base) < 0) return -6;

    /* Transfer 512 bytes (256 words) to DATA port */
    outsw(base + REG_DATA, buffer, 256);

    /* Issue cache flush to ensure data hits the disk */
    write_command(base, ATA_CMD_CACHE_FLUSH);

    /* Wait until not busy */
    if (ata_wait_not_busy(base) < 0) return -7;

    return 0; /* success */
}

/* Optional: print detected drives for boot log */
void ata_print_drives(void) {
    uint8_t sector[512];

    for (int i = 0; i < 4; ++i) {
        if (!ata_drives[i].exists) continue;

        const char* interface = ata_drives[i].is_atapi ? "ATAPI" : "ATA";
        const char* device_type = "Unknown";

        if (ata_drives[i].is_atapi) {
            // For ATAPI devices, most likely CD/DVD-ROM
            device_type = "CD/DVD-ROM";
        } else {
            // Re-issue IDENTIFY to get rotation rate
            uint16_t base = ata_base_for(i);
            uint8_t drive = ata_drive_bit_for(i);

            select_drive(base, drive, 0);
            io_wait();
            outb(base + REG_SECCNT, 0);
            outb(base + REG_LBA_LO, 0);
            outb(base + REG_LBA_MID, 0);
            outb(base + REG_LBA_HI, 0);
            write_command(base, ATA_CMD_IDENTIFY);
            io_wait();

            if (ata_wait_drq(base) == 0) {
                uint16_t ident[256];
                ata_insw(base, ident, 256);

                uint16_t rotation_rate = ident[217];

                if (rotation_rate == 1) {
                    device_type = "SSD";
                } else if (rotation_rate >= 3600 && rotation_rate <= 15000) {
                    device_type = "HDD";
                } else if (rotation_rate == 0) {
                    device_type = "HDD (default)";
                } else {
                    device_type = "Unknown ATA Device";
                }
            } else {
                device_type = "Unknown ATA Device (no IDENTIFY)";
            }
        }

        VGA_Writef("Drive %d: %s (%s - %s)\n", i, ata_drives[i].model, interface, device_type);

        if (!ata_drives[i].is_atapi) {
            /* Try reading MBR sector 0 */
            if (ata_read_sector_lba28(i, 0, sector) == 0) {
                VGA_Write("  Partitions:\n");

                for (int p = 0; p < 4; ++p) {
                    uint8_t* entry = sector + 0x1BE + p * 16;
                    ata_partition_t part;

                    part.bootable = entry[0];
                    part.type = entry[4];
                    part.start_lba = *(uint32_t*)(entry + 8);
                    part.size_sectors = *(uint32_t*)(entry + 12);

                    if (part.type != 0 && part.size_sectors > 0) {
                        VGA_Writef("    P%d: boot=%02x type=%02x start=%u size=%u\n",
                                   p + 1,
                                   part.bootable,
                                   part.type,
                                   part.start_lba,
                                   part.size_sectors);
                    }
                }
            } else {
                VGA_Write("  Could not read MBR!\n");
            }
        }
    }
}
