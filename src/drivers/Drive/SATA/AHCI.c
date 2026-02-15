#include "moduos/drivers/Drive/SATA/AHCI.h"

/* AHCI DMA sizing
 * We allocate command tables large enough for AHCI_PRDT_MAX_ENTRIES PRDTs.
 */
#define AHCI_PRDT_MAX_ENTRIES 128

/* AHCI PRDT entry length field (DBC) is 22 bits (0-based), allowing up to 4MiB per PRD. */
#define AHCI_PRD_MAX_BYTES (4u * 1024u * 1024u)

#define AHCI_MAX_BYTES_PER_CMD (AHCI_PRDT_MAX_ENTRIES * AHCI_PRD_MAX_BYTES)
/* Sector count in the FIS is 16-bit. Use 65535 as a conservative max. */
#define AHCI_MAX_SECTORS_PER_CMD (((AHCI_MAX_BYTES_PER_CMD / 512u) < 65535u) ? (AHCI_MAX_BYTES_PER_CMD / 512u) : 65535u)

/* Worst-case PRDT sizing if the caller buffer is physically fragmented (one PRD per page).
 * Also account for potential unaligned start (can consume an extra page).
 */
#define AHCI_MAX_BYTES_PER_CMD_WORST (AHCI_PRDT_MAX_ENTRIES * (uint32_t)PAGE_SIZE - ((uint32_t)PAGE_SIZE - 1u))
#define AHCI_MAX_SECTORS_PER_CMD_WORST (AHCI_MAX_BYTES_PER_CMD_WORST / 512u)
#include "moduos/drivers/PCI/pci.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/macros.h"
#include "moduos/arch/AMD64/interrupts/pic.h"
#include "moduos/arch/AMD64/interrupts/irq.h"
#include "moduos/arch/AMD64/interrupts/timer.h"
#include "moduos/kernel/interrupts/hlt_wait.h"
#include "moduos/kernel/mdinit.h"
#include "moduos/kernel/multiboot2.h"
#include <stdint.h>
#include <stddef.h>

static ahci_controller_t ahci_controller;
static int ahci_irq_line = -1;
static int ahci_force_poll = 0;

static void ahci_irq_handler(void);

// Memory allocation helper for DMA buffers
static void* ahci_alloc_dma(size_t size, size_t alignment) {
    void *ptr = kmalloc_aligned(size, alignment);
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
// Timing / wait helpers
// ===========================================================================

/* Note: PIT runs at 100Hz by default (10ms/tick). */
static inline void ahci_cpu_pause(void) {
    __asm__ volatile("pause" ::: "memory");
}

/*
 * Legacy delay helper used in init/reset/identify paths.
 * IMPORTANT: Do not use this in the read/write hot path.
 */
static void ahci_usleep(uint32_t us) {
    for (volatile uint32_t i = 0; i < us * 100; i++) {
        ahci_cpu_pause();
    }
}

/*
 * Wait until (port->tfd & mask) becomes 0, or until timeout_ms elapses.
 *
 * This is used in the I/O hot path. Do not sleep in 1ms increments; poll tightly
 * with PAUSE (or HLT if IRQs are expected) and use the system tick counter only
 * for the timeout.
 */
static int ahci_wait_tfd_clear(hba_port_t *port, uint32_t mask, uint32_t timeout_ms) {
    const uint64_t start = get_system_ticks();
    const uint64_t timeout_ticks = ms_to_ticks(timeout_ms);

    /* Some hypervisors can "lose" timer interrupts briefly while a device is wedged.
     * If the PIT tick stops advancing, a pure tick-based timeout will never fire.
     *
     * Keep a conservative fallback budget based on a bounded busy-wait sleep.
     * This is only used if we detect that ticks are not moving.
     */
    uint32_t fallback_ms = timeout_ms;
    uint64_t last_tick = start;
    uint32_t stagnant_iters = 0;

    uint32_t backoff = 0;
    for (;;) {
        if ((port->tfd & mask) == 0) return 0;

        const uint64_t now = get_system_ticks();
        if ((now - start) >= timeout_ticks) return -1;

        if (now == last_tick) {
            /* If ticks are not advancing, don't deadlock forever. */
            stagnant_iters++;
            if (stagnant_iters >= 50000u) {
                stagnant_iters = 0;
                if (fallback_ms == 0) return -1;
                fallback_ms--;
                ahci_usleep(1000); /* ~1ms best-effort */
            }
        } else {
            last_tick = now;
            stagnant_iters = 0;
        }

        /* Default to PAUSE polling (fast). Avoid HLT here: if timer IRQs are stuck,
         * HLT can turn a short stall into a permanent hang.
         */
        backoff++;
        (void)backoff;
        ahci_cpu_pause();
    }
}

/*
 * Wait for a command slot to complete (CI bit cleared or completion flag set),
 * or error/timeout. Used in the read/write hot path.
 */
static int ahci_wait_cmd_done(uint8_t port_num, int slot, uint32_t timeout_ms) {
    ahci_port_info_t *pi = &ahci_controller.ports[port_num];
    hba_port_t *port = pi->port;

    const uint64_t start = get_system_ticks();
    const uint64_t timeout_ticks = ms_to_ticks(timeout_ms);

    uint32_t fallback_ms = timeout_ms;
    uint64_t last_tick = start;
    uint32_t stagnant_iters = 0;

    for (;;) {
        if (pi->error_slots & (1u << slot)) return -1;
        if (pi->completed_slots & (1u << slot)) return 0;
        if ((port->ci & (1u << slot)) == 0) return 0;
        if (port->is & HBA_PxIS_TFES) return -1;

        const uint64_t now = get_system_ticks();
        if ((now - start) >= timeout_ticks) return -1;

        if (now == last_tick) {
            stagnant_iters++;
            if (stagnant_iters >= 50000u) {
                stagnant_iters = 0;
                if (fallback_ms == 0) return -1;
                fallback_ms--;
                ahci_usleep(1000);
            }
        } else {
            last_tick = now;
            stagnant_iters = 0;
        }

        /* Avoid HLT here for the same reason as ahci_wait_tfd_clear():
         * if timer interrupts are not firing, HLT can deadlock.
         */
        ahci_cpu_pause();
    }
}

/*
 * Build PRDT entries for a (possibly non-contiguous) virtual buffer.
 * Coalesces physically contiguous ranges and grows each PRD up to AHCI_PRD_MAX_BYTES.
 */
static int ahci_build_prdt(hba_cmd_table_t *cmdtbl, const void *buffer, uint32_t bytes, uint16_t *out_prdtl) {
    if (!cmdtbl || !buffer || bytes == 0 || !out_prdtl) return -1;

    /* Fast path: if the buffer is physically contiguous, emit a single PRD.
     * This avoids per-page paging_virt_to_phys() calls in the hot path.
     */
    {
        const uint64_t v0 = (uint64_t)(uintptr_t)buffer;
        const uint64_t p0 = paging_virt_to_phys(v0);
        const uint64_t plast = paging_virt_to_phys(v0 + (uint64_t)bytes - 1u);
        if (plast == (p0 + (uint64_t)bytes - 1u)) {
            if (bytes > AHCI_PRD_MAX_BYTES) return -1; /* caller should chunk */
            cmdtbl->prdt_entry[0].dba  = (uint32_t)p0;
            cmdtbl->prdt_entry[0].dbau = (uint32_t)(p0 >> 32);
            cmdtbl->prdt_entry[0].dbc  = bytes - 1u;
            cmdtbl->prdt_entry[0].i    = 1;
            *out_prdtl = 1;
            return 0;
        }
    }

    const uint8_t *v = (const uint8_t*)buffer;
    uint32_t remaining = bytes;

    uint16_t prdtl = 0;
    uint64_t cur_phys = 0;
    uint32_t cur_len = 0;

    while (remaining > 0) {
        const uint64_t vaddr = (uint64_t)(uintptr_t)v;
        const uint32_t page_off = (uint32_t)(vaddr & (PAGE_SIZE - 1ULL));
        uint32_t chunk = (uint32_t)(PAGE_SIZE - page_off);
        if (chunk > remaining) chunk = remaining;

        const uint64_t phys = paging_virt_to_phys(vaddr);

        /* Try to extend current PRD if physically contiguous and within max length. */
        if (cur_len > 0 && phys == (cur_phys + cur_len) && (cur_len + chunk) <= AHCI_PRD_MAX_BYTES) {
            cur_len += chunk;
        } else {
            /* Flush previous PRD if any. */
            if (cur_len > 0) {
                cmdtbl->prdt_entry[prdtl].dba  = (uint32_t)cur_phys;
                cmdtbl->prdt_entry[prdtl].dbau = (uint32_t)(cur_phys >> 32);
                cmdtbl->prdt_entry[prdtl].dbc  = cur_len - 1;
                cmdtbl->prdt_entry[prdtl].i    = 0;
                prdtl++;
                if (prdtl >= AHCI_PRDT_MAX_ENTRIES) return -1;
            }

            cur_phys = phys;
            cur_len  = chunk;
        }

        v += chunk;
        remaining -= chunk;
    }

    /* Flush last PRD */
    if (cur_len == 0) return -1;
    cmdtbl->prdt_entry[prdtl].dba  = (uint32_t)cur_phys;
    cmdtbl->prdt_entry[prdtl].dbau = (uint32_t)(cur_phys >> 32);
    cmdtbl->prdt_entry[prdtl].dbc  = cur_len - 1;
    cmdtbl->prdt_entry[prdtl].i    = 1; /* interrupt on last */
    prdtl++;

    *out_prdtl = prdtl;
    return 0;
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
    int timeout = 500; // 500ms timeout
    while (timeout > 0) {
        if (!(port->cmd & HBA_PxCMD_FR) && !(port->cmd & HBA_PxCMD_CR))
            break;
        ahci_usleep(1000); // 1ms
        timeout--;
    }
    
    if (timeout == 0) {
        COM_LOG_WARN(COM1_PORT, "Port stop timeout");
    }
}

void ahci_start_cmd(hba_port_t *port) {
    // Wait until CR (bit 15) is cleared
    int timeout = 500;
    while ((port->cmd & HBA_PxCMD_CR) && timeout > 0) {
        ahci_usleep(1000);
        timeout--;
    }
    
    if (timeout == 0) {
        COM_LOG_WARN(COM1_PORT, "Port start timeout");
        return;
    }
    
    // Set FRE (bit 4) and ST (bit 0)
    port->cmd |= HBA_PxCMD_FRE;
    port->cmd |= HBA_PxCMD_ST;
    
    // Posted read to ensure writes complete
    (void)port->cmd;
}

int ahci_port_rebase(hba_port_t *port, int portno) {
    ahci_stop_cmd(port);
    
    // Allocate command list (1K aligned per AHCI spec)
    void *clb = ahci_alloc_dma(1024, 1024);
    if (!clb) {
        COM_LOG_ERROR(COM1_PORT, "Failed to allocate command list");
        return -1;
    }
    
    // Get physical address for DMA
    uint64_t clb_phys = paging_virt_to_phys((uint64_t)clb);
    
    port->clb = (uint32_t)clb_phys;
    port->clbu = (uint32_t)(clb_phys >> 32);
    
    ahci_controller.ports[portno].cmd_list = (hba_cmd_header_t*)clb;
    
    // Allocate FIS (256 byte aligned per AHCI spec)
    void *fb = ahci_alloc_dma(256, 256);
    if (!fb) {
        COM_LOG_ERROR(COM1_PORT, "Failed to allocate FIS");
        return -1;
    }
    
    uint64_t fb_phys = paging_virt_to_phys((uint64_t)fb);
    
    port->fb = (uint32_t)fb_phys;
    port->fbu = (uint32_t)(fb_phys >> 32);
    
    ahci_controller.ports[portno].fis = (hba_fis_t*)fb;
    
    // Allocate command tables (128 byte aligned per AHCI spec, 32 slots)
    hba_cmd_header_t *cmdheader = (hba_cmd_header_t*)clb;
    for (int i = 0; i < 32; i++) {
        cmdheader[i].prdtl = AHCI_PRDT_MAX_ENTRIES;
        
        /* Allocate command table big enough for a large PRDT.
         * hba_cmd_table_t includes prdt_entry[1] already.
         */
        void *ctba = ahci_alloc_dma(sizeof(hba_cmd_table_t) + (AHCI_PRDT_MAX_ENTRIES - 1) * sizeof(hba_prdt_entry_t), 128);
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

static inline void cpu_hlt_once(void) {
    /* Ensure interrupts are enabled while halting.
     * This prevents deadlocks if called from code paths that might temporarily
     * run with IF=0.
     */
    __asm__ volatile("sti; hlt");
}

static void ahci_irq_handler(void) {
    if (!ahci_controller.abar) return;

    uint32_t is = ahci_controller.abar->is;
    if (is == 0) {
        /* Spurious */
        if (ahci_irq_line >= 0) pic_send_eoi((uint8_t)ahci_irq_line);
        return;
    }

    /* Clear port interrupt status first (bottom-up), then global status (top-down) */
    for (int p = 0; p < AHCI_MAX_PORTS; p++) {
        if ((is & (1u << p)) == 0) continue;

        ahci_port_info_t *pi = &ahci_controller.ports[p];
        hba_port_t *port = pi->port;
        if (!port) continue;

        uint32_t pis = port->is;
        
        /* Clear port interrupt status (write-1-to-clear) */
        port->is = pis;
        
        /* Posted read to ensure the write completes before continuing */
        (void)port->is;

        uint32_t ci_now = port->ci;
        uint32_t ci_prev = pi->last_ci;
        uint32_t done = ci_prev & ~ci_now; /* 1->0 transitions */
        if (done) {
            pi->completed_slots |= done;
        }
        pi->last_ci = ci_now;

        if (pis & HBA_PxIS_TFES) {
            /* Mark all active slots as errored */
            pi->error_slots |= ci_now;
        }
    }

    /* Clear global HBA interrupt status (write-1-to-clear) */
    ahci_controller.abar->is = is;
    
    /* Posted read to ensure the write completes */
    (void)ahci_controller.abar->is;

    if (ahci_irq_line >= 0) pic_send_eoi((uint8_t)ahci_irq_line);
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
    uint32_t sig = port->sig;
    
    switch (sig) {
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
    
    uint32_t sig = port->sig;
    com_printf(COM1_PORT, "[AHCI] Port %d signature: 0x%08x\n", port_num, sig);
    
    if (sig == SATA_SIG_ATAPI) {
        if (port_info->type != AHCI_DEV_SATAPI) {
            com_printf(COM1_PORT, "[AHCI] Signature indicates SATAPI, updating type\n");
            port_info->type = AHCI_DEV_SATAPI;
        }
    }
    
    // Skip IDENTIFY for SATAPI devices - they use IDENTIFY PACKET DEVICE
    if (port_info->type == AHCI_DEV_SATAPI) {
        com_write_string(COM1_PORT, "[AHCI] SATAPI device detected - using IDENTIFY PACKET DEVICE\n");
        
        // Set a generic model name for SATAPI
        const char *default_model = "SATAPI Optical Drive";
        for (int i = 0; default_model[i] != '\0' && i < 40; i++) {
            port_info->model[i] = default_model[i];
        }
        port_info->model[40] = '\0';
        port_info->serial[0] = '\0';
        port_info->sector_count = 0;
        port_info->sector_size = 2048;  // SATAPI uses 2048-byte sectors
        
        // Now try to get actual model string with IDENTIFY PACKET DEVICE
        // Log port state
        com_printf(COM1_PORT, "[AHCI] Port %d state: CMD=0x%08x TFD=0x%08x SSTS=0x%08x\n",
                  port_num, port->cmd, port->tfd, port->ssts);
        
        // Clear any errors
        if (port->serr != 0) {
            com_printf(COM1_PORT, "[AHCI] Clearing SERR=0x%08x\n", port->serr);
            port->serr = (uint32_t)-1;
        }
        
        // Clear pending interrupts
        port->is = (uint32_t)-1;
        
        int slot = ahci_find_cmdslot(port);
        if (slot == -1) {
            com_write_string(COM1_PORT, "[AHCI] No free command slot for SATAPI IDENTIFY\n");
            return 0; // Return success anyway - we have a default model
        }
        
        hba_cmd_header_t *cmdheader = &port_info->cmd_list[slot];
        cmdheader->cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
        cmdheader->w = 0;  // Read from device
        cmdheader->prdtl = 1;
        cmdheader->a = 0;  // Not ATAPI command, this is ATA IDENTIFY PACKET
        
        hba_cmd_table_t *cmdtbl = port_info->cmd_tables[slot];
        
        // Allocate buffer for IDENTIFY PACKET data (512 bytes, 2-byte aligned is sufficient)
        uint16_t *identify_buf = (uint16_t*)ahci_alloc_dma(512, 2);
        if (!identify_buf) {
            com_write_string(COM1_PORT, "[AHCI] Failed to allocate SATAPI identify buffer\n");
            return 0; // Return success with default model
        }
        
        uint64_t identify_phys = paging_virt_to_phys((uint64_t)identify_buf);
        
        // Setup PRDT
        cmdtbl->prdt_entry[0].dba = (uint32_t)identify_phys;
        cmdtbl->prdt_entry[0].dbau = (uint32_t)(identify_phys >> 32);
        cmdtbl->prdt_entry[0].dbc = 511;  // 512 bytes (0-based)
        cmdtbl->prdt_entry[0].i = 1;
        
        // Setup command FIS for IDENTIFY PACKET DEVICE
        fis_reg_h2d_t *cmdfis = (fis_reg_h2d_t*)(&cmdtbl->cfis);
        
        cmdfis->fis_type = FIS_TYPE_REG_H2D;
        cmdfis->c = 1;  // Command
        cmdfis->command = 0xA1;  // IDENTIFY PACKET DEVICE (NOT 0xEC)
        
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
        if (ahci_wait_tfd_clear(port, (0x80 | 0x08), 500 /*ms*/) != 0) {
            com_write_string(COM1_PORT, "[AHCI] SATAPI port hung before IDENTIFY\n");
            kfree(identify_buf);
            return 0; // Return success with default model
        }

        // Issue command
        port->ci = 1u << slot;
        port_info->last_ci |= (1u << slot);
        
        // Posted read to ensure command register write completes
        (void)port->ci;

        // Wait for completion (prefer fast polling with a real timeout; IRQs may not fire on some hypervisors)
        port_info->completed_slots &= ~(1u << slot);
        port_info->error_slots &= ~(1u << slot);
        if (ahci_wait_cmd_done(port_num, slot, 5000 /*ms*/) != 0) {
            com_write_string(COM1_PORT, "[AHCI] SATAPI IDENTIFY PACKET timeout\n");
            kfree(identify_buf);
            return 0; // Return success with default model
        }
        
        // Parse IDENTIFY PACKET data (same format as regular IDENTIFY)
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
        
        com_write_string(COM1_PORT, "[AHCI] Successfully identified SATAPI device\n");
        kfree(identify_buf);
        return 0;
    }
    
    // Log port state before IDENTIFY
    com_printf(COM1_PORT, "[AHCI] Port %d state before IDENTIFY: CMD=0x%08x TFD=0x%08x SSTS=0x%08x SERR=0x%08x\n",
              port_num, port->cmd, port->tfd, port->ssts, port->serr);
    
    // Check if port is in error state
    if (port->serr != 0) {
        com_printf(COM1_PORT, "[AHCI] Port %d has errors, clearing SERR=0x%08x\n", port_num, port->serr);
        port->serr = (uint32_t)-1;  // Clear errors
    }
    
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
    cmdheader->a = 0;  // Not ATAPI
    
    hba_cmd_table_t *cmdtbl = port_info->cmd_tables[slot];
    
    // Allocate buffer for IDENTIFY data (512 bytes, 2-byte aligned is sufficient)
    uint16_t *identify_buf = (uint16_t*)ahci_alloc_dma(512, 2);
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
    if (ahci_wait_tfd_clear(port, (0x80 | 0x08), 500 /*ms*/) != 0) {
        COM_LOG_ERROR(COM1_PORT, "Port hung");
        kfree(identify_buf);
        return -1;
    }
    
    // Issue command
    port->ci = 1u << slot;
    ahci_controller.ports[port_num].last_ci |= (1u << slot);
    
    // Posted read to ensure command register write completes
    (void)port->ci;
    
    // Wait for completion (prefer fast polling with a real timeout; IRQs may not fire on some hypervisors)
    port_info->completed_slots &= ~(1u << slot);
    port_info->error_slots &= ~(1u << slot);
    if (ahci_wait_cmd_done(port_num, slot, 5000 /*ms*/) != 0) {
        COM_LOG_ERROR(COM1_PORT, "IDENTIFY command timeout");
        com_printf(COM1_PORT, "[AHCI] Port %d CI=0x%08x TFD=0x%08x IS=0x%08x SERR=0x%08x\n",
                   port_num, port->ci, port->tfd, port->is, port->serr);
        kfree(identify_buf);
        return -1;
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
    
    /* Chunk large transfers to avoid overflowing the command table PRDT.
     * Our cmd tables are sized for AHCI_PRDT_MAX_ENTRIES entries.
     */
    uint8_t *buf8 = (uint8_t*)buffer;
    uint32_t remaining = count;

    while (remaining > 0) {
        /* Start optimistic for throughput, but fall back if the PRDT would overflow
         * (e.g. physically fragmented caller buffers).
         */
        uint32_t this_count = (remaining > AHCI_MAX_SECTORS_PER_CMD) ? AHCI_MAX_SECTORS_PER_CMD : remaining;
        if (this_count > AHCI_MAX_SECTORS_PER_CMD_WORST) this_count = AHCI_MAX_SECTORS_PER_CMD_WORST;

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

        hba_cmd_table_t *cmdtbl = port_info->cmd_tables[slot];

        uint16_t prdtl = 0;
        for (;;) {
            const uint32_t bytes = this_count * 512u;
            if (ahci_build_prdt(cmdtbl, buf8, bytes, &prdtl) == 0) break;

            if (this_count <= 1) {
                COM_LOG_ERROR(COM1_PORT, "Failed to build PRDT");
                return -1;
            }
            this_count >>= 1;
        }
        cmdheader->prdtl = prdtl;

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
    
    cmdfis->countl = (uint8_t)this_count;
    cmdfis->counth = (uint8_t)(this_count >> 8);
    
    // Wait for port to be ready (BSY/DRQ clear)
    if (ahci_wait_tfd_clear(port, (0x80 | 0x08), 500 /*ms*/) != 0) {
        COM_LOG_ERROR(COM1_PORT, "Port hung");
        return -1;
    }

    // Issue command
    port->ci = 1u << slot;
    ahci_controller.ports[port_num].last_ci |= (1u << slot);
    
    // Posted read to ensure command register write completes
    (void)port->ci;

    // Wait for completion
    ahci_controller.ports[port_num].completed_slots &= ~(1u << slot);
    ahci_controller.ports[port_num].error_slots &= ~(1u << slot);
    if (ahci_wait_cmd_done(port_num, slot, 5000 /*ms*/) != 0) {
        COM_LOG_ERROR(COM1_PORT, "Read command timeout/error");
        return -1;
    }

    /* DMA writes bypass CPU cache. Flush cache to ensure CPU reads fresh data.
     * Without this, the CPU might read stale cached data instead of the DMA-transferred data. */
    __asm__ volatile("mfence" ::: "memory");
    
    /* Invalidate cache lines for the DMA buffer to force CPU to read from RAM */
    for (uint32_t i = 0; i < this_count * 512u; i += 64) {
        __asm__ volatile("clflush (%0)" :: "r"(buf8 + i) : "memory");
    }
    __asm__ volatile("mfence" ::: "memory");

    /* Advance */
    buf8 += (this_count * 512u);
    start_lba += this_count;
    remaining -= this_count;
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

    const uint8_t *buf8 = (const uint8_t*)buffer;
    uint32_t remaining = count;

    while (remaining > 0) {
        /* Start optimistic for throughput, but fall back if the PRDT would overflow
         * (e.g. physically fragmented caller buffers).
         */
        uint32_t this_count = (remaining > AHCI_MAX_SECTORS_PER_CMD) ? AHCI_MAX_SECTORS_PER_CMD : remaining;
        if (this_count > AHCI_MAX_SECTORS_PER_CMD_WORST) this_count = AHCI_MAX_SECTORS_PER_CMD_WORST;

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

        hba_cmd_table_t *cmdtbl = port_info->cmd_tables[slot];

        uint16_t prdtl = 0;
        for (;;) {
            const uint32_t bytes = this_count * 512u;
            if (ahci_build_prdt(cmdtbl, buf8, bytes, &prdtl) == 0) break;

            if (this_count <= 1) {
                COM_LOG_ERROR(COM1_PORT, "Failed to build PRDT");
                return -1;
            }
            this_count >>= 1;
        }
        cmdheader->prdtl = prdtl;

        /* DMA reads data from RAM. Flush CPU cache to ensure data is visible to DMA.
         * Without this, the DMA controller might read stale data from RAM while the
         * fresh data sits in CPU cache. */
        __asm__ volatile("mfence" ::: "memory");
        
        /* Flush cache lines for the write buffer to push data to RAM */
        for (uint32_t i = 0; i < this_count * 512u; i += 64) {
            __asm__ volatile("clflush (%0)" :: "r"(buf8 + i) : "memory");
        }
        __asm__ volatile("mfence" ::: "memory");

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
    
    cmdfis->countl = (uint8_t)this_count;
    cmdfis->counth = (uint8_t)(this_count >> 8);
    
    /* Flush command structures to RAM before DMA reads them.
     * The HBA needs to read the command FIS and PRDT from RAM. */
    __asm__ volatile("mfence" ::: "memory");
    
    /* Flush command table (includes FIS and PRDT) */
    for (uintptr_t addr = (uintptr_t)cmdtbl; addr < (uintptr_t)cmdtbl + sizeof(hba_cmd_table_t) + (prdtl * sizeof(hba_prdt_entry_t)); addr += 64) {
        __asm__ volatile("clflush (%0)" :: "r"(addr) : "memory");
    }
    
    /* Flush command header */
    __asm__ volatile("clflush (%0)" :: "r"(cmdheader) : "memory");
    
    __asm__ volatile("mfence" ::: "memory");
    
    // Wait for port to be ready (BSY/DRQ clear)
    if (ahci_wait_tfd_clear(port, (0x80 | 0x08), 500 /*ms*/) != 0) {
        COM_LOG_ERROR(COM1_PORT, "Port hung");
        return -1;
    }

    // Issue command
    port->ci = 1u << slot;
    ahci_controller.ports[port_num].last_ci |= (1u << slot);
    
    // Posted read to ensure command register write completes
    (void)port->ci;

    // Wait for completion
    ahci_controller.ports[port_num].completed_slots &= ~(1u << slot);
    ahci_controller.ports[port_num].error_slots &= ~(1u << slot);
    if (ahci_wait_cmd_done(port_num, slot, 5000 /*ms*/) != 0) {
        COM_LOG_ERROR(COM1_PORT, "Write command timeout/error");
        com_printf(COM1_PORT, "[AHCI] write fail: port %u CI=0x%08x TFD=0x%08x IS=0x%08x SERR=0x%08x\n",
                   port_num, port->ci, port->tfd, port->is, port->serr);
        return -1;
    }

    com_printf(COM1_PORT, "[AHCI] write ok: port %u LBA=%llu count=%u\n",
               port_num, (unsigned long long)start_lba, (unsigned)this_count);

    buf8 += (this_count * 512u);
    start_lba += this_count;
    remaining -= this_count;
    }

    return 0;
}

int ahci_flush_cache(uint8_t port_num) {
    if (port_num >= AHCI_MAX_PORTS) return -1;

    ahci_port_info_t *port_info = &ahci_controller.ports[port_num];
    hba_port_t *port = port_info->port;

    if (!port || port_info->type != AHCI_DEV_SATA) return -1;

    com_printf(COM1_PORT, "[AHCI] flush: port %u issuing ATA FLUSH CACHE\n", port_num);

    // Clear pending interrupts
    port->is = (uint32_t)-1;

    int slot = ahci_find_cmdslot(port);
    if (slot == -1) {
        COM_LOG_ERROR(COM1_PORT, "No free command slot");
        return -1;
    }

    hba_cmd_header_t *cmdheader = &port_info->cmd_list[slot];
    cmdheader->cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmdheader->w = 0;  // Non-data command
    cmdheader->prdtl = 0;

    hba_cmd_table_t *cmdtbl = port_info->cmd_tables[slot];

    fis_reg_h2d_t *cmdfis = (fis_reg_h2d_t*)(&cmdtbl->cfis);
    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->c = 1;
    /* Use FLUSH CACHE EXT (0xEA) to match the extended write commands.
     * Using legacy FLUSH CACHE (0xE7) with extended writes can cause data loss. */
    cmdfis->command = ATA_CMD_FLUSH_CACHE_EXT;

    cmdfis->lba0 = 0;
    cmdfis->lba1 = 0;
    cmdfis->lba2 = 0;
    cmdfis->device = 1 << 6;
    cmdfis->lba3 = 0;
    cmdfis->lba4 = 0;
    cmdfis->lba5 = 0;
    cmdfis->countl = 0;
    cmdfis->counth = 0;

    if (ahci_wait_tfd_clear(port, (0x80 | 0x08), 500 /*ms*/) != 0) {
        COM_LOG_ERROR(COM1_PORT, "Port hung");
        return -1;
    }

    port->ci = 1u << slot;
    ahci_controller.ports[port_num].last_ci |= (1u << slot);
    (void)port->ci;

    ahci_controller.ports[port_num].completed_slots &= ~(1u << slot);
    ahci_controller.ports[port_num].error_slots &= ~(1u << slot);
    if (ahci_wait_cmd_done(port_num, slot, 5000 /*ms*/) != 0) {
        COM_LOG_ERROR(COM1_PORT, "Flush cache timeout/error");
        return -1;
    }

    return 0;
}

// ===========================================================================
// HBA Reset and BIOS Handoff
// ===========================================================================

static int ahci_bios_handoff(hba_mem_t *abar) {
    // Check if BOHC (BIOS/OS Handoff Control) is supported
    uint32_t cap2 = abar->cap2;
    if (!(cap2 & (1 << 0))) {
        // BOH not supported, continue without handoff
        return 0;
    }
    
    // BOHC is at offset 0x28 from ABAR
    volatile uint32_t *bohc = (volatile uint32_t*)((uintptr_t)abar + 0x28);
    
    // Set OS Ownership (bit 1)
    *bohc |= (1 << 1);
    
    // Wait for BIOS to release ownership (bit 0 should clear)
    int timeout = 25; // 25ms timeout
    while ((*bohc & (1 << 0)) && timeout > 0) {
        ahci_usleep(1000); // 1ms
        timeout--;
    }
    
    if (timeout == 0) {
        COM_LOG_WARN(COM1_PORT, "BIOS handoff timeout");
        return -1;
    }
    
    COM_LOG_INFO(COM1_PORT, "BIOS handoff successful");
    return 0;
}

static int ahci_hba_reset(hba_mem_t *abar) {
    COM_LOG_INFO(COM1_PORT, "Resetting HBA");
    
    // Set HBA Reset bit
    abar->ghc |= (1 << 0); // HBA_GHC_HR
    
    // Wait for reset to complete (bit should clear when done)
    int timeout = 1000; // 1 second timeout
    while ((abar->ghc & (1 << 0)) && timeout > 0) {
        ahci_usleep(1000); // 1ms
        timeout--;
    }
    
    if (timeout == 0) {
        COM_LOG_ERROR(COM1_PORT, "HBA reset timeout");
        return -1;
    }
    
    // Wait a bit for controller to be ready
    ahci_usleep(1000);
    
    COM_LOG_INFO(COM1_PORT, "HBA reset complete");
    return 0;
}

// ===========================================================================
// Port Probing
// ===========================================================================

int ahci_probe_ports(void) {
    uint32_t pi = ahci_controller.abar->pi;
    int port_count = 0;
    
    COM_LOG_INFO(COM1_PORT, "Probing AHCI ports");
    
    for (int i = 0; i < 32; i++) {
        if (pi & (1 << i)) {
            hba_port_t *port = (hba_port_t*)((uintptr_t)ahci_controller.abar + 0x100 + (i * 0x80));
            
            // Check device type
            ahci_device_type_t type = ahci_check_type(port);
            
            if (type != AHCI_DEV_NULL) {
                ahci_controller.ports[i].type = type;
                ahci_controller.ports[i].port_num = i;
                ahci_controller.ports[i].port = port;
                
                com_write_string(COM1_PORT, "[AHCI] Found ");
                com_write_string(COM1_PORT, ahci_get_device_type_string(type));
                com_write_string(COM1_PORT, " drive on port ");
                char port_str[4];
                port_str[0] = '0' + i;
                port_str[1] = '\0';
                com_write_string(COM1_PORT, port_str);
                com_write_string(COM1_PORT, "\n");
                
                // Clear any pending/stale interrupts before initialization
                port->is = (uint32_t)-1;
                
                // Disable port interrupts during initialization
                port->ie = 0;
                
                // Init slot completion tracking
                ahci_controller.ports[i].last_ci = port->ci;
                ahci_controller.ports[i].completed_slots = 0;
                ahci_controller.ports[i].error_slots = 0;
                
                // Rebase port
                if (ahci_port_rebase(port, i) == 0) {
                    // Try to identify device
                    if (ahci_identify_device(i) == 0) {
                        com_write_string(COM1_PORT, "[AHCI]   Model: ");
                        com_write_string(COM1_PORT, ahci_controller.ports[i].model);
                        com_write_string(COM1_PORT, "\n");
                    }
                    
                    // Clear status again after device initialization
                    port->is = (uint32_t)-1;
                    
                    // Now enable port interrupts (completion + error)
                    port->ie = HBA_PxIS_DHRS | HBA_PxIS_PSS | HBA_PxIS_DPS | HBA_PxIS_SDBS | HBA_PxIS_TFES;
                } else {
                    COM_LOG_ERROR(COM1_PORT, "Failed to rebase port");
                }
                
                port_count++;
            }
        }
    }
    
    ahci_controller.port_count = port_count;
    return port_count;
}

// ===========================================================================
// Initialization
// ===========================================================================

int ahci_init(void) {
    com_write_string(COM1_PORT, "[AHCI] Initializing AHCI driver...\n");
    
    // Find AHCI controller via PCI
    pci_device_t *pci_dev = pci_find_class(AHCI_CLASS_STORAGE, AHCI_SUBCLASS_SATA);
    
    if (!pci_dev) {
        com_write_string(COM1_PORT, "[AHCI] No AHCI controller found\n");
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
    uint32_t abar_raw = pci_read_bar(pci_dev, 5);
    
    com_printf(COM1_PORT, "[AHCI] Raw BAR5 = 0x%08x\n", abar_raw);
    
    // Check if this is a memory BAR (bit 0 should be 0)
    if (abar_raw & 0x1) {
        com_write_string(COM1_PORT, "[AHCI] ERROR: ABAR is I/O BAR, expected memory BAR\n");
        return -1;
    }
    
    // Mask off the lower 4 bits (type and prefetchable bits)
    uint64_t abar_phys = abar_raw & ~0xFULL;
    
    if (abar_phys == 0) {
        com_write_string(COM1_PORT, "[AHCI] ERROR: Invalid ABAR (zero address)\n");
        return -1;
    }
    
    com_printf(COM1_PORT, "[AHCI] ABAR physical = 0x%08x\n", (uint32_t)abar_phys);
    
    // Ensure paging is initialized
    com_write_string(COM1_PORT, "[AHCI] Checking paging initialization...\n");
    if (!paging_get_pml4()) {
        com_write_string(COM1_PORT, "[AHCI] Paging not initialized, calling paging_init()...\n");
        paging_init();
        if (!paging_get_pml4()) {
            com_write_string(COM1_PORT, "[AHCI] ERROR: Failed to initialize paging\n");
            return -1;
        }
        com_write_string(COM1_PORT, "[AHCI] Paging initialized successfully\n");
    } else {
        com_write_string(COM1_PORT, "[AHCI] Paging already initialized\n");
    }
    
    // Use ioremap() to map the MMIO region
    // AHCI HBA memory is at least 8KB, map 16KB to be safe (covers all port registers)
    com_printf(COM1_PORT, "[AHCI] Calling ioremap(0x%08x, 0x4000)...\n", (uint32_t)abar_phys);
    
    void *abar_virt = ioremap(abar_phys, 0x4000);
    
    if (!abar_virt) {
        com_write_string(COM1_PORT, "[AHCI] ERROR: ioremap() returned NULL\n");
        com_write_string(COM1_PORT, "[AHCI] This usually means page allocation failed\n");
        return -1;
    }
    
    uint64_t abar_virt_addr = (uint64_t)(uintptr_t)abar_virt;
    com_write_string(COM1_PORT, "[AHCI] ioremap() returned virtual = 0x");
    com_printf(COM1_PORT, "%08x", (uint32_t)(abar_virt_addr >> 32));
    com_printf(COM1_PORT, "%08x\n", (uint32_t)(abar_virt_addr & 0xFFFFFFFF));
    
    // Sanity check
    if (abar_virt_addr == abar_phys) {
        com_write_string(COM1_PORT, "[AHCI] WARNING: Virtual = Physical (identity mapping)\n");
        com_write_string(COM1_PORT, "[AHCI] This may fail on real hardware\n");
    }
    
    ahci_controller.abar = (hba_mem_t*)abar_virt;
    
    // Add memory barriers before accessing MMIO
    __asm__ volatile("mfence" ::: "memory");
    
    // Test read with error handling
    com_write_string(COM1_PORT, "[AHCI] Testing MMIO access to CAP register...\n");
    
    // Add memory barrier before volatile read
    __asm__ volatile("" ::: "memory");
    
    volatile uint32_t cap = ahci_controller.abar->cap;
    
    // Add memory barrier after volatile read
    __asm__ volatile("mfence" ::: "memory");
    
    com_printf(COM1_PORT, "[AHCI] Successfully read CAP = 0x%08x\n", cap);
    
    // Verify the CAP value is reasonable
    if (cap == 0) {
        com_write_string(COM1_PORT, "[AHCI] ERROR: CAP register is 0x00000000\n");
        com_write_string(COM1_PORT, "[AHCI] Device may not be responding or mapping failed\n");
        return -1;
    }
    
    if (cap == 0xFFFFFFFF) {
        com_write_string(COM1_PORT, "[AHCI] ERROR: CAP register is 0xFFFFFFFF\n");
        com_write_string(COM1_PORT, "[AHCI] Device may be disconnected or mapping failed\n");
        return -1;
    }
    
    // Read version register as additional test
    volatile uint32_t version = ahci_controller.abar->vs;
    com_printf(COM1_PORT, "[AHCI] AHCI Version = 0x%08x\n", version);
    
    com_write_string(COM1_PORT, "[AHCI] MMIO mapping verified - ABAR is accessible\n");
    
    // Perform BIOS handoff
    com_write_string(COM1_PORT, "[AHCI] Performing BIOS handoff...\n");
    if (ahci_bios_handoff(ahci_controller.abar) != 0) {
        com_write_string(COM1_PORT, "[AHCI] WARNING: BIOS handoff failed, continuing anyway\n");
    } else {
        com_write_string(COM1_PORT, "[AHCI] BIOS handoff successful\n");
    }
    
    // Reset HBA
    com_write_string(COM1_PORT, "[AHCI] Resetting HBA...\n");
    if (ahci_hba_reset(ahci_controller.abar) != 0) {
        com_write_string(COM1_PORT, "[AHCI] ERROR: HBA reset failed\n");
        return -1;
    }
    com_write_string(COM1_PORT, "[AHCI] HBA reset complete\n");
    
    // Enable AHCI mode
    com_write_string(COM1_PORT, "[AHCI] Enabling AHCI mode...\n");
    ahci_controller.abar->ghc |= HBA_GHC_AHCI_ENABLE;

    // Optional boot arg: "ahci-poll" forces polling mode (do not enable GHC.IE, do not install IRQ handler).
    ahci_force_poll = 0;
    {
        uint64_t mb2 = mdinit_get_mb2_ptr();
        if (mb2) {
            struct multiboot_tag *t = multiboot2_find_tag((void*)(uintptr_t)mb2, MULTIBOOT_TAG_TYPE_CMDLINE);
            if (t) {
                struct multiboot_tag_string *s = (struct multiboot_tag_string*)t;
                const char *cmd = s->string;
                // minimal token match: look for "ahci-poll" as a standalone token
                if (cmd) {
                    const char tok[] = "ahci-poll";
                    for (size_t i = 0; cmd[i]; i++) {
                        if (i > 0 && cmd[i-1] != ' ' && cmd[i-1] != '\t') continue;
                        size_t j = 0;
                        while (tok[j] && cmd[i + j] == tok[j]) j++;
                        if (tok[j] == 0) {
                            char end = cmd[i + j];
                            if (end == 0 || end == ' ' || end == '\t') {
                                ahci_force_poll = 1;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    // Wait for controller to be ready
    ahci_usleep(1000);
    
    // Probe ports first (before enabling global interrupts)
    com_write_string(COM1_PORT, "[AHCI] Probing ports...\n");
    int ports = ahci_probe_ports();
    
    if (ports == 0) {
        com_write_string(COM1_PORT, "[AHCI] WARNING: No AHCI drives detected\n");
        return -1;
    }
    
    com_printf(COM1_PORT, "[AHCI] Detected %d drive(s)\n", ports);
    
    // Now that all ports are initialized, enable interrupts
    if (ahci_force_poll) {
        com_write_string(COM1_PORT, "[AHCI] Boot arg ahci-poll set: forcing polling mode (interrupts disabled)\n");
        ahci_irq_line = -1;
        ahci_controller.abar->ghc &= ~HBA_GHC_IE;
    } else {
        // Install INTx IRQ handler (PIC) before enabling interrupts
        if (pci_dev && pci_dev->interrupt_line < 16) {
            ahci_irq_line = (int)pci_dev->interrupt_line;
            irq_install_handler(ahci_irq_line, ahci_irq_handler);
            com_printf(COM1_PORT, "[AHCI] Installed INTx handler on IRQ %d\n", ahci_irq_line);
        } else {
            com_write_string(COM1_PORT, "[AHCI] WARNING: No valid PCI interrupt line; staying in polling mode\n");
            ahci_irq_line = -1;
        }

        // Enable global interrupts only after all ports are ready
        if (ahci_irq_line >= 0) {
            ahci_controller.abar->ghc |= HBA_GHC_IE;
            (void)ahci_controller.abar->ghc;
            com_write_string(COM1_PORT, "[AHCI] Global interrupts enabled\n");
        }
    }
    
    com_write_string(COM1_PORT, "[AHCI] Driver initialized successfully\n");
    
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