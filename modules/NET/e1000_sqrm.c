//
// e1000_sqrm.c - Intel E1000 Network Card Driver
//
// Part of ModuOS kernel - SQRM network module
//

#include "moduos/kernel/sqrm.h"

/*
 * Intel E1000 NIC Driver
 *
 * Supports Intel 8254x family of Gigabit Ethernet controllers
 * Implements L2 networking (raw Ethernet frames)
 *
 * PCI Device IDs:
 *   0x100E - 82540EM (common in QEMU/VirtualBox)
 *   0x100F - 82545EM
 *   0x10EA - 82577LM
 *
 * Reference: Intel 8254x Family Gigabit Ethernet Controller
 *            Software Developer's Manual
 */

SQRM_DEFINE_MODULE(SQRM_TYPE_NET, "e1000");

// E1000 Register Offsets
#define E1000_REG_CTRL      0x0000  // Device Control
#define E1000_REG_STATUS    0x0008  // Device Status
#define E1000_REG_EECD      0x0010  // EEPROM Control
#define E1000_REG_EERD      0x0014  // EEPROM Read
#define E1000_REG_CTRL_EXT  0x0018  // Extended Device Control
#define E1000_REG_ICR       0x00C0  // Interrupt Cause Read
#define E1000_REG_IMS       0x00D0  // Interrupt Mask Set
#define E1000_REG_IMC       0x00D8  // Interrupt Mask Clear
#define E1000_REG_RCTL      0x0100  // Receive Control
#define E1000_REG_TCTL      0x0400  // Transmit Control
#define E1000_REG_RDBAL     0x2800  // RX Descriptor Base Low
#define E1000_REG_RDBAH     0x2804  // RX Descriptor Base High
#define E1000_REG_RDLEN     0x2808  // RX Descriptor Length
#define E1000_REG_RDH       0x2810  // RX Descriptor Head
#define E1000_REG_RDT       0x2818  // RX Descriptor Tail
#define E1000_REG_TDBAL     0x3800  // TX Descriptor Base Low
#define E1000_REG_TDBAH     0x3804  // TX Descriptor Base High
#define E1000_REG_TDLEN     0x3808  // TX Descriptor Length
#define E1000_REG_TDH       0x3810  // TX Descriptor Head
#define E1000_REG_TDT       0x3818  // TX Descriptor Tail
#define E1000_REG_MTA       0x5200  // Multicast Table Array
#define E1000_REG_RAL       0x5400  // Receive Address Low
#define E1000_REG_RAH       0x5404  // Receive Address High

// Control Register Bits
#define E1000_CTRL_SLU      (1 << 6)   // Set Link Up
#define E1000_CTRL_RST      (1 << 26)  // Device Reset
#define E1000_CTRL_ASDE     (1 << 5)   // Auto-Speed Detection Enable
#define E1000_CTRL_PHY_RST  (1 << 31)  // PHY Reset

// Receive Control Bits
#define E1000_RCTL_EN       (1 << 1)   // Receiver Enable
#define E1000_RCTL_SBP      (1 << 2)   // Store Bad Packets
#define E1000_RCTL_UPE      (1 << 3)   // Unicast Promiscuous Enable
#define E1000_RCTL_MPE      (1 << 4)   // Multicast Promiscuous Enable
#define E1000_RCTL_BAM      (1 << 15)  // Broadcast Accept Mode
#define E1000_RCTL_BSIZE_8K (3 << 16)  // Buffer Size 8K
#define E1000_RCTL_SECRC    (1 << 26)  // Strip Ethernet CRC

// Transmit Control Bits
#define E1000_TCTL_EN       (1 << 1)   // Transmit Enable
#define E1000_TCTL_PSP      (1 << 3)   // Pad Short Packets

// Descriptor Status Bits
#define E1000_DESC_STATUS_DD    (1 << 0)  // Descriptor Done
#define E1000_DESC_STATUS_EOP   (1 << 1)  // End of Packet
#define E1000_DESC_CMD_EOP      (1 << 0)  // End of Packet
#define E1000_DESC_CMD_RS       (1 << 3)  // Report Status

// Configuration
#define E1000_NUM_RX_DESC   32
#define E1000_NUM_TX_DESC   32
#define E1000_BUFFER_SIZE   8192

// PCI Vendor/Device IDs
#define E1000_VENDOR_ID     0x8086
#define E1000_DEVICE_82540EM 0x100E
#define E1000_DEVICE_82545EM 0x100F

// RX/TX Descriptor
typedef struct {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed)) e1000_desc_t;

// Driver State
typedef struct {
    volatile uint8_t *mmio_base;
    uint8_t mac_addr[6];
    
    e1000_desc_t *rx_descs;
    uint8_t *rx_buffers[E1000_NUM_RX_DESC];
    uint32_t rx_tail;
    
    e1000_desc_t *tx_descs;
    uint8_t *tx_buffers[E1000_NUM_TX_DESC];
    uint32_t tx_tail;
    
    int link_up;
} e1000_state_t;

static e1000_state_t g_e1000;
static const sqrm_kernel_api_t *g_api;
static const uint16_t COM1_PORT = 0x3F8;

// MMIO Register Access
static inline void e1000_write_reg(uint32_t reg, uint32_t value) {
    *((volatile uint32_t *)(g_e1000.mmio_base + reg)) = value;
}

static inline uint32_t e1000_read_reg(uint32_t reg) {
    return *((volatile uint32_t *)(g_e1000.mmio_base + reg));
}

/**
 * Read MAC address from EEPROM
 * 
 * The MAC address is stored in the first 3 words of the EEPROM.
 * 
 * @param mac_out Output buffer for 6-byte MAC address
 * @return 0 on success, negative on error
 */
static int e1000_read_mac_addr(uint8_t mac_out[6]) {
    if (!mac_out) {
        return -22; // -EINVAL
    }
    
    // Try reading from EEPROM
    for (int i = 0; i < 3; i++) {
        uint32_t eerd = e1000_read_reg(E1000_REG_EERD);
        eerd = (i << 8) | 1;  // Address + Start bit
        e1000_write_reg(E1000_REG_EERD, eerd);
        
        // Wait for read to complete (timeout after 1000 iterations)
        int timeout = 1000;
        while (timeout-- > 0) {
            eerd = e1000_read_reg(E1000_REG_EERD);
            if (eerd & (1 << 4)) {  // Done bit
                uint16_t data = (eerd >> 16) & 0xFFFF;
                mac_out[i * 2] = data & 0xFF;
                mac_out[i * 2 + 1] = (data >> 8) & 0xFF;
                break;
            }
        }
        
        if (timeout <= 0) {
            // Fallback: read from hardware registers
            if (i == 0) {
                uint32_t ral = e1000_read_reg(E1000_REG_RAL);
                uint32_t rah = e1000_read_reg(E1000_REG_RAH);
                mac_out[0] = ral & 0xFF;
                mac_out[1] = (ral >> 8) & 0xFF;
                mac_out[2] = (ral >> 16) & 0xFF;
                mac_out[3] = (ral >> 24) & 0xFF;
                mac_out[4] = rah & 0xFF;
                mac_out[5] = (rah >> 8) & 0xFF;
                return 0;
            }
        }
    }
    
    return 0;
}

/**
 * Initialize receive descriptors and buffers
 * 
 * Allocates DMA buffers and sets up the RX descriptor ring.
 * 
 * @return 0 on success, negative on error
 */
static int e1000_init_rx(void) {
    // Allocate RX descriptor ring
    g_e1000.rx_descs = (e1000_desc_t *)g_api->kmalloc(
        sizeof(e1000_desc_t) * E1000_NUM_RX_DESC);
    if (!g_e1000.rx_descs) {
        return -4; // -ENOMEM
    }
    
    // Zero-initialize descriptors
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        g_e1000.rx_descs[i].addr = 0;
        g_e1000.rx_descs[i].status = 0;
    }
    
    // Allocate RX buffers
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        g_e1000.rx_buffers[i] = (uint8_t *)g_api->kmalloc(E1000_BUFFER_SIZE);
        if (!g_e1000.rx_buffers[i]) {
            return -4; // -ENOMEM
        }
        
        // Get physical address and set in descriptor
        uint64_t phys = g_api->virt_to_phys((uint64_t)g_e1000.rx_buffers[i]);
        g_e1000.rx_descs[i].addr = phys;
    }
    
    // Set RX descriptor base and length
    uint64_t rx_phys = g_api->virt_to_phys((uint64_t)g_e1000.rx_descs);
    e1000_write_reg(E1000_REG_RDBAL, rx_phys & 0xFFFFFFFF);
    e1000_write_reg(E1000_REG_RDBAH, (rx_phys >> 32) & 0xFFFFFFFF);
    e1000_write_reg(E1000_REG_RDLEN, E1000_NUM_RX_DESC * sizeof(e1000_desc_t));
    
    // Initialize head and tail
    e1000_write_reg(E1000_REG_RDH, 0);
    e1000_write_reg(E1000_REG_RDT, E1000_NUM_RX_DESC - 1);
    g_e1000.rx_tail = 0;
    
    // Enable receiver
    uint32_t rctl = E1000_RCTL_EN | E1000_RCTL_BAM | 
                    E1000_RCTL_BSIZE_8K | E1000_RCTL_SECRC;
    e1000_write_reg(E1000_REG_RCTL, rctl);
    
    return 0;
}

/**
 * Initialize transmit descriptors and buffers
 * 
 * Allocates DMA buffers and sets up the TX descriptor ring.
 * 
 * @return 0 on success, negative on error
 */
static int e1000_init_tx(void) {
    // Allocate TX descriptor ring
    g_e1000.tx_descs = (e1000_desc_t *)g_api->kmalloc(
        sizeof(e1000_desc_t) * E1000_NUM_TX_DESC);
    if (!g_e1000.tx_descs) {
        return -4; // -ENOMEM
    }
    
    // Zero-initialize descriptors
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        g_e1000.tx_descs[i].addr = 0;
        g_e1000.tx_descs[i].status = E1000_DESC_STATUS_DD; // Mark as done
    }
    
    // Allocate TX buffers
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        g_e1000.tx_buffers[i] = (uint8_t *)g_api->kmalloc(E1000_BUFFER_SIZE);
        if (!g_e1000.tx_buffers[i]) {
            return -4; // -ENOMEM
        }
    }
    
    // Set TX descriptor base and length
    uint64_t tx_phys = g_api->virt_to_phys((uint64_t)g_e1000.tx_descs);
    e1000_write_reg(E1000_REG_TDBAL, tx_phys & 0xFFFFFFFF);
    e1000_write_reg(E1000_REG_TDBAH, (tx_phys >> 32) & 0xFFFFFFFF);
    e1000_write_reg(E1000_REG_TDLEN, E1000_NUM_TX_DESC * sizeof(e1000_desc_t));
    
    // Initialize head and tail
    e1000_write_reg(E1000_REG_TDH, 0);
    e1000_write_reg(E1000_REG_TDT, 0);
    g_e1000.tx_tail = 0;
    
    // Enable transmitter with padding
    uint32_t tctl = E1000_TCTL_EN | E1000_TCTL_PSP | (0x0F << 4) | (0x40 << 12);
    e1000_write_reg(E1000_REG_TCTL, tctl);
    
    return 0;
}

/**
 * Detect and initialize E1000 hardware
 * 
 * Scans PCI bus for E1000 devices and initializes the first one found.
 * 
 * @return 0 on success, negative on error
 */
static int e1000_detect_and_init(void) {
    if (!g_api->pci_find_device) {
        return -38; // -ENOSYS
    }
    
    // Search for E1000 device
    uint32_t bus = 0, slot = 0, func = 0;
    int found = 0;
    
    for (bus = 0; bus < 256 && !found; bus++) {
        for (slot = 0; slot < 32 && !found; slot++) {
            for (func = 0; func < 8; func++) {
                uint32_t vendor_device = g_api->pci_cfg_read32(
                    bus, slot, func, 0);
                uint16_t vendor = vendor_device & 0xFFFF;
                uint16_t device = (vendor_device >> 16) & 0xFFFF;
                
                if (vendor == E1000_VENDOR_ID && 
                    (device == E1000_DEVICE_82540EM || 
                     device == E1000_DEVICE_82545EM)) {
                    found = 1;
                    break;
                }
            }
        }
    }
    
    if (!found) {
        return -1; // Device not found
    }
    
    bus--; slot--; // Adjust for loop increment
    
    if (g_api->com_write_string) {
        g_api->com_write_string(COM1_PORT, "[e1000] Found Intel E1000 NIC\n");
    }
    
    // Read BAR0 (MMIO address)
    uint32_t bar0 = g_api->pci_cfg_read32(bus, slot, func, 0x10);
    uint64_t mmio_addr = bar0 & 0xFFFFFFF0;
    
    // Enable bus mastering and memory space
    uint32_t cmd = g_api->pci_cfg_read32(bus, slot, func, 0x04);
    cmd |= 0x6; // Bus Master + Memory Space
    g_api->pci_cfg_write32(bus, slot, func, 0x04, cmd);
    
    // Map MMIO region
    g_e1000.mmio_base = (volatile uint8_t *)g_api->ioremap(mmio_addr, 0x20000);
    if (!g_e1000.mmio_base) {
        return -4; // -ENOMEM
    }
    
    // Reset device
    e1000_write_reg(E1000_REG_CTRL, E1000_CTRL_RST);
    
    // Wait for reset to complete (simple delay)
    for (volatile int i = 0; i < 10000; i++) {
        // Busy wait
    }
    
    // Disable interrupts
    e1000_write_reg(E1000_REG_IMC, 0xFFFFFFFF);
    e1000_read_reg(E1000_REG_ICR); // Clear pending interrupts
    
    // Read MAC address
    e1000_read_mac_addr(g_e1000.mac_addr);
    
    // Initialize RX and TX
    int ret = e1000_init_rx();
    if (ret < 0) {
        return ret;
    }
    
    ret = e1000_init_tx();
    if (ret < 0) {
        return ret;
    }
    
    // Set link up
    uint32_t ctrl = e1000_read_reg(E1000_REG_CTRL);
    ctrl |= E1000_CTRL_SLU | E1000_CTRL_ASDE;
    e1000_write_reg(E1000_REG_CTRL, ctrl);
    
    // Check link status
    uint32_t status = e1000_read_reg(E1000_REG_STATUS);
    g_e1000.link_up = (status & (1 << 1)) ? 1 : 0;
    
    return 0;
}

// Network API Implementation
static int net_get_link_up(void) {
    uint32_t status = e1000_read_reg(E1000_REG_STATUS);
    return (status & (1 << 1)) ? 1 : 0;
}

static int net_get_mtu(uint32_t *out) {
    if (!out) {
        return -22; // -EINVAL
    }
    *out = 1500;
    return 0;
}

static int net_get_mac(uint8_t out_mac[6]) {
    if (!out_mac) {
        return -22; // -EINVAL
    }
    
    for (int i = 0; i < 6; i++) {
        out_mac[i] = g_e1000.mac_addr[i];
    }
    
    return 0;
}

static int net_tx_frame(const void *frame, size_t len) {
    if (!frame || len == 0 || len > E1000_BUFFER_SIZE) {
        return -22; // -EINVAL
    }
    
    uint32_t tail = g_e1000.tx_tail;
    e1000_desc_t *desc = &g_e1000.tx_descs[tail];
    
    // Wait if descriptor is not available
    if (!(desc->status & E1000_DESC_STATUS_DD)) {
        return -11; // -EAGAIN
    }
    
    // Copy frame to TX buffer
    for (size_t i = 0; i < len; i++) {
        g_e1000.tx_buffers[tail][i] = ((const uint8_t *)frame)[i];
    }
    
    // Set up descriptor
    uint64_t phys = g_api->virt_to_phys((uint64_t)g_e1000.tx_buffers[tail]);
    desc->addr = phys;
    desc->length = len;
    desc->status = 0;
    desc->checksum = 0;
    desc->special = 0;
    
    // Set command bits
    uint8_t cmd = E1000_DESC_CMD_EOP | E1000_DESC_CMD_RS;
    *((uint8_t *)desc + 11) = cmd;
    
    // Update tail pointer
    tail = (tail + 1) % E1000_NUM_TX_DESC;
    g_e1000.tx_tail = tail;
    e1000_write_reg(E1000_REG_TDT, tail);
    
    return 0;
}

static int net_rx_poll(void *out_frame, size_t out_cap, size_t *out_len) {
    if (!out_frame || !out_len) {
        return -22; // -EINVAL
    }
    
    uint32_t tail = g_e1000.rx_tail;
    e1000_desc_t *desc = &g_e1000.rx_descs[tail];
    
    // Check if descriptor has data
    if (!(desc->status & E1000_DESC_STATUS_DD)) {
        *out_len = 0;
        return 0; // No frame available
    }
    
    // Get frame length
    uint16_t len = desc->length;
    if (len > out_cap) {
        *out_len = 0;
        return -28; // -ENOSPC
    }
    
    // Copy frame data
    for (uint16_t i = 0; i < len; i++) {
        ((uint8_t *)out_frame)[i] = g_e1000.rx_buffers[tail][i];
    }
    
    *out_len = len;
    return 0;
}

static int net_rx_consume(void) {
    uint32_t tail = g_e1000.rx_tail;
    e1000_desc_t *desc = &g_e1000.rx_descs[tail];
    
    // Clear descriptor status
    desc->status = 0;
    
    // Update tail pointer
    tail = (tail + 1) % E1000_NUM_RX_DESC;
    g_e1000.rx_tail = tail;
    e1000_write_reg(E1000_REG_RDT, (tail - 1) % E1000_NUM_RX_DESC);
    
    return 0;
}

static const sqrm_net_api_v1_t g_net_api = {
    .get_link_up = net_get_link_up,
    .get_mtu = net_get_mtu,
    .get_mac = net_get_mac,
    .tx_frame = net_tx_frame,
    .rx_poll = net_rx_poll,
    .rx_consume = net_rx_consume,
};

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    if (!api || api->abi_version != SQRM_ABI_VERSION) {
        return -1;
    }
    
    g_api = api;
    
    if (g_api->com_write_string) {
        g_api->com_write_string(COM1_PORT, "[e1000] Intel E1000 driver initializing\n");
    }
    
    // Detect and initialize hardware
    int ret = e1000_detect_and_init();
    if (ret < 0) {
        if (g_api->com_write_string) {
            g_api->com_write_string(COM1_PORT, "[e1000] No E1000 device found\n");
        }
        return -1; // Allow autoload to continue
    }
    
    // Register network service
    if (g_api->sqrm_service_register) {
        ret = g_api->sqrm_service_register("net", &g_net_api, sizeof(g_net_api));
        if (ret < 0) {
            if (g_api->com_write_string) {
                g_api->com_write_string(COM1_PORT, "[e1000] Failed to register service\n");
            }
            return -1;
        }
    }
    
    if (g_api->com_write_string) {
        g_api->com_write_string(COM1_PORT, "[e1000] Driver initialized successfully\n");
        g_api->com_write_string(COM1_PORT, "[e1000] MAC: ");
        
        // Print MAC address
        char hex[] = "0123456789ABCDEF";
        for (int i = 0; i < 6; i++) {
            char buf[4] = {0};
            buf[0] = hex[(g_e1000.mac_addr[i] >> 4) & 0xF];
            buf[1] = hex[g_e1000.mac_addr[i] & 0xF];
            buf[2] = (i < 5) ? ':' : '\n';
            g_api->com_write_string(COM1_PORT, buf);
        }
    }
    
    return 0; // Success - claim network slot
}