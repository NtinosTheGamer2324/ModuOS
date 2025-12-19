#include "moduos/drivers/USB/Controllers/ehci.h"
#include "moduos/drivers/USB/usb.h"
#include "moduos/drivers/PCI/pci.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/io/io.h"
#include "moduos/kernel/interrupts/irq.h"
#include "moduos/arch/AMD64/interrupts/pic.h"
#include "moduos/kernel/macros.h"
#include <stddef.h>
#include "moduos/kernel/memory/string.h"

static ehci_controller_t *global_ehci = NULL;

// Transfer tracking
typedef struct ehci_transfer_info {
    usb_transfer_t *transfer;
    usb_device_t *device;
    ehci_qtd_t *first_qtd;
    ehci_qh_t *qh;
    struct ehci_transfer_info *next;
} ehci_transfer_info_t;

static ehci_transfer_info_t *active_transfers = NULL;

// Forward declarations
static void ehci_free_qtd(ehci_qtd_t *qtd);
static ehci_qtd_t* ehci_alloc_qtd(ehci_controller_t *ehci);
static ehci_qh_t* ehci_create_qh(ehci_controller_t *ehci, uint8_t addr, uint8_t endpoint,
                                  uint8_t speed, uint16_t max_packet);
static int ehci_create_qtd(ehci_controller_t *ehci, ehci_qtd_t *qtd, uint8_t pid,
                           void *buffer, uint16_t length, int toggle);

// Track transfer
static void ehci_track_transfer(ehci_transfer_info_t *info) {
    info->next = active_transfers;
    active_transfers = info;
}

static int ehci_setup_periodic_schedule(ehci_controller_t *ehci) {
    COM_LOG_INFO(COM1_PORT, "EHCI: Setting up periodic schedule");
    
    // Create interrupt QHs for different polling intervals
    for (int i = 0; i < 8; i++) {
        ehci->interrupt_qhs[i] = ehci_create_qh(ehci, 0, 0, USB_SPEED_HIGH, 8);
        if (!ehci->interrupt_qhs[i]) {
            COM_LOG_ERROR(COM1_PORT, "EHCI: Failed to create interrupt QH %d", i);
            for (int j = 0; j < i; j++) {
                if (ehci->interrupt_qhs[j]) kfree(ehci->interrupt_qhs[j]);
            }
            return -1;
        }
        
        ehci->interrupt_qhs[i]->characteristics |= EHCI_QH_CH_H;
        ehci->interrupt_qhs[i]->capabilities = (0x01 << 0);
        ehci->interrupt_qhs[i]->next_qtd_ptr = EHCI_LP_TERMINATE;
        ehci->interrupt_qhs[i]->alt_next_qtd_ptr = EHCI_LP_TERMINATE;
        ehci->interrupt_qhs[i]->token = 0;
    }
    
    // Link QHs in tree structure
    for (int i = 7; i > 0; i--) {
        uint64_t next_qh_phys = paging_virt_to_phys((uintptr_t)ehci->interrupt_qhs[i-1]);
        if (next_qh_phys == 0) {
            COM_LOG_ERROR(COM1_PORT, "EHCI: Failed to get QH physical address");
            return -1;
        }
        ehci->interrupt_qhs[i]->qh_link_ptr = (uint32_t)next_qh_phys | EHCI_LP_TYPE_QH;
    }
    
    ehci->interrupt_qhs[0]->qh_link_ptr = EHCI_LP_TERMINATE;
    
    // Memory barrier BEFORE programming frame list
    __asm__ volatile("mfence" ::: "memory");
    
    // Point all frame list entries to appropriate interval QH
    for (int i = 0; i < EHCI_FRAMELIST_COUNT; i++) {
        int interval_idx = 0;
        
        for (int j = 7; j >= 0; j--) {
            int period = 1 << j;
            if (i % period == 0) {
                interval_idx = j;
                break;
            }
        }
        
        uint64_t qh_phys = paging_virt_to_phys((uintptr_t)ehci->interrupt_qhs[interval_idx]);
        if (qh_phys == 0) {
            COM_LOG_ERROR(COM1_PORT, "EHCI: Failed to get periodic list QH address");
            return -1;
        }
        ehci->periodic_list[i] = (uint32_t)qh_phys | EHCI_LP_TYPE_QH;
    }
    
    // Final memory barrier
    __asm__ volatile("mfence" ::: "memory");
    
    COM_LOG_OK(COM1_PORT, "EHCI: Periodic schedule tree configured");
    return 0;
}

// Updated interrupt transfer submission
int ehci_submit_interrupt_transfer(usb_device_t *dev, usb_transfer_t *transfer) {
    if (!dev || !dev->controller || !transfer) return -1;
    
    ehci_controller_t *ehci = (ehci_controller_t*)dev->controller->controller_data;
    
    uint8_t pid = (transfer->endpoint & 0x80) ? USB_PID_IN : USB_PID_OUT;
    uint8_t ep_num = transfer->endpoint & 0x0F;
    
    // Allocate qTD for this transfer
    ehci_qtd_t *int_qtd = ehci_alloc_qtd(ehci);
    if (!int_qtd) {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Failed to allocate interrupt qTD");
        transfer->status = USB_TRANSFER_STATUS_ERROR;
        return -1;
    }
    
    // Create the qTD
    if (ehci_create_qtd(ehci, int_qtd, pid, transfer->buffer, transfer->length, 1) != 0) {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Failed to create interrupt qTD");
        ehci_free_qtd(int_qtd);
        transfer->status = USB_TRANSFER_STATUS_ERROR;
        return -1;
    }
    
    uint64_t int_qtd_phys = paging_virt_to_phys((uintptr_t)int_qtd);
    if (int_qtd_phys == 0) {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Failed to get qTD physical address");
        ehci_free_qtd(int_qtd);
        return -1;
    }
    
    int_qtd->next_qtd_ptr = EHCI_LP_TERMINATE;
    
    // Create QH for this endpoint
    ehci_qh_t *qh = ehci_create_qh(ehci, dev->address, ep_num, dev->speed, dev->max_packet_size);
    if (!qh) {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Failed to create interrupt QH");
        ehci_free_qtd(int_qtd);
        return -1;
    }
    
    // Configure QH for interrupt transfer
    // USB keyboards typically request 10ms polling, but we'll use 8ms
    qh->capabilities = (0x01 << 0);  // S-mask: execute in microframe 0
    qh->next_qtd_ptr = (uint32_t)int_qtd_phys;
    qh->alt_next_qtd_ptr = EHCI_LP_TERMINATE;
    qh->token = 0;  // Clear any status bits
    
    // Memory barrier
    __asm__ volatile("mfence" ::: "memory");
    
    // Get QH physical address
    uint64_t qh_phys = paging_virt_to_phys((uintptr_t)qh);
    if (qh_phys == 0) {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Failed to get QH physical address");
        kfree(qh);
        ehci_free_qtd(int_qtd);
        return -1;
    }
    
    // Insert this QH into the 8ms interval chain (index 3)
    // Link: new_qh -> old_chain
    qh->qh_link_ptr = ehci->interrupt_qhs[3]->qh_link_ptr;
    __asm__ volatile("mfence" ::: "memory");
    
    // Link: interval_qh -> new_qh
    ehci->interrupt_qhs[3]->qh_link_ptr = (uint32_t)qh_phys | EHCI_LP_TYPE_QH;
    __asm__ volatile("mfence" ::: "memory");
    
    // Track this transfer
    ehci_transfer_info_t *info = (ehci_transfer_info_t*)kmalloc(sizeof(ehci_transfer_info_t));
    if (!info) {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Failed to allocate transfer info");
        // Unlink QH
        ehci->interrupt_qhs[3]->qh_link_ptr = qh->qh_link_ptr;
        kfree(qh);
        ehci_free_qtd(int_qtd);
        return -1;
    }
    
    info->transfer = transfer;
    info->device = dev;
    info->first_qtd = int_qtd;
    info->qh = qh;
    
    ehci_track_transfer(info);
    
    transfer->status = USB_TRANSFER_STATUS_PENDING;
    
    return 0;
}

// Process completed transfers
static void ehci_process_completed_transfers(ehci_controller_t *ehci) {
    ehci_transfer_info_t **curr = &active_transfers;
    
    while (*curr) {
        ehci_transfer_info_t *info = *curr;
        ehci_qh_t *qh = info->qh;
        ehci_qtd_t *qtd = info->first_qtd;
        
        // Check if transfer is complete (not active anymore)
        if (!(qtd->token & EHCI_QTD_TOKEN_STATUS_ACTIVE)) {
            usb_transfer_t *transfer = info->transfer;
            
            // Check for errors
            uint32_t error_bits = EHCI_QTD_TOKEN_STATUS_HALTED | 
                                  EHCI_QTD_TOKEN_STATUS_DBERR | 
                                  EHCI_QTD_TOKEN_STATUS_BABBLE | 
                                  EHCI_QTD_TOKEN_STATUS_XACTERR;
            
            if (qtd->token & error_bits) {
                transfer->status = USB_TRANSFER_STATUS_ERROR;
                transfer->actual_length = 0;
            } else {
                // Calculate actual bytes transferred
                uint32_t bytes_left = (qtd->token >> 16) & 0x7FFF;
                transfer->actual_length = transfer->length - bytes_left;
                transfer->status = USB_TRANSFER_STATUS_COMPLETED;
            }
            
            // Remove from active list
            *curr = info->next;
            
            // Call completion callback (HID driver will resubmit)
            if (transfer->callback) {
                transfer->callback(info->device, transfer);
            }
            
            // Cleanup this completed transfer
            ehci_free_qtd(info->first_qtd);
            // Note: Don't free QH yet - it might be reused for resubmission
            // The callback (HID driver) handles resubmission
            kfree(info);
            
            continue;
        }
        
        curr = &(*curr)->next;
    }
}
static inline void ehci_delay_ms(int ms) {
    for (volatile int i = 0; i < ms * 1000; i++);
}

// IRQ handler
static void ehci_irq_handler(void) {
    if (!global_ehci) {
        pic_send_eoi(11);
        return;
    }
    
    ehci_controller_t *ehci = global_ehci;
    uint32_t status = ehci_read32(ehci, EHCI_OP_USBSTS);
    
    // Process even if status is 0
    if (status & EHCI_STS_USBINT) {
        ehci_process_completed_transfers(ehci);
    }
    
    if (status & EHCI_STS_ERROR) {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Error");
    }
    
    if (status & EHCI_STS_PCD) {
        for (int i = 0; i < ehci->num_ports; i++) {
            uint32_t ps = ehci_read32(ehci, EHCI_OP_PORTSC + (i * 4));
            if (ps & (EHCI_PORT_CSC | EHCI_PORT_PEDC | EHCI_PORT_OCC)) {
                ehci_write32(ehci, EHCI_OP_PORTSC + (i * 4), ps);
            }
        }
    }
    
    // Always clear status
    if (status) {
        ehci_write32(ehci, EHCI_OP_USBSTS, status);
    }
    
    // send EOI
    pic_send_eoi(ehci->pci_dev->interrupt_line);
}

// Helper functions
uint32_t ehci_read32(ehci_controller_t *ehci, uint32_t reg) {
    return ehci->op_regs[reg / 4];
}

void ehci_write32(ehci_controller_t *ehci, uint32_t reg, uint32_t value) {
    ehci->op_regs[reg / 4] = value;
}

// Allocate qTD
static ehci_qtd_t* ehci_alloc_qtd(ehci_controller_t *ehci) {
    for (int i = 0; i < ehci->qtd_pool_count; i++) {
        // Check if this TD is free by looking at the next pointer
        // A TD with TERMINATE in next_qtd_ptr and token=0 is free
        if (ehci->qtd_pool[i].next_qtd_ptr == EHCI_LP_TERMINATE && 
            ehci->qtd_pool[i].token == 0) {
            // Mark as allocated by setting a non-terminate value temporarily
            ehci->qtd_pool[i].next_qtd_ptr = 0xDEADBEEF;  // Temp marker
            return &ehci->qtd_pool[i];
        }
    }
    return NULL;
}

// Define ehci_free_qtd
static void ehci_free_qtd(ehci_qtd_t *qtd) {
    qtd->next_qtd_ptr = EHCI_LP_TERMINATE;
    qtd->alt_next_qtd_ptr = EHCI_LP_TERMINATE;
    qtd->token = 0;
    for (int i = 0; i < 5; i++) {
        qtd->buffer_ptr[i] = 0;
    }
}

// Create QH
static ehci_qh_t* ehci_create_qh(ehci_controller_t *ehci, uint8_t addr, uint8_t endpoint,
                                  uint8_t speed, uint16_t max_packet) {
    ehci_qh_t *qh = (ehci_qh_t*)kmalloc_aligned(sizeof(ehci_qh_t), 32);
    if (!qh) return NULL;
    
    memset(qh, 0, sizeof(ehci_qh_t));
    
    uint32_t characteristics = (addr & EHCI_QH_CH_DEVADDR_MASK) |
                               ((endpoint << EHCI_QH_CH_ENDPT_SHIFT) & EHCI_QH_CH_ENDPT_MASK) |
                               ((max_packet << EHCI_QH_CH_MAXPKT_SHIFT) & EHCI_QH_CH_MAXPKT_MASK);
    
    if (speed == USB_SPEED_HIGH) {
        characteristics |= EHCI_QH_CH_EPS_HIGH;
    } else if (speed == USB_SPEED_FULL) {
        characteristics |= EHCI_QH_CH_EPS_FULL;
    } else if (speed == USB_SPEED_LOW) {
        characteristics |= EHCI_QH_CH_EPS_LOW;
    }
    
    // DTC: Data Toggle Control from qTD
    characteristics |= EHCI_QH_CH_DTC;
    
    // NAK Counter Reload (15 = infinite retries)
    characteristics |= ((15 << EHCI_QH_CH_RL_SHIFT) & EHCI_QH_CH_RL_MASK);
    
    // For control endpoints, set C-mask (split transaction)
    uint32_t capabilities = 0;
    if (endpoint == 0) {
        // Control endpoint - need proper split transaction handling for non-high-speed
        if (speed != USB_SPEED_HIGH) {
            // For full/low speed through high-speed hub
            capabilities |= (0x01 << 0); // S-mask: start split at microframe 0
            capabilities |= (0x1C << 8); // C-mask: complete splits at microframes 2,3,4
        }
    }
    
    qh->qh_link_ptr = EHCI_LP_TERMINATE;
    qh->characteristics = characteristics;
    qh->capabilities = capabilities;
    qh->current_qtd_ptr = 0;
    qh->next_qtd_ptr = EHCI_LP_TERMINATE;
    qh->alt_next_qtd_ptr = EHCI_LP_TERMINATE;
    qh->token = 0;  // Clear status bits
    
    for (int i = 0; i < 5; i++) {
        qh->buffer_ptr[i] = 0;
    }
    
    return qh;
}

// Create qTD
static int ehci_create_qtd(ehci_controller_t *ehci, ehci_qtd_t *qtd, uint8_t pid,
                           void *buffer, uint16_t length, int toggle) {
    qtd->next_qtd_ptr = EHCI_LP_TERMINATE;
    qtd->alt_next_qtd_ptr = EHCI_LP_TERMINATE;
    
    uint32_t token = EHCI_QTD_TOKEN_STATUS_ACTIVE |
                     ((3 << EHCI_QTD_TOKEN_CERR_SHIFT) & EHCI_QTD_TOKEN_CERR_MASK) |
                     (length << 16);
    
    if (pid == USB_PID_SETUP) {
        token |= EHCI_QTD_TOKEN_PID_SETUP;
    } else if (pid == USB_PID_IN) {
        token |= EHCI_QTD_TOKEN_PID_IN;
    } else if (pid == USB_PID_OUT) {
        token |= EHCI_QTD_TOKEN_PID_OUT;
    }
    
    if (toggle) {
        token |= (1 << 31);
    }
    
    token |= EHCI_QTD_TOKEN_IOC;
    
    qtd->token = token;
    
    if (buffer && length > 0) {
        // CRITICAL: Properly translate buffer virtual address to physical
        uintptr_t virt_addr = (uintptr_t)buffer;
        uint64_t phys_addr;
        
        // Check if this is a high-half kernel address (heap)
        if (virt_addr >= 0xFFFF800000000000ULL) {
            phys_addr = paging_virt_to_phys(virt_addr);
            if (phys_addr == 0) {
                COM_LOG_ERROR(COM1_PORT, "EHCI: Failed to translate heap buffer");
                return -1;
            }
        }
        // Check if this is identity-mapped low memory (kernel text/data/stack)
        else if (virt_addr < 0x40000000ULL) {
            // Identity mapped - physical == virtual
            phys_addr = virt_addr;
        }
        else {
            // Try page tables first
            phys_addr = paging_virt_to_phys(virt_addr);
            if (phys_addr == 0) {
                COM_LOG_ERROR(COM1_PORT, "EHCI: Failed to translate buffer address");
                return -1;
            }
        }
        
        qtd->buffer_ptr[0] = (uint32_t)phys_addr;
        
        // Handle buffers that span multiple pages
        for (int i = 1; i < 5 && length > (4096 * i); i++) {
            uint64_t next_virt = virt_addr + (4096 * i);
            uint64_t next_page_phys;
            
            if (virt_addr >= 0xFFFF800000000000ULL) {
                next_page_phys = paging_virt_to_phys(next_virt);
            } else if (next_virt < 0x40000000ULL) {
                next_page_phys = next_virt;
            } else {
                next_page_phys = paging_virt_to_phys(next_virt);
            }
            
            if (next_page_phys == 0) {
                COM_LOG_ERROR(COM1_PORT, "EHCI: Failed to translate buffer page");
                return -1;
            }
            qtd->buffer_ptr[i] = (uint32_t)(next_page_phys & 0xFFFFF000);
        }
    } else {
        for (int i = 0; i < 5; i++) {
            qtd->buffer_ptr[i] = 0;
        }
    }
    
    return 0;
}

int ehci_control_transfer(usb_device_t *dev, usb_setup_packet_t *setup, void *data) {
    if (!dev || !dev->controller || !setup) {
        return -1;
    }
    
    ehci_controller_t *ehci = (ehci_controller_t*)dev->controller->controller_data;
    
    com_write_string(COM1_PORT, "[EHCI-XFER] Starting control transfer\n");
    com_write_string(COM1_PORT, "[EHCI-XFER] Device addr=");
    char hex[20];
    hex[0] = '0' + dev->address;
    hex[1] = ' ';
    hex[2] = 's'; hex[3] = 'p'; hex[4] = 'e'; hex[5] = 'e'; hex[6] = 'd'; hex[7] = '=';
    hex[8] = '0' + dev->speed;
    hex[9] = ' ';
    hex[10] = 'm'; hex[11] = 'p'; hex[12] = 's'; hex[13] = '=';
    hex[14] = '0' + (dev->max_packet_size / 10);
    hex[15] = '0' + (dev->max_packet_size % 10);
    hex[16] = '\n';
    hex[17] = 0;
    com_write_string(COM1_PORT, hex);
    
    // Create QH for this transfer
    ehci_qh_t *qh = ehci_create_qh(ehci, dev->address, 0, dev->speed, dev->max_packet_size);
    if (!qh) {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Failed to create control QH");
        return -1;
    }
    
    com_write_string(COM1_PORT, "[EHCI-XFER] QH created, characteristics=0x");
    uint32_t val = qh->characteristics;
    for (int i = 7; i >= 0; i--) {
        hex[i] = "0123456789abcdef"[val & 0xF];
        val >>= 4;
    }
    hex[8] = '\n';
    hex[9] = 0;
    com_write_string(COM1_PORT, hex);
    
    // Allocate TDs
    ehci_qtd_t *setup_td = ehci_alloc_qtd(ehci);
    ehci_qtd_t *data_td = NULL;
    ehci_qtd_t *status_td = ehci_alloc_qtd(ehci);
    
    if (!setup_td || !status_td) {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Failed to allocate TDs");
        if (setup_td) ehci_free_qtd(setup_td);
        if (status_td) ehci_free_qtd(status_td);
        kfree(qh);
        return -1;
    }
    
    // Get physical addresses of all TDs upfront
    uint64_t setup_td_phys = paging_virt_to_phys((uintptr_t)setup_td);
    uint64_t status_td_phys = paging_virt_to_phys((uintptr_t)status_td);
    uint64_t data_td_phys = 0;
    
    com_write_string(COM1_PORT, "[EHCI-XFER] TDs: setup=0x");
    val = (uint32_t)setup_td_phys;
    for (int i = 7; i >= 0; i--) {
        hex[i] = "0123456789abcdef"[val & 0xF];
        val >>= 4;
    }
    hex[8] = ' ';
    hex[9] = 's'; hex[10] = 't'; hex[11] = 'a'; hex[12] = 't'; hex[13] = 'u'; hex[14] = 's'; hex[15] = '='; hex[16] = '0'; hex[17] = 'x';
    val = (uint32_t)status_td_phys;
    for (int i = 7; i >= 0; i--) {
        hex[18+i] = "0123456789abcdef"[val & 0xF];
        val >>= 4;
    }
    hex[26] = '\n';
    hex[27] = 0;
    com_write_string(COM1_PORT, hex);
    
    if (setup_td_phys == 0 || status_td_phys == 0) {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Failed to get TD physical addresses");
        ehci_free_qtd(setup_td);
        ehci_free_qtd(status_td);
        kfree(qh);
        return -1;
    }
    
    // Setup stage (always DATA0)
    com_write_string(COM1_PORT, "[EHCI-XFER] Creating setup TD\n");
    if (ehci_create_qtd(ehci, setup_td, USB_PID_SETUP, setup, sizeof(usb_setup_packet_t), 0) != 0) {
        ehci_free_qtd(setup_td);
        ehci_free_qtd(status_td);
        kfree(qh);
        return -1;
    }
    
    com_write_string(COM1_PORT, "[EHCI-XFER] Setup TD token=0x");
    val = setup_td->token;
    for (int i = 7; i >= 0; i--) {
        hex[i] = "0123456789abcdef"[val & 0xF];
        val >>= 4;
    }
    hex[8] = ' ';
    hex[9] = 'b'; hex[10] = 'u'; hex[11] = 'f'; hex[12] = '='; hex[13] = '0'; hex[14] = 'x';
    val = setup_td->buffer_ptr[0];
    for (int i = 7; i >= 0; i--) {
        hex[15+i] = "0123456789abcdef"[val & 0xF];
        val >>= 4;
    }
    hex[23] = '\n';
    hex[24] = 0;
    com_write_string(COM1_PORT, hex);
    
    // Data stage (if needed)
    int data_length = setup->wLength;
    com_write_string(COM1_PORT, "[EHCI-XFER] Data length=");
    hex[0] = '0' + (data_length / 10);
    hex[1] = '0' + (data_length % 10);
    hex[2] = '\n';
    hex[3] = 0;
    com_write_string(COM1_PORT, hex);
    
    if (data_length > 0 && data) {
        data_td = ehci_alloc_qtd(ehci);
        if (!data_td) {
            COM_LOG_ERROR(COM1_PORT, "EHCI: Failed to allocate data TD");
            ehci_free_qtd(setup_td);
            ehci_free_qtd(status_td);
            kfree(qh);
            return -1;
        }
        
        data_td_phys = paging_virt_to_phys((uintptr_t)data_td);
        if (data_td_phys == 0) {
            COM_LOG_ERROR(COM1_PORT, "EHCI: Failed to get data TD physical address");
            ehci_free_qtd(setup_td);
            ehci_free_qtd(data_td);
            ehci_free_qtd(status_td);
            kfree(qh);
            return -1;
        }
        
        uint8_t data_pid = (setup->bmRequestType & USB_DIR_IN) ? USB_PID_IN : USB_PID_OUT;
        com_write_string(COM1_PORT, "[EHCI-XFER] Creating data TD (");
        com_write_string(COM1_PORT, (data_pid == USB_PID_IN) ? "IN" : "OUT");
        com_write_string(COM1_PORT, ")\n");
        
        if (ehci_create_qtd(ehci, data_td, data_pid, data, data_length, 1) != 0) {
            ehci_free_qtd(setup_td);
            ehci_free_qtd(data_td);
            ehci_free_qtd(status_td);
            kfree(qh);
            return -1;
        }
        
        // Link: Setup -> Data (using physical addresses)
        setup_td->next_qtd_ptr = (uint32_t)data_td_phys;
        com_write_string(COM1_PORT, "[EHCI-XFER] Linked setup->data\n");
    }
    
    // Status stage (opposite direction of data, or IN if no data)
    uint8_t status_pid;
    if (data_length > 0) {
        status_pid = (setup->bmRequestType & USB_DIR_IN) ? USB_PID_OUT : USB_PID_IN;
    } else {
        status_pid = USB_PID_IN;
    }
    
    com_write_string(COM1_PORT, "[EHCI-XFER] Creating status TD (");
    com_write_string(COM1_PORT, (status_pid == USB_PID_IN) ? "IN" : "OUT");
    com_write_string(COM1_PORT, ")\n");
    
    if (ehci_create_qtd(ehci, status_td, status_pid, NULL, 0, 1) != 0) {
        ehci_free_qtd(setup_td);
        if (data_td) ehci_free_qtd(data_td);
        ehci_free_qtd(status_td);
        kfree(qh);
        return -1;
    }
    
    // Link chain using physical addresses
    if (data_td) {
        data_td->next_qtd_ptr = (uint32_t)status_td_phys;
    } else {
        setup_td->next_qtd_ptr = (uint32_t)status_td_phys;
    }
    status_td->next_qtd_ptr = EHCI_LP_TERMINATE;
    
    com_write_string(COM1_PORT, "[EHCI-XFER] TD chain linked\n");
    
    // Get QH physical address
    uint64_t qh_phys = paging_virt_to_phys((uintptr_t)qh);
    if (qh_phys == 0) {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Failed to get QH physical address");
        ehci_free_qtd(setup_td);
        if (data_td) ehci_free_qtd(data_td);
        ehci_free_qtd(status_td);
        kfree(qh);
        return -1;
    }
    
    com_write_string(COM1_PORT, "[EHCI-XFER] QH phys=0x");
    val = (uint32_t)qh_phys;
    for (int i = 7; i >= 0; i--) {
        hex[i] = "0123456789abcdef"[val & 0xF];
        val >>= 4;
    }
    hex[8] = '\n';
    hex[9] = 0;
    com_write_string(COM1_PORT, hex);
    
    // Attach TD chain to QH overlay
    qh->next_qtd_ptr = (uint32_t)setup_td_phys;
    qh->alt_next_qtd_ptr = EHCI_LP_TERMINATE;
    qh->token = 0;  // Clear token to allow new transfer
    
    // Memory barrier to ensure all writes complete
    __asm__ volatile("mfence" ::: "memory");
    
    com_write_string(COM1_PORT, "[EHCI-XFER] Inserting QH into async schedule\n");
    
    // Insert into async schedule BEFORE the async QH (which points to itself)
    qh->qh_link_ptr = ehci->async_qh->qh_link_ptr;
    __asm__ volatile("mfence" ::: "memory");
    ehci->async_qh->qh_link_ptr = (uint32_t)qh_phys | EHCI_LP_TYPE_QH;
    __asm__ volatile("mfence" ::: "memory");
    
    com_write_string(COM1_PORT, "[EHCI-XFER] QH inserted, waiting for hardware\n");
    
    // Wait a moment for hardware to pick up the new QH
    ehci_delay_ms(2);
    
    // Ring doorbell to notify controller
    uint32_t cmd = ehci_read32(ehci, EHCI_OP_USBCMD);
    ehci_write32(ehci, EHCI_OP_USBCMD, cmd | EHCI_CMD_IAAD);
    
    com_write_string(COM1_PORT, "[EHCI-XFER] Doorbell rung, waiting for IAA\n");
    
    // Wait for IAA (Interrupt on Async Advance)
    // Note: IAA timeout is normal if interrupts are working - the transfer
    // continues anyway and completes successfully
    int iaa_timeout = 100;
    while (iaa_timeout-- > 0) {
        uint32_t status = ehci_read32(ehci, EHCI_OP_USBSTS);
        if (status & EHCI_STS_IAA) {
            ehci_write32(ehci, EHCI_OP_USBSTS, EHCI_STS_IAA);  // Clear IAA
            com_write_string(COM1_PORT, "[EHCI-XFER] IAA received\n");
            break;
        }
        ehci_delay_ms(1);
    }
    
    if (iaa_timeout <= 0) {
        // This is normal - IAA may be handled by interrupt handler
        com_write_string(COM1_PORT, "[EHCI-XFER] IAA timeout (continuing anyway)\n");
    }
    
    com_write_string(COM1_PORT, "[EHCI-XFER] Waiting for transfer completion\n");
    
    // Now wait for transfer completion
    int timeout = 1000;
    int last_print = 0;
    while (timeout-- > 0) {
        // Check if any TD in the chain is still active
        if (!(setup_td->token & EHCI_QTD_TOKEN_STATUS_ACTIVE) &&
            (!data_td || !(data_td->token & EHCI_QTD_TOKEN_STATUS_ACTIVE)) &&
            !(status_td->token & EHCI_QTD_TOKEN_STATUS_ACTIVE)) {
            break;
        }
        
        // Print status every 100ms
        if (timeout % 100 == 0 && timeout != last_print) {
            last_print = timeout;
            com_write_string(COM1_PORT, "[EHCI-XFER] Still waiting... setup token=0x");
            val = setup_td->token;
            for (int i = 7; i >= 0; i--) {
                hex[i] = "0123456789abcdef"[val & 0xF];
                val >>= 4;
            }
            hex[8] = '\n';
            hex[9] = 0;
            com_write_string(COM1_PORT, hex);
        }
        
        ehci_delay_ms(1);
    }
    
    // Check for errors in any TD
    int result = -1;
    uint32_t error_bits = EHCI_QTD_TOKEN_STATUS_HALTED | 
                          EHCI_QTD_TOKEN_STATUS_DBERR | 
                          EHCI_QTD_TOKEN_STATUS_BABBLE | 
                          EHCI_QTD_TOKEN_STATUS_XACTERR;
    
    com_write_string(COM1_PORT, "[EHCI-XFER] Transfer complete, checking results\n");
    com_write_string(COM1_PORT, "[EHCI-XFER] Final tokens: setup=0x");
    val = setup_td->token;
    for (int i = 7; i >= 0; i--) {
        hex[i] = "0123456789abcdef"[val & 0xF];
        val >>= 4;
    }
    hex[8] = '\n';
    hex[9] = 0;
    com_write_string(COM1_PORT, hex);
    
    if (data_td) {
        com_write_string(COM1_PORT, "[EHCI-XFER] data=0x");
        val = data_td->token;
        for (int i = 7; i >= 0; i--) {
            hex[i] = "0123456789abcdef"[val & 0xF];
            val >>= 4;
        }
        hex[8] = '\n';
        hex[9] = 0;
        com_write_string(COM1_PORT, hex);
    }
    
    com_write_string(COM1_PORT, "[EHCI-XFER] status=0x");
    val = status_td->token;
    for (int i = 7; i >= 0; i--) {
        hex[i] = "0123456789abcdef"[val & 0xF];
        val >>= 4;
    }
    hex[8] = '\n';
    hex[9] = 0;
    com_write_string(COM1_PORT, hex);
    
    if (timeout > 0) {
        if (!(setup_td->token & error_bits) &&
            (!data_td || !(data_td->token & error_bits)) &&
            !(status_td->token & error_bits)) {
            result = 0;
            com_write_string(COM1_PORT, "[EHCI-XFER] Transfer successful!\n");
        } else {
            COM_LOG_ERROR(COM1_PORT, "EHCI: Transfer error");
        }
    } else {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Control transfer timeout");
    }
    
    // Remove from async schedule - need to find our QH and unlink it
    ehci_qh_t *prev_qh = ehci->async_qh;
    uint32_t prev_link = prev_qh->qh_link_ptr;
    
    // Traverse the circular list
    for (int i = 0; i < 100; i++) {  // Safety limit
        uint32_t next_phys = prev_link & 0xFFFFFFE0;  // Clear type and T bits
        
        if (next_phys == (uint32_t)qh_phys) {
            // Found our QH, unlink it
            prev_qh->qh_link_ptr = qh->qh_link_ptr;
            __asm__ volatile("mfence" ::: "memory");
            break;
        }
        
        if (next_phys == ehci->async_qh_phys) {
            // Back to start, QH not found (shouldn't happen)
            break;
        }
        
        // Move to next QH (need to convert physical back to virtual)
        // This is tricky - we need to search our known QHs
        if (next_phys == ehci->async_qh_phys) {
            break;
        }
        
        prev_qh = qh;  // Assume we'll find it next iteration
        prev_link = qh->qh_link_ptr;
    }
    
    // Wait for async schedule to stabilize
    ehci_delay_ms(1);
    
    // Cleanup
    ehci_free_qtd(setup_td);
    if (data_td) ehci_free_qtd(data_td);
    ehci_free_qtd(status_td);
    kfree(qh);
    
    return result;
}

// Reset controller
static int ehci_reset(ehci_controller_t *ehci) {
    COM_LOG_INFO(COM1_PORT, "EHCI: Resetting");
    
    uint32_t cmd = ehci_read32(ehci, EHCI_OP_USBCMD);
    cmd &= ~EHCI_CMD_RS;
    ehci_write32(ehci, EHCI_OP_USBCMD, cmd);
    
    int timeout = 1000;
    while (!(ehci_read32(ehci, EHCI_OP_USBSTS) & EHCI_STS_HCHALTED) && timeout--) {
        for (volatile int i = 0; i < 1000; i++);
    }
    
    if (timeout <= 0) {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Failed to halt");
        return -1;
    }
    
    cmd = ehci_read32(ehci, EHCI_OP_USBCMD);
    cmd |= EHCI_CMD_HCRESET;
    ehci_write32(ehci, EHCI_OP_USBCMD, cmd);
    
    timeout = 1000;
    while ((ehci_read32(ehci, EHCI_OP_USBCMD) & EHCI_CMD_HCRESET) && timeout--) {
        for (volatile int i = 0; i < 1000; i++);
    }
    
    if (timeout <= 0) {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Reset timeout");
        return -1;
    }
    
    COM_LOG_OK(COM1_PORT, "EHCI: Reset complete");
    return 0;
}

// Controller operations
static usb_controller_ops_t ehci_ops;

// Probe EHCI
int ehci_probe(pci_device_t *pci_dev) {
    COM_LOG_INFO(COM1_PORT, "EHCI: Probing");
    
    ehci_controller_t *ehci = (ehci_controller_t*)kmalloc(sizeof(ehci_controller_t));
    if (!ehci) {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Allocation failed");
        return -1;
    }
    
    memset(ehci, 0, sizeof(ehci_controller_t));
    ehci->pci_dev = pci_dev;
    ehci->next_address = 1;
    
    // Read BAR0
    uint32_t bar0 = pci_read_config(pci_dev->bus, pci_dev->device, pci_dev->function, 0x10);
    ehci->mmio_phys = bar0 & 0xFFFFFFF0;
    
    com_write_string(COM1_PORT, "[INFO] EHCI: BAR0 physical address: 0x");
    char hex_buf[12];
    uint32_t val = ehci->mmio_phys;
    for (int i = 7; i >= 0; i--) {
        hex_buf[i] = "0123456789abcdef"[val & 0xF];
        val >>= 4;
    }
    hex_buf[8] = '\0';
    com_write_string(COM1_PORT, hex_buf);
    com_write_string(COM1_PORT, "\n");
    
    // Map MMIO region to virtual address space
    // EHCI typically needs at least 4KB, but we'll map more to be safe
    ehci->mmio_base = (volatile uint8_t*)ioremap(ehci->mmio_phys, 8192);
    if (!ehci->mmio_base) {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Failed to map MMIO region");
        kfree(ehci);
        return -1;
    }
    
    // Verify MMIO access
    uint8_t cap_length = ehci->mmio_base[EHCI_CAP_CAPLENGTH];
    if (cap_length == 0 || cap_length > 0x40) {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Invalid capability length");
        kfree(ehci);
        return -1;
    }
    
    com_write_string(COM1_PORT, "[INFO] EHCI: Capability length: 0x");
    char cap_hex[4];
    cap_hex[0] = "0123456789abcdef"[(cap_length >> 4) & 0xF];
    cap_hex[1] = "0123456789abcdef"[cap_length & 0xF];
    cap_hex[2] = '\0';
    com_write_string(COM1_PORT, cap_hex);
    com_write_string(COM1_PORT, "\n");
    
    COM_LOG_OK(COM1_PORT, "EHCI: MMIO accessible");
    
    // Enable bus mastering and memory space
    uint16_t command = pci_read_config(pci_dev->bus, pci_dev->device, pci_dev->function, 0x04) & 0xFFFF;
    command |= 0x06;
    pci_write_config(pci_dev->bus, pci_dev->device, pci_dev->function, 0x04, command);
    
    // Set up registers
    ehci->cap_regs = (volatile uint32_t*)ehci->mmio_base;
    ehci->op_regs = (volatile uint32_t*)(ehci->mmio_base + cap_length);
    
    // Read number of ports
    uint32_t hcsparams = ehci->cap_regs[EHCI_CAP_HCSPARAMS / 4];
    ehci->num_ports = hcsparams & EHCI_HCSPARAMS_N_PORTS_MASK;
    
    com_write_string(COM1_PORT, "[INFO] EHCI: Number of ports: ");
    char port_str[4];
    port_str[0] = '0' + ehci->num_ports;
    port_str[1] = '\0';
    com_write_string(COM1_PORT, port_str);
    com_write_string(COM1_PORT, "\n");
    
    // Reset controller
    if (ehci_reset(ehci) < 0) {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Reset failed");
        kfree(ehci);
        return -1;
    }
    
    // Allocate periodic frame list
    ehci->periodic_list = (uint32_t*)kmalloc_aligned(EHCI_FRAMELIST_COUNT * 4, 4096);
    if (!ehci->periodic_list) {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Failed to allocate periodic list");
        kfree(ehci);
        return -1;
    }
    
    for (int i = 0; i < EHCI_FRAMELIST_COUNT; i++) {
        ehci->periodic_list[i] = EHCI_LP_TERMINATE;
    }
    
    // Convert to physical address for DMA
    ehci->periodic_list_phys = (uint32_t)paging_virt_to_phys((uintptr_t)ehci->periodic_list);
    
    // Allocate qTD pool (32 descriptors should be enough for typical usage)
    ehci->qtd_pool_count = 32;
    ehci->qtd_pool = (ehci_qtd_t*)kmalloc_aligned(sizeof(ehci_qtd_t) * ehci->qtd_pool_count, 32);
    if (!ehci->qtd_pool) {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Failed to allocate qTD pool");
        kfree(ehci->periodic_list);
        kfree(ehci);
        return -1;
    }
    
    // Initialize all qTDs as free
    for (int i = 0; i < ehci->qtd_pool_count; i++) {
        ehci_free_qtd(&ehci->qtd_pool[i]);
    }
    
    // Setup periodic schedule after qTD pool is ready
    if (ehci_setup_periodic_schedule(ehci) != 0) {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Failed to setup periodic schedule");
        kfree(ehci->qtd_pool);
        kfree(ehci->periodic_list);
        kfree(ehci);
        return -1;
    }
    
    COM_LOG_OK(COM1_PORT, "EHCI: Memory structures allocated");
    
    // Create async QH
    ehci->async_qh = ehci_create_qh(ehci, 0, 0, USB_SPEED_HIGH, 64);
    if (!ehci->async_qh) {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Failed to create async QH");
        kfree(ehci->qtd_pool);
        kfree(ehci->periodic_list);
        kfree(ehci);
        return -1;
    }
    
    ehci->async_qh->characteristics |= EHCI_QH_CH_H;
    uint64_t async_qh_phys = paging_virt_to_phys((uintptr_t)ehci->async_qh);
    ehci->async_qh->qh_link_ptr = (uint32_t)async_qh_phys | EHCI_LP_TYPE_QH;
    ehci->async_qh_phys = (uint32_t)async_qh_phys;
    
    // Create other QHs
    ehci->control_qh = ehci_create_qh(ehci, 0, 0, USB_SPEED_HIGH, 64);
    ehci->bulk_qh = ehci_create_qh(ehci, 0, 0, USB_SPEED_HIGH, 512);
    ehci->interrupt_qh = ehci_create_qh(ehci, 0, 0, USB_SPEED_HIGH, 64);
    
    if (!ehci->control_qh || !ehci->bulk_qh || !ehci->interrupt_qh) {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Failed to create queue heads");
        kfree(ehci->async_qh);
        kfree(ehci->qtd_pool);
        kfree(ehci->periodic_list);
        kfree(ehci);
        return -1;
    }
    
    COM_LOG_OK(COM1_PORT, "EHCI: Queue heads created");
    
    // Program base addresses
    ehci_write32(ehci, EHCI_OP_PERIODICLISTBASE, ehci->periodic_list_phys);
    ehci_write32(ehci, EHCI_OP_ASYNCLISTADDR, ehci->async_qh_phys);
    
    // Set up IRQ
    if (ehci->pci_dev->interrupt_line != 0xFF && ehci->pci_dev->interrupt_line < 16) {
        global_ehci = ehci;
        irq_install_handler(ehci->pci_dev->interrupt_line, ehci_irq_handler);
        COM_LOG_OK(COM1_PORT, "EHCI: IRQ installed");
    } else {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Invalid IRQ");
        kfree(ehci->interrupt_qh);
        kfree(ehci->bulk_qh);
        kfree(ehci->control_qh);
        kfree(ehci->async_qh);
        kfree(ehci->qtd_pool);
        kfree(ehci->periodic_list);
        kfree(ehci);
        return -1;
    }
    
    // Enable interrupts
    ehci_write32(ehci, EHCI_OP_USBINTR, 
                 EHCI_INTR_USBINT | EHCI_INTR_ERROR | EHCI_INTR_PCD | 
                 EHCI_INTR_IAA | EHCI_INTR_HSE | EHCI_INTR_FLR);
    
    COM_LOG_INFO(COM1_PORT, "EHCI: Starting controller (stage 1: async only)");
        
    // Stage 1: Start with ONLY async schedule
    uint32_t cmd = EHCI_CMD_RS |        // Run
                   EHCI_CMD_ASE |       // Async Schedule Enable ONLY
                   EHCI_CMD_FLS_1024 |  // 1024 frame list
                   (8 << EHCI_CMD_ITC_SHIFT);  // Interrupt threshold
    ehci_write32(ehci, EHCI_OP_USBCMD, cmd);
        
    // Wait for controller to start and async schedule to activate
    int timeout = 1000;
    while (timeout-- > 0) {
        uint32_t status = ehci_read32(ehci, EHCI_OP_USBSTS);
        
        // Check if controller is running and async is active
        if (!(status & EHCI_STS_HCHALTED) && (status & EHCI_STS_ASS)) {
            break;
        }
        
        ehci_delay_ms(1);
    }
    
    if (timeout <= 0) {
        uint32_t status = ehci_read32(ehci, EHCI_OP_USBSTS);
        COM_LOG_ERROR(COM1_PORT, "EHCI: Stage 1 failed (status=0x%08x)", status);
        // Cleanup...
        return -1;
    }
    
    COM_LOG_OK(COM1_PORT, "EHCI: Async schedule running");
    
    // Stage 2: Now enable periodic schedule
    COM_LOG_INFO(COM1_PORT, "EHCI: Starting stage 2: periodic schedule");
    
    cmd = ehci_read32(ehci, EHCI_OP_USBCMD);
    cmd |= EHCI_CMD_PSE;  // Add Periodic Schedule Enable
    ehci_write32(ehci, EHCI_OP_USBCMD, cmd);
    
    // Wait for periodic schedule to activate
    timeout = 1000;
    while (timeout-- > 0) {
        uint32_t status = ehci_read32(ehci, EHCI_OP_USBSTS);
        
        if (status & EHCI_STS_PSS) {
            break;
        }
        
        ehci_delay_ms(1);
    }
    
    if (timeout <= 0) {
        uint32_t status = ehci_read32(ehci, EHCI_OP_USBSTS);
        COM_LOG_ERROR(COM1_PORT, "EHCI: Stage 2 failed (status=0x%08x)", status);
        // Cleanup...
        return -1;
    }
    
    COM_LOG_OK(COM1_PORT, "EHCI: Controller running (async + periodic)");
    
    // Set configure flag AFTER controller is fully running
    ehci_write32(ehci, EHCI_OP_CONFIGFLAG, EHCI_CONFIGFLAG_CF);
    
    // Longer stabilization delay for hardware to settle
    for (volatile int i = 0; i < 500000; i++);
    
    // Verify both schedules are still active
    uint32_t final_status = ehci_read32(ehci, EHCI_OP_USBSTS);
    if ((final_status & EHCI_STS_HCHALTED) || 
        !(final_status & EHCI_STS_ASS) || 
        !(final_status & EHCI_STS_PSS)) {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Controller state unstable (status=0x%08x)", final_status);
        return -1;
    }
    
    COM_LOG_OK(COM1_PORT, "EHCI: Controller verified stable");
    
    // Create USB controller structure
    usb_controller_t *controller = (usb_controller_t*)kmalloc(sizeof(usb_controller_t));
    if (!controller) {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Controller allocation failed");
        irq_uninstall_handler(ehci->pci_dev->interrupt_line);
        kfree(ehci->interrupt_qh);
        kfree(ehci->bulk_qh);
        kfree(ehci->control_qh);
        kfree(ehci->async_qh);
        kfree(ehci->qtd_pool);
        kfree(ehci->periodic_list);
        kfree(ehci);
        return -1;
    }
    
    memset(controller, 0, sizeof(usb_controller_t));
    controller->name = "EHCI";
    controller->num_ports = ehci->num_ports;
    controller->controller_data = ehci;
    controller->ops = &ehci_ops;
    
    if (usb_register_controller(controller) != 0) {
        COM_LOG_ERROR(COM1_PORT, "EHCI: Failed to register controller");
        kfree(controller);
        irq_uninstall_handler(ehci->pci_dev->interrupt_line);
        kfree(ehci->interrupt_qh);
        kfree(ehci->bulk_qh);
        kfree(ehci->control_qh);
        kfree(ehci->async_qh);
        kfree(ehci->qtd_pool);
        kfree(ehci->periodic_list);
        kfree(ehci);
        return -1;
    }
    
    COM_LOG_OK(COM1_PORT, "EHCI: Initialized");
    return 0;
}



// Reset port
void ehci_reset_port(usb_controller_t *controller, uint8_t port) {
    ehci_controller_t *ehci = (ehci_controller_t*)controller->controller_data;
    uint32_t port_reg = EHCI_OP_PORTSC + (port * 4);
    
    uint32_t status = ehci_read32(ehci, port_reg);
    
    if (!(status & EHCI_PORT_CCS)) return;
    
    if (status & EHCI_PORT_PED) {
        status &= ~EHCI_PORT_PED;
        ehci_write32(ehci, port_reg, status);
    }
    
    status = ehci_read32(ehci, port_reg);
    status |= EHCI_PORT_PR;
    status &= ~EHCI_PORT_PED;
    ehci_write32(ehci, port_reg, status);
    
    // Don't wait here - return and let timer handle the rest
}

// Cancel transfer
int ehci_cancel_transfer(usb_device_t *dev, usb_transfer_t *transfer) {
    if (!dev || !dev->controller || !transfer) return -1;
    
    ehci_transfer_info_t **curr = &active_transfers;
    
    while (*curr) {
        if ((*curr)->transfer == transfer) {
            ehci_transfer_info_t *info = *curr;
            *curr = info->next;
            
            info->qh->token &= ~EHCI_QTD_TOKEN_STATUS_ACTIVE;
            
            transfer->status = USB_TRANSFER_STATUS_ERROR;
            kfree(info);
            return 0;
        }
        
        curr = &(*curr)->next;
    }
    
    return -1;
}

// Forward declare shutdown
void ehci_shutdown(usb_controller_t *controller);

// Controller operations
static usb_controller_ops_t ehci_ops = {
    .init = NULL,
    .shutdown = ehci_shutdown,
    .reset_port = ehci_reset_port,
    .control_transfer = ehci_control_transfer,
    .interrupt_transfer = NULL,
    .bulk_transfer = NULL,
    .submit_interrupt_transfer = ehci_submit_interrupt_transfer,
    .cancel_transfer = ehci_cancel_transfer,
};

// Shutdown
void ehci_shutdown(usb_controller_t *controller) {
    ehci_controller_t *ehci = (ehci_controller_t*)controller->controller_data;
    
    if (ehci->pci_dev->interrupt_line != 0xFF && ehci->pci_dev->interrupt_line < 16) {
        irq_uninstall_handler(ehci->pci_dev->interrupt_line);
    }
    
    ehci_write32(ehci, EHCI_OP_USBINTR, 0);
    
    uint32_t cmd = ehci_read32(ehci, EHCI_OP_USBCMD);
    cmd &= ~EHCI_CMD_RS;
    ehci_write32(ehci, EHCI_OP_USBCMD, cmd);
    
    int timeout = 1000;
    while (!(ehci_read32(ehci, EHCI_OP_USBSTS) & EHCI_STS_HCHALTED) && timeout--) {
        for (volatile int i = 0; i < 1000; i++);
    }
    
    if (ehci->qtd_pool) kfree(ehci->qtd_pool);
    if (ehci->interrupt_qh) kfree(ehci->interrupt_qh);
    if (ehci->bulk_qh) kfree(ehci->bulk_qh);
    if (ehci->control_qh) kfree(ehci->control_qh);
    if (ehci->async_qh) kfree(ehci->async_qh);
    if (ehci->periodic_list) kfree(ehci->periodic_list);
    
    if (global_ehci == ehci) global_ehci = NULL;
    
    kfree(ehci);
}