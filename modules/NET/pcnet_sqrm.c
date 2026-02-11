//
// pcnet_sqrm.c - AMD PCnet/LANCE Network Driver
//
// Part of ModuOS kernel modules
// Supports AMD LANCE/PCnet32 network adapters (PCI)
//

#include "moduos/kernel/sqrm.h"

SQRM_DEFINE_MODULE(SQRM_TYPE_NET, "pcnet");

// PCI IDs for AMD PCnet adapters
#define PCI_VENDOR_AMD          0x1022
#define PCI_DEVICE_PCNET32      0x2000
#define PCI_DEVICE_PCNET_HOME   0x2001

// PCnet register offsets (32-bit mode)
#define PCNET_IO_APROM00    0x00    // MAC address bytes 0-3
#define PCNET_IO_APROM04    0x04    // MAC address bytes 4-5
#define PCNET_IO_RDP        0x10    // Register Data Port
#define PCNET_IO_RAP        0x14    // Register Address Port
#define PCNET_IO_RESET      0x18    // Reset register
#define PCNET_IO_BDP        0x1C    // Bus Data Port

// CSR registers
#define CSR0_INIT       0x0001
#define CSR0_STRT       0x0002
#define CSR0_STOP       0x0004
#define CSR0_TDMD       0x0008
#define CSR0_TXON       0x0010
#define CSR0_RXON       0x0020
#define CSR0_INEA       0x0040
#define CSR0_IDON       0x0100
#define CSR0_TINT       0x0200
#define CSR0_RINT       0x0400
#define CSR0_MERR       0x0800
#define CSR0_MISS       0x1000
#define CSR0_CERR       0x2000
#define CSR0_BABL       0x4000
#define CSR0_ERR        0x8000

#define CSR3_BSWP       0x0004      // Byte swap
#define CSR15_PROM      0x8000      // Promiscuous mode

// Buffer sizes
#define RX_RING_SIZE    16
#define TX_RING_SIZE    16
#define BUFFER_SIZE     1536

// Descriptor status bits
#define DESC_OWN        0x80000000  // Owned by controller
#define DESC_ERR        0x40000000  // Error
#define DESC_STP        0x02000000  // Start of packet
#define DESC_ENP        0x01000000  // End of packet

static const uint16_t COM1_PORT = 0x3F8;

// Forward declarations
struct pcnet_device;
typedef struct pcnet_device pcnet_device_t;

// DMA descriptor
typedef struct {
    uint32_t base;      // Buffer address
    uint32_t length;    // Buffer length and flags
    uint32_t status;    // Status flags
    uint32_t reserved;  // Reserved/user data
} __attribute__((packed)) pcnet_desc_t;

// Initialization block
typedef struct {
    uint16_t mode;
    uint8_t  rlen;      // RX ring length (log2)
    uint8_t  tlen;      // TX ring length (log2)
    uint8_t  mac[6];    // MAC address
    uint16_t reserved;
    uint64_t filter;    // Multicast filter
    uint32_t rx_ring;   // RX descriptor ring address
    uint32_t tx_ring;   // TX descriptor ring address
} __attribute__((packed)) pcnet_init_block_t;

struct pcnet_device {
    uint16_t io_base;
    uint8_t irq;
    uint8_t mac[6];
    
    pcnet_init_block_t *init_block;
    pcnet_desc_t *rx_ring;
    pcnet_desc_t *tx_ring;
    void **rx_buffers;
    void **tx_buffers;
    
    uint32_t rx_idx;
    uint32_t tx_idx;
    
    int link_up;
    const sqrm_kernel_api_t *api;
};

static pcnet_device_t *g_pcnet_dev = NULL;

// Helper functions for CSR access
static void pcnet_write_csr(pcnet_device_t *dev, uint16_t reg, uint16_t val) {
    if (!dev || !dev->api) return;
    dev->api->outw(dev->io_base + PCNET_IO_RAP, reg);
    dev->api->outw(dev->io_base + PCNET_IO_RDP, val);
}

static uint16_t pcnet_read_csr(pcnet_device_t *dev, uint16_t reg) {
    if (!dev || !dev->api) return 0;
    dev->api->outw(dev->io_base + PCNET_IO_RAP, reg);
    return dev->api->inw(dev->io_base + PCNET_IO_RDP);
}

static void pcnet_write_bcr(pcnet_device_t *dev, uint16_t reg, uint16_t val) {
    if (!dev || !dev->api) return;
    dev->api->outw(dev->io_base + PCNET_IO_RAP, reg);
    dev->api->outw(dev->io_base + PCNET_IO_BDP, val);
}

static void pcnet_irq_handler(void) {
    if (!g_pcnet_dev) return;
    
    uint16_t csr0 = pcnet_read_csr(g_pcnet_dev, 0);
    
    // Clear interrupt flags
    pcnet_write_csr(g_pcnet_dev, 0, csr0 & (CSR0_BABL | CSR0_CERR | 
                                             CSR0_MISS | CSR0_MERR | 
                                             CSR0_RINT | CSR0_TINT | 
                                             CSR0_IDON));
    
    if (g_pcnet_dev->api && g_pcnet_dev->api->pic_send_eoi) {
        g_pcnet_dev->api->pic_send_eoi(g_pcnet_dev->irq);
    }
}

static int pcnet_detect_pci(const sqrm_kernel_api_t *api, 
                           uint16_t *out_io_base, uint8_t *out_irq) {
    // Simplified PCI scan - real implementation would scan PCI config space
    // For now, assume standard PCnet IO base
    *out_io_base = 0xC100;  // Common VMware/QEMU address
    *out_irq = 11;          // Common IRQ
    
    if (api->com_write_string) {
        api->com_write_string(COM1_PORT, "[pcnet] Assuming IO base 0xC100, IRQ 11\n");
    }
    
    return 0;
}

static int pcnet_init_device(pcnet_device_t *dev) {
    if (!dev || !dev->api) return -1;
    
    const sqrm_kernel_api_t *api = dev->api;
    
    // Reset the controller
    api->inw(dev->io_base + PCNET_IO_RESET);
    if (api->sleep_ms) api->sleep_ms(10);
    
    // Read MAC address from APROM
    uint32_t mac_lo = api->inl(dev->io_base + PCNET_IO_APROM00);
    uint32_t mac_hi = api->inl(dev->io_base + PCNET_IO_APROM04);
    
    dev->mac[0] = mac_lo & 0xFF;
    dev->mac[1] = (mac_lo >> 8) & 0xFF;
    dev->mac[2] = (mac_lo >> 16) & 0xFF;
    dev->mac[3] = (mac_lo >> 24) & 0xFF;
    dev->mac[4] = mac_hi & 0xFF;
    dev->mac[5] = (mac_hi >> 8) & 0xFF;
    
    // Switch to 32-bit mode
    api->outl(dev->io_base + PCNET_IO_RDP, 0);
    
    // Enable byte swapping if needed (BCR20)
    pcnet_write_bcr(dev, 20, 0x0002);
    
    // Allocate DMA buffers
    dev->init_block = api->kmalloc(sizeof(pcnet_init_block_t));
    dev->rx_ring = api->kmalloc(sizeof(pcnet_desc_t) * RX_RING_SIZE);
    dev->tx_ring = api->kmalloc(sizeof(pcnet_desc_t) * TX_RING_SIZE);
    dev->rx_buffers = api->kmalloc(sizeof(void*) * RX_RING_SIZE);
    dev->tx_buffers = api->kmalloc(sizeof(void*) * TX_RING_SIZE);
    
    if (!dev->init_block || !dev->rx_ring || !dev->tx_ring || 
        !dev->rx_buffers || !dev->tx_buffers) {
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
    
    // Setup initialization block
    dev->init_block->mode = 0;
    dev->init_block->rlen = 4;  // log2(16)
    dev->init_block->tlen = 4;  // log2(16)
    for (int i = 0; i < 6; i++) {
        dev->init_block->mac[i] = dev->mac[i];
    }
    dev->init_block->reserved = 0;
    dev->init_block->filter = 0;
    dev->init_block->rx_ring = (uint32_t)(uintptr_t)dev->rx_ring;
    dev->init_block->tx_ring = (uint32_t)(uintptr_t)dev->tx_ring;
    
    // Setup RX descriptors
    for (int i = 0; i < RX_RING_SIZE; i++) {
        dev->rx_ring[i].base = (uint32_t)(uintptr_t)dev->rx_buffers[i];
        dev->rx_ring[i].length = (-BUFFER_SIZE) & 0xFFFF;
        dev->rx_ring[i].status = DESC_OWN;
        dev->rx_ring[i].reserved = 0;
    }
    
    // Setup TX descriptors
    for (int i = 0; i < TX_RING_SIZE; i++) {
        dev->tx_ring[i].base = (uint32_t)(uintptr_t)dev->tx_buffers[i];
        dev->tx_ring[i].length = 0;
        dev->tx_ring[i].status = 0;
        dev->tx_ring[i].reserved = 0;
    }
    
    dev->rx_idx = 0;
    dev->tx_idx = 0;
    
    // Point CSR1/CSR2 to init block
    uint32_t init_addr = (uint32_t)(uintptr_t)dev->init_block;
    pcnet_write_csr(dev, 1, init_addr & 0xFFFF);
    pcnet_write_csr(dev, 2, (init_addr >> 16) & 0xFFFF);
    
    // Initialize
    pcnet_write_csr(dev, 0, CSR0_INIT);
    
    // Wait for initialization
    int timeout = 1000;
    while (timeout-- > 0) {
        if (pcnet_read_csr(dev, 0) & CSR0_IDON) break;
        if (api->sleep_ms) api->sleep_ms(1);
    }
    
    if (timeout <= 0) {
        if (api->com_write_string) {
            api->com_write_string(COM1_PORT, "[pcnet] Initialization timeout\n");
        }
        return -1;
    }
    
    // Enable interrupts and start
    pcnet_write_csr(dev, 0, CSR0_IDON | CSR0_STRT | CSR0_INEA);
    
    dev->link_up = 1;
    
    if (api->com_write_string) {
        api->com_write_string(COM1_PORT, "[pcnet] Device initialized successfully\n");
    }
    
    return 0;
}

// Network API implementation
static int net_get_link_up(void) {
    return g_pcnet_dev ? g_pcnet_dev->link_up : 0;
}

static int net_get_mtu(uint32_t *out) {
    if (!out) return -22; // -EINVAL
    *out = 1500;
    return 0;
}

static int net_get_mac(uint8_t out_mac[6]) {
    if (!out_mac || !g_pcnet_dev) return -22;
    for (int i = 0; i < 6; i++) {
        out_mac[i] = g_pcnet_dev->mac[i];
    }
    return 0;
}

static int net_tx_frame(const void *frame, size_t len) {
    if (!g_pcnet_dev || !frame || len == 0 || len > BUFFER_SIZE) {
        return -22; // -EINVAL
    }
    
    pcnet_device_t *dev = g_pcnet_dev;
    pcnet_desc_t *desc = &dev->tx_ring[dev->tx_idx];
    
    // Check if descriptor is available
    if (desc->status & DESC_OWN) {
        return -11; // -EAGAIN
    }
    
    // Copy frame to buffer
    uint8_t *buf = (uint8_t*)dev->tx_buffers[dev->tx_idx];
    for (size_t i = 0; i < len; i++) {
        buf[i] = ((const uint8_t*)frame)[i];
    }
    
    // Setup descriptor
    desc->length = (-len) & 0xFFFF;
    desc->status = DESC_OWN | DESC_STP | DESC_ENP;
    
    // Advance index
    dev->tx_idx = (dev->tx_idx + 1) % TX_RING_SIZE;
    
    // Trigger transmission
    pcnet_write_csr(dev, 0, CSR0_TDMD);
    
    return 0;
}

static int net_rx_poll(void *out_frame, size_t out_cap, size_t *out_len) {
    if (!g_pcnet_dev || !out_frame || !out_len) return -22;
    
    pcnet_device_t *dev = g_pcnet_dev;
    pcnet_desc_t *desc = &dev->rx_ring[dev->rx_idx];
    
    // Check if packet available
    if (desc->status & DESC_OWN) {
        *out_len = 0;
        return 0; // No packet
    }
    
    // Check for errors
    if (desc->status & DESC_ERR) {
        // Reset descriptor
        desc->status = DESC_OWN;
        dev->rx_idx = (dev->rx_idx + 1) % RX_RING_SIZE;
        return -5; // -EIO
    }
    
    // Get packet length
    size_t pkt_len = desc->length & 0xFFFF;
    if (pkt_len > out_cap) {
        *out_len = pkt_len;
        return -90; // -EMSGSIZE
    }
    
    // Copy packet
    uint8_t *buf = (uint8_t*)dev->rx_buffers[dev->rx_idx];
    for (size_t i = 0; i < pkt_len; i++) {
        ((uint8_t*)out_frame)[i] = buf[i];
    }
    
    *out_len = pkt_len;
    return 1; // Packet available
}

static int net_rx_consume(void) {
    if (!g_pcnet_dev) return -22;
    
    pcnet_device_t *dev = g_pcnet_dev;
    pcnet_desc_t *desc = &dev->rx_ring[dev->rx_idx];
    
    // Return descriptor to hardware
    desc->status = DESC_OWN;
    dev->rx_idx = (dev->rx_idx + 1) % RX_RING_SIZE;
    
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
        api->com_write_string(COM1_PORT, "[pcnet] AMD PCnet driver initializing\n");
    }
    
    // Check for required capabilities
    if (!api->inb || !api->outb || !api->inw || !api->outw || 
        !api->inl || !api->outl || !api->kmalloc) {
        if (api->com_write_string) {
            api->com_write_string(COM1_PORT, "[pcnet] Missing required capabilities\n");
        }
        return -1;
    }
    
    // Allocate device structure
    pcnet_device_t *dev = api->kmalloc(sizeof(pcnet_device_t));
    if (!dev) return -1;
    
    dev->api = api;
    dev->link_up = 0;
    
    // Detect hardware
    if (pcnet_detect_pci(api, &dev->io_base, &dev->irq) != 0) {
        if (api->com_write_string) {
            api->com_write_string(COM1_PORT, "[pcnet] No compatible hardware found\n");
        }
        api->kfree(dev);
        return -1;
    }
    
    g_pcnet_dev = dev;
    
    // Initialize device
    if (pcnet_init_device(dev) != 0) {
        if (api->com_write_string) {
            api->com_write_string(COM1_PORT, "[pcnet] Device initialization failed\n");
        }
        api->kfree(dev);
        g_pcnet_dev = NULL;
        return -1;
    }
    
    // Install IRQ handler
    if (api->irq_install_handler) {
        api->irq_install_handler(dev->irq, pcnet_irq_handler);
    }
    
    // Register network service
    if (api->sqrm_service_register) {
        api->sqrm_service_register("net", &g_net_api, sizeof(g_net_api));
    }
    
    if (api->com_write_string) {
        api->com_write_string(COM1_PORT, "[pcnet] Driver loaded successfully\n");
    }
    
    return 0;
}