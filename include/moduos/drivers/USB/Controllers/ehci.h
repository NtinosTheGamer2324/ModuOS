#ifndef EHCI_H
#define EHCI_H

#include "moduos/drivers/USB/usb.h"
#include "moduos/drivers/PCI/pci.h"
#include <stdint.h>

// EHCI PCI Class/Subclass/ProgIF
#define EHCI_PCI_CLASS          0x0C
#define EHCI_PCI_SUBCLASS       0x03
#define EHCI_PCI_PROG_IF        0x20

// EHCI Capability Register Offsets
#define EHCI_CAP_CAPLENGTH      0x00
#define EHCI_CAP_HCIVERSION     0x02
#define EHCI_CAP_HCSPARAMS      0x04
#define EHCI_CAP_HCCPARAMS      0x08

// EHCI Operational Register Offsets (relative to CAPLENGTH)
#define EHCI_OP_USBCMD          0x00
#define EHCI_OP_USBSTS          0x04
#define EHCI_OP_USBINTR         0x08
#define EHCI_OP_FRINDEX         0x0C
#define EHCI_OP_CTRLDSSEGMENT   0x10
#define EHCI_OP_PERIODICLISTBASE 0x14
#define EHCI_OP_ASYNCLISTADDR   0x18
#define EHCI_OP_CONFIGFLAG      0x40
#define EHCI_OP_PORTSC          0x44  // Base, +4 for each port

// USBCMD - Command Register Bits
#define EHCI_CMD_RS             (1 << 0)  // Run/Stop
#define EHCI_CMD_HCRESET        (1 << 1)  // Host Controller Reset
#define EHCI_CMD_FLS_MASK       (3 << 2)  // Frame List Size
#define EHCI_CMD_FLS_1024       (0 << 2)
#define EHCI_CMD_FLS_512        (1 << 2)
#define EHCI_CMD_FLS_256        (2 << 2)
#define EHCI_CMD_PSE            (1 << 4)  // Periodic Schedule Enable
#define EHCI_CMD_ASE            (1 << 5)  // Async Schedule Enable
#define EHCI_CMD_IAAD           (1 << 6)  // Interrupt on Async Advance Doorbell
#define EHCI_CMD_LHCR           (1 << 7)  // Light Host Controller Reset
#define EHCI_CMD_ASPMC_SHIFT    8
#define EHCI_CMD_ASPMC_MASK     (3 << 8)  // Async Schedule Park Mode Count
#define EHCI_CMD_ASPME          (1 << 11) // Async Schedule Park Mode Enable
#define EHCI_CMD_ITC_SHIFT      16
#define EHCI_CMD_ITC_MASK       (0xFF << 16) // Interrupt Threshold Control

// USBSTS - Status Register Bits
#define EHCI_STS_USBINT         (1 << 0)  // USB Interrupt
#define EHCI_STS_ERROR          (1 << 1)  // USB Error Interrupt
#define EHCI_STS_PCD            (1 << 2)  // Port Change Detect
#define EHCI_STS_FLR            (1 << 3)  // Frame List Rollover
#define EHCI_STS_HSE            (1 << 4)  // Host System Error
#define EHCI_STS_IAA            (1 << 5)  // Interrupt on Async Advance
#define EHCI_STS_HCHALTED       (1 << 12) // HC Halted
#define EHCI_STS_RECLAMATION    (1 << 13) // Reclamation
#define EHCI_STS_PSS            (1 << 14) // Periodic Schedule Status
#define EHCI_STS_ASS            (1 << 15) // Async Schedule Status

// USBINTR - Interrupt Enable Register Bits
#define EHCI_INTR_USBINT        (1 << 0)  // USB Interrupt Enable
#define EHCI_INTR_ERROR         (1 << 1)  // USB Error Interrupt Enable
#define EHCI_INTR_PCD           (1 << 2)  // Port Change Detect Enable
#define EHCI_INTR_FLR           (1 << 3)  // Frame List Rollover Enable
#define EHCI_INTR_HSE           (1 << 4)  // Host System Error Enable
#define EHCI_INTR_IAA           (1 << 5)  // Interrupt on Async Advance Enable

// HCSPARAMS - Structural Parameters Bits
#define EHCI_HCSPARAMS_N_PORTS_MASK 0x0F  // Number of ports
#define EHCI_HCSPARAMS_PPC      (1 << 4)  // Port Power Control
#define EHCI_HCSPARAMS_N_PCC_SHIFT 8
#define EHCI_HCSPARAMS_N_PCC_MASK (0x0F << 8) // Number of Ports per CC
#define EHCI_HCSPARAMS_N_CC_SHIFT 12
#define EHCI_HCSPARAMS_N_CC_MASK (0x0F << 12) // Number of Companion Controllers

// HCCPARAMS - Capability Parameters Bits
#define EHCI_HCCPARAMS_ADC      (1 << 0)  // 64-bit Addressing Capability
#define EHCI_HCCPARAMS_PFL      (1 << 1)  // Programmable Frame List Flag
#define EHCI_HCCPARAMS_ASPC     (1 << 2)  // Async Schedule Park Capability
#define EHCI_HCCPARAMS_IST_SHIFT 4
#define EHCI_HCCPARAMS_IST_MASK (0x0F << 4) // Isochronous Scheduling Threshold
#define EHCI_HCCPARAMS_EECP_SHIFT 8
#define EHCI_HCCPARAMS_EECP_MASK (0xFF << 8) // EHCI Extended Capabilities Pointer

// PORTSC - Port Status and Control Register Bits
#define EHCI_PORT_CCS           (1 << 0)  // Current Connect Status
#define EHCI_PORT_CSC           (1 << 1)  // Connect Status Change
#define EHCI_PORT_PED           (1 << 2)  // Port Enabled/Disabled
#define EHCI_PORT_PEDC          (1 << 3)  // Port Enable/Disable Change
#define EHCI_PORT_OCA           (1 << 4)  // Over-current Active
#define EHCI_PORT_OCC           (1 << 5)  // Over-current Change
#define EHCI_PORT_FPR           (1 << 6)  // Force Port Resume
#define EHCI_PORT_SUSPEND       (1 << 7)  // Suspend
#define EHCI_PORT_PR            (1 << 8)  // Port Reset
#define EHCI_PORT_LS_SHIFT      10
#define EHCI_PORT_LS_MASK       (3 << 10) // Line Status
#define EHCI_PORT_PP            (1 << 12) // Port Power
#define EHCI_PORT_OWNER         (1 << 13) // Port Owner
#define EHCI_PORT_IC_SHIFT      14
#define EHCI_PORT_IC_MASK       (3 << 14) // Port Indicator Control
#define EHCI_PORT_TC_SHIFT      16
#define EHCI_PORT_TC_MASK       (0x0F << 16) // Port Test Control
#define EHCI_PORT_WKCNNT        (1 << 20) // Wake on Connect Enable
#define EHCI_PORT_WKDSCNNT      (1 << 21) // Wake on Disconnect Enable
#define EHCI_PORT_WKOC          (1 << 22) // Wake on Over-current Enable

// CONFIGFLAG Register Bits
#define EHCI_CONFIGFLAG_CF      (1 << 0)  // Configure Flag

// qTD (Queue Element Transfer Descriptor) Token Bits
#define EHCI_QTD_TOKEN_STATUS_ACTIVE    (1 << 7)
#define EHCI_QTD_TOKEN_STATUS_HALTED    (1 << 6)
#define EHCI_QTD_TOKEN_STATUS_DBERR     (1 << 5)
#define EHCI_QTD_TOKEN_STATUS_BABBLE    (1 << 4)
#define EHCI_QTD_TOKEN_STATUS_XACTERR   (1 << 3)
#define EHCI_QTD_TOKEN_STATUS_MISSED    (1 << 2)
#define EHCI_QTD_TOKEN_PID_OUT          (0 << 8)
#define EHCI_QTD_TOKEN_PID_IN           (1 << 8)
#define EHCI_QTD_TOKEN_PID_SETUP        (2 << 8)
#define EHCI_QTD_TOKEN_CERR_SHIFT       10
#define EHCI_QTD_TOKEN_CERR_MASK        (3 << 10)
#define EHCI_QTD_TOKEN_IOC              (1 << 15)

// QH (Queue Head) Characteristics Bits
#define EHCI_QH_CH_DEVADDR_MASK         0x7F
#define EHCI_QH_CH_INACT                (1 << 7)
#define EHCI_QH_CH_ENDPT_SHIFT          8
#define EHCI_QH_CH_ENDPT_MASK           (0x0F << 8)
#define EHCI_QH_CH_EPS_SHIFT            12
#define EHCI_QH_CH_EPS_MASK             (3 << 12)
#define EHCI_QH_CH_EPS_FULL             (0 << 12)
#define EHCI_QH_CH_EPS_LOW              (1 << 12)
#define EHCI_QH_CH_EPS_HIGH             (2 << 12)
#define EHCI_QH_CH_DTC                  (1 << 14)
#define EHCI_QH_CH_H                    (1 << 15)
#define EHCI_QH_CH_MAXPKT_SHIFT         16
#define EHCI_QH_CH_MAXPKT_MASK          (0x7FF << 16)
#define EHCI_QH_CH_C                    (1 << 27)
#define EHCI_QH_CH_RL_SHIFT             28
#define EHCI_QH_CH_RL_MASK              (0x0F << 28)

// Link Pointer Bits
#define EHCI_LP_TERMINATE               (1 << 0)
#define EHCI_LP_TYPE_SHIFT              1
#define EHCI_LP_TYPE_MASK               (3 << 1)
#define EHCI_LP_TYPE_ITD                (0 << 1)
#define EHCI_LP_TYPE_QH                 (1 << 1)
#define EHCI_LP_TYPE_SITD               (2 << 1)
#define EHCI_LP_TYPE_FSTN               (3 << 1)

// Frame List Size
#define EHCI_FRAMELIST_COUNT            1024

// qTD (Queue Element Transfer Descriptor)
typedef struct ehci_qtd {
    uint32_t next_qtd_ptr;
    uint32_t alt_next_qtd_ptr;
    uint32_t token;
    uint32_t buffer_ptr[5];
    
    // Software-only fields
    uint32_t reserved[3];
} __attribute__((aligned(32))) ehci_qtd_t;

// QH (Queue Head)
typedef struct ehci_qh {
    uint32_t qh_link_ptr;
    uint32_t characteristics;
    uint32_t capabilities;
    uint32_t current_qtd_ptr;
    
    // Overlay area (matches qTD structure)
    uint32_t next_qtd_ptr;
    uint32_t alt_next_qtd_ptr;
    uint32_t token;
    uint32_t buffer_ptr[5];
    
    // Software-only fields
    uint32_t reserved[4];
} __attribute__((aligned(32))) ehci_qh_t;

// EHCI Controller Data
typedef struct {
    pci_device_t *pci_dev;
    volatile uint8_t *mmio_base;
    volatile uint32_t *cap_regs;
    volatile uint32_t *op_regs;
    uint64_t mmio_phys;
    
    uint32_t *periodic_list;
    uint32_t periodic_list_phys;
    
    ehci_qh_t *async_qh;
    uint32_t async_qh_phys;
    
    ehci_qh_t *control_qh;
    ehci_qh_t *bulk_qh;
    ehci_qh_t *interrupt_qh;
    
    // ADD THIS LINE - Interrupt QH tree for periodic schedule
    ehci_qh_t *interrupt_qhs[8];
    
    ehci_qtd_t *qtd_pool;
    int qtd_pool_count;
    
    uint8_t num_ports;
    uint8_t next_address;
} ehci_controller_t;

// EHCI Functions
int ehci_init(void);
int ehci_probe(pci_device_t *pci_dev);
void ehci_shutdown(usb_controller_t *controller);

// Port operations
void ehci_reset_port(usb_controller_t *controller, uint8_t port);
int ehci_detect_device(usb_controller_t *controller, uint8_t port);

// Transfer functions
int ehci_control_transfer(usb_device_t *dev, usb_setup_packet_t *setup, void *data);
int ehci_interrupt_transfer(usb_device_t *dev, uint8_t endpoint, void *data, uint16_t len);
int ehci_bulk_transfer(usb_device_t *dev, uint8_t endpoint, void *data, uint16_t len);

// Helper functions
uint32_t ehci_read32(ehci_controller_t *ehci, uint32_t reg);
void ehci_write32(ehci_controller_t *ehci, uint32_t reg, uint32_t value);

#endif // EHCI_H
