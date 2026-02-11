//
// i82540em_sqrm.c - Intel 82540EM Gigabit Ethernet Driver
//
// Part of ModuOS kernel modules
// Supports Intel 82540EM/82545EM (e1000 family, older variant)
//

#include "moduos/kernel/sqrm.h"

SQRM_DEFINE_MODULE(SQRM_TYPE_NET, "i82540em");

// PCI IDs
#define PCI_VENDOR_INTEL        0x8086
#define PCI_DEVICE_82540EM      0x100E
#define PCI_DEVICE_82545EM      0x100F

// Register offsets
#define E1000_CTRL      0x00000  // Device Control
#define E1000_STATUS    0x00008  // Device Status
#define E1000_EECD      0x00010  // EEPROM Control
#define E1000_EERD      0x00014  // EEPROM Read
#define E1000_CTRL_EXT  0x00018  // Extended Device Control
#define E1000_MDIC      0x00020  // MDI Control
#define E1000_FCAL      0x00028  // Flow Control Address Low
#define E1000_FCAH      0x0002C  // Flow Control Address High
#define E1000_FCT       0x00030  // Flow Control Type
#define E1000_VET       0x00038  // VLAN Ether Type
#define E1000_ICR       0x000C0  // Interrupt Cause Read
#define E1000_ICS       0x000C8  // Interrupt Cause Set
#define E1000_IMS       0x000D0  // Interrupt Mask Set
#define E1000_IMC       0x000D8  // Interrupt Mask Clear
#define E1000_RCTL      0x00100  // Receive Control
#define E1000_TCTL      0x00400  // Transmit Control
#define E1000_TIPG      0x00410  // TX Inter-packet gap
#define E1000_RDBAL     0x02800  // RX Descriptor Base Low
#define E1000_RDBAH     0x02804  // RX Descriptor Base High
#define E1000_RDLEN     0x02808  // RX Descriptor Length
#define E1000_RDH       0x02810  // RX Descriptor Head
#define E1000_RDT       0x02818  // RX Descriptor Tail
#define E1000_TDBAL     0x03800  // TX Descriptor Base Low
#define E1000_TDBAH     0x03804  // TX Descriptor Base High
#define E1000_TDLEN     0x03808  // TX Descriptor Length
#define E1000_TDH       0x03810  // TX Descriptor Head
#define E1000_TDT       0x03818  // TX Descriptor Tail
#define E1000_MTA       0x05200  // Multicast Table Array
#define E1000_RAL       0x05400  // Receive Address Low
#define E1000_RAH       0x05404  // Receive Address High

// Control bits
#define E1000_CTRL_FD       0x00000001  // Full duplex
#define E1000_CTRL_LRST     0x00000008  // Link reset
#define E1000_CTRL_ASDE     0x00000020  // Auto-speed detection
#define E1000_CTRL_SLU      0x00000040  // Set link up
#define E1000_CTRL_ILOS     0x00000080  // Invert loss of signal
#define E1000_CTRL_RST      0x04000000  // Device reset
#define E1000_CTRL_VME      0x40000000  // VLAN mode enable
#define E1000_CTRL_PHY_RST  0x80000000  // PHY reset

// Status bits
#define E1000_STATUS_FD     0x00000001  // Full duplex
#define E1000_STATUS_LU     0x00000002  // Link up
#define E1000_STATUS_SPEED  0x000000C0  // Speed mask

// RCTL bits
#define E1000_RCTL_EN       0x00000002  // Receive enable
#define E1000_RCTL_SBP      0x00000004  // Store bad packets
#define E1000_RCTL_UPE      0x00000008  // Unicast promiscuous
#define E1000_RCTL_MPE      0x00000010  // Multicast promiscuous
#define E1000_RCTL_BAM      0x00008000  // Broadcast accept mode
#define E1000_RCTL_BSIZE    0x00030000  // Buffer size
#define E1000_RCTL_SECRC    0x04000000  // Strip CRC

// TCTL bits
#define E1000_TCTL_EN       0x00000002  // Transmit enable
#define E1000_TCTL_PSP      0x00000008  // Pad short packets
#define E1000_TCTL_CT       0x00000FF0  // Collision threshold
#define E1000_TCTL_COLD     0x003FF000  // Collision distance

// Interrupt bits
#define E1000_ICR_TXDW      0x00000001  // TX descriptor written back
#define E1000_ICR_TXQE      0x00000002  // TX queue empty
#define E1000_ICR_LSC       0x00000004  // Link status change
#define E1000_ICR_RXSEQ     0x00000008  // RX sequence error
#define E1000_ICR_RXDMT0    0x00000010  // RX descriptor min threshold
#define E1000_ICR_RXO       0x00000040  // RX overrun
#define E1000_ICR_RXT0      0x00000080  // RX timer interrupt

// Descriptor definitions
#define E1000_RXD_STAT_DD   0x01  // Descriptor done
#define E1000_RXD_STAT_EOP  0x02  // End of packet
#define E1000_TXD_STAT_DD   0x01  // Descriptor done
#define E1000_TXD_CMD_EOP   0x01  // End of packet
#define E1000_TXD_CMD_IFCS  0x02  // Insert FCS
#define E1000_TXD_CMD_RS    0x08  // Report status

#define RX_RING_SIZE        64
#define TX_RING_SIZE        64
#define BUFFER_SIZE         2048

static const uint16_t COM1_PORT = 0x3F8;

// Descriptor structures
typedef struct {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed)) e1000_rx_desc_t;

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

typedef struct {
    volatile uint8_t *mmio_base;
    uint8_t irq;
    uint8_t mac[6];
    
    e1000_rx_desc_t *rx_ring;
    e1000_tx_desc_t *tx_ring;
    void **rx_buffers;
    void **tx_buffers;
    
    uint16_t rx_idx;
    uint16_t tx_idx;
    
    uint32_t rx_packets;
    uint32_t tx_packets;
    
    int link_up;
    const sqrm_kernel_api_t *api;
} e1000_device_t;

static e1000_device_t *g_e1000_dev = NULL;

// MMIO access helpers
static inline uint32_t e1000_read32(e1000_device_t *dev, uint32_t reg) {
    volatile uint32_t *addr = (volatile uint32_t *)((uintptr_t)dev->mmio_base + (uintptr_t)reg);
    return *addr;
}

static inline void e1000_write32(e1000_device_t *dev, uint32_t reg, uint32_t val) {
    volatile uint32_t *addr = (volatile uint32_t *)((uintptr_t)dev->mmio_base + (uintptr_t)reg);
    *addr = val;
}

static uint16_t e1000_read_eeprom(e1000_device_t *dev, uint8_t addr) {
    uint32_t val = (1 << 0) | ((uint32_t)addr << 8);
    e1000_write32(dev, E1000_EERD, val);
    
    // Wait for read to complete
    int timeout = 1000;
    while (timeout-- > 0) {
        val = e1000_read32(dev, E1000_EERD);
        if (val & (1 << 4)) break;  // DONE bit
        if (dev->api->sleep_ms) dev->api->sleep_ms(1);
    }
    
    return (val >> 16) & 0xFFFF;
}

static void e1000_irq_handler(void) {
    if (!g_e1000_dev) return;
    
    e1000_device_t *dev = g_e1000_dev;
    
    // Read and clear interrupts
    uint32_t icr = e1000_read32(dev, E1000_ICR);
    
    if (icr & E1000_ICR_RXT0) {
        dev->rx_packets++;
    }
    
    if (icr & E1000_ICR_TXDW) {
        dev->tx_packets++;
    }
    
    if (icr & E1000_ICR_LSC) {
        uint32_t status = e1000_read32(dev, E1000_STATUS);
        dev->link_up = !!(status & E1000_STATUS_LU);
    }
    
    if (dev->api->pic_send_eoi) {
        dev->api->pic_send_eoi(dev->irq);
    }
}

static int e1000_reset(e1000_device_t *dev) {
    // Reset device
    e1000_write32(dev, E1000_CTRL, E1000_CTRL_RST);
    
    if (dev->api->sleep_ms) {
        dev->api->sleep_ms(10);
    }
    
    // Wait for reset to complete
    int timeout = 1000;
    while (timeout-- > 0) {
        if (!(e1000_read32(dev, E1000_CTRL) & E1000_CTRL_RST)) break;
        if (dev->api->sleep_ms) dev->api->sleep_ms(1);
    }
    
    if (timeout <= 0) return -1;
    
    // Disable interrupts
    e1000_write32(dev, E1000_IMC, 0xFFFFFFFF);
    e1000_read32(dev, E1000_ICR);
    
    return 0;
}

static int e1000_init_device(e1000_device_t *dev) {
    const sqrm_kernel_api_t *api = dev->api;
    
    // Reset chip
    if (e1000_reset(dev) != 0) {
        return -1;
    }
    
    // Read MAC from EEPROM
    uint16_t mac_word;
    mac_word = e1000_read_eeprom(dev, 0);
    dev->mac[0] = mac_word & 0xFF;
    dev->mac[1] = (mac_word >> 8) & 0xFF;
    mac_word = e1000_read_eeprom(dev, 1);
    dev->mac[2] = mac_word & 0xFF;
    dev->mac[3] = (mac_word >> 8) & 0xFF;
    mac_word = e1000_read_eeprom(dev, 2);
    dev->mac[4] = mac_word & 0xFF;
    dev->mac[5] = (mac_word >> 8) & 0xFF;
    
    // Allocate descriptor rings
    dev->rx_ring = api->kmalloc(sizeof(e1000_rx_desc_t) * RX_RING_SIZE);
    dev->tx_ring = api->kmalloc(sizeof(e1000_tx_desc_t) * TX_RING_SIZE);
    dev->rx_buffers = api->kmalloc(sizeof(void*) * RX_RING_SIZE);
    dev->tx_buffers = api->kmalloc(sizeof(void*) * TX_RING_SIZE);
    
    if (!dev->rx_ring || !dev->tx_ring || !dev->rx_buffers || !dev->tx_buffers) {
        return -1;
    }
    
    // Allocate packet buffers
    for (int i = 0; i < RX_RING_SIZE; i++) {
        dev->rx_buffers[i] = api->kmalloc(BUFFER_SIZE);
        if (!dev->rx_buffers[i]) return -1;
    }
    
    for (int i = 0; i < TX_RING_SIZE; i++) {
        dev->tx_buffers[i] = api->kmalloc(BUFFER_SIZE);
        if (!dev->tx_buffers[i]) return -1;
    }
    
    // Setup RX descriptors
    for (int i = 0; i < RX_RING_SIZE; i++) {
        dev->rx_ring[i].addr = api->virt_to_phys((uint64_t)(uintptr_t)dev->rx_buffers[i]);
        dev->rx_ring[i].status = 0;
    }
    
    // Setup TX descriptors
    for (int i = 0; i < TX_RING_SIZE; i++) {
        dev->tx_ring[i].addr = api->virt_to_phys((uint64_t)(uintptr_t)dev->tx_buffers[i]);
        dev->tx_ring[i].cmd = 0;
        dev->tx_ring[i].status = E1000_TXD_STAT_DD;
    }
    
    dev->rx_idx = 0;
    dev->tx_idx = 0;
    
    // Clear multicast table
    for (int i = 0; i < 128; i++) {
        e1000_write32(dev, E1000_MTA + (i * 4), 0);
    }
    
    // Set MAC address
    uint32_t ral = dev->mac[0] | (dev->mac[1] << 8) | 
                   (dev->mac[2] << 16) | (dev->mac[3] << 24);
    uint32_t rah = dev->mac[4] | (dev->mac[5] << 8) | (1 << 31);  // Valid bit
    e1000_write32(dev, E1000_RAL, ral);
    e1000_write32(dev, E1000_RAH, rah);
    
    // Setup RX ring
    uint64_t rx_phys = api->virt_to_phys((uint64_t)(uintptr_t)dev->rx_ring);
    e1000_write32(dev, E1000_RDBAL, (uint32_t)(rx_phys & 0xFFFFFFFFu));
    e1000_write32(dev, E1000_RDBAH, (uint32_t)((rx_phys >> 32) & 0xFFFFFFFFu));
    e1000_write32(dev, E1000_RDLEN, RX_RING_SIZE * sizeof(e1000_rx_desc_t));
    e1000_write32(dev, E1000_RDH, 0);
    e1000_write32(dev, E1000_RDT, RX_RING_SIZE - 1);
    
    // Setup TX ring
    uint64_t tx_phys = api->virt_to_phys((uint64_t)(uintptr_t)dev->tx_ring);
    e1000_write32(dev, E1000_TDBAL, (uint32_t)(tx_phys & 0xFFFFFFFFu));
    e1000_write32(dev, E1000_TDBAH, (uint32_t)((tx_phys >> 32) & 0xFFFFFFFFu));
    e1000_write32(dev, E1000_TDLEN, TX_RING_SIZE * sizeof(e1000_tx_desc_t));
    e1000_write32(dev, E1000_TDH, 0);
    e1000_write32(dev, E1000_TDT, 0);
    
    // Enable RX
    e1000_write32(dev, E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | 
                                   E1000_RCTL_SECRC | (2 << 16));  // 2KB buffers
    
    // Enable TX
    e1000_write32(dev, E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP |
                                   (0x0F << 4) |   // CT
                                   (0x40 << 12));  // COLD
    
    e1000_write32(dev, E1000_TIPG, 0x00702008);
    
    // Enable interrupts
    e1000_write32(dev, E1000_IMS, E1000_ICR_RXT0 | E1000_ICR_TXDW | E1000_ICR_LSC);
    
    // Set link up
    uint32_t ctrl = e1000_read32(dev, E1000_CTRL);
    e1000_write32(dev, E1000_CTRL, ctrl | E1000_CTRL_SLU);
    
    // Check link status
    uint32_t status = e1000_read32(dev, E1000_STATUS);
    dev->link_up = !!(status & E1000_STATUS_LU);
    
    return 0;
}

// Network API implementation
static int net_get_link_up(void) {
    if (!g_e1000_dev) return 0;
    
    uint32_t status = e1000_read32(g_e1000_dev, E1000_STATUS);
    g_e1000_dev->link_up = !!(status & E1000_STATUS_LU);
    
    return g_e1000_dev->link_up;
}

static int net_get_mtu(uint32_t *out) {
    if (!out) return -22;
    *out = 1500;
    return 0;
}

static int net_get_mac(uint8_t out_mac[6]) {
    if (!out_mac || !g_e1000_dev) return -22;
    for (int i = 0; i < 6; i++) {
        out_mac[i] = g_e1000_dev->mac[i];
    }
    return 0;
}

static int net_tx_frame(const void *frame, size_t len) {
    if (!g_e1000_dev || !frame || len == 0 || len > BUFFER_SIZE) {
        return -22;
    }
    
    e1000_device_t *dev = g_e1000_dev;
    uint16_t idx = dev->tx_idx;
    e1000_tx_desc_t *desc = &dev->tx_ring[idx];
    
    // Check if descriptor available
    if (!(desc->status & E1000_TXD_STAT_DD)) {
        return -11;
    }
    
    // Copy frame
    uint8_t *dst = (uint8_t*)dev->tx_buffers[idx];
    const uint8_t *src = (const uint8_t*)frame;
    for (size_t i = 0; i < len; i++) {
        dst[i] = src[i];
    }
    
    // Setup descriptor
    desc->length = len;
    desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    desc->status = 0;
    
    // Advance index
    dev->tx_idx = (dev->tx_idx + 1) % TX_RING_SIZE;
    
    // Update tail
    e1000_write32(dev, E1000_TDT, dev->tx_idx);
    
    return 0;
}

static int net_rx_poll(void *out_frame, size_t out_cap, size_t *out_len) {
    if (!g_e1000_dev || !out_frame || !out_len) return -22;
    
    e1000_device_t *dev = g_e1000_dev;
    uint16_t idx = dev->rx_idx;
    e1000_rx_desc_t *desc = &dev->rx_ring[idx];
    
    // Check if packet available
    if (!(desc->status & E1000_RXD_STAT_DD)) {
        *out_len = 0;
        return 0;
    }
    
    // Get length
    uint16_t pkt_len = desc->length;
    
    if (pkt_len > out_cap) {
        *out_len = pkt_len;
        return -90;
    }
    
    // Copy packet
    uint8_t *src = (uint8_t*)dev->rx_buffers[idx];
    uint8_t *dst = (uint8_t*)out_frame;
    for (size_t i = 0; i < pkt_len; i++) {
        dst[i] = src[i];
    }
    
    *out_len = pkt_len;
    return 1;
}

static int net_rx_consume(void) {
    if (!g_e1000_dev) return -22;
    
    e1000_device_t *dev = g_e1000_dev;
    e1000_rx_desc_t *desc = &dev->rx_ring[dev->rx_idx];
    
    // Reset descriptor
    desc->status = 0;
    
    // Advance index
    dev->rx_idx = (dev->rx_idx + 1) % RX_RING_SIZE;
    
    // Update tail
    e1000_write32(dev, E1000_RDT, (dev->rx_idx - 1 + RX_RING_SIZE) % RX_RING_SIZE);
    
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
    if (!api || api->abi_version != SQRM_ABI_VERSION) return -1;
    
    if (api->com_write_string) {
        api->com_write_string(COM1_PORT, "[i82540em] Intel 82540EM driver initializing\n");
    }
    
    // Check capabilities
    if (!api->kmalloc || !api->kfree || !api->virt_to_phys) {
        if (api->com_write_string) {
            api->com_write_string(COM1_PORT, "[i82540em] Missing required capabilities\n");
        }
        return -1;
    }
    
    // Allocate device
    e1000_device_t *dev = api->kmalloc(sizeof(e1000_device_t));
    if (!dev) return -1;
    
    dev->api = api;
    // Discover device via PCI and map BAR0
    if (!api->pci_cfg_read32 || !api->pci_cfg_write32 || !api->ioremap) {
        if (api->com_write_string) api->com_write_string(COM1_PORT, "[i82540em] Missing PCI/ioremap capabilities\n");
        api->kfree(dev);
        return -1;
    }

    uint8_t found_bus = 0, found_slot = 0, found_func = 0;
    int found = 0;
    for (uint16_t bus = 0; bus < 256 && !found; bus++) {
        for (uint8_t slot = 0; slot < 32 && !found; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vd = api->pci_cfg_read32((uint8_t)bus, slot, func, 0);
                uint16_t vendor = (uint16_t)(vd & 0xFFFFu);
                uint16_t device = (uint16_t)((vd >> 16) & 0xFFFFu);
                if (vendor == PCI_VENDOR_INTEL && (device == PCI_DEVICE_82540EM || device == PCI_DEVICE_82545EM)) {
                    found_bus = (uint8_t)bus;
                    found_slot = slot;
                    found_func = func;
                    found = 1;
                    break;
                }
            }
        }
    }

    if (!found) {
        if (api->com_write_string) api->com_write_string(COM1_PORT, "[i82540em] No matching device found\n");
        api->kfree(dev);
        return -1;
    }

    uint32_t bar0 = api->pci_cfg_read32(found_bus, found_slot, found_func, 0x10);
    uint64_t mmio_phys = (uint64_t)(bar0 & 0xFFFFFFF0u);

    uint32_t cmd = api->pci_cfg_read32(found_bus, found_slot, found_func, 0x04);
    cmd |= 0x6u; // MEM + BUS MASTER
    api->pci_cfg_write32(found_bus, found_slot, found_func, 0x04, cmd);

    dev->mmio_base = (volatile uint8_t*)api->ioremap(mmio_phys, 0x20000);
    if (!dev->mmio_base) {
        if (api->com_write_string) api->com_write_string(COM1_PORT, "[i82540em] ioremap failed\n");
        api->kfree(dev);
        return -1;
    }

    dev->irq = 11;
    dev->rx_packets = 0;
    dev->tx_packets = 0;
    
    g_e1000_dev = dev;
    
    // Initialize device
    if (e1000_init_device(dev) != 0) {
        if (api->com_write_string) {
            api->com_write_string(COM1_PORT, "[i82540em] Device initialization failed\n");
        }
        api->kfree(dev);
        g_e1000_dev = NULL;
        return -1;
    }
    
    // Install IRQ handler
    if (api->irq_install_handler) {
        api->irq_install_handler(dev->irq, e1000_irq_handler);
    }
    
    // Register service
    if (api->sqrm_service_register) {
        api->sqrm_service_register("net", &g_net_api, sizeof(g_net_api));
    }
    
    if (api->com_write_string) {
        api->com_write_string(COM1_PORT, "[i82540em] Driver loaded successfully\n");
    }
    
    return 0;
}