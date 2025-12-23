#ifndef USB_HID_H
#define USB_HID_H

#include "moduos/drivers/USB/usb.h"
#include <stdint.h>

// HID Class Codes
#define USB_CLASS_HID               0x03

// HID Subclass Codes
#define HID_SUBCLASS_NONE           0x00
#define HID_SUBCLASS_BOOT           0x01

// HID Protocol Codes
#define HID_PROTOCOL_NONE           0x00
#define HID_PROTOCOL_KEYBOARD       0x01
#define HID_PROTOCOL_MOUSE          0x02

// HID Descriptor Types
#define HID_DESC_HID                0x21
#define HID_DESC_REPORT             0x22
#define HID_DESC_PHYSICAL           0x23

// HID Class-Specific Requests
#define HID_REQ_GET_REPORT          0x01
#define HID_REQ_GET_IDLE            0x02
#define HID_REQ_GET_PROTOCOL        0x03
#define HID_REQ_SET_REPORT          0x09
#define HID_REQ_SET_IDLE            0x0A
#define HID_REQ_SET_PROTOCOL        0x0B

// HID Report Types
#define HID_REPORT_INPUT            0x01
#define HID_REPORT_OUTPUT           0x02
#define HID_REPORT_FEATURE          0x03

// HID Boot Protocol Keyboard Modifiers
#define HID_MOD_LEFT_CTRL           (1 << 0)
#define HID_MOD_LEFT_SHIFT          (1 << 1)
#define HID_MOD_LEFT_ALT            (1 << 2)
#define HID_MOD_LEFT_GUI            (1 << 3)
#define HID_MOD_RIGHT_CTRL          (1 << 4)
#define HID_MOD_RIGHT_SHIFT         (1 << 5)
#define HID_MOD_RIGHT_ALT           (1 << 6)
#define HID_MOD_RIGHT_GUI           (1 << 7)

// HID Boot Protocol Mouse Buttons
#define HID_MOUSE_BUTTON_LEFT       (1 << 0)
#define HID_MOUSE_BUTTON_RIGHT      (1 << 1)
#define HID_MOUSE_BUTTON_MIDDLE     (1 << 2)

// HID Descriptor
typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdHID;
    uint8_t bCountryCode;
    uint8_t bNumDescriptors;
    uint8_t bDescriptorType2;
    uint16_t wDescriptorLength;
} __attribute__((packed)) hid_descriptor_t;

// HID Boot Protocol Keyboard Report
typedef struct {
    uint8_t modifiers;      // Modifier keys (Ctrl, Shift, Alt, etc.)
    uint8_t reserved;       // Reserved (always 0)
    uint8_t keys[6];        // Up to 6 simultaneous key presses
} __attribute__((packed)) hid_keyboard_report_t;

// HID Boot Protocol Mouse Report
typedef struct {
    uint8_t buttons;        // Button states
    int8_t x;              // X movement
    int8_t y;              // Y movement
    int8_t wheel;          // Wheel movement (optional)
} __attribute__((packed)) hid_mouse_report_t;

// HID Device Structure
typedef struct hid_device {
    usb_device_t *usb_dev;
    uint8_t interface_num;
    uint8_t endpoint_in;
    uint8_t endpoint_out;
    uint16_t max_packet_size;
    
    uint8_t protocol;       // Boot or Report protocol
    uint8_t subclass;
    uint8_t country_code;
    
    uint16_t report_desc_length;
    uint8_t *report_desc;
    
    // Device type specific data
    union {
        hid_keyboard_report_t keyboard;
        hid_mouse_report_t mouse;
    } report;
    
    // Previous report for change detection
    union {
        hid_keyboard_report_t keyboard;
        hid_mouse_report_t mouse;
    } last_report;
    
    // Transfer buffer and structure
    void *transfer_buffer;
    usb_transfer_t *active_transfer;
    
    // Callback for input reports
    void (*input_callback)(void *device, void *report, uint16_t length);
    void *callback_data;
} hid_device_t;

// HID USB Keyboard Scan Codes (Usage IDs)
#define HID_KEY_NONE            0x00
#define HID_KEY_A               0x04
#define HID_KEY_B               0x05
#define HID_KEY_C               0x06
#define HID_KEY_D               0x07
#define HID_KEY_E               0x08
#define HID_KEY_F               0x09
#define HID_KEY_G               0x0A
#define HID_KEY_H               0x0B
#define HID_KEY_I               0x0C
#define HID_KEY_J               0x0D
#define HID_KEY_K               0x0E
#define HID_KEY_L               0x0F
#define HID_KEY_M               0x10
#define HID_KEY_N               0x11
#define HID_KEY_O               0x12
#define HID_KEY_P               0x13
#define HID_KEY_Q               0x14
#define HID_KEY_R               0x15
#define HID_KEY_S               0x16
#define HID_KEY_T               0x17
#define HID_KEY_U               0x18
#define HID_KEY_V               0x19
#define HID_KEY_W               0x1A
#define HID_KEY_X               0x1B
#define HID_KEY_Y               0x1C
#define HID_KEY_Z               0x1D
#define HID_KEY_1               0x1E
#define HID_KEY_2               0x1F
#define HID_KEY_3               0x20
#define HID_KEY_4               0x21
#define HID_KEY_5               0x22
#define HID_KEY_6               0x23
#define HID_KEY_7               0x24
#define HID_KEY_8               0x25
#define HID_KEY_9               0x26
#define HID_KEY_0               0x27
#define HID_KEY_ENTER           0x28
#define HID_KEY_ESCAPE          0x29
#define HID_KEY_BACKSPACE       0x2A
#define HID_KEY_TAB             0x2B
#define HID_KEY_SPACE           0x2C
#define HID_KEY_MINUS           0x2D
#define HID_KEY_EQUAL           0x2E
#define HID_KEY_LEFT_BRACKET    0x2F
#define HID_KEY_RIGHT_BRACKET   0x30
#define HID_KEY_BACKSLASH       0x31
#define HID_KEY_SEMICOLON       0x33
#define HID_KEY_APOSTROPHE      0x34
#define HID_KEY_GRAVE           0x35
#define HID_KEY_COMMA           0x36
#define HID_KEY_PERIOD          0x37
#define HID_KEY_SLASH           0x38
#define HID_KEY_CAPS_LOCK       0x39
#define HID_KEY_F1              0x3A
#define HID_KEY_F2              0x3B
#define HID_KEY_F3              0x3C
#define HID_KEY_F4              0x3D
#define HID_KEY_F5              0x3E
#define HID_KEY_F6              0x3F
#define HID_KEY_F7              0x40
#define HID_KEY_F8              0x41
#define HID_KEY_F9              0x42
#define HID_KEY_F10             0x43
#define HID_KEY_F11             0x44
#define HID_KEY_F12             0x45
#define HID_KEY_PRINT_SCREEN    0x46
#define HID_KEY_SCROLL_LOCK     0x47
#define HID_KEY_PAUSE           0x48
#define HID_KEY_INSERT          0x49
#define HID_KEY_HOME            0x4A
#define HID_KEY_PAGE_UP         0x4B
#define HID_KEY_DELETE          0x4C
#define HID_KEY_END             0x4D
#define HID_KEY_PAGE_DOWN       0x4E
#define HID_KEY_RIGHT_ARROW     0x4F
#define HID_KEY_LEFT_ARROW      0x50
#define HID_KEY_DOWN_ARROW      0x51
#define HID_KEY_UP_ARROW        0x52
#define HID_KEY_NUM_LOCK        0x53

// HID Functions
int hid_init(void);
void hid_init_tick(void);
int hid_probe(usb_device_t *dev);
void hid_disconnect(usb_device_t *dev);

// HID Operations
int hid_set_protocol(hid_device_t *hid, uint8_t protocol);
int hid_set_idle(hid_device_t *hid, uint8_t duration);
int hid_get_report(hid_device_t *hid, uint8_t report_type, uint8_t report_id, void *buffer, uint16_t length);
int hid_set_report(hid_device_t *hid, uint8_t report_type, uint8_t report_id, void *buffer, uint16_t length);

// Device-specific functions
int hid_keyboard_init(hid_device_t *hid);
int hid_mouse_init(hid_device_t *hid);
int hid_start_interrupt_transfers(hid_device_t *hid);
int hid_stop_interrupt_transfers(hid_device_t *hid);

// Legacy polling functions (for compatibility)
void hid_keyboard_poll(hid_device_t *hid);
void hid_mouse_poll(hid_device_t *hid);

// Utility functions
char hid_keycode_to_ascii(uint8_t keycode, uint8_t modifiers);
const char* hid_keycode_to_string(uint8_t keycode);

// Poll all HID devices
void hid_poll_all(void);

#endif // USB_HID_H
