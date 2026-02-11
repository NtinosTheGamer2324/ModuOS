#pragma once

// Shared internal ABI between ModuOS in-tree SQRM modules.
// NOTE: This is not part of the stable external SDK yet.

#include <stdint.h>
#include <stddef.h>

// Minimal USB descriptor types
#define USB_DESC_DEVICE         0x01
#define USB_DESC_CONFIGURATION  0x02
#define USB_DESC_INTERFACE      0x04
#define USB_DESC_ENDPOINT       0x05

// Standard requests
#define USB_REQ_GET_DESCRIPTOR  0x06
#define USB_REQ_SET_ADDRESS     0x05
#define USB_REQ_SET_CONFIGURATION 0x09

// HID class
#define USB_CLASS_HID           0x03
#define HID_DESC_HID            0x21
#define HID_DESC_REPORT         0x22

#define HID_REQ_SET_PROTOCOL    0x0B
#define HID_REQ_SET_IDLE        0x0A

#define HID_PROTOCOL_BOOT       0
#define HID_PROTOCOL_REPORT     1

typedef enum {
    USB_SPEED_LOW  = 1,
    USB_SPEED_FULL = 2,
    USB_SPEED_HIGH = 3,
} usb_speed_t;

typedef struct {
    int controller_index;
    uint8_t addr;      // assigned USB address
    uint8_t speed;     // usb_speed_t
    uint16_t vid;
    uint16_t pid;
    uint8_t dev_class;
    uint8_t dev_subclass;
    uint8_t dev_protocol;
} usb_device_info_v1_t;

typedef struct {
    // Target
    int controller_index;
    uint8_t dev_addr;
    uint8_t endpoint;   // endpoint number
    uint8_t speed;      // usb_speed_t
    uint8_t ep_mps;      // max packet size (for interrupt; optional)

    // Data
    void *data;
    uint32_t length;

    // Results
    int32_t status;
    uint32_t actual_length;
} usb_int_in_xfer_v1_t;

typedef void (*usb_int_in_cb_v1_t)(usb_int_in_xfer_v1_t *xfer, void *user);

// USB core service API exported by usb.sqrm
typedef struct {
    int (*get_device_count)(void);
    int (*get_device_info)(int idx, usb_device_info_v1_t *out);

    // Standard control transfers (device requests)
    // bmRequestType allows class/interface requests (e.g., HID).
    int (*control_in)(int controller_index, uint8_t addr, uint8_t speed,
                      uint8_t bmRequestType, uint8_t request,
                      uint16_t value, uint16_t index,
                      void *data, uint16_t len);
    int (*control_out)(int controller_index, uint8_t addr, uint8_t speed,
                       uint8_t bmRequestType, uint8_t request,
                       uint16_t value, uint16_t index,
                       const void *data, uint16_t len);

    int (*set_address)(int controller_index, uint8_t speed, uint8_t new_addr);
    int (*set_configuration)(int controller_index, uint8_t addr, uint8_t speed, uint8_t cfg_value);

    // Interrupt IN (poll)
    int (*interrupt_in)(usb_int_in_xfer_v1_t *xfer, uint32_t timeout_ms);

    // Interrupt IN (async, IRQ-driven)
    // Submits an interrupt-IN transfer and returns immediately.
    // Completion will invoke cb (from IRQ context / bottom-half depending on controller impl).
    int (*interrupt_in_async)(usb_int_in_xfer_v1_t *xfer, usb_int_in_cb_v1_t cb, void *user);
} usbcore_api_v1_t;
