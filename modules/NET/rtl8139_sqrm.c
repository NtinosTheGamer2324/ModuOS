//
// rtl8139_sqrm.c - Realtek RTL8139 Network Driver
//
// Part of ModuOS kernel modules
// Improved version with better performance and features
//

#include "moduos/kernel/sqrm.h"

SQRM_DEFINE_MODULE(SQRM_TYPE_NET, "rtl8139");

// PCI IDs
#define PCI_VENDOR_REALTEK      0x10EC
#define PCI_DEVICE_RTL8139      0x8139

// Register offsets
#define RTL_IDR0        0x00    // MAC address
#define RTL_MAR0        0x08    // Multicast filter
#define RTL_TSD0        0x10    // Transmit status (4 descriptors)
#define RTL_TSD1        0x14
#define RTL_TSD2        0x18
#define RTL_TSD3        0x1C
#define RTL_TSAD0       0x20    // Transmit start address (4 descriptors)
#define RTL_TSAD1       0x24
#define RTL_TSAD2       0x28
#define RTL_TSAD3       0x2C
#define RTL_RBSTART     0x30    // Receive buffer start address
#define RTL_ERBCR       0x34    // Early RX byte count
#define RTL_ERSR        0x36    // Early RX status
#define RTL_CR          0x37    // Command register
#define RTL_CAPR        0x38    // Current address of packet read
#define RTL_CBR         0x3A    // Current buffer address
#define RTL_IMR         0x3C    // Interrupt mask
#define RTL_ISR         0x3E    // Interrupt status
#define RTL_TCR         0x40    // Transmit configuration
#define RTL_RCR         0x44    // Receive configuration
#define RTL_TCTR        0x48    // Timer count
#define RTL_MPC         0x4C    // Missed packet count
#define RTL_CR9346      0x50    // 93C46 command register
#define RTL_CONFIG0     0x51    // Configuration 0
#define RTL_CONFIG1     0x52    // Configuration 1
#define RTL_MSR         0x58    // Media status register
#define RTL_CONFIG3     0x59    // Configuration 3
#define RTL_CONFIG4     0x5A    // Configuration 4
#define RTL_MULINT      0x5C    // Multiple interrupt select
#define RTL_BMCR        0x62    // Basic mode control register
#define RTL_BMSR        0x64    // Basic mode status register

// Command register bits
#define RTL_CR_RST      0x10    // Reset
#define RTL_CR_RE       0x08    // Receiver enable
#define RTL_CR_TE       0x04    // Transmitter enable
#define RTL_CR_BUFE     0x01    // RX buffer empty

// Interrupt bits
#define RTL_INT_ROK     0x0001  // Receive OK
#define RTL_INT_RER     0x0002  // Receive error
#define RTL_INT_TOK     0x0004  // Transmit OK
#define RTL_INT_TER     0x0008  // Transmit error
#define RTL_INT_RXOVW   0x0010  // RX buffer overflow
#define RTL_INT_PUN     0x0020  // Packet underrun
#define RTL_INT_FOVW    0x0040  // RX FIFO overflow
#define RTL_INT_LENCHG  0x2000  // Cable length change
#define RTL_INT_TIMEOUT 0x4000  // Timeout
#define RTL_INT_SERR    0x8000  // System error

// Transmit status bits
#define RTL_TSD_OWN     0x00002000  // DMA operation completed
#define RTL_TSD_TUN     0x00004000  // Transmit FIFO underrun
#define RTL_TSD_TOK     0x00008000  // Transmit OK
#define RTL_TSD_SIZE_MASK   0x00001FFF

// Receive configuration
#define RTL_RCR_AAP     0x00000001  // Accept all packets
#define RTL_RCR_APM     0x00000002  // Accept physical match
#define RTL_RCR_AM      0x00000004  // Accept multicast
#define RTL_RCR_AB      0x00000008  // Accept broadcast
#define RTL_RCR_AR      0x00000010  // Accept runt
#define RTL_RCR_AER     0x00000020  // Accept error
#define RTL_RCR_WRAP    0x00000080  // Wrap at end of buffer
#define RTL_RCR_MXDMA   0x00000700  // Max DMA burst
#define RTL_RCR_RBLEN   0x00001800  // RX buffer length
#define RTL_RCR_RXFTH   0x0000E000  // RX FIFO threshold

// Transmit configuration
#define RTL_TCR_CLRABT  0x00000001  // Clear abort
#define RTL_TCR_MXDMA   0x00000700  // Max DMA burst
#define RTL_TCR_IFG     0x03000000  // Interframe gap

// 93C46 command bits
#define RTL_CR9346_EEM0 0x40    // Operating mode
#define RTL_CR9346_EEM1 0x80

// Buffer sizes
#define RX_BUF_SIZE     (8192 + 16 + 1500)  // 8KB + headroom + packet
#define TX_BUF_SIZE     1536
#define NUM_TX_DESC     4

static const uint16_t COM1_PORT = 0x3F8;

typedef struct {
    uint16_t io_base;
    uint8_t irq;
    uint8_t mac[6];
    
    void *rx_buffer;
    void *tx_buffers[NUM_TX_DESC];
    
    uint16_t rx_offset;
    uint8_t tx_idx;
    
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_errors;
    uint32_t tx_errors;
    
    int link_up;
    const sqrm_kernel_api_t *api;
} rtl8139_device_t;

static rtl8139_device_t *g_rtl_dev = NULL;

// Helper functions
static inline uint8_t rtl_read8(rtl8139_device_t *dev, uint16_t reg) {
    return dev->api->inb(dev->io_base + reg);
}

static inline uint16_t rtl_read16(rtl8139_device_t *dev, uint16_t reg) {
    return dev->api->inw(dev->io_base + reg);
}

static inline uint32_t rtl_read32(rtl8139_device_t *dev, uint16_t reg) {
    return dev->api->inl(dev->io_base + reg);
}

static inline void rtl_write8(rtl8139_device_t *dev, uint16_t reg, uint8_t val) {
    dev->api->outb(dev->io_base + reg, val);
}

static inline void rtl_write16(rtl8139_device_t *dev, uint16_t reg, uint16_t val) {
    dev->api->outw(dev->io_base + reg, val);
}

static inline void rtl_write32(rtl8139_device_t *dev, uint16_t reg, uint32_t val) {
    dev->api->outl(dev->io_base + reg, val);
}

static void rtl8139_irq_handler(void) {
    if (!g_rtl_dev) return;
    
    rtl8139_device_t *dev = g_rtl_dev;
    
    // Read and acknowledge interrupts
    uint16_t isr = rtl_read16(dev, RTL_ISR);
    rtl_write16(dev, RTL_ISR, isr);
    
    if (isr & RTL_INT_ROK) {
        dev->rx_packets++;
    }
    
    if (isr & RTL_INT_TOK) {
        dev->tx_packets++;
    }
    
    if (isr & (RTL_INT_RER | RTL_INT_RXOVW | RTL_INT_FOVW)) {
        dev->rx_errors++;
    }
    
    if (isr & (RTL_INT_TER | RTL_INT_PUN)) {
        dev->tx_errors++;
    }
    
    if (dev->api->pic_send_eoi) {
        dev->api->pic_send_eoi(dev->irq);
    }
}

static int rtl8139_reset(rtl8139_device_t *dev) {
    // Enable write access to config registers
    rtl_write8(dev, RTL_CR9346, RTL_CR9346_EEM0 | RTL_CR9346_EEM1);
    
    // Reset chip
    rtl_write8(dev, RTL_CR, RTL_CR_RST);
    
    // Wait for reset to complete
    int timeout = 1000;
    while (timeout-- > 0) {
        if (!(rtl_read8(dev, RTL_CR) & RTL_CR_RST)) break;
        if (dev->api->sleep_ms) dev->api->sleep_ms(1);
    }
    
    if (timeout <= 0) return -1;
    
    // Lock config registers
    rtl_write8(dev, RTL_CR9346, 0x00);
    
    return 0;
}

static int rtl8139_init_device(rtl8139_device_t *dev) {
    const sqrm_kernel_api_t *api = dev->api;
    
    // Reset chip
    if (rtl8139_reset(dev) != 0) {
        return -1;
    }
    
    // Read MAC address
    for (int i = 0; i < 6; i++) {
        dev->mac[i] = rtl_read8(dev, RTL_IDR0 + i);
    }
    
    // Allocate RX buffer
    dev->rx_buffer = api->kmalloc(RX_BUF_SIZE);
    if (!dev->rx_buffer) return -1;
    
    // Allocate TX buffers
    for (int i = 0; i < NUM_TX_DESC; i++) {
        dev->tx_buffers[i] = api->kmalloc(TX_BUF_SIZE);
        if (!dev->tx_buffers[i]) return -1;
    }
    
    dev->rx_offset = 0;
    dev->tx_idx = 0;
    
    // Enable write to config
    rtl_write8(dev, RTL_CR9346, RTL_CR9346_EEM0 | RTL_CR9346_EEM1);
    
    // Set RX buffer
    rtl_write32(dev, RTL_RBSTART, (uint32_t)(uintptr_t)dev->rx_buffer);
    
    // Set TX buffers
    rtl_write32(dev, RTL_TSAD0, (uint32_t)(uintptr_t)dev->tx_buffers[0]);
    rtl_write32(dev, RTL_TSAD1, (uint32_t)(uintptr_t)dev->tx_buffers[1]);
    rtl_write32(dev, RTL_TSAD2, (uint32_t)(uintptr_t)dev->tx_buffers[2]);
    rtl_write32(dev, RTL_TSAD3, (uint32_t)(uintptr_t)dev->tx_buffers[3]);
    
    // Enable RX and TX
    rtl_write8(dev, RTL_CR, RTL_CR_RE | RTL_CR_TE);
    
    // Configure RX: Accept broadcast + physical match, 8K buffer, no FIFO threshold
    rtl_write32(dev, RTL_RCR, RTL_RCR_AB | RTL_RCR_APM | RTL_RCR_WRAP |
                             (7 << 8) |    // Max DMA burst (unlimited)
                             (0 << 11) |   // 8K buffer
                             (7 << 13));   // No FIFO threshold
    
    // Configure TX: Max DMA burst, default IFG
    rtl_write32(dev, RTL_TCR, (7 << 8) | (3 << 24));
    
    // Clear any pending interrupts (do not enable yet)
    rtl_write16(dev, RTL_ISR, 0xFFFF);
    
    // Disable interrupts during initialization
    rtl_write16(dev, RTL_IMR, 0x0000);
    
    // Lock config
    rtl_write8(dev, RTL_CR9346, 0x00);
    
    // Check link status
    uint8_t msr = rtl_read8(dev, RTL_MSR);
    dev->link_up = !(msr & 0x04);  // LINKB bit
    
    return 0;
}

// Network API implementation
static int net_get_link_up(void) {
    if (!g_rtl_dev) return 0;
    
    uint8_t msr = rtl_read8(g_rtl_dev, RTL_MSR);
    g_rtl_dev->link_up = !(msr & 0x04);
    
    return g_rtl_dev->link_up;
}

static int net_get_mtu(uint32_t *out) {
    if (!out) return -22;
    *out = 1500;
    return 0;
}

static int net_get_mac(uint8_t out_mac[6]) {
    if (!out_mac || !g_rtl_dev) return -22;
    for (int i = 0; i < 6; i++) {
        out_mac[i] = g_rtl_dev->mac[i];
    }
    return 0;
}

static int net_tx_frame(const void *frame, size_t len) {
    if (!g_rtl_dev || !frame || len == 0 || len > TX_BUF_SIZE) {
        return -22;
    }
    
    rtl8139_device_t *dev = g_rtl_dev;
    uint8_t idx = dev->tx_idx;
    
    // Check if TX descriptor is available
    uint32_t tsd = rtl_read32(dev, RTL_TSD0 + (idx * 4));
    if (!(tsd & RTL_TSD_OWN)) {
        return -11; // -EAGAIN
    }
    
    // Copy frame to TX buffer
    uint8_t *dst = (uint8_t*)dev->tx_buffers[idx];
    const uint8_t *src = (const uint8_t*)frame;
    for (size_t i = 0; i < len; i++) {
        dst[i] = src[i];
    }
    
    // Start transmission
    rtl_write32(dev, RTL_TSD0 + (idx * 4), len & RTL_TSD_SIZE_MASK);
    
    // Advance to next descriptor
    dev->tx_idx = (dev->tx_idx + 1) % NUM_TX_DESC;
    
    return 0;
}

static int net_rx_poll(void *out_frame, size_t out_cap, size_t *out_len) {
    if (!g_rtl_dev || !out_frame || !out_len) return -22;
    
    rtl8139_device_t *dev = g_rtl_dev;
    
    // Check if buffer empty
    uint8_t cr = rtl_read8(dev, RTL_CR);
    if (cr & RTL_CR_BUFE) {
        *out_len = 0;
        return 0;
    }
    
    // Get current offset
    uint16_t offset = dev->rx_offset;
    uint8_t *rx_buf = (uint8_t*)dev->rx_buffer;
    
    // Read packet header (4 bytes: status + length)
    uint16_t status = *(uint16_t*)(rx_buf + offset);
    uint16_t pkt_len = *(uint16_t*)(rx_buf + offset + 2);
    
    // Check status
    if (!(status & 0x0001)) {  // ROK bit
        dev->rx_errors++;
        // Skip this packet
        offset = (offset + pkt_len + 4 + 3) & ~3;  // Align to 4 bytes
        dev->rx_offset = offset % RX_BUF_SIZE;
        rtl_write16(dev, RTL_CAPR, dev->rx_offset - 0x10);
        return -5;
    }
    
    // Get actual packet length (minus CRC)
    pkt_len -= 4;
    
    if (pkt_len > out_cap) {
        *out_len = pkt_len;
        return -90;
    }
    
    // Copy packet data
    uint8_t *dst = (uint8_t*)out_frame;
    for (size_t i = 0; i < pkt_len; i++) {
        dst[i] = rx_buf[(offset + 4 + i) % RX_BUF_SIZE];
    }
    
    *out_len = pkt_len;
    
    // Save offset for consume
    dev->rx_offset = (offset + pkt_len + 4 + 3) & ~3;
    
    return 1;
}

static int net_rx_consume(void) {
    if (!g_rtl_dev) return -22;
    
    rtl8139_device_t *dev = g_rtl_dev;
    
    // Update CAPR (with -16 offset quirk)
    rtl_write16(dev, RTL_CAPR, (dev->rx_offset - 0x10) & 0xFFFF);
    
    dev->rx_offset %= RX_BUF_SIZE;
    
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
        api->com_write_string(COM1_PORT, "[rtl8139_v2] RTL8139 driver initializing\n");
    }
    
    // Check capabilities
    if (!api->inb || !api->outb || !api->inw || !api->outw ||
        !api->inl || !api->outl || !api->kmalloc) {
        if (api->com_write_string) {
            api->com_write_string(COM1_PORT, "[rtl8139_v2] Missing required capabilities\n");
        }
        return -1;
    }
    
    // Allocate device
    rtl8139_device_t *dev = api->kmalloc(sizeof(rtl8139_device_t));
    if (!dev) return -1;
    
    dev->api = api;
    dev->io_base = 0xC000;  // Common PCI address
    dev->irq = 11;
    dev->rx_packets = 0;
    dev->tx_packets = 0;
    dev->rx_errors = 0;
    dev->tx_errors = 0;
    
    g_rtl_dev = dev;
    
    // Initialize device
    if (rtl8139_init_device(dev) != 0) {
        if (api->com_write_string) {
            api->com_write_string(COM1_PORT, "[rtl8139_v2] Device initialization failed\n");
        }
        api->kfree(dev);
        g_rtl_dev = NULL;
        return -1;
    }
    
    // Install IRQ handler
    if (api->irq_install_handler) {
        api->irq_install_handler(dev->irq, rtl8139_irq_handler);
    }
    
    // Now it's safe to enable interrupts after handler is installed
    rtl_write16(dev, RTL_ISR, 0xFFFF);  // Clear any pending interrupts again
    rtl_write16(dev, RTL_IMR, RTL_INT_ROK | RTL_INT_RER | RTL_INT_TOK | 
                             RTL_INT_TER | RTL_INT_RXOVW | RTL_INT_PUN |
                             RTL_INT_FOVW);
    
    // Register service
    if (api->sqrm_service_register) {
        api->sqrm_service_register("net", &g_net_api, sizeof(g_net_api));
    }
    
    if (api->com_write_string) {
        api->com_write_string(COM1_PORT, "[rtl8139_v2] Driver loaded successfully\n");
    }
    
    return 0;
}