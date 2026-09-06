#ifndef USBMASTER_HC_API_H
#define USBMASTER_HC_API_H

#include <stdint.h>
#include <stddef.h>

#include <stdbool.h>

typedef struct __attribute__((packed)) {
    uint8_t  RequestType;
    uint8_t  Request;
    uint16_t Value;
    uint16_t Index;
    uint16_t Length;
} usb_setup_packet_t;

typedef struct {
    int (*send_control)(uint8_t device_address, usb_setup_packet_t packet, void *out_buffer, size_t response_len);
    int (*get_port_count)(void);
    int (*get_port_connected)(int port);
    int (*enable_port)(int port);
} uhci_hc_api_t;

/* Port states */
typedef enum {
    pconnected = true,
    pdisconnected = false
} port_state;

/* Error status NonUSB, Driver ones */
typedef enum {
    esuccess = 0,
    egeneral,
    eallocfail,
    etimeout,
    enotfound,
    ebarnotio,
} err;

#endif