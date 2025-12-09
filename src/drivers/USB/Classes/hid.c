#include "moduos/drivers/USB/Classes/hid.h"
#include "moduos/drivers/USB/usb.h"
#include "moduos/drivers/input/input.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/io/io.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/macros.h"

// Temporary replacement for kprintf
#define kprintf(fmt, ...) com_printf(COM1_PORT, fmt, ##__VA_ARGS__)

// Global HID driver
static usb_driver_t hid_usb_driver;
static hid_device_t *hid_devices[16];
static int hid_device_count = 0;

// Forward declarations
static int hid_parse_interface(hid_device_t *hid, usb_device_t *dev);
static void hid_process_keyboard_report(hid_device_t *hid);
static void hid_process_mouse_report(hid_device_t *hid);
static void hid_interrupt_callback(usb_device_t *dev, usb_transfer_t *transfer);

// Initialize HID subsystem
int hid_init(void) {
    kprintf("[HID] Initializing HID subsystem\n");
    
    hid_device_count = 0;
    memset(hid_devices, 0, sizeof(hid_devices));
    
    // Register HID driver with USB subsystem
    hid_usb_driver.name = "USB HID Driver";
    hid_usb_driver.vendor_id = 0;
    hid_usb_driver.product_id = 0;
    hid_usb_driver.device_class = USB_CLASS_HID;
    hid_usb_driver.device_subclass = 0;
    hid_usb_driver.probe = hid_probe;
    hid_usb_driver.disconnect = hid_disconnect;
    
    usb_register_driver(&hid_usb_driver);
    
    kprintf("[HID] HID subsystem initialized\n");
    return 0;
}

// Probe HID device
int hid_probe(usb_device_t *dev) {
    if (!dev) {
        return -1;
    }
    
    kprintf("[HID] Probing HID device\n");
    
    // Allocate HID device structure
    hid_device_t *hid = (hid_device_t*)kmalloc(sizeof(hid_device_t));
    if (!hid) {
        kprintf("[HID] Failed to allocate HID device\n");
        return -1;
    }
    
    memset(hid, 0, sizeof(hid_device_t));
    hid->usb_dev = dev;
    
    // Parse interface descriptor
    if (hid_parse_interface(hid, dev) != 0) {
        kprintf("[HID] Failed to parse interface\n");
        kfree(hid);
        return -1;
    }
    
    // Set boot protocol for boot devices
    if (hid->subclass == HID_SUBCLASS_BOOT) {
        kprintf("[HID] Setting boot protocol\n");
        if (hid_set_protocol(hid, 0) != 0) {
            kprintf("[HID] Failed to set boot protocol\n");
        }
    }
    
    // Set idle rate (indefinite - only report on change)
    hid_set_idle(hid, 0);
    
    // Initialize device based on protocol
    if (hid->protocol == HID_PROTOCOL_KEYBOARD) {
        kprintf("[HID] Keyboard detected\n");
        if (hid_keyboard_init(hid) == 0) {
            kprintf("[HID] Keyboard initialized successfully\n");
            
            // Start interrupt transfers
            if (hid_start_interrupt_transfers(hid) == 0) {
                kprintf("[HID] Keyboard interrupt transfers started\n");
            } else {
                kprintf("[HID] Failed to start interrupt transfers\n");
                kfree(hid->transfer_buffer);
                kfree(hid);
                return -1;
            }
        }
    } else if (hid->protocol == HID_PROTOCOL_MOUSE) {
        kprintf("[HID] Mouse detected\n");
        if (hid_mouse_init(hid) == 0) {
            kprintf("[HID] Mouse initialized successfully\n");
            
            // Start interrupt transfers
            if (hid_start_interrupt_transfers(hid) == 0) {
                kprintf("[HID] Mouse interrupt transfers started\n");
            } else {
                kprintf("[HID] Failed to start interrupt transfers\n");
                kfree(hid->transfer_buffer);
                kfree(hid);
                return -1;
            }
        }
    } else {
        kprintf("[HID] Generic HID device (protocol %d)\n", hid->protocol);
        kfree(hid);
        return -1;
    }
    
    // Add to device list
    if (hid_device_count < 16) {
        hid_devices[hid_device_count++] = hid;
        dev->driver_data = hid;
    }
    
    return 0;
}

// Disconnect HID device
void hid_disconnect(usb_device_t *dev) {
    if (!dev || !dev->driver_data) {
        return;
    }
    
    hid_device_t *hid = (hid_device_t*)dev->driver_data;
    
    kprintf("[HID] Disconnecting HID device\n");
    
    // Stop interrupt transfers
    hid_stop_interrupt_transfers(hid);
    
    // Remove from device list
    for (int i = 0; i < hid_device_count; i++) {
        if (hid_devices[i] == hid) {
            for (int j = i; j < hid_device_count - 1; j++) {
                hid_devices[j] = hid_devices[j + 1];
            }
            hid_device_count--;
            break;
        }
    }
    
    // Free transfer buffer
    if (hid->transfer_buffer) {
        kfree(hid->transfer_buffer);
    }
    
    // Free report descriptor
    if (hid->report_desc) {
        kfree(hid->report_desc);
    }
    
    // Free device structure
    kfree(hid);
    dev->driver_data = NULL;
}

// Parse interface descriptor to find HID information
static int hid_parse_interface(hid_device_t *hid, usb_device_t *dev) {
    // Get configuration descriptor
    uint8_t config_buffer[256];
    int result = usb_get_descriptor(dev, USB_DESC_CONFIGURATION, 0, config_buffer, sizeof(config_buffer));
    if (result < 0) {
        kprintf("[HID] Failed to get configuration descriptor\n");
        return -1;
    }
    
    // Parse configuration descriptor
    uint8_t *ptr = config_buffer;
    uint8_t *end = config_buffer + ((config_buffer[3] << 8) | config_buffer[2]);
    
    int found_interface = 0;
    
    while (ptr < end) {
        uint8_t length = ptr[0];
        uint8_t type = ptr[1];
        
        if (length == 0) break;
        
        // Interface descriptor
        if (type == USB_DESC_INTERFACE) {
            uint8_t interface_class = ptr[5];
            uint8_t interface_subclass = ptr[6];
            uint8_t interface_protocol = ptr[7];
            
            if (interface_class == USB_CLASS_HID) {
                hid->interface_num = ptr[2];
                hid->subclass = interface_subclass;
                hid->protocol = interface_protocol;
                found_interface = 1;
                kprintf("[HID] Found HID interface: subclass=%d, protocol=%d\n", 
                       interface_subclass, interface_protocol);
            }
        }
        
        // HID descriptor
        if (type == HID_DESC_HID && found_interface) {
            hid_descriptor_t *hid_desc = (hid_descriptor_t*)ptr;
            hid->country_code = hid_desc->bCountryCode;
            hid->report_desc_length = hid_desc->wDescriptorLength;
            kprintf("[HID] HID descriptor: report length=%d\n", hid->report_desc_length);
        }
        
        // Endpoint descriptor
        if (type == USB_DESC_ENDPOINT && found_interface) {
            uint8_t ep_addr = ptr[2];
            uint8_t ep_attr = ptr[3];
            uint16_t max_packet = ptr[4] | (ptr[5] << 8);
            
            // Check if it's an interrupt endpoint
            if ((ep_attr & 0x03) == 0x03) {
                if (ep_addr & 0x80) {
                    // IN endpoint
                    hid->endpoint_in = ep_addr;
                    hid->max_packet_size = max_packet;
                    kprintf("[HID] IN endpoint: 0x%02x, max_packet=%d\n", ep_addr, max_packet);
                } else {
                    // OUT endpoint
                    hid->endpoint_out = ep_addr;
                    kprintf("[HID] OUT endpoint: 0x%02x\n", ep_addr);
                }
            }
        }
        
        ptr += length;
    }
    
    if (!found_interface || !hid->endpoint_in) {
        kprintf("[HID] No HID interface or IN endpoint found\n");
        return -1;
    }
    
    return 0;
}

// Set HID protocol (0=boot, 1=report)
int hid_set_protocol(hid_device_t *hid, uint8_t protocol) {
    if (!hid || !hid->usb_dev) {
        return -1;
    }
    
    return usb_control_transfer(hid->usb_dev,
                               USB_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RECIPIENT_INTERFACE,
                               HID_REQ_SET_PROTOCOL,
                               protocol,
                               hid->interface_num,
                               NULL,
                               0);
}

// Set HID idle rate
int hid_set_idle(hid_device_t *hid, uint8_t duration) {
    if (!hid || !hid->usb_dev) {
        return -1;
    }
    
    return usb_control_transfer(hid->usb_dev,
                               USB_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RECIPIENT_INTERFACE,
                               HID_REQ_SET_IDLE,
                               (duration << 8) | 0,  // Duration in upper byte, Report ID in lower
                               hid->interface_num,
                               NULL,
                               0);
}

// Get HID report
int hid_get_report(hid_device_t *hid, uint8_t report_type, uint8_t report_id, void *buffer, uint16_t length) {
    if (!hid || !hid->usb_dev || !buffer) {
        return -1;
    }
    
    return usb_control_transfer(hid->usb_dev,
                               USB_DIR_IN | USB_REQ_TYPE_CLASS | USB_REQ_RECIPIENT_INTERFACE,
                               HID_REQ_GET_REPORT,
                               (report_type << 8) | report_id,
                               hid->interface_num,
                               buffer,
                               length);
}

// Set HID report
int hid_set_report(hid_device_t *hid, uint8_t report_type, uint8_t report_id, void *buffer, uint16_t length) {
    if (!hid || !hid->usb_dev || !buffer) {
        return -1;
    }
    
    return usb_control_transfer(hid->usb_dev,
                               USB_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RECIPIENT_INTERFACE,
                               HID_REQ_SET_REPORT,
                               (report_type << 8) | report_id,
                               hid->interface_num,
                               buffer,
                               length);
}

// Initialize keyboard
int hid_keyboard_init(hid_device_t *hid) {
    if (!hid) {
        return -1;
    }
    
    memset(&hid->report.keyboard, 0, sizeof(hid_keyboard_report_t));
    memset(&hid->last_report.keyboard, 0, sizeof(hid_keyboard_report_t));
    
    // Allocate transfer buffer
    hid->transfer_buffer = kmalloc(sizeof(hid_keyboard_report_t));
    if (!hid->transfer_buffer) {
        return -1;
    }
    
    memset(hid->transfer_buffer, 0, sizeof(hid_keyboard_report_t));
    
    // Set LEDs off
    uint8_t led_report = 0;
    hid_set_report(hid, HID_REPORT_OUTPUT, 0, &led_report, 1);
    
    kprintf("[HID] Keyboard ready for input\n");
    return 0;
}

// Initialize mouse
int hid_mouse_init(hid_device_t *hid) {
    if (!hid) {
        return -1;
    }
    
    memset(&hid->report.mouse, 0, sizeof(hid_mouse_report_t));
    memset(&hid->last_report.mouse, 0, sizeof(hid_mouse_report_t));
    
    // Allocate transfer buffer
    hid->transfer_buffer = kmalloc(sizeof(hid_mouse_report_t));
    if (!hid->transfer_buffer) {
        return -1;
    }
    
    memset(hid->transfer_buffer, 0, sizeof(hid_mouse_report_t));
    
    kprintf("[HID] Mouse ready for input\n");
    return 0;
}

// Interrupt transfer callback
static void hid_interrupt_callback(usb_device_t *dev, usb_transfer_t *transfer) {
    if (!dev || !dev->driver_data || !transfer) {
        return;
    }
    
    hid_device_t *hid = (hid_device_t*)dev->driver_data;
    
    // Check transfer status
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        // Error occurred - resubmit the transfer
        usb_submit_interrupt_transfer(dev, hid->endpoint_in, hid->transfer_buffer,
                                      hid->max_packet_size, hid_interrupt_callback, hid);
        return;
    }
    
    // Process the report based on device type
    if (hid->protocol == HID_PROTOCOL_KEYBOARD) {
        hid_keyboard_report_t *report = (hid_keyboard_report_t*)transfer->buffer;
        
        // Check if report changed
        if (memcmp(report, &hid->last_report.keyboard, sizeof(hid_keyboard_report_t)) != 0) {
            // Update current report
            memcpy(&hid->report.keyboard, report, sizeof(hid_keyboard_report_t));
            
            // Process keyboard input
            hid_process_keyboard_report(hid);
            
            // Save for next comparison
            memcpy(&hid->last_report.keyboard, report, sizeof(hid_keyboard_report_t));
        }
    } else if (hid->protocol == HID_PROTOCOL_MOUSE) {
        hid_mouse_report_t *report = (hid_mouse_report_t*)transfer->buffer;
        
        // Check if report changed
        if (memcmp(report, &hid->last_report.mouse, sizeof(hid_mouse_report_t)) != 0) {
            // Update current report
            memcpy(&hid->report.mouse, report, sizeof(hid_mouse_report_t));
            
            // Process mouse input
            hid_process_mouse_report(hid);
            
            // Save for next comparison
            memcpy(&hid->last_report.mouse, report, sizeof(hid_mouse_report_t));
        }
    }
    
    // Resubmit the transfer for continuous monitoring
    usb_submit_interrupt_transfer(dev, hid->endpoint_in, hid->transfer_buffer,
                                  hid->max_packet_size, hid_interrupt_callback, hid);
}

// Start interrupt transfers for HID device
int hid_start_interrupt_transfers(hid_device_t *hid) {
    if (!hid || !hid->usb_dev || !hid->transfer_buffer) {
        return -1;
    }
    
    kprintf("[HID] Starting interrupt transfers on endpoint 0x%02x\n", hid->endpoint_in);
    
    // Submit initial interrupt transfer
    int result = usb_submit_interrupt_transfer(
        hid->usb_dev,
        hid->endpoint_in,
        hid->transfer_buffer,
        hid->max_packet_size,
        hid_interrupt_callback,
        hid
    );
    
    if (result < 0) {
        kprintf("[HID] Failed to submit interrupt transfer\n");
        return -1;
    }
    
    kprintf("[HID] Interrupt transfers started successfully\n");
    return 0;
}

// Stop interrupt transfers for HID device
int hid_stop_interrupt_transfers(hid_device_t *hid) {
    if (!hid || !hid->usb_dev) {
        return -1;
    }
    
    // Cancel active transfer if any
    if (hid->active_transfer) {
        usb_cancel_transfer(hid->usb_dev, hid->active_transfer);
        hid->active_transfer = NULL;
    }
    
    return 0;
}

// Process keyboard report
static void hid_process_keyboard_report(hid_device_t *hid) {
    hid_keyboard_report_t *report = &hid->report.keyboard;
    
    // Call input system to process the report
    extern void usb_process_keyboard_report(hid_device_t *hid);
    usb_process_keyboard_report(hid);
}

// Process mouse report
static void hid_process_mouse_report(hid_device_t *hid) {
    hid_mouse_report_t *report = &hid->report.mouse;
    
    // TODO: Call input system to process mouse report
    // For now just log in debug mode
}

// Convert HID keycode to ASCII character
char hid_keycode_to_ascii(uint8_t keycode, uint8_t modifiers) {
    int shift = (modifiers & (HID_MOD_LEFT_SHIFT | HID_MOD_RIGHT_SHIFT)) != 0;
    
    // Letters
    if (keycode >= HID_KEY_A && keycode <= HID_KEY_Z) {
        char c = 'a' + (keycode - HID_KEY_A);
        if (shift) c = 'A' + (keycode - HID_KEY_A);
        return c;
    }
    
    // Numbers
    if (keycode >= HID_KEY_1 && keycode <= HID_KEY_9) {
        if (!shift) return '1' + (keycode - HID_KEY_1);
        // Shifted number keys
        const char shifted[] = "!@#$%^&*(";
        return shifted[keycode - HID_KEY_1];
    }
    if (keycode == HID_KEY_0) {
        return shift ? ')' : '0';
    }
    
    // Special characters
    switch (keycode) {
        case HID_KEY_SPACE: return ' ';
        case HID_KEY_ENTER: return '\n';
        case HID_KEY_TAB: return '\t';
        case HID_KEY_BACKSPACE: return '\b';
        case HID_KEY_MINUS: return shift ? '_' : '-';
        case HID_KEY_EQUAL: return shift ? '+' : '=';
        case HID_KEY_LEFT_BRACKET: return shift ? '{' : '[';
        case HID_KEY_RIGHT_BRACKET: return shift ? '}' : ']';
        case HID_KEY_BACKSLASH: return shift ? '|' : '\\';
        case HID_KEY_SEMICOLON: return shift ? ':' : ';';
        case HID_KEY_APOSTROPHE: return shift ? '"' : '\'';
        case HID_KEY_GRAVE: return shift ? '~' : '`';
        case HID_KEY_COMMA: return shift ? '<' : ',';
        case HID_KEY_PERIOD: return shift ? '>' : '.';
        case HID_KEY_SLASH: return shift ? '?' : '/';
        default: return 0;
    }
}

// Convert HID keycode to string
const char* hid_keycode_to_string(uint8_t keycode) {
    switch (keycode) {
        case HID_KEY_ESCAPE: return "ESC";
        case HID_KEY_F1: return "F1";
        case HID_KEY_F2: return "F2";
        case HID_KEY_F3: return "F3";
        case HID_KEY_F4: return "F4";
        case HID_KEY_F5: return "F5";
        case HID_KEY_F6: return "F6";
        case HID_KEY_F7: return "F7";
        case HID_KEY_F8: return "F8";
        case HID_KEY_F9: return "F9";
        case HID_KEY_F10: return "F10";
        case HID_KEY_F11: return "F11";
        case HID_KEY_F12: return "F12";
        case HID_KEY_PRINT_SCREEN: return "PRINT_SCREEN";
        case HID_KEY_SCROLL_LOCK: return "SCROLL_LOCK";
        case HID_KEY_PAUSE: return "PAUSE";
        case HID_KEY_INSERT: return "INSERT";
        case HID_KEY_HOME: return "HOME";
        case HID_KEY_PAGE_UP: return "PAGE_UP";
        case HID_KEY_DELETE: return "DELETE";
        case HID_KEY_END: return "END";
        case HID_KEY_PAGE_DOWN: return "PAGE_DOWN";
        case HID_KEY_RIGHT_ARROW: return "RIGHT";
        case HID_KEY_LEFT_ARROW: return "LEFT";
        case HID_KEY_DOWN_ARROW: return "DOWN";
        case HID_KEY_UP_ARROW: return "UP";
        case HID_KEY_NUM_LOCK: return "NUM_LOCK";
        case HID_KEY_CAPS_LOCK: return "CAPS_LOCK";
        default: return NULL;
    }
}