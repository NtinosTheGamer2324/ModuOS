#pragma once
/*
 * sqrm_usb.h — SQRM USB ABI: transfer types, UHCI controller API,
 *              and the minimal USB core service API.
 *
 * Available to: USB modules.
 *
 * Service name conventions:
 *   USB core  : "sqrm_usb_v1"         (sqrm_usb_api_v1_t)
 *   UHCI ctl  : "usbctl_uhci"         (sqrm_usbctl_uhci_api_v1_t)
 *   OHCI ctl  : "usbctl_ohci"         (future)
 *   EHCI ctl  : "usbctl_ehci"         (future)
 *
 * Register / retrieve services via sqrm_service_register / sqrm_service_get
 * in sqrm_kernel_api_t.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Enumerations                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    SQRM_USB_SPEED_LOW  = 1,   /* 1.5 Mbit/s  */
    SQRM_USB_SPEED_FULL = 2,   /*  12 Mbit/s  */
    SQRM_USB_SPEED_HIGH = 3,   /* 480 Mbit/s  */
} sqrm_usb_speed_t;

typedef enum {
    SQRM_USB_XFER_CONTROL   = 1,
    SQRM_USB_XFER_BULK      = 2,
    SQRM_USB_XFER_INTERRUPT = 3,
} sqrm_usb_xfer_type_t;

/* ------------------------------------------------------------------ */
/*  Setup packet (8 bytes, USB spec §9.3)                             */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) sqrm_usb_setup_packet_t;

/* ------------------------------------------------------------------ */
/*  Transfer descriptor                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    /* Target endpoint */
    uint8_t dev_addr;       /* USB device address (0 = default / unconfigured) */
    uint8_t endpoint;       /* endpoint number */
    uint8_t speed;          /* sqrm_usb_speed_t */
    uint8_t xfer_type;      /* sqrm_usb_xfer_type_t */

    /* CONTROL transfers only */
    sqrm_usb_setup_packet_t setup;

    /* Data stage */
    void    *data;
    uint32_t length;
    uint8_t  direction_in;  /* 1 = IN (device→host), 0 = OUT (host→device) */

    /* Results (filled in by the host controller) */
    int32_t  status;        /* 0 on success, negative errno on failure */
    uint32_t actual_length;
} sqrm_usb_transfer_v1_t;

typedef uint32_t sqrm_usb_xfer_handle_t;
#define SQRM_USB_XFER_INVALID_HANDLE 0u

/* ------------------------------------------------------------------ */
/*  UHCI controller info                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  bus, device, function;
    uint8_t  irq_line;
    uint16_t io_base;       /* UHCI uses I/O ports */
} sqrm_uhci_controller_info_v1_t;

/* ------------------------------------------------------------------ */
/*  UHCI host-controller service API                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    /* Discovery */
    int (*get_controller_count)(void);
    int (*get_controller_info)(int index, sqrm_uhci_controller_info_v1_t *out);

    /*
     * submit() — enqueue a transfer.
     * The implementation may copy fields from *xfer; do not free it until
     * after wait() or cancel() completes.
     * Returns a non-zero handle on success, SQRM_USB_XFER_INVALID_HANDLE on error.
     */
    sqrm_usb_xfer_handle_t (*submit)(int controller_index,
                                     sqrm_usb_transfer_v1_t *xfer);

    /* wait() — block until transfer is done or timeout_ms elapses; return 0 on success */
    int (*wait)  (sqrm_usb_xfer_handle_t handle, uint32_t timeout_ms);

    /* cancel() — attempt to cancel a pending transfer; return 0 on success */
    int (*cancel)(sqrm_usb_xfer_handle_t handle);
} sqrm_usbctl_uhci_api_v1_t;

/* ------------------------------------------------------------------ */
/*  USB core service API (high-level, implemented by the USB core mod) */
/* ------------------------------------------------------------------ */

typedef struct {
    int (*get_controller_count)(void);
    int (*get_device_count)(void);
    int (*enumerate)(void);   /* request a (re)enumeration of the bus */
} sqrm_usb_api_v1_t;

#ifdef __cplusplus
}
#endif