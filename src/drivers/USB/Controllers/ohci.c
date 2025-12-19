#include "moduos/drivers/USB/Controllers/ohci.h"
#include "moduos/drivers/USB/usb.h"
#include "moduos/drivers/PCI/pci.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/paging.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/io/io.h"
#include "moduos/kernel/interrupts/irq.h"
#include "moduos/arch/AMD64/interrupts/pic.h"
#include "moduos/kernel/macros.h"
#include <stddef.h>

static ohci_controller_t *global_ohci = NULL;

// Transfer tracking
typedef struct ohci_transfer_info {
    usb_transfer_t *transfer;
    usb_device_t *device;
    ohci_td_t *first_td;
    ohci_ed_t *ed;
    struct ohci_transfer_info *next;
} ohci_transfer_info_t;

static ohci_transfer_info_t *active_transfers = NULL;

// Forward declarations
static void ohci_free_td(ohci_td_t *td);
static ohci_td_t* ohci_alloc_td(ohci_controller_t *ohci);
static usb_controller_ops_t ohci_ops;

// Track transfer
static void ohci_track_transfer(ohci_transfer_info_t *info) {
    info->next = active_transfers;
    active_transfers = info;
}

// Process completed transfers from done queue
static void ohci_process_done_queue(ohci_controller_t *ohci) {
    uint32_t done_head = ohci->hcca->done_head;
    if (done_head == 0) return;
    
    ohci->hcca->done_head = 0;
    
    while (done_head != 0) {
        ohci_td_t *td = (ohci_td_t*)(done_head & ~0xF);
        uint32_t next_td = td->next_td;
        
        // Find transfer
        ohci_transfer_info_t **curr = &active_transfers;
        while (*curr) {
            if ((*curr)->first_td == td) {
                ohci_transfer_info_t *info = *curr;
                *curr = info->next;
                
                usb_transfer_t *transfer = info->transfer;
                
                // Check completion code
                uint32_t cc = (td->control >> OHCI_TD_CC_SHIFT) & 0xF;
                if (cc == OHCI_TD_CC_NOERROR) {
                    transfer->status = USB_TRANSFER_STATUS_COMPLETED;
                    transfer->actual_length = transfer->length;
                } else {
                    transfer->status = USB_TRANSFER_STATUS_ERROR;
                    transfer->actual_length = 0;
                }
                
                // Callback
                if (transfer->callback) {
                    transfer->callback(info->device, transfer);
                }
                
                ohci_free_td(td);
                kfree(info);
                break;
            }
            curr = &(*curr)->next;
        }
        
        done_head = next_td;
    }
}

// IRQ handler
static void ohci_irq_handler(void) {
    if (!global_ohci) {
        pic_send_eoi(11);
        return;
    }
    
    ohci_controller_t *ohci = global_ohci;
    uint32_t status = ohci_read32(ohci, OHCI_REG_INTERRUPT_STATUS);
    
    if (status) {
        if (status & OHCI_INT_WDH) {
            ohci_process_done_queue(ohci);
        }
        
        if (status & OHCI_INT_SO) {
            COM_LOG_WARN(COM1_PORT, "OHCI: Scheduling overrun");
        }
        
        if (status & OHCI_INT_RHSC) {
            COM_LOG_INFO(COM1_PORT, "OHCI: Root hub change");
        }
        
        if (status & OHCI_INT_UE) {
            COM_LOG_ERROR(COM1_PORT, "OHCI: Unrecoverable error");
        }
        
        ohci_write32(ohci, OHCI_REG_INTERRUPT_STATUS, status);
    }
    
    pic_send_eoi(ohci->pci_dev->interrupt_line);
}

// Helper functions
uint32_t ohci_read32(ohci_controller_t *ohci, uint32_t reg) {
    return ohci->mmio_base[reg / 4];
}

void ohci_write32(ohci_controller_t *ohci, uint32_t reg, uint32_t value) {
    ohci->mmio_base[reg / 4] = value;
}

// Allocate TD
static ohci_td_t* ohci_alloc_td(ohci_controller_t *ohci) {
    for (int i = 0; i < ohci->td_pool_count; i++) {
        if (ohci->td_pool[i].control == 0) {
            return &ohci->td_pool[i];
        }
    }
    return NULL;
}

// Free TD (implementation)
static void ohci_free_td(ohci_td_t *td) {
    td->control = 0;
    td->current_buffer_ptr = 0;
    td->next_td = 0;
    td->buffer_end = 0;
}

// Create ED
static ohci_ed_t* ohci_create_ed(ohci_controller_t *ohci, uint8_t addr, uint8_t endpoint, 
                                  uint8_t speed, uint16_t max_packet, uint8_t direction) {
    ohci_ed_t *ed = (ohci_ed_t*)kmalloc(sizeof(ohci_ed_t));
    if (!ed) return NULL;
    
    uint32_t control = (addr & OHCI_ED_FA_MASK) |
                       ((endpoint << OHCI_ED_EN_SHIFT) & OHCI_ED_EN_MASK) |
                       ((max_packet << OHCI_ED_MPS_SHIFT) & OHCI_ED_MPS_MASK);
    
    if (speed == USB_SPEED_LOW) control |= OHCI_ED_S;
    
    if (direction == USB_PID_IN) {
        control |= OHCI_ED_D_IN;
    } else if (direction == USB_PID_OUT) {
        control |= OHCI_ED_D_OUT;
    } else {
        control |= OHCI_ED_D_TD;
    }
    
    ed->control = control;
    ed->tail_ptr = 0;
    ed->head_ptr = 0;
    ed->next_ed = 0;
    
    return ed;
}

// Create TD
static int ohci_create_td(ohci_controller_t *ohci, ohci_td_t *td, uint8_t direction,
                          void *buffer, uint16_t length, int toggle) {
    uint32_t control = 0;
    
    if (direction == USB_PID_SETUP) {
        control |= OHCI_TD_DP_SETUP;
    } else if (direction == USB_PID_IN) {
        control |= OHCI_TD_DP_IN;
    } else if (direction == USB_PID_OUT) {
        control |= OHCI_TD_DP_OUT;
    }
    
    control |= ((toggle & 3) << OHCI_TD_T_SHIFT);
    
    if (direction == USB_PID_IN) {
        control |= OHCI_TD_R;
    }
    
    control |= (0 << OHCI_TD_DI_SHIFT);  // Interrupt immediately
    
    td->control = control;
    td->current_buffer_ptr = (uint32_t)buffer;
    if (length > 0) {
        td->buffer_end = (uint32_t)buffer + length - 1;
    } else {
        td->buffer_end = 0;
    }
    td->next_td = 0;
    
    return 0;
}

// Reset controller
static int ohci_reset(ohci_controller_t *ohci) {
    COM_LOG_INFO(COM1_PORT, "OHCI: Resetting");
    
    uint32_t control = ohci_read32(ohci, OHCI_REG_CONTROL);
    
    if (control & OHCI_CTRL_IR) {
        COM_LOG_INFO(COM1_PORT, "OHCI: Taking ownership from BIOS");
        ohci_write32(ohci, OHCI_REG_COMMAND_STATUS, OHCI_CMD_OCR);
        
        int timeout = 1000;
        while ((ohci_read32(ohci, OHCI_REG_CONTROL) & OHCI_CTRL_IR) && timeout--) {
            for (volatile int i = 0; i < 10000; i++);
        }
        
        if (timeout <= 0) {
            COM_LOG_ERROR(COM1_PORT, "OHCI: Failed to take ownership");
            return -1;
        }
    }
    
    ohci_write32(ohci, OHCI_REG_COMMAND_STATUS, OHCI_CMD_HCR);
    
    int timeout = 100;
    while ((ohci_read32(ohci, OHCI_REG_COMMAND_STATUS) & OHCI_CMD_HCR) && timeout--) {
        for (volatile int i = 0; i < 1000; i++);
    }
    
    if (timeout <= 0) {
        COM_LOG_ERROR(COM1_PORT, "OHCI: Reset timeout");
        return -1;
    }
    
    COM_LOG_OK(COM1_PORT, "OHCI: Reset complete");
    return 0;
}

// Probe OHCI
int ohci_probe(pci_device_t *pci_dev) {
    COM_LOG_INFO(COM1_PORT, "OHCI: Probing");
    
    ohci_controller_t *ohci = (ohci_controller_t*)kmalloc(sizeof(ohci_controller_t));
    if (!ohci) {
        COM_LOG_ERROR(COM1_PORT, "OHCI: Allocation failed");
        return -1;
    }
    
    memset(ohci, 0, sizeof(ohci_controller_t));
    ohci->pci_dev = pci_dev;
    ohci->next_address = 1;
    
    uint32_t bar0 = pci_read_config(pci_dev->bus, pci_dev->device, pci_dev->function, 0x10);
    ohci->mmio_phys = bar0 & 0xFFFFFFF0;
    
    // Map MMIO region to virtual memory using ioremap
    COM_LOG_INFO(COM1_PORT, "OHCI: Mapping MMIO at physical 0x%08x", ohci->mmio_phys);
    ohci->mmio_base = (volatile uint32_t*)ioremap(ohci->mmio_phys, 4096);
    
    if (!ohci->mmio_base) {
        COM_LOG_ERROR(COM1_PORT, "OHCI: Failed to map MMIO region");
        kfree(ohci);
        return -1;
    }
    
    COM_LOG_OK(COM1_PORT, "OHCI: MMIO mapped to virtual 0x%p", ohci->mmio_base);
    
    uint16_t command = pci_read_config(pci_dev->bus, pci_dev->device, pci_dev->function, 0x04) & 0xFFFF;
    command |= 0x06;
    pci_write_config(pci_dev->bus, pci_dev->device, pci_dev->function, 0x04, command);
    
    if (ohci_reset(ohci) < 0) {
        kfree(ohci);
        return -1;
    }
    
    ohci->hcca = (ohci_hcca_t*)kmalloc_aligned(sizeof(ohci_hcca_t), 256);
    if (!ohci->hcca) {
        COM_LOG_ERROR(COM1_PORT, "OHCI: Failed to allocate HCCA");
        kfree(ohci);
        return -1;
    }
    
    memset(ohci->hcca, 0, sizeof(ohci_hcca_t));
    ohci->hcca_phys = (uint32_t)ohci->hcca;
    
    ohci->td_pool_count = 64;
    ohci->td_pool = (ohci_td_t*)kmalloc(sizeof(ohci_td_t) * ohci->td_pool_count);
    if (!ohci->td_pool) {
        COM_LOG_ERROR(COM1_PORT, "OHCI: Failed to allocate TD pool");
        kfree(ohci->hcca);
        kfree(ohci);
        return -1;
    }
    
    for (int i = 0; i < ohci->td_pool_count; i++) {
        ohci_free_td(&ohci->td_pool[i]);
    }
    
    ohci->control_head = ohci_create_ed(ohci, 0, 0, USB_SPEED_FULL, 64, 0);
    ohci->bulk_head = ohci_create_ed(ohci, 0, 0, USB_SPEED_FULL, 64, 0);
    
    if (!ohci->control_head || !ohci->bulk_head) {
        COM_LOG_ERROR(COM1_PORT, "OHCI: Failed to create queue heads");
        kfree(ohci->td_pool);
        kfree(ohci->hcca);
        kfree(ohci);
        return -1;
    }
    
    ohci->control_head->control |= OHCI_ED_K;
    ohci->bulk_head->control |= OHCI_ED_K;
    
    ohci_write32(ohci, OHCI_REG_HCCA, ohci->hcca_phys);
    ohci_write32(ohci, OHCI_REG_CONTROL_HEAD_ED, (uint32_t)ohci->control_head);
    ohci_write32(ohci, OHCI_REG_BULK_HEAD_ED, (uint32_t)ohci->bulk_head);
    ohci_write32(ohci, OHCI_REG_FM_INTERVAL, 0x2EDF | (1 << 31));
    ohci_write32(ohci, OHCI_REG_PERIODIC_START, 0x2A2F);
    
    if (ohci->pci_dev->interrupt_line != 0xFF && ohci->pci_dev->interrupt_line < 16) {
        global_ohci = ohci;
        irq_install_handler(ohci->pci_dev->interrupt_line, ohci_irq_handler);
        COM_LOG_OK(COM1_PORT, "OHCI: IRQ %d installed", ohci->pci_dev->interrupt_line);
    } else {
        COM_LOG_ERROR(COM1_PORT, "OHCI: Invalid IRQ");
        kfree(ohci->bulk_head);
        kfree(ohci->control_head);
        kfree(ohci->td_pool);
        kfree(ohci->hcca);
        kfree(ohci);
        return -1;
    }
    
    ohci_write32(ohci, OHCI_REG_INTERRUPT_ENABLE, 
                 OHCI_INT_MIE | OHCI_INT_WDH | OHCI_INT_RHSC | 
                 OHCI_INT_UE | OHCI_INT_RD | OHCI_INT_SO);
    
    uint32_t control = OHCI_CTRL_HCFS_OPERATIONAL | 
                       OHCI_CTRL_CLE | OHCI_CTRL_BLE | OHCI_CTRL_PLE;
    ohci_write32(ohci, OHCI_REG_CONTROL, control);
    
    uint32_t rh_desc_a = ohci_read32(ohci, OHCI_REG_RH_DESCRIPTOR_A);
    ohci->num_ports = rh_desc_a & 0xFF;
    
    ohci_write32(ohci, OHCI_REG_RH_STATUS, OHCI_RH_LPSC);
    for (volatile int i = 0; i < 100000; i++);
    
    usb_controller_t *controller = (usb_controller_t*)kmalloc(sizeof(usb_controller_t));
    if (!controller) {
        COM_LOG_ERROR(COM1_PORT, "OHCI: Controller allocation failed");
        irq_uninstall_handler(ohci->pci_dev->interrupt_line);
        kfree(ohci->bulk_head);
        kfree(ohci->control_head);
        kfree(ohci->td_pool);
        kfree(ohci->hcca);
        kfree(ohci);
        return -1;
    }
    
    memset(controller, 0, sizeof(usb_controller_t));
    controller->name = "OHCI";
    controller->num_ports = ohci->num_ports;
    controller->controller_data = ohci;
    controller->ops = &ohci_ops;
    
    if (usb_register_controller(controller) != 0) {
        kfree(controller);
        irq_uninstall_handler(ohci->pci_dev->interrupt_line);
        kfree(ohci->bulk_head);
        kfree(ohci->control_head);
        kfree(ohci->td_pool);
        kfree(ohci->hcca);
        kfree(ohci);
        return -1;
    }
    
    COM_LOG_OK(COM1_PORT, "OHCI: Initialized");
    return 0;
}

// Reset port
void ohci_reset_port(usb_controller_t *controller, uint8_t port) {
    ohci_controller_t *ohci = (ohci_controller_t*)controller->controller_data;
    uint32_t port_reg = OHCI_REG_RH_PORT_STATUS + (port * 4);
    
    COM_LOG_INFO(COM1_PORT, "OHCI: Resetting port %d", port);
    
    ohci_write32(ohci, port_reg, OHCI_PORT_PRS);
    for (volatile int i = 0; i < 100000; i++);
    
    ohci_write32(ohci, port_reg, OHCI_PORT_PRSC);
    for (volatile int i = 0; i < 100000; i++);
    
    uint32_t status = ohci_read32(ohci, port_reg);
    
    if (status & OHCI_PORT_CCS) {
        uint8_t speed = (status & OHCI_PORT_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
        
        ohci_write32(ohci, port_reg, OHCI_PORT_PES);
        
        usb_device_t *dev = usb_alloc_device(controller);
        if (dev) {
            dev->port = port;
            dev->speed = speed;
            
            if (usb_enumerate_device(dev) != 0) {
                COM_LOG_ERROR(COM1_PORT, "OHCI: Enumeration failed");
                usb_free_device(dev);
            }
        }
    }
}

// Submit interrupt transfer
int ohci_submit_interrupt_transfer(usb_device_t *dev, usb_transfer_t *transfer) {
    if (!dev || !dev->controller || !transfer) return -1;
    
    ohci_controller_t *ohci = (ohci_controller_t*)dev->controller->controller_data;
    
    ohci_td_t *int_td = ohci_alloc_td(ohci);
    if (!int_td) {
        transfer->status = USB_TRANSFER_STATUS_ERROR;
        return -1;
    }
    
    uint8_t direction = (transfer->endpoint & 0x80) ? USB_PID_IN : USB_PID_OUT;
    uint8_t ep_num = transfer->endpoint & 0x0F;
    
    ohci_create_td(ohci, int_td, direction, transfer->buffer, transfer->length, 1);
    int_td->next_td = 0;
    
    ohci_ed_t *ed = ohci_create_ed(ohci, dev->address, ep_num, dev->speed, 
                                   dev->max_packet_size, direction);
    if (!ed) {
        ohci_free_td(int_td);
        return -1;
    }
    
    ed->head_ptr = (uint32_t)int_td;
    ed->tail_ptr = 0;
    ed->control &= ~OHCI_ED_K;
    
    ohci_transfer_info_t *info = (ohci_transfer_info_t*)kmalloc(sizeof(ohci_transfer_info_t));
    if (!info) {
        kfree(ed);
        ohci_free_td(int_td);
        return -1;
    }
    
    info->transfer = transfer;
    info->device = dev;
    info->first_td = int_td;
    info->ed = ed;
    
    ohci_track_transfer(info);
    
    if (!ohci->interrupt_eds[0]) {
        ohci->interrupt_eds[0] = ed;
        for (int i = 0; i < 32; i++) {
            if (ohci->hcca->interrupt_table[i] == 0) {
                ohci->hcca->interrupt_table[i] = (uint32_t)ed;
            }
        }
    } else {
        ed->next_ed = ohci->interrupt_eds[0]->next_ed;
        ohci->interrupt_eds[0]->next_ed = (uint32_t)ed;
    }
    
    transfer->status = USB_TRANSFER_STATUS_PENDING;
    return 0;
}

// Cancel transfer
int ohci_cancel_transfer(usb_device_t *dev, usb_transfer_t *transfer) {
    if (!dev || !dev->controller || !transfer) return -1;
    
    ohci_transfer_info_t **curr = &active_transfers;
    
    while (*curr) {
        if ((*curr)->transfer == transfer) {
            ohci_transfer_info_t *info = *curr;
            *curr = info->next;
            
            info->ed->control |= OHCI_ED_K;
            
            transfer->status = USB_TRANSFER_STATUS_ERROR;
            kfree(info);
            return 0;
        }
        
        curr = &(*curr)->next;
    }
    
    return -1;
}

// Controller operations
static usb_controller_ops_t ohci_ops = {
    .init = NULL,
    .shutdown = ohci_shutdown,
    .reset_port = ohci_reset_port,
    .control_transfer = NULL,
    .interrupt_transfer = NULL,
    .bulk_transfer = NULL,
    .submit_interrupt_transfer = ohci_submit_interrupt_transfer,
    .cancel_transfer = ohci_cancel_transfer,
};

// Shutdown
void ohci_shutdown(usb_controller_t *controller) {
    ohci_controller_t *ohci = (ohci_controller_t*)controller->controller_data;
    
    if (ohci->pci_dev->interrupt_line != 0xFF && ohci->pci_dev->interrupt_line < 16) {
        irq_uninstall_handler(ohci->pci_dev->interrupt_line);
    }
    
    ohci_write32(ohci, OHCI_REG_INTERRUPT_DISABLE, OHCI_INT_MIE);
    
    uint32_t control = ohci_read32(ohci, OHCI_REG_CONTROL);
    control &= ~OHCI_CTRL_HCFS_MASK;
    control |= OHCI_CTRL_HCFS_SUSPEND;
    ohci_write32(ohci, OHCI_REG_CONTROL, control);
    
    if (ohci->td_pool) kfree(ohci->td_pool);
    if (ohci->control_head) kfree(ohci->control_head);
    if (ohci->bulk_head) kfree(ohci->bulk_head);
    if (ohci->hcca) kfree(ohci->hcca);
    
    if (global_ohci == ohci) global_ohci = NULL;
    
    kfree(ohci);
}