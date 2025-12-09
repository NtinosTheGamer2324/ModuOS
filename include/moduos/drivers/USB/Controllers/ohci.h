#ifndef OHCI_H
#define OHCI_H

#include "moduos/drivers/USB/usb.h"
#include "moduos/drivers/PCI/pci.h"
#include <stdint.h>

// OHCI PCI Class/Subclass/ProgIF
#define OHCI_PCI_CLASS          0x0C
#define OHCI_PCI_SUBCLASS       0x03
#define OHCI_PCI_PROG_IF        0x10

// OHCI Memory-Mapped Register Offsets
#define OHCI_REG_REVISION           0x00
#define OHCI_REG_CONTROL            0x04
#define OHCI_REG_COMMAND_STATUS     0x08
#define OHCI_REG_INTERRUPT_STATUS   0x0C
#define OHCI_REG_INTERRUPT_ENABLE   0x10
#define OHCI_REG_INTERRUPT_DISABLE  0x14
#define OHCI_REG_HCCA               0x18
#define OHCI_REG_PERIOD_CURRENT_ED  0x1C
#define OHCI_REG_CONTROL_HEAD_ED    0x20
#define OHCI_REG_CONTROL_CURRENT_ED 0x24
#define OHCI_REG_BULK_HEAD_ED       0x28
#define OHCI_REG_BULK_CURRENT_ED    0x2C
#define OHCI_REG_DONE_HEAD          0x30
#define OHCI_REG_FM_INTERVAL        0x34
#define OHCI_REG_FM_REMAINING       0x38
#define OHCI_REG_FM_NUMBER          0x3C
#define OHCI_REG_PERIODIC_START     0x40
#define OHCI_REG_LS_THRESHOLD       0x44
#define OHCI_REG_RH_DESCRIPTOR_A    0x48
#define OHCI_REG_RH_DESCRIPTOR_B    0x4C
#define OHCI_REG_RH_STATUS          0x50
#define OHCI_REG_RH_PORT_STATUS     0x54  // Base, +4 for each port

// HcControl Register Bits
#define OHCI_CTRL_CBSR_MASK         0x03  // Control/Bulk Service Ratio
#define OHCI_CTRL_PLE               (1 << 2)  // Periodic List Enable
#define OHCI_CTRL_IE                (1 << 3)  // Isochronous Enable
#define OHCI_CTRL_CLE               (1 << 4)  // Control List Enable
#define OHCI_CTRL_BLE               (1 << 5)  // Bulk List Enable
#define OHCI_CTRL_HCFS_MASK         (3 << 6)  // Host Controller Functional State
#define OHCI_CTRL_HCFS_RESET        (0 << 6)
#define OHCI_CTRL_HCFS_RESUME       (1 << 6)
#define OHCI_CTRL_HCFS_OPERATIONAL  (2 << 6)
#define OHCI_CTRL_HCFS_SUSPEND      (3 << 6)
#define OHCI_CTRL_IR                (1 << 8)  // Interrupt Routing
#define OHCI_CTRL_RWC               (1 << 9)  // Remote Wakeup Connected
#define OHCI_CTRL_RWE               (1 << 10) // Remote Wakeup Enable

// HcCommandStatus Register Bits
#define OHCI_CMD_HCR                (1 << 0)  // Host Controller Reset
#define OHCI_CMD_CLF                (1 << 1)  // Control List Filled
#define OHCI_CMD_BLF                (1 << 2)  // Bulk List Filled
#define OHCI_CMD_OCR                (1 << 3)  // Ownership Change Request
#define OHCI_CMD_SOC_MASK           (3 << 16) // Scheduling Overrun Count

// Interrupt Status/Enable/Disable Register Bits
#define OHCI_INT_SO                 (1 << 0)  // Scheduling Overrun
#define OHCI_INT_WDH                (1 << 1)  // Writeback Done Head
#define OHCI_INT_SF                 (1 << 2)  // Start of Frame
#define OHCI_INT_RD                 (1 << 3)  // Resume Detected
#define OHCI_INT_UE                 (1 << 4)  // Unrecoverable Error
#define OHCI_INT_FNO                (1 << 5)  // Frame Number Overflow
#define OHCI_INT_RHSC               (1 << 6)  // Root Hub Status Change
#define OHCI_INT_OC                 (1 << 30) // Ownership Change
#define OHCI_INT_MIE                (1 << 31) // Master Interrupt Enable

// Root Hub Status Register Bits
#define OHCI_RH_LPS                 (1 << 0)  // Local Power Status
#define OHCI_RH_OCI                 (1 << 1)  // OverCurrent Indicator
#define OHCI_RH_DRWE                (1 << 15) // Device Remote Wakeup Enable
#define OHCI_RH_LPSC                (1 << 16) // Local Power Status Change
#define OHCI_RH_OCIC                (1 << 17) // OverCurrent Indicator Change
#define OHCI_RH_CRWE                (1 << 31) // Clear Remote Wakeup Enable

// Root Hub Port Status Register Bits
#define OHCI_PORT_CCS               (1 << 0)  // Current Connect Status
#define OHCI_PORT_PES               (1 << 1)  // Port Enable Status
#define OHCI_PORT_PSS               (1 << 2)  // Port Suspend Status
#define OHCI_PORT_POCI              (1 << 3)  // Port OverCurrent Indicator
#define OHCI_PORT_PRS               (1 << 4)  // Port Reset Status
#define OHCI_PORT_PPS               (1 << 8)  // Port Power Status
#define OHCI_PORT_LSDA              (1 << 9)  // Low Speed Device Attached
#define OHCI_PORT_CSC               (1 << 16) // Connect Status Change
#define OHCI_PORT_PESC              (1 << 17) // Port Enable Status Change
#define OHCI_PORT_PSSC              (1 << 18) // Port Suspend Status Change
#define OHCI_PORT_OCIC              (1 << 19) // Port OverCurrent Indicator Change
#define OHCI_PORT_PRSC              (1 << 20) // Port Reset Status Change

// ED (Endpoint Descriptor) Control Bits
#define OHCI_ED_FA_MASK             0x7F      // Function Address
#define OHCI_ED_EN_SHIFT            7
#define OHCI_ED_EN_MASK             (0xF << 7) // Endpoint Number
#define OHCI_ED_D_SHIFT             11
#define OHCI_ED_D_MASK              (3 << 11)  // Direction
#define OHCI_ED_D_TD                (0 << 11)  // From TD
#define OHCI_ED_D_OUT               (1 << 11)  // OUT
#define OHCI_ED_D_IN                (2 << 11)  // IN
#define OHCI_ED_S                   (1 << 13)  // Speed (0=Full, 1=Low)
#define OHCI_ED_K                   (1 << 14)  // Skip
#define OHCI_ED_F                   (1 << 15)  // Format (0=Gen, 1=Iso)
#define OHCI_ED_MPS_SHIFT           16
#define OHCI_ED_MPS_MASK            (0x7FF << 16) // Max Packet Size

// TD (Transfer Descriptor) Control Bits
#define OHCI_TD_R                   (1 << 18)  // Buffer Rounding
#define OHCI_TD_DP_SHIFT            19
#define OHCI_TD_DP_MASK             (3 << 19)  // Direction/PID
#define OHCI_TD_DP_SETUP            (0 << 19)
#define OHCI_TD_DP_OUT              (1 << 19)
#define OHCI_TD_DP_IN               (2 << 19)
#define OHCI_TD_DI_SHIFT            21
#define OHCI_TD_DI_MASK             (7 << 21)  // Delay Interrupt
#define OHCI_TD_T_SHIFT             24
#define OHCI_TD_T_MASK              (3 << 24)  // Data Toggle
#define OHCI_TD_EC_SHIFT            26
#define OHCI_TD_EC_MASK             (3 << 26)  // Error Count
#define OHCI_TD_CC_SHIFT            28
#define OHCI_TD_CC_MASK             (0xF << 28) // Condition Code
#define OHCI_TD_CC_NOERROR          0
#define OHCI_TD_CC_CRC              1
#define OHCI_TD_CC_BITSTUFFING      2
#define OHCI_TD_CC_DATATOGGLEMISMATCH 3
#define OHCI_TD_CC_STALL            4
#define OHCI_TD_CC_DEVICENOTRESPONDING 5
#define OHCI_TD_CC_PIDCHECKFAILURE  6
#define OHCI_TD_CC_UNEXPECTEDPID    7
#define OHCI_TD_CC_DATAOVERRUN      8
#define OHCI_TD_CC_DATAUNDERRUN     9
#define OHCI_TD_CC_BUFFEROVERRUN    12
#define OHCI_TD_CC_BUFFERUNDERRUN   13
#define OHCI_TD_CC_NOTACCESSED      15

// HCCA (Host Controller Communications Area)
typedef struct {
    uint32_t interrupt_table[32];
    uint16_t frame_number;
    uint16_t pad1;
    uint32_t done_head;
    uint8_t reserved[116];
} __attribute__((aligned(256))) ohci_hcca_t;

// ED (Endpoint Descriptor)
typedef struct ohci_ed {
    uint32_t control;
    uint32_t tail_ptr;
    uint32_t head_ptr;
    uint32_t next_ed;
    
    // Software-only fields
    uint32_t reserved[4];
} __attribute__((aligned(16))) ohci_ed_t;

// TD (Transfer Descriptor)
typedef struct ohci_td {
    uint32_t control;
    uint32_t current_buffer_ptr;
    uint32_t next_td;
    uint32_t buffer_end;
    
    // Software-only fields
    uint32_t reserved[4];
} __attribute__((aligned(16))) ohci_td_t;

// OHCI Controller Data
typedef struct {
    pci_device_t *pci_dev;
    volatile uint32_t *mmio_base;
    uint32_t mmio_phys;
    
    ohci_hcca_t *hcca;
    uint32_t hcca_phys;
    
    ohci_ed_t *control_head;
    ohci_ed_t *bulk_head;
    ohci_ed_t *interrupt_eds[32];
    
    ohci_td_t *td_pool;
    int td_pool_count;
    
    uint8_t num_ports;
    uint8_t next_address;
} ohci_controller_t;

// OHCI Functions
int ohci_init(void);
int ohci_probe(pci_device_t *pci_dev);
void ohci_shutdown(usb_controller_t *controller);

// Port operations
void ohci_reset_port(usb_controller_t *controller, uint8_t port);
int ohci_detect_device(usb_controller_t *controller, uint8_t port);

// Transfer functions
int ohci_control_transfer(usb_device_t *dev, usb_setup_packet_t *setup, void *data);
int ohci_interrupt_transfer(usb_device_t *dev, uint8_t endpoint, void *data, uint16_t len);
int ohci_bulk_transfer(usb_device_t *dev, uint8_t endpoint, void *data, uint16_t len);

// Helper functions
uint32_t ohci_read32(ohci_controller_t *ohci, uint32_t reg);
void ohci_write32(ohci_controller_t *ohci, uint32_t reg, uint32_t value);

#endif // OHCI_H
