#include "moduos/drivers/USB/usb.h"
#include "moduos/drivers/USB/Classes/hid.h"
#include "moduos/drivers/USB/Controllers/uhci.h"
#include "moduos/drivers/USB/Controllers/ohci.h"
#include "moduos/drivers/USB/Controllers/ehci.h"
#include "moduos/drivers/PCI/pci.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/macros.h"
#include "moduos/kernel/interrupts/irq.h"

// Global USB state
static usb_controller_t *usb_controllers = NULL;
static usb_driver_t *usb_drivers = NULL;
static uint8_t next_device_address = 1;

// PCI class codes for USB controllers
#define PCI_CLASS_SERIAL_BUS    0x0C
#define PCI_SUBCLASS_USB        0x03
#define PCI_PROG_IF_UHCI        0x00
#define PCI_PROG_IF_OHCI        0x10
#define PCI_PROG_IF_EHCI        0x20

// Enumeration state machine types (MUST BE DECLARED BEFORE USE)
typedef enum {
    USB_ENUM_GET_DESC8,
    USB_ENUM_WAIT_DESC8,
    USB_ENUM_SET_ADDRESS,
    USB_ENUM_WAIT_ADDRESS,
    USB_ENUM_GET_DESC_FULL,
    USB_ENUM_WAIT_DESC_FULL,
    USB_ENUM_SET_CONFIG,
    USB_ENUM_WAIT_CONFIG,
    USB_ENUM_COMPLETE
} usb_enum_state_t;

typedef struct usb_enum_context {
    usb_device_t *dev;
    usb_enum_state_t state;
    uint8_t new_address;
    uint8_t desc_buffer[18];
    int retry_count;
    int wait_ticks;
    struct usb_enum_context *next;
} usb_enum_context_t;

static usb_enum_context_t *active_enumerations = NULL;

// Forward declarations
static void usb_scan_pci_bus(void);
static void usb_enumerate_all_ports(void);

// Initialize entire USB subsystem
void usb_init(void) {
    COM_LOG_INFO(COM1_PORT, "=== Initializing USB Subsystem ===");
    
    usb_controllers = NULL;
    usb_drivers = NULL;
    next_device_address = 1;
    active_enumerations = NULL;
    
    // Scan PCI bus and initialize all USB controllers automatically
    usb_scan_pci_bus();
    
    // Give controllers time to stabilize
    for (volatile int i = 0; i < 1000000; i++);
    
    // Enumerate devices on all ports
    usb_enumerate_all_ports();
    
    COM_LOG_OK(COM1_PORT, "USB subsystem fully initialized");
}

// Scan PCI bus for all USB controllers and initialize them
static void usb_scan_pci_bus(void) {
    COM_LOG_INFO(COM1_PORT, "Scanning PCI bus for USB controllers...");
    
    int uhci_count = 0, ohci_count = 0, ehci_count = 0;
    
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                uint16_t vendor = pci_read_config(bus, dev, func, 0) & 0xFFFF;
                if (vendor == 0xFFFF) continue;
                
                uint32_t class_info = pci_read_config(bus, dev, func, 0x08);
                uint8_t class = (class_info >> 24) & 0xFF;
                uint8_t subclass = (class_info >> 16) & 0xFF;
                uint8_t prog_if = (class_info >> 8) & 0xFF;
                
                if (class == PCI_CLASS_SERIAL_BUS && subclass == PCI_SUBCLASS_USB) {
                    uint8_t irq = pci_read_config(bus, dev, func, 0x3C) & 0xFF;
                    
                    pci_device_t *pci_dev = (pci_device_t*)kmalloc(sizeof(pci_device_t));
                    if (!pci_dev) continue;
                    
                    pci_dev->bus = bus;
                    pci_dev->device = dev;
                    pci_dev->function = func;
                    pci_dev->vendor_id = vendor;
                    pci_dev->device_id = (pci_read_config(bus, dev, func, 0) >> 16) & 0xFFFF;
                    pci_dev->interrupt_line = irq;
                    
                    if (prog_if == PCI_PROG_IF_UHCI) {
                        COM_LOG_INFO(COM1_PORT, "Found UHCI at %02x:%02x.%x IRQ=%d", 
                                    bus, dev, func, irq);
                        if (uhci_probe(pci_dev) == 0) uhci_count++;
                    } else if (prog_if == PCI_PROG_IF_OHCI) {
                        COM_LOG_INFO(COM1_PORT, "Found OHCI at %02x:%02x.%x IRQ=%d", 
                                    bus, dev, func, irq);
                        if (ohci_probe(pci_dev) == 0) ohci_count++;
                    } else if (prog_if == PCI_PROG_IF_EHCI) {
                        COM_LOG_INFO(COM1_PORT, "Found EHCI at %02x:%02x.%x IRQ=%d", 
                                    bus, dev, func, irq);
                        if (ehci_probe(pci_dev) == 0) ehci_count++;
                    } else {
                        kfree(pci_dev);
                    }
                }
            }
        }
    }
    
    COM_LOG_OK(COM1_PORT, "Found %d UHCI, %d OHCI, %d EHCI controllers", 
               uhci_count, ohci_count, ehci_count);
}

// Enumerate all ports on all controllers
static void usb_enumerate_all_ports(void) {
    COM_LOG_INFO(COM1_PORT, "Enumerating devices on all USB ports...");
    
    usb_controller_t *ctrl = usb_controllers;
    int device_count = 0;
    
    while (ctrl) {
        for (uint8_t port = 0; port < ctrl->num_ports; port++) {
            if (ctrl->ops && ctrl->ops->reset_port) {
                ctrl->ops->reset_port(ctrl, port);
                // Count devices (check if enumeration added a device)
                usb_device_t *dev = ctrl->devices;
                while (dev) {
                    if (dev->port == port && dev->address != 0) {
                        device_count++;
                        break;
                    }
                    dev = dev->next;
                }
            }
        }
        ctrl = ctrl->next;
    }
    
    COM_LOG_OK(COM1_PORT, "Found %d USB device(s)", device_count);
}

// Register a USB controller
int usb_register_controller(usb_controller_t *controller) {
    if (!controller) return -1;
    
    controller->next = usb_controllers;
    usb_controllers = controller;
    
    COM_LOG_OK(COM1_PORT, "Registered %s controller", controller->name);
    
    // Initialize the controller
    if (controller->ops && controller->ops->init) {
        if (controller->ops->init(controller) != 0) {
            COM_LOG_ERROR(COM1_PORT, "Failed to initialize %s controller", controller->name);
            return -1;
        }
    }
    
    return 0;
}

// Unregister a USB controller
void usb_unregister_controller(usb_controller_t *controller) {
    if (!controller) return;
    
    usb_controller_t **curr = &usb_controllers;
    while (*curr) {
        if (*curr == controller) {
            *curr = controller->next;
            break;
        }
        curr = &(*curr)->next;
    }
}

// Register a USB driver
int usb_register_driver(usb_driver_t *driver) {
    if (!driver) return -1;
    
    driver->next = usb_drivers;
    usb_drivers = driver;
    
    COM_LOG_OK(COM1_PORT, "Registered USB driver: %s", driver->name);
    
    // Try to match with existing devices
    usb_controller_t *ctrl = usb_controllers;
    while (ctrl) {
        usb_device_t *dev = ctrl->devices;
        while (dev) {
            usb_match_drivers(dev);
            dev = dev->next;
        }
        ctrl = ctrl->next;
    }
    
    return 0;
}

// Unregister a USB driver
void usb_unregister_driver(usb_driver_t *driver) {
    if (!driver) return;
    
    usb_driver_t **curr = &usb_drivers;
    while (*curr) {
        if (*curr == driver) {
            *curr = driver->next;
            break;
        }
        curr = &(*curr)->next;
    }
}

// Allocate a USB device
usb_device_t* usb_alloc_device(usb_controller_t *controller) {
    usb_device_t *dev = (usb_device_t*)kmalloc(sizeof(usb_device_t));
    if (!dev) return NULL;
    
    memset(dev, 0, sizeof(usb_device_t));
    dev->controller = controller;
    dev->address = 0;
    dev->max_packet_size = 8;  // Default for initial enumeration
    
    // Add to controller's device list
    dev->next = controller->devices;
    controller->devices = dev;
    
    return dev;
}

// Free a USB device
void usb_free_device(usb_device_t *dev) {
    if (!dev) return;
    
    // Remove from controller's device list
    if (dev->controller) {
        usb_device_t **curr = &dev->controller->devices;
        while (*curr) {
            if (*curr == dev) {
                *curr = dev->next;
                break;
            }
            curr = &(*curr)->next;
        }
    }
    
    if (dev->config) kfree(dev->config);
    kfree(dev);
}

// Enumerate a USB device (async via state machine)
int usb_enumerate_device(usb_device_t *dev) {
    if (!dev || !dev->controller) return -1;
    
    COM_LOG_INFO(COM1_PORT, "Starting async enumeration for device on port %d", dev->port);
    
    usb_enum_context_t *ctx = kmalloc(sizeof(usb_enum_context_t));
    if (!ctx) return -1;
    
    memset(ctx, 0, sizeof(usb_enum_context_t));
    ctx->dev = dev;
    ctx->state = USB_ENUM_GET_DESC8;
    ctx->retry_count = 1000;
    ctx->wait_ticks = 0;
    
    ctx->next = active_enumerations;
    active_enumerations = ctx;
    
    return 0;
}

// Match device with available drivers
void usb_match_drivers(usb_device_t *dev) {
    if (!dev) return;
    
    usb_driver_t *driver = usb_drivers;
    while (driver) {
        int match = 0;
        
        // Check vendor/product ID match
        if (driver->vendor_id != 0 && driver->product_id != 0) {
            if (dev->descriptor.idVendor == driver->vendor_id &&
                dev->descriptor.idProduct == driver->product_id) {
                match = 1;
            }
        }
        
        // Check device class match
        if (driver->device_class != 0) {
            if (dev->descriptor.bDeviceClass == driver->device_class) {
                if (driver->device_subclass == 0 ||
                    dev->descriptor.bDeviceSubClass == driver->device_subclass) {
                    match = 1;
                }
            }
        }
        
        if (match && driver->probe) {
            COM_LOG_INFO(COM1_PORT, "Probing driver: %s", driver->name);
            if (driver->probe(dev) == 0) {
                COM_LOG_OK(COM1_PORT, "Driver %s attached", driver->name);
                dev->driver = driver;
                return;
            }
        }
        
        driver = driver->next;
    }
}

// Control transfer
int usb_control_transfer(usb_device_t *dev, uint8_t request_type, uint8_t request,
                        uint16_t value, uint16_t index, void *data, uint16_t length) {
    if (!dev || !dev->controller || !dev->controller->ops) return -1;
    
    usb_setup_packet_t setup;
    setup.bmRequestType = request_type;
    setup.bRequest = request;
    setup.wValue = value;
    setup.wIndex = index;
    setup.wLength = length;
    
    if (dev->controller->ops->control_transfer) {
        return dev->controller->ops->control_transfer(dev, &setup, data);
    }
    
    return -1;
}

// Get descriptor
int usb_get_descriptor(usb_device_t *dev, uint8_t desc_type, uint8_t desc_index,
                      void *buffer, uint16_t length) {
    return usb_control_transfer(dev,
                               USB_DIR_IN | USB_REQ_TYPE_STANDARD | USB_REQ_RECIPIENT_DEVICE,
                               USB_REQ_GET_DESCRIPTOR,
                               (desc_type << 8) | desc_index,
                               0,
                               buffer,
                               length);
}

// Set address
int usb_set_address(usb_device_t *dev, uint8_t address) {
    return usb_control_transfer(dev,
                               USB_DIR_OUT | USB_REQ_TYPE_STANDARD | USB_REQ_RECIPIENT_DEVICE,
                               USB_REQ_SET_ADDRESS,
                               address,
                               0,
                               NULL,
                               0);
}

// Set configuration
int usb_set_configuration(usb_device_t *dev, uint8_t config) {
    return usb_control_transfer(dev,
                               USB_DIR_OUT | USB_REQ_TYPE_STANDARD | USB_REQ_RECIPIENT_DEVICE,
                               USB_REQ_SET_CONFIGURATION,
                               config,
                               0,
                               NULL,
                               0);
}

// Allocate a transfer
usb_transfer_t* usb_alloc_transfer(void) {
    usb_transfer_t *transfer = (usb_transfer_t*)kmalloc(sizeof(usb_transfer_t));
    if (!transfer) return NULL;
    
    memset(transfer, 0, sizeof(usb_transfer_t));
    transfer->status = USB_TRANSFER_STATUS_PENDING;
    
    return transfer;
}

// Free a transfer
void usb_free_transfer(usb_transfer_t *transfer) {
    if (transfer) kfree(transfer);
}

// Submit interrupt transfer (async, interrupt-driven)
int usb_submit_interrupt_transfer(usb_device_t *dev, uint8_t endpoint, void *buffer,
                                   uint16_t length, usb_transfer_callback_t callback,
                                   void *callback_data) {
    if (!dev || !dev->controller || !dev->controller->ops) return -1;
    
    usb_transfer_t *transfer = usb_alloc_transfer();
    if (!transfer) return -1;
    
    transfer->device = dev;
    transfer->endpoint = endpoint;
    transfer->buffer = buffer;
    transfer->length = length;
    transfer->callback = callback;
    transfer->callback_data = callback_data;
    transfer->status = USB_TRANSFER_STATUS_PENDING;
    
    // Add to device's active transfer list
    transfer->next = dev->active_transfers;
    dev->active_transfers = transfer;
    
    // Submit to controller (interrupt-driven)
    if (dev->controller->ops->submit_interrupt_transfer) {
        return dev->controller->ops->submit_interrupt_transfer(dev, transfer);
    }
    
    usb_free_transfer(transfer);
    return -1;
}

// Cancel a transfer
int usb_cancel_transfer(usb_device_t *dev, usb_transfer_t *transfer) {
    if (!dev || !dev->controller || !dev->controller->ops || !transfer) return -1;
    
    // Remove from active transfer list
    usb_transfer_t **curr = &dev->active_transfers;
    while (*curr) {
        if (*curr == transfer) {
            *curr = transfer->next;
            break;
        }
        curr = &(*curr)->next;
    }
    
    // Cancel in controller
    if (dev->controller->ops->cancel_transfer) {
        return dev->controller->ops->cancel_transfer(dev, transfer);
    }
    
    return -1;
}

// Enumeration state machine tick (called from timer)
void usb_enumeration_tick(void) {
    usb_enum_context_t **curr = &active_enumerations;
    
    while (*curr) {
        usb_enum_context_t *ctx = *curr;
        
        if (--ctx->retry_count <= 0) {
            COM_LOG_ERROR(COM1_PORT, "USB: Enumeration timeout");
            *curr = ctx->next;
            usb_free_device(ctx->dev);
            kfree(ctx);
            continue;
        }
        
        if (ctx->wait_ticks > 0) {
            ctx->wait_ticks--;
            curr = &(*curr)->next;
            continue;
        }
        
        switch (ctx->state) {
            case USB_ENUM_GET_DESC8:
                usb_get_descriptor(ctx->dev, USB_DESC_DEVICE, 0, ctx->desc_buffer, 8);
                ctx->state = USB_ENUM_WAIT_DESC8;
                ctx->wait_ticks = 10;
                break;
                
            case USB_ENUM_WAIT_DESC8:
                ctx->dev->max_packet_size = ctx->desc_buffer[7];
                ctx->new_address = next_device_address++;
                if (next_device_address > 127) next_device_address = 1;
                ctx->state = USB_ENUM_SET_ADDRESS;
                break;
                
            case USB_ENUM_SET_ADDRESS:
                usb_set_address(ctx->dev, ctx->new_address);
                ctx->state = USB_ENUM_WAIT_ADDRESS;
                ctx->wait_ticks = 20;
                break;
                
            case USB_ENUM_WAIT_ADDRESS:
                ctx->dev->address = ctx->new_address;
                ctx->state = USB_ENUM_GET_DESC_FULL;
                break;
                
            case USB_ENUM_GET_DESC_FULL:
                usb_get_descriptor(ctx->dev, USB_DESC_DEVICE, 0, ctx->desc_buffer, 18);
                ctx->state = USB_ENUM_WAIT_DESC_FULL;
                ctx->wait_ticks = 10;
                break;
                
            case USB_ENUM_WAIT_DESC_FULL:
                memcpy(&ctx->dev->descriptor, ctx->desc_buffer, sizeof(usb_device_descriptor_t));
                COM_LOG_OK(COM1_PORT, "Device VID=%04x PID=%04x", 
                          ctx->dev->descriptor.idVendor, ctx->dev->descriptor.idProduct);
                ctx->state = USB_ENUM_SET_CONFIG;
                break;
                
            case USB_ENUM_SET_CONFIG:
                usb_set_configuration(ctx->dev, 1);
                ctx->state = USB_ENUM_WAIT_CONFIG;
                ctx->wait_ticks = 10;
                break;
                
            case USB_ENUM_WAIT_CONFIG:
                ctx->state = USB_ENUM_COMPLETE;
                break;
                
            case USB_ENUM_COMPLETE:
                usb_match_drivers(ctx->dev);
                *curr = ctx->next;
                kfree(ctx);
                continue;
        }
        
        curr = &(*curr)->next;
    }
}

// Main USB tick function (called from timer)
void usb_tick(void) {
    usb_enumeration_tick();
    
    // Also process HID initialization
    hid_init_tick();
}