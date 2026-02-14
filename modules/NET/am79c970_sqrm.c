//
// am79c970_sqrm.c - AMD Am79C970 PCnet-PCI II Network Driver
//
// Part of ModuOS kernel modules
// Optimized variant for Am79C970/Am79C973 chips with enhanced features
//

#include "moduos/kernel/sqrm.h"

SQRM_DEFINE_MODULE(SQRM_TYPE_NET, "am79c970");

// PCI IDs
#define PCI_VENDOR_AMD              0x1022
#define PCI_DEVICE_AM79C970         0x2000  // PCnet-PCI II
#define PCI_DEVICE_AM79C973         0x2625  // PCnet-FAST III

// Register offsets (DWIO - 32-bit I/O mode)
#define AM79_DWIO_APROM     0x00
#define AM79_DWIO_RDP       0x10
#define AM79_DWIO_RAP       0x14
#define AM79_DWIO_RESET     0x18
#define AM79_DWIO_BDP       0x1C

// CSR0 bits
#define CSR0_INIT           (1 << 0)
#define CSR0_STRT           (1 << 1)
#define CSR0_STOP           (1 << 2)
#define CSR0_TDMD           (1 << 3)
#define CSR0_TXON           (1 << 4)
#define CSR0_RXON           (1 << 5)
#define CSR0_INEA           (1 << 6)
#define CSR0_IDON           (1 << 8)
#define CSR0_TINT           (1 << 9)
#define CSR0_RINT           (1 << 10)
#define CSR0_MERR           (1 << 11)
#define CSR0_MISS           (1 << 12)
#define CSR0_CERR           (1 << 13)
#define CSR0_BABL           (1 << 14)
#define CSR0_ERR            (1 << 15)

// BCR registers
#define BCR18_LINBC         (1 << 7)    // Link status
#define BCR20_SSIZE32       (1 << 8)    // 32-bit software size

// Descriptor bits
#define RX_OWN              (1u << 31)
#define RX_ERR              (1u << 30)
#define RX_FRAM             (1u << 29)
#define RX_OFLO             (1u << 28)
#define RX_CRC              (1u << 27)
#define RX_BUFF             (1u << 26)
#define RX_STP              (1u << 25)
#define RX_ENP              (1u << 24)

#define TX_OWN              (1u << 31)
#define TX_ERR              (1u << 30)
#define TX_ADD_FCS          (1u << 29)
#define TX_MORE             (1u << 28)
#define TX_ONE              (1u << 27)
#define TX_DEF              (1u << 26)
#define TX_STP              (1u << 25)
#define TX_ENP              (1u << 24)

// Configuration
#define RX_RING_LEN_BITS    4   // 2^4 = 16 descriptors
#define TX_RING_LEN_BITS    4   // 2^4 = 16 descriptors
#define RX_RING_SIZE        (1 << RX_RING_LEN_BITS)
#define TX_RING_SIZE        (1 << TX_RING_LEN_BITS)
#define PKT_BUF_SIZE        1544

static const uint16_t COM1_PORT = 0x3F8;

// Descriptor structure
typedef struct {
    uint32_t addr;
    uint32_t length;
    uint32_t status;
    uint32_t user;
} __attribute__((packed)) am79_desc_t;

// Init block structure (mode 2 - SSIZE32)
typedef struct {
    uint16_t mode;
    uint8_t  rlen;          // RX ring length (log2)
    uint8_t  tlen;          // TX ring length (log2)
    uint8_t  phys_addr[6];  // MAC address
    uint16_t reserved1;
    uint64_t log_filter;    // Logical address filter
    uint32_t rx_ring;       // RX ring base address
    uint32_t tx_ring;       // TX ring base address
} __attribute__((packed)) am79_init_block_t;

typedef struct {
    uint16_t io_base;
    uint8_t irq;
    uint8_t mac[6];
    
    am79_init_block_t *init_block;
    am79_desc_t *rx_ring;
    am79_desc_t *tx_ring;
    
    void **rx_buffers;
    void **tx_buffers;
    
    uint16_t rx_idx;
    uint16_t tx_idx;
    uint16_t tx_tail;
    
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_errors;
    uint32_t tx_errors;
    
    int link_up;
    const sqrm_kernel_api_t *api;
} am79_device_t;

static am79_device_t *g_dev = NULL;

// CSR/BCR access helpers
static inline void write_csr(am79_device_t *dev, uint8_t reg, uint16_t val) {
    dev->api->outl(dev->io_base + AM79_DWIO_RAP, reg);
    dev->api->outl(dev->io_base + AM79_DWIO_RDP, val);
}

static inline uint16_t read_csr(am79_device_t *dev, uint8_t reg) {
    dev->api->outl(dev->io_base + AM79_DWIO_RAP, reg);
    return dev->api->inl(dev->io_base + AM79_DWIO_RDP);
}

static inline void write_bcr(am79_device_t *dev, uint8_t reg, uint16_t val) {
    dev->api->outl(dev->io_base + AM79_DWIO_RAP, reg);
    dev->api->outl(dev->io_base + AM79_DWIO_BDP, val);
}

static inline uint16_t read_bcr(am79_device_t *dev, uint8_t reg) {
    dev->api->outl(dev->io_base + AM79_DWIO_RAP, reg);
    return dev->api->inl(dev->io_base + AM79_DWIO_BDP);
}

static void am79_irq_handler(void) {
    if (!g_dev) return;
    
    uint16_t csr0 = read_csr(g_dev, 0);
    
    // Acknowledge interrupts
    write_csr(g_dev, 0, csr0 & (CSR0_BABL | CSR0_CERR | CSR0_MISS | 
                                 CSR0_MERR | CSR0_RINT | CSR0_TINT | 
                                 CSR0_IDON));
    
    // Handle RX
    if (csr0 & CSR0_RINT) {
        g_dev->rx_packets++;
    }
    
    // Handle TX
    if (csr0 & CSR0_TINT) {
        g_dev->tx_packets++;
    }
    
    // Handle errors
    if (csr0 & (CSR0_BABL | CSR0_CERR | CSR0_MISS | CSR0_MERR)) {
        if (csr0 & CSR0_BABL) g_dev->tx_errors++;
        if (csr0 & (CSR0_MISS | CSR0_MERR)) g_dev->rx_errors++;
    }
    
    if (g_dev->api->pic_send_eoi) {
        g_dev->api->pic_send_eoi(g_dev->irq);
    }
}

static int am79_reset(am79_device_t *dev) {
    // Reset the chip
    dev->api->inl(dev->io_base + AM79_DWIO_RESET);
    
    if (dev->api->sleep_ms) {
        dev->api->sleep_ms(10);
    }
    
    // Switch to 32-bit software mode
    write_bcr(dev, 20, BCR20_SSIZE32);
    
    return 0;
}

static int am79_read_mac(am79_device_t *dev) {
    // Read MAC from APROM
    for (int i = 0; i < 6; i += 4) {
        uint32_t val = dev->api->inl(dev->io_base + AM79_DWIO_APROM + i);
        dev->mac[i] = val & 0xFF;
        if (i + 1 < 6) dev->mac[i + 1] = (val >> 8) & 0xFF;
        if (i + 2 < 6) dev->mac[i + 2] = (val >> 16) & 0xFF;
        if (i + 3 < 6) dev->mac[i + 3] = (val >> 24) & 0xFF;
    }
    
    return 0;
}

static int am79_setup_rings(am79_device_t *dev) {
    const sqrm_kernel_api_t *api = dev->api;
    
    // Allocate structures
    dev->init_block = api->kmalloc(sizeof(am79_init_block_t));
    dev->rx_ring = api->kmalloc(sizeof(am79_desc_t) * RX_RING_SIZE);
    dev->tx_ring = api->kmalloc(sizeof(am79_desc_t) * TX_RING_SIZE);
    dev->rx_buffers = api->kmalloc(sizeof(void*) * RX_RING_SIZE);
    dev->tx_buffers = api->kmalloc(sizeof(void*) * TX_RING_SIZE);
    
    if (!dev->init_block || !dev->rx_ring || !dev->tx_ring ||
        !dev->rx_buffers || !dev->tx_buffers) {
        return -1;
    }
    
    // Allocate packet buffers
    for (int i = 0; i < RX_RING_SIZE; i++) {
        dev->rx_buffers[i] = api->kmalloc(PKT_BUF_SIZE);
        if (!dev->rx_buffers[i]) return -1;
    }
    
    for (int i = 0; i < TX_RING_SIZE; i++) {
        dev->tx_buffers[i] = api->kmalloc(PKT_BUF_SIZE);
        if (!dev->tx_buffers[i]) return -1;
    }
    
    // Setup init block
    dev->init_block->mode = 0;
    dev->init_block->rlen = RX_RING_LEN_BITS;
    dev->init_block->tlen = TX_RING_LEN_BITS;
    for (int i = 0; i < 6; i++) {
        dev->init_block->phys_addr[i] = dev->mac[i];
    }
    dev->init_block->reserved1 = 0;
    dev->init_block->log_filter = 0;
    dev->init_block->rx_ring = (uint32_t)(uintptr_t)dev->rx_ring;
    dev->init_block->tx_ring = (uint32_t)(uintptr_t)dev->tx_ring;
    
    // Setup RX descriptors
    for (int i = 0; i < RX_RING_SIZE; i++) {
        dev->rx_ring[i].addr = (uint32_t)(uintptr_t)dev->rx_buffers[i];
        dev->rx_ring[i].length = (uint32_t)(-PKT_BUF_SIZE) | 0xF000;
        dev->rx_ring[i].status = RX_OWN;
        dev->rx_ring[i].user = 0;
    }
    
    // Setup TX descriptors
    for (int i = 0; i < TX_RING_SIZE; i++) {
        dev->tx_ring[i].addr = (uint32_t)(uintptr_t)dev->tx_buffers[i];
        dev->tx_ring[i].length = 0;
        dev->tx_ring[i].status = 0;
        dev->tx_ring[i].user = 0;
    }
    
    dev->rx_idx = 0;
    dev->tx_idx = 0;
    dev->tx_tail = 0;
    
    return 0;
}

static int am79_init_chip(am79_device_t *dev) {
    // Point to init block
    uint32_t addr = (uint32_t)(uintptr_t)dev->init_block;
    write_csr(dev, 1, addr & 0xFFFF);
    write_csr(dev, 2, (addr >> 16) & 0xFFFF);
    
    // Trigger init
    write_csr(dev, 0, CSR0_INIT);
    
    // Wait for init done
    int timeout = 100;
    while (timeout-- > 0) {
        if (read_csr(dev, 0) & CSR0_IDON) break;
        if (dev->api->sleep_ms) dev->api->sleep_ms(10);
    }
    
    if (timeout <= 0) return -1;
    
    // Clear IDON and start (do not enable interrupts yet)
    write_csr(dev, 0, CSR0_IDON | CSR0_STRT);
    
    // Check link status
    uint16_t bcr18 = read_bcr(dev, 18);
    dev->link_up = !!(bcr18 & BCR18_LINBC);
    
    return 0;
}

// Network API implementation
static int net_get_link_up(void) {
    if (!g_dev) return 0;
    
    // Update link status from BCR18
    uint16_t bcr18 = read_bcr(g_dev, 18);
    g_dev->link_up = !!(bcr18 & BCR18_LINBC);
    
    return g_dev->link_up;
}

static int net_get_mtu(uint32_t *out) {
    if (!out) return -22;
    *out = 1500;
    return 0;
}

static int net_get_mac(uint8_t out_mac[6]) {
    if (!out_mac || !g_dev) return -22;
    for (int i = 0; i < 6; i++) {
        out_mac[i] = g_dev->mac[i];
    }
    return 0;
}

static int net_tx_frame(const void *frame, size_t len) {
    if (!g_dev || !frame || len == 0 || len > PKT_BUF_SIZE) {
        return -22;
    }
    
    am79_device_t *dev = g_dev;
    uint16_t idx = dev->tx_idx;
    am79_desc_t *desc = &dev->tx_ring[idx];
    
    // Check if descriptor available
    if (desc->status & TX_OWN) {
        return -11; // -EAGAIN
    }
    
    // Copy frame
    uint8_t *buf = (uint8_t*)dev->tx_buffers[idx];
    const uint8_t *src = (const uint8_t*)frame;
    for (size_t i = 0; i < len; i++) {
        buf[i] = src[i];
    }
    
    // Setup descriptor
    desc->length = (uint32_t)(-len) | 0xF000;
    desc->status = TX_OWN | TX_STP | TX_ENP;
    
    // Advance index
    dev->tx_idx = (dev->tx_idx + 1) % TX_RING_SIZE;
    
    // Trigger transmission
    write_csr(dev, 0, CSR0_TDMD);
    
    return 0;
}

static int net_rx_poll(void *out_frame, size_t out_cap, size_t *out_len) {
    if (!g_dev || !out_frame || !out_len) return -22;
    
    am79_device_t *dev = g_dev;
    uint16_t idx = dev->rx_idx;
    am79_desc_t *desc = &dev->rx_ring[idx];
    
    // Check ownership
    if (desc->status & RX_OWN) {
        *out_len = 0;
        return 0;
    }
    
    // Check errors
    if (desc->status & RX_ERR) {
        dev->rx_errors++;
        desc->status = RX_OWN;
        dev->rx_idx = (dev->rx_idx + 1) % RX_RING_SIZE;
        return -5;
    }
    
    // Get length (minus 4-byte FCS)
    uint32_t msg_len = desc->length & 0xFFF;
    if (msg_len < 4) msg_len = 0;
    else msg_len -= 4;
    
    if (msg_len > out_cap) {
        *out_len = msg_len;
        return -90;
    }
    
    // Copy packet
    uint8_t *src = (uint8_t*)dev->rx_buffers[idx];
    uint8_t *dst = (uint8_t*)out_frame;
    for (size_t i = 0; i < msg_len; i++) {
        dst[i] = src[i];
    }
    
    *out_len = msg_len;
    return 1;
}

static int net_rx_consume(void) {
    if (!g_dev) return -22;
    
    am79_device_t *dev = g_dev;
    am79_desc_t *desc = &dev->rx_ring[dev->rx_idx];
    
    desc->status = RX_OWN;
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
        api->com_write_string(COM1_PORT, "[am79c970] AMD Am79C970 driver initializing\n");
    }
    
    // Check capabilities
    if (!api->inl || !api->outl || !api->kmalloc) {
        if (api->com_write_string) {
            api->com_write_string(COM1_PORT, "[am79c970] Missing required capabilities\n");
        }
        return -1;
    }
    
    // Allocate device
    am79_device_t *dev = api->kmalloc(sizeof(am79_device_t));
    if (!dev) return -1;
    
    dev->api = api;
    dev->io_base = 0xC100;  // Standard IO base
    dev->irq = 11;          // Standard IRQ
    dev->rx_packets = 0;
    dev->tx_packets = 0;
    dev->rx_errors = 0;
    dev->tx_errors = 0;
    dev->link_up = 0;
    
    g_dev = dev;
    
    // Reset chip
    if (am79_reset(dev) != 0) {
        api->kfree(dev);
        g_dev = NULL;
        return -1;
    }
    
    // Read MAC
    am79_read_mac(dev);
    
    // Setup rings
    if (am79_setup_rings(dev) != 0) {
        if (api->com_write_string) {
            api->com_write_string(COM1_PORT, "[am79c970] Failed to setup rings\n");
        }
        api->kfree(dev);
        g_dev = NULL;
        return -1;
    }
    
    // Initialize chip
    if (am79_init_chip(dev) != 0) {
        if (api->com_write_string) {
            api->com_write_string(COM1_PORT, "[am79c970] Chip initialization failed\n");
        }
        api->kfree(dev);
        g_dev = NULL;
        return -1;
    }
    
    // Install IRQ handler
    if (api->irq_install_handler) {
        api->irq_install_handler(dev->irq, am79_irq_handler);
    }
    
    // Now enable interrupts after handler is installed
    uint16_t csr0 = read_csr(dev, 0);
    write_csr(dev, 0, csr0 | CSR0_INEA);
    
    // Register service
    if (api->sqrm_service_register) {
        api->sqrm_service_register("net", &g_net_api, sizeof(g_net_api));
    }
    
    if (api->com_write_string) {
        api->com_write_string(COM1_PORT, "[am79c970] Driver loaded successfully\n");
    }
    
    return 0;
}