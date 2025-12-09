#ifndef MODUOS_DRIVERS_USB_H
#define MODUOS_DRIVERS_USB_H

#include <stdint.h>
#include <stdbool.h>

// USB Speeds
#define USB_SPEED_LOW       0
#define USB_SPEED_FULL      1
#define USB_SPEED_HIGH      2

// USB Request Types
#define USB_REQ_TYPE_STANDARD   0x00
#define USB_REQ_TYPE_CLASS      0x20
#define USB_REQ_TYPE_VENDOR     0x40

// USB Request Recipients
#define USB_REQ_RECIPIENT_DEVICE    0x00
#define USB_REQ_RECIPIENT_INTERFACE 0x01
#define USB_REQ_RECIPIENT_ENDPOINT  0x02

// USB Standard Requests
#define USB_REQ_GET_STATUS          0x00
#define USB_REQ_CLEAR_FEATURE       0x01
#define USB_REQ_SET_FEATURE         0x03
#define USB_REQ_SET_ADDRESS         0x05
#define USB_REQ_GET_DESCRIPTOR      0x06
#define USB_REQ_SET_DESCRIPTOR      0x07
#define USB_REQ_GET_CONFIGURATION   0x08
#define USB_REQ_SET_CONFIGURATION   0x09
#define USB_REQ_GET_INTERFACE       0x0A
#define USB_REQ_SET_INTERFACE       0x0B
#define USB_REQ_SYNCH_FRAME         0x0C

// USB Descriptor Types
#define USB_DESC_DEVICE             0x01
#define USB_DESC_CONFIGURATION      0x02
#define USB_DESC_STRING             0x03
#define USB_DESC_INTERFACE          0x04
#define USB_DESC_ENDPOINT           0x05
#define USB_DESC_HID                0x21
#define USB_DESC_REPORT             0x22
#define USB_DESC_PHYSICAL           0x23
#define USB_DESC_HUB                0x29

// USB Device Classes
#define USB_CLASS_PER_INTERFACE     0x00
#define USB_CLASS_AUDIO             0x01
#define USB_CLASS_COMM              0x02
#define USB_CLASS_HID               0x03
#define USB_CLASS_PHYSICAL          0x05
#define USB_CLASS_IMAGE             0x06
#define USB_CLASS_PRINTER           0x07
#define USB_CLASS_MASS_STORAGE      0x08
#define USB_CLASS_HUB               0x09
#define USB_CLASS_CDC_DATA          0x0A
#define USB_CLASS_SMART_CARD        0x0B
#define USB_CLASS_CONTENT_SECURITY  0x0D
#define USB_CLASS_VIDEO             0x0E
#define USB_CLASS_DIAGNOSTIC        0xDC
#define USB_CLASS_WIRELESS          0xE0
#define USB_CLASS_MISC              0xEF
#define USB_CLASS_APP_SPECIFIC      0xFE
#define USB_CLASS_VENDOR_SPECIFIC   0xFF

// USB Endpoint Directions
#define USB_DIR_OUT                 0x00
#define USB_DIR_IN                  0x80

// USB PID (Packet ID) tokens
#define USB_PID_SETUP               0x2D
#define USB_PID_IN                  0x69
#define USB_PID_OUT                 0xE1

// USB Endpoint Types
#define USB_ENDPOINT_TYPE_CONTROL       0x00
#define USB_ENDPOINT_TYPE_ISOCHRONOUS   0x01
#define USB_ENDPOINT_TYPE_BULK          0x02
#define USB_ENDPOINT_TYPE_INTERRUPT     0x03

// USB Setup Packet
typedef struct {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) usb_setup_packet_t;

// USB Device Descriptor
typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} __attribute__((packed)) usb_device_descriptor_t;

// USB Configuration Descriptor
typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
} __attribute__((packed)) usb_config_descriptor_t;

// USB Interface Descriptor
typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} __attribute__((packed)) usb_interface_descriptor_t;

// USB Endpoint Descriptor
typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} __attribute__((packed)) usb_endpoint_descriptor_t;

// Forward declarations
typedef struct usb_device usb_device_t;
typedef struct usb_driver usb_driver_t;
typedef struct usb_controller usb_controller_t;
typedef struct usb_transfer usb_transfer_t;

// USB Transfer Callback
typedef void (*usb_transfer_callback_t)(usb_device_t *dev, usb_transfer_t *transfer);

// USB Transfer Status
#define USB_TRANSFER_STATUS_PENDING     0
#define USB_TRANSFER_STATUS_COMPLETED   1
#define USB_TRANSFER_STATUS_ERROR       2
#define USB_TRANSFER_STATUS_STALLED     3
#define USB_TRANSFER_STATUS_TIMEOUT     4

// USB Transfer Structure
struct usb_transfer {
    usb_device_t *device;
    uint8_t endpoint;
    void *buffer;
    uint16_t length;
    uint16_t actual_length;
    uint8_t status;
    
    usb_transfer_callback_t callback;
    void *callback_data;
    
    usb_transfer_t *next;
};

// USB Device Structure
struct usb_device {
    uint8_t address;
    uint8_t speed;
    uint8_t port;
    uint16_t max_packet_size;
    
    usb_device_descriptor_t descriptor;
    usb_config_descriptor_t *config;
    
    usb_controller_t *controller;
    usb_driver_t *driver;
    void *driver_data;
    
    usb_transfer_t *active_transfers;
    
    struct usb_device *next;
};

// USB Driver Structure
struct usb_driver {
    const char *name;
    
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t device_class;
    uint8_t device_subclass;
    
    int (*probe)(usb_device_t *dev);
    void (*disconnect)(usb_device_t *dev);
    
    usb_driver_t *next;
};

// USB Controller Operations
typedef struct {
    int (*init)(usb_controller_t *controller);
    void (*shutdown)(usb_controller_t *controller);
    void (*reset_port)(usb_controller_t *controller, uint8_t port);
    int (*control_transfer)(usb_device_t *dev, usb_setup_packet_t *setup, void *data);
    int (*interrupt_transfer)(usb_device_t *dev, uint8_t endpoint, void *data, uint16_t len);
    int (*bulk_transfer)(usb_device_t *dev, uint8_t endpoint, void *data, uint16_t len);
    
    // Async transfer functions (interrupt-driven)
    int (*submit_interrupt_transfer)(usb_device_t *dev, usb_transfer_t *transfer);
    int (*cancel_transfer)(usb_device_t *dev, usb_transfer_t *transfer);
} usb_controller_ops_t;

// USB Controller Structure
struct usb_controller {
    const char *name;
    void *regs;
    uint8_t num_ports;
    
    usb_controller_ops_t *ops;
    void *controller_data;
    
    usb_device_t *devices;
    usb_controller_t *next;
};

// USB Subsystem Functions
void usb_init(void);
int usb_register_controller(usb_controller_t *controller);
void usb_unregister_controller(usb_controller_t *controller);

int usb_register_driver(usb_driver_t *driver);
void usb_unregister_driver(usb_driver_t *driver);

// USB Device Management
usb_device_t* usb_alloc_device(usb_controller_t *controller);
void usb_free_device(usb_device_t *dev);
int usb_enumerate_device(usb_device_t *dev);
void usb_match_drivers(usb_device_t *dev);

// USB Transfers
int usb_control_transfer(usb_device_t *dev, uint8_t request_type, uint8_t request,
                        uint16_t value, uint16_t index, void *data, uint16_t length);
int usb_get_descriptor(usb_device_t *dev, uint8_t desc_type, uint8_t desc_index,
                      void *buffer, uint16_t length);
int usb_set_address(usb_device_t *dev, uint8_t address);
int usb_set_configuration(usb_device_t *dev, uint8_t config);

// Async Transfer Functions
usb_transfer_t* usb_alloc_transfer(void);
void usb_free_transfer(usb_transfer_t *transfer);
int usb_submit_interrupt_transfer(usb_device_t *dev, uint8_t endpoint, void *buffer,
                                   uint16_t length, usb_transfer_callback_t callback,
                                   void *callback_data);
int usb_cancel_transfer(usb_device_t *dev, usb_transfer_t *transfer);

#endif // MODUOS_DRIVERS_USB_H