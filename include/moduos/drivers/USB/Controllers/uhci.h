#ifndef UHCI_H
#define UHCI_H

#include "moduos/drivers/USB/usb.h"
#include "moduos/drivers/PCI/pci.h"
#include <stdint.h>

// UHCI PCI Class/Subclass/ProgIF
#define UHCI_PCI_CLASS          0x0C
#define UHCI_PCI_SUBCLASS       0x03
#define UHCI_PCI_PROG_IF        0x00

// UHCI I/O Register Offsets
#define UHCI_REG_USBCMD         0x00
#define UHCI_REG_USBSTS         0x02
#define UHCI_REG_USBINTR        0x04
#define UHCI_REG_FRNUM          0x06
#define UHCI_REG_FRBASEADD      0x08
#define UHCI_REG_SOFMOD         0x0C
#define UHCI_REG_PORTSC1        0x10
#define UHCI_REG_PORTSC2        0x12

// USBCMD - Command Register Bits
#define UHCI_CMD_RS             (1 << 0)  // Run/Stop
#define UHCI_CMD_HCRESET        (1 << 1)  // Host Controller Reset
#define UHCI_CMD_GRESET         (1 << 2)  // Global Reset
#define UHCI_CMD_EGSM           (1 << 3)  // Enter Global Suspend Mode
#define UHCI_CMD_FGR            (1 << 4)  // Force Global Resume
#define UHCI_CMD_SWDBG          (1 << 5)  // Software Debug
#define UHCI_CMD_CF             (1 << 6)  // Configure Flag
#define UHCI_CMD_MAXP           (1 << 7)  // Max Packet (0=32, 1=64)

// USBSTS - Status Register Bits
#define UHCI_STS_USBINT         (1 << 0)  // USB Interrupt
#define UHCI_STS_ERROR          (1 << 1)  // USB Error Interrupt
#define UHCI_STS_RD             (1 << 2)  // Resume Detect
#define UHCI_STS_HSE            (1 << 3)  // Host System Error
#define UHCI_STS_HCPE           (1 << 4)  // Host Controller Process Error
#define UHCI_STS_HCH            (1 << 5)  // HC Halted

// USBINTR - Interrupt Enable Register Bits
#define UHCI_INTR_TIMEOUT       (1 << 0)  // Timeout/CRC Interrupt Enable
#define UHCI_INTR_RESUME        (1 << 1)  // Resume Interrupt Enable
#define UHCI_INTR_IOC           (1 << 2)  // Interrupt On Complete Enable
#define UHCI_INTR_SP            (1 << 3)  // Short Packet Interrupt Enable

// PORTSC - Port Status and Control Register Bits
#define UHCI_PORT_CCS           (1 << 0)  // Current Connect Status
#define UHCI_PORT_CSC           (1 << 1)  // Connect Status Change
#define UHCI_PORT_PED           (1 << 2)  // Port Enabled/Disabled
#define UHCI_PORT_PEDC          (1 << 3)  // Port Enable/Disable Change
#define UHCI_PORT_LS            (3 << 4)  // Line Status
#define UHCI_PORT_RD            (1 << 6)  // Resume Detect
#define UHCI_PORT_LSDA          (1 << 8)  // Low Speed Device Attached
#define UHCI_PORT_PR            (1 << 9)  // Port Reset
#define UHCI_PORT_SUSP          (1 << 12) // Suspend

// Transfer Descriptor (TD) Link Pointer bits
#define UHCI_TD_LINK_TERMINATE  (1 << 0)
#define UHCI_TD_LINK_QH         (1 << 1)
#define UHCI_TD_LINK_DEPTH      (1 << 2)

// Transfer Descriptor (TD) Control/Status bits
#define UHCI_TD_STATUS_ACTLEN_MASK  0x7FF
#define UHCI_TD_STATUS_BITSTUFF     (1 << 17)
#define UHCI_TD_STATUS_CRC          (1 << 18)
#define UHCI_TD_STATUS_NAK          (1 << 19)
#define UHCI_TD_STATUS_BABBLE       (1 << 20)
#define UHCI_TD_STATUS_DBUFFER      (1 << 21)
#define UHCI_TD_STATUS_STALLED      (1 << 22)
#define UHCI_TD_STATUS_ACTIVE       (1 << 23)
#define UHCI_TD_STATUS_IOC          (1 << 24)
#define UHCI_TD_STATUS_IOS          (1 << 25)
#define UHCI_TD_STATUS_LS           (1 << 26)
#define UHCI_TD_STATUS_C_ERR_MASK   (3 << 27)
#define UHCI_TD_STATUS_SPD          (1 << 29)

// Transfer Descriptor (TD) Token bits
#define UHCI_TD_TOKEN_PID_IN        0x69
#define UHCI_TD_TOKEN_PID_OUT       0xE1
#define UHCI_TD_TOKEN_PID_SETUP     0x2D

// Queue Head (QH) Link Pointer bits
#define UHCI_QH_LINK_TERMINATE  (1 << 0)
#define UHCI_QH_LINK_QH         (1 << 1)

// Frame List Size
#define UHCI_FRAMELIST_COUNT    1024

// Transfer Descriptor (TD)
typedef struct uhci_td {
    uint32_t link_ptr;
    uint32_t status;
    uint32_t token;
    uint32_t buffer_ptr;
    
    // Software-only fields (not seen by hardware)
    uint32_t reserved[4];
} __attribute__((aligned(16))) uhci_td_t;

// Queue Head (QH)
typedef struct uhci_qh {
    uint32_t head_link_ptr;
    uint32_t element_link_ptr;
    
    // Software-only fields
    uint32_t reserved[6];
} __attribute__((aligned(16))) uhci_qh_t;

// UHCI Controller Data
typedef struct {
    pci_device_t *pci_dev;
    uint16_t iobase;
    
    uint32_t *frame_list;
    uint32_t frame_list_phys;
    
    uhci_qh_t *control_qh;
    uhci_qh_t *bulk_qh;
    uhci_qh_t *interrupt_qh;
    
    uhci_td_t *td_pool;
    int td_pool_count;
    
    uint8_t next_address;
} uhci_controller_t;

// UHCI Functions
int uhci_init(void);
int uhci_probe(pci_device_t *pci_dev);
void uhci_shutdown(usb_controller_t *controller);

// Port operations
void uhci_reset_port(usb_controller_t *controller, uint8_t port);
int uhci_detect_device(usb_controller_t *controller, uint8_t port);

// Transfer functions
int uhci_control_transfer(usb_device_t *dev, usb_setup_packet_t *setup, void *data);
int uhci_interrupt_transfer(usb_device_t *dev, uint8_t endpoint, void *data, uint16_t len);
int uhci_bulk_transfer(usb_device_t *dev, uint8_t endpoint, void *data, uint16_t len);

// Helper functions
uint16_t uhci_read16(uhci_controller_t *uhci, uint16_t reg);
uint32_t uhci_read32(uhci_controller_t *uhci, uint16_t reg);
void uhci_write16(uhci_controller_t *uhci, uint16_t reg, uint16_t value);
void uhci_write32(uhci_controller_t *uhci, uint16_t reg, uint32_t value);

#endif // UHCI_H