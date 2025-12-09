#include "moduos/drivers/USB/Controllers/uhci.h"
#include "moduos/drivers/USB/usb.h"
#include "moduos/drivers/PCI/pci.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/io/io.h"
#include "moduos/kernel/macros.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/interrupts/irq.h"
#include "moduos/arch/AMD64/interrupts/pic.h"

static uhci_controller_t *global_uhci = NULL;

// Transfer tracking
typedef struct uhci_transfer_info {
    usb_transfer_t *transfer;
    usb_device_t *device;
    uhci_td_t *first_td;
    uhci_td_t *last_td;
    int td_count;
    uint8_t is_control;
    struct uhci_transfer_info *next;
} uhci_transfer_info_t;

static uhci_transfer_info_t *active_transfers = NULL;

char* int_to_cstr(int value) {
    int temp = value;
    int len = 0;

    if (value == 0) len = 1;
    else {
        if (value < 0) { len++; temp = -value; }
        while (temp > 0) { temp /= 10; len++; }
    }

    char* buf = kmalloc(len + 1); // +1 for null terminator
    if (!buf) return NULL;

    buf[len] = '\0';
    int i = len - 1;
    temp = (value < 0) ? -value : value;

    if (temp == 0) buf[0] = '0';

    while (temp > 0) {
        buf[i--] = '0' + (temp % 10);
        temp /= 10;
    }

    if (value < 0) buf[0] = '-';

    return buf;
}


// Track transfer
static void uhci_track_transfer(uhci_transfer_info_t *info) {
    info->next = active_transfers;
    active_transfers = info;
}

// Find completed transfer
static uhci_transfer_info_t* uhci_find_completed_transfer(uhci_td_t *td) {
    uhci_transfer_info_t **curr = &active_transfers;
    
    while (*curr) {
        uhci_transfer_info_t *info = *curr;
        uhci_td_t *check_td = info->first_td;
        
        for (int i = 0; i < info->td_count; i++) {
            if (check_td == td) {
                *curr = info->next;
                return info;
            }
            if (check_td->link_ptr & UHCI_TD_LINK_TERMINATE) break;
            check_td = (uhci_td_t*)(check_td->link_ptr & 0xFFFFFFF0);
        }
        
        curr = &(*curr)->next;
    }
    
    return NULL;
}

// Process completed transfers (called from IRQ)
static void uhci_process_completed_transfers(uhci_controller_t *uhci) {
    uhci_qh_t *queues[] = {uhci->interrupt_qh, uhci->control_qh, uhci->bulk_qh};
    
    for (int q = 0; q < 3; q++) {
        uhci_qh_t *qh = queues[q];
        if (!qh) continue;
        
        uint32_t *link_ptr = &qh->element_link_ptr;
        
        while (*link_ptr != UHCI_QH_LINK_TERMINATE) {
            uhci_td_t *td = (uhci_td_t*)(*link_ptr & 0xFFFFFFF0);
            
            if (!(td->status & UHCI_TD_STATUS_ACTIVE)) {
                uhci_transfer_info_t *info = uhci_find_completed_transfer(td);
                
                if (info && info->transfer && info->device) {
                    usb_transfer_t *transfer = info->transfer;
                    
                    // Check for errors
                    if (td->status & (UHCI_TD_STATUS_STALLED | UHCI_TD_STATUS_BABBLE |
                                     UHCI_TD_STATUS_CRC | UHCI_TD_STATUS_BITSTUFF)) {
                        transfer->status = USB_TRANSFER_STATUS_ERROR;
                        transfer->actual_length = 0;
                    } else {
                        uint32_t actlen = ((td->status & UHCI_TD_STATUS_ACTLEN_MASK) + 1) & 0x7FF;
                        if (actlen == 0x7FF) actlen = 0;
                        transfer->actual_length = actlen;
                        transfer->status = USB_TRANSFER_STATUS_COMPLETED;
                    }
                    
                    *link_ptr = info->last_td->link_ptr;
                    
                    // Free TDs
                    uhci_td_t *free_td = info->first_td;
                    for (int i = 0; i < info->td_count; i++) {
                        uhci_td_t *next = NULL;
                        if (!(free_td->link_ptr & UHCI_TD_LINK_TERMINATE)) {
                            next = (uhci_td_t*)(free_td->link_ptr & 0xFFFFFFF0);
                        }
                        kfree(free_td);
                        if (!next) break;
                        free_td = next;
                    }
                    
                    // Callback
                    if (transfer->callback) {
                        transfer->callback(info->device, transfer);
                    }
                    
                    kfree(info);
                    continue;
                }
            }
            
            link_ptr = &td->link_ptr;
        }
    }
}

// IRQ handler
static void uhci_irq_handler(void) {
    if (!global_uhci) {
        pic_send_eoi(11);  // Default IRQ
        return;
    }
    
    uhci_controller_t *uhci = global_uhci;
    uint16_t status = uhci_read16(uhci, UHCI_REG_USBSTS);
    
    if (status == 0) {
        pic_send_eoi(uhci->pci_dev->interrupt_line);
        return;
    }
    
    if (status & UHCI_STS_USBINT) {
        uhci_process_completed_transfers(uhci);
    }
    
    if (status & UHCI_STS_ERROR) {
        COM_LOG_ERROR(COM1_PORT, "UHCI: USB error interrupt");
    }
    
    // Clear status
    uhci_write16(uhci, UHCI_REG_USBSTS, status);
    pic_send_eoi(uhci->pci_dev->interrupt_line);
}

// Helper functions
uint16_t uhci_read16(uhci_controller_t *uhci, uint16_t reg) {
    return inw(uhci->iobase + reg);
}

uint32_t uhci_read32(uhci_controller_t *uhci, uint16_t reg) {
    return inl(uhci->iobase + reg);
}

void uhci_write16(uhci_controller_t *uhci, uint16_t reg, uint16_t value) {
    outw(uhci->iobase + reg, value);
}

void uhci_write32(uhci_controller_t *uhci, uint16_t reg, uint32_t value) {
    outl(uhci->iobase + reg, value);
}

// Allocate TD
static uhci_td_t* uhci_alloc_td(uhci_controller_t *uhci) {
    uhci_td_t *td = (uhci_td_t*)kmalloc(sizeof(uhci_td_t));
    if (!td) return NULL;
    
    memset(td, 0, sizeof(uhci_td_t));
    td->link_ptr = UHCI_TD_LINK_TERMINATE;
    td->status = UHCI_TD_STATUS_ACTIVE;
    
    return td;
}

// Allocate QH
static uhci_qh_t* uhci_alloc_qh(void) {
    uhci_qh_t *qh = (uhci_qh_t*)kmalloc(sizeof(uhci_qh_t));
    if (!qh) return NULL;
    
    memset(qh, 0, sizeof(uhci_qh_t));
    qh->head_link_ptr = UHCI_QH_LINK_TERMINATE;
    qh->element_link_ptr = UHCI_QH_LINK_TERMINATE;
    
    return qh;
}

// Reset controller
static int uhci_reset(uhci_controller_t *uhci) {
    COM_LOG_INFO(COM1_PORT, "UHCI: Resetting controller");
    
    uhci_write16(uhci, UHCI_REG_USBCMD, 0);
    
    int timeout = 1000;
    while (timeout--) {
        if (uhci_read16(uhci, UHCI_REG_USBSTS) & UHCI_STS_HCH) break;
        for (volatile int i = 0; i < 1000; i++);
    }
    
    if (timeout <= 0) {
        COM_LOG_ERROR(COM1_PORT, "UHCI: Failed to halt");
        return -1;
    }
    
    uhci_write16(uhci, UHCI_REG_USBCMD, UHCI_CMD_HCRESET);
    
    timeout = 1000;
    while (timeout--) {
        if (!(uhci_read16(uhci, UHCI_REG_USBCMD) & UHCI_CMD_HCRESET)) break;
        for (volatile int i = 0; i < 1000; i++);
    }
    
    if (timeout <= 0) {
        COM_LOG_ERROR(COM1_PORT, "UHCI: Reset timeout");
        return -1;
    }
    
    uhci_write16(uhci, UHCI_REG_USBSTS, 0xFFFF);
    
    COM_LOG_OK(COM1_PORT, "UHCI: Reset complete");
    return 0;
}

// Initialize frame list
static int uhci_init_framelist(uhci_controller_t *uhci) {
    uhci->frame_list = (uint32_t*)kmalloc(UHCI_FRAMELIST_COUNT * sizeof(uint32_t));
    if (!uhci->frame_list) {
        COM_LOG_ERROR(COM1_PORT, "UHCI: Failed to allocate frame list");
        return -1;
    }
    
    memset(uhci->frame_list, 0, UHCI_FRAMELIST_COUNT * sizeof(uint32_t));
    uhci->frame_list_phys = (uint32_t)(uintptr_t)uhci->frame_list;
    
    for (int i = 0; i < UHCI_FRAMELIST_COUNT; i++) {
        uhci->frame_list[i] = UHCI_QH_LINK_TERMINATE;
    }
    
    uhci_write32(uhci, UHCI_REG_FRBASEADD, uhci->frame_list_phys);
    uhci_write16(uhci, UHCI_REG_FRNUM, 0);
    
    COM_LOG_OK(COM1_PORT, "UHCI: Frame list initialized");
    return 0;
}

// Initialize queues
static int uhci_init_queues(uhci_controller_t *uhci) {
    uhci->control_qh = uhci_alloc_qh();
    uhci->bulk_qh = uhci_alloc_qh();
    uhci->interrupt_qh = uhci_alloc_qh();
    
    if (!uhci->control_qh || !uhci->bulk_qh || !uhci->interrupt_qh) {
        COM_LOG_ERROR(COM1_PORT, "UHCI: Failed to allocate queues");
        
        return -1;
    }
    
    uint32_t control_qh_phys = (uint32_t)(uintptr_t)uhci->control_qh;
    uint32_t bulk_qh_phys = (uint32_t)(uintptr_t)uhci->bulk_qh;
    
    uhci->interrupt_qh->head_link_ptr = control_qh_phys | UHCI_QH_LINK_QH;
    uhci->control_qh->head_link_ptr = bulk_qh_phys | UHCI_QH_LINK_QH;
    uhci->bulk_qh->head_link_ptr = UHCI_QH_LINK_TERMINATE;
    
    uint32_t interrupt_qh_phys = (uint32_t)(uintptr_t)uhci->interrupt_qh;
    for (int i = 0; i < UHCI_FRAMELIST_COUNT; i++) {
        uhci->frame_list[i] = interrupt_qh_phys | UHCI_QH_LINK_QH;
    }
    
    COM_LOG_OK(COM1_PORT, "UHCI: Queues initialized");
    return 0;
}

// Start controller
static int uhci_start(uhci_controller_t *uhci) {
    COM_LOG_INFO(COM1_PORT, "UHCI: Starting controller");
    
    if (uhci->pci_dev->interrupt_line != 0xFF && uhci->pci_dev->interrupt_line < 16) {
        global_uhci = uhci;
        irq_install_handler(uhci->pci_dev->interrupt_line, uhci_irq_handler);
        COM_LOG_OK(COM1_PORT, "UHCI: IRQ ");
        char* uhci_PCIDEV_InterruptLINE = int_to_cstr(uhci->pci_dev->interrupt_line);
        if (uhci_PCIDEV_InterruptLINE) {
            com_write_string(COM1_PORT, uhci_PCIDEV_InterruptLINE);
            com_write_string(COM1_PORT, " Installed");
            kfree(uhci_PCIDEV_InterruptLINE);            // free allocated memory
        }

    } else {
        COM_LOG_ERROR(COM1_PORT, "UHCI: Invalid IRQ line");
        return -1;
    }
    
    uhci_write16(uhci, UHCI_REG_USBINTR, 
                 UHCI_INTR_IOC | UHCI_INTR_TIMEOUT | UHCI_INTR_RESUME | UHCI_INTR_SP);
    
    uint16_t cmd = UHCI_CMD_RS | UHCI_CMD_CF | UHCI_CMD_MAXP;
    uhci_write16(uhci, UHCI_REG_USBCMD, cmd);
    
    for (volatile int i = 0; i < 100000; i++);
    
    uint16_t status = uhci_read16(uhci, UHCI_REG_USBSTS);
    if (status & UHCI_STS_HCH) {
        COM_LOG_ERROR(COM1_PORT, "UHCI: Failed to start");
        return -1;
    }
    
    COM_LOG_OK(COM1_PORT, "UHCI: Controller started");
    return 0;
}


// converts int to dynamically allocated string

// Reset port
void uhci_reset_port(usb_controller_t *controller, uint8_t port) {
    if (port >= controller->num_ports) return;
    
    uhci_controller_t *uhci = (uhci_controller_t*)controller->controller_data;
    uint16_t port_reg = (port == 0) ? UHCI_REG_PORTSC1 : UHCI_REG_PORTSC2;
    
    uint16_t port_status = uhci_read16(uhci, port_reg);
    if (!(port_status & UHCI_PORT_CCS)) return;
    
    COM_LOG_INFO(COM1_PORT, "UHCI: Device on port");
    char* port_str = int_to_cstr(port);
    if (port_str) {
        com_write_string(COM1_PORT, port_str); // prints port number
        kfree(port_str);            // free allocated memory
    }
    
    uhci_write16(uhci, port_reg, UHCI_PORT_PR);
    for (volatile int i = 0; i < 500000; i++);
    
    uhci_write16(uhci, port_reg, 0);
    for (volatile int i = 0; i < 100000; i++);
    
    port_status = uhci_read16(uhci, port_reg);
    uhci_write16(uhci, port_reg, port_status | UHCI_PORT_PED);
    
    port_status = uhci_read16(uhci, port_reg);
    uint8_t speed = (port_status & UHCI_PORT_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
    
    usb_device_t *dev = usb_alloc_device(controller);
    if (dev) {
        dev->port = port;
        dev->speed = speed;
        
        if (usb_enumerate_device(dev) != 0) {
            COM_LOG_ERROR(COM1_PORT, "UHCI: Enumeration failed");
            usb_free_device(dev);
        }
    }
}

// Submit interrupt transfer
int uhci_submit_interrupt_transfer(usb_device_t *dev, usb_transfer_t *transfer) {
    if (!dev || !dev->controller || !transfer) return -1;
    
    uhci_controller_t *uhci = (uhci_controller_t*)dev->controller->controller_data;
    
    uhci_td_t *int_td = uhci_alloc_td(uhci);
    if (!int_td) {
        transfer->status = USB_TRANSFER_STATUS_ERROR;
        return -1;
    }
    
    uint8_t pid = (transfer->endpoint & 0x80) ? UHCI_TD_TOKEN_PID_IN : UHCI_TD_TOKEN_PID_OUT;
    uint8_t ep_num = transfer->endpoint & 0x0F;
    
    uint32_t data_phys = (uint32_t)(uintptr_t)transfer->buffer;
    int_td->buffer_ptr = data_phys;
    int_td->token = (transfer->length - 1) | (dev->address << 8) | 
                   (ep_num << 15) | (1 << 19) | (pid << 24);
    int_td->status = UHCI_TD_STATUS_ACTIVE | UHCI_TD_STATUS_IOC | (3 << 27);
    
    if (dev->speed == USB_SPEED_LOW) {
        int_td->status |= UHCI_TD_STATUS_LS;
    }
    int_td->link_ptr = UHCI_TD_LINK_TERMINATE;
    
    uhci_transfer_info_t *info = (uhci_transfer_info_t*)kmalloc(sizeof(uhci_transfer_info_t));
    if (!info) {
        kfree(int_td);
        return -1;
    }
    
    info->transfer = transfer;
    info->device = dev;
    info->first_td = int_td;
    info->last_td = int_td;
    info->td_count = 1;
    info->is_control = 0;
    
    uhci_track_transfer(info);
    
    uint32_t int_td_phys = (uint32_t)(uintptr_t)int_td;
    uhci_qh_t *qh = uhci->interrupt_qh;
    
    if (qh->element_link_ptr == UHCI_QH_LINK_TERMINATE) {
        qh->element_link_ptr = int_td_phys;
    } else {
        uhci_td_t *last_td = (uhci_td_t*)(qh->element_link_ptr & 0xFFFFFFF0);
        while (last_td->link_ptr != UHCI_TD_LINK_TERMINATE) {
            last_td = (uhci_td_t*)(last_td->link_ptr & 0xFFFFFFF0);
        }
        last_td->link_ptr = int_td_phys;
    }
    
    transfer->status = USB_TRANSFER_STATUS_PENDING;
    return 0;
}

// Cancel transfer
int uhci_cancel_transfer(usb_device_t *dev, usb_transfer_t *transfer) {
    if (!dev || !dev->controller || !transfer) return -1;
    
    uhci_transfer_info_t **curr = &active_transfers;
    
    while (*curr) {
        if ((*curr)->transfer == transfer) {
            uhci_transfer_info_t *info = *curr;
            *curr = info->next;
            
            uhci_td_t *td = info->first_td;
            for (int i = 0; i < info->td_count; i++) {
                td->status &= ~UHCI_TD_STATUS_ACTIVE;
                if (td->link_ptr & UHCI_TD_LINK_TERMINATE) break;
                td = (uhci_td_t*)(td->link_ptr & 0xFFFFFFF0);
            }
            
            transfer->status = USB_TRANSFER_STATUS_ERROR;
            kfree(info);
            return 0;
        }
        
        curr = &(*curr)->next;
    }
    
    return -1;
}

// Controller operations
static usb_controller_ops_t uhci_ops = {
    .init = NULL,
    .shutdown = uhci_shutdown,
    .reset_port = uhci_reset_port,
    .control_transfer = NULL,
    .interrupt_transfer = NULL,
    .bulk_transfer = NULL,
    .submit_interrupt_transfer = uhci_submit_interrupt_transfer,
    .cancel_transfer = uhci_cancel_transfer,
};

// Initialize UHCI
static int uhci_controller_init(usb_controller_t *controller) {
    uhci_controller_t *uhci = (uhci_controller_t*)controller->controller_data;
    
    if (uhci_reset(uhci) != 0) return -1;
    if (uhci_init_framelist(uhci) != 0) return -1;
    if (uhci_init_queues(uhci) != 0) return -1;
    if (uhci_start(uhci) != 0) return -1;
    
    return 0;
}

// Shutdown
void uhci_shutdown(usb_controller_t *controller) {
    if (!controller) return;
    
    uhci_controller_t *uhci = (uhci_controller_t*)controller->controller_data;
    
    if (uhci->pci_dev->interrupt_line != 0xFF && uhci->pci_dev->interrupt_line < 16) {
        irq_uninstall_handler(uhci->pci_dev->interrupt_line);
    }
    
    uhci_write16(uhci, UHCI_REG_USBINTR, 0);
    uhci_write16(uhci, UHCI_REG_USBCMD, 0);
    
    if (uhci->frame_list) kfree(uhci->frame_list);
    if (uhci->control_qh) kfree(uhci->control_qh);
    if (uhci->bulk_qh) kfree(uhci->bulk_qh);
    if (uhci->interrupt_qh) kfree(uhci->interrupt_qh);
    
    if (global_uhci == uhci) global_uhci = NULL;
    
    kfree(uhci);
    kfree(controller);
}

// Probe UHCI
int uhci_probe(pci_device_t *pci_dev) {
    COM_LOG_INFO(COM1_PORT, "UHCI: Probing controller");
    
    pci_enable_bus_mastering(pci_dev);
    pci_enable_io_space(pci_dev);
    
    uint32_t bar4 = pci_read_bar(pci_dev, 4);
    if (!(bar4 & PCI_BAR_IO)) {
        COM_LOG_ERROR(COM1_PORT, "UHCI: BAR4 not I/O");
        return -1;
    }
    
    uint16_t iobase = bar4 & 0xFFFE;
    
    uhci_controller_t *uhci = (uhci_controller_t*)kmalloc(sizeof(uhci_controller_t));
    if (!uhci) {
        COM_LOG_ERROR(COM1_PORT, "UHCI: Allocation failed");
        return -1;
    }
    
    memset(uhci, 0, sizeof(uhci_controller_t));
    uhci->pci_dev = pci_dev;
    uhci->iobase = iobase;
    uhci->next_address = 1;
    
    usb_controller_t *controller = (usb_controller_t*)kmalloc(sizeof(usb_controller_t));
    if (!controller) {
        kfree(uhci);
        COM_LOG_ERROR(COM1_PORT, "UHCI: Controller allocation failed");
        return -1;
    }
    
    memset(controller, 0, sizeof(usb_controller_t));
    controller->name = "UHCI";
    controller->num_ports = 2;
    controller->ops = &uhci_ops;
    controller->controller_data = uhci;
    
    uhci_ops.init = uhci_controller_init;
    
    if (usb_register_controller(controller) != 0) {
        kfree(controller);
        kfree(uhci);
        return -1;
    }
    
    COM_LOG_OK(COM1_PORT, "UHCI: Initialized");
    return 0;
}