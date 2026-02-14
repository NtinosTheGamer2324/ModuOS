//
// ne2000_sqrm.c - NE2000 Compatible Network Driver
//
// Part of ModuOS kernel modules
// Supports NE2000, RTL8029, and compatible ISA/PCI NICs
//

#include "moduos/kernel/sqrm.h"

SQRM_DEFINE_MODULE(SQRM_TYPE_NET, "ne2000");

// NE2000 register offsets
#define NE_CMD          0x00    // Command register
#define NE_PSTART       0x01    // Page start register
#define NE_PSTOP        0x02    // Page stop register
#define NE_BOUNDARY     0x03    // Boundary pointer
#define NE_TSR          0x04    // Transmit status register
#define NE_TPSR         0x04    // Transmit page start
#define NE_NCR          0x05    // Number of collisions
#define NE_TBCR0        0x05    // Transmit byte count 0
#define NE_FIFO         0x06    // FIFO
#define NE_TBCR1        0x06    // Transmit byte count 1
#define NE_ISR          0x07    // Interrupt status
#define NE_RSAR0        0x08    // Remote start address 0
#define NE_CRDA0        0x08    // Current remote DMA address 0
#define NE_RSAR1        0x09    // Remote start address 1
#define NE_CRDA1        0x09    // Current remote DMA address 1
#define NE_RBCR0        0x0A    // Remote byte count 0
#define NE_RBCR1        0x0B    // Remote byte count 1
#define NE_RSR          0x0C    // Receive status
#define NE_RCR          0x0C    // Receive configuration
#define NE_TCR          0x0D    // Transmit configuration
#define NE_CNTR0        0x0D    // Frame alignment errors
#define NE_DCR          0x0E    // Data configuration
#define NE_CNTR1        0x0E    // CRC errors
#define NE_IMR          0x0F    // Interrupt mask
#define NE_CNTR2        0x0F    // Missed packet errors
#define NE_DATAPORT     0x10    // Data port
#define NE_RESET        0x1F    // Reset port

// Command register bits
#define NE_CMD_STP      0x01    // Stop
#define NE_CMD_STA      0x02    // Start
#define NE_CMD_TXP      0x04    // Transmit packet
#define NE_CMD_RD0      0x08    // Remote DMA read
#define NE_CMD_RD1      0x10    // Remote DMA write
#define NE_CMD_RD2      0x20    // Send packet
#define NE_CMD_PS0      0x40    // Page select 0
#define NE_CMD_PS1      0x80    // Page select 1

#define NE_CMD_PAGE0    0x00
#define NE_CMD_PAGE1    0x40
#define NE_CMD_PAGE2    0x80

// Interrupt status bits
#define NE_ISR_PRX      0x01    // Packet received
#define NE_ISR_PTX      0x02    // Packet transmitted
#define NE_ISR_RXE      0x04    // Receive error
#define NE_ISR_TXE      0x08    // Transmit error
#define NE_ISR_OVW      0x10    // Overwrite warning
#define NE_ISR_CNT      0x20    // Counter overflow
#define NE_ISR_RDC      0x40    // Remote DMA complete
#define NE_ISR_RST      0x80    // Reset status

// Transmit configuration
#define NE_TCR_CRC      0x01    // Inhibit CRC
#define NE_TCR_LB0      0x02    // Loopback mode 0
#define NE_TCR_LB1      0x04    // Loopback mode 1
#define NE_TCR_ATD      0x08    // Auto transmit disable
#define NE_TCR_OFST     0x10    // Collision offset enable

// Receive configuration
#define NE_RCR_SEP      0x01    // Save error packets
#define NE_RCR_AR       0x02    // Accept runt packets
#define NE_RCR_AB       0x04    // Accept broadcast
#define NE_RCR_AM       0x08    // Accept multicast
#define NE_RCR_PRO      0x10    // Promiscuous mode
#define NE_RCR_MON      0x20    // Monitor mode

// Data configuration
#define NE_DCR_WTS      0x01    // Word transfer select
#define NE_DCR_BOS      0x02    // Byte order select
#define NE_DCR_LAS      0x04    // Long address select
#define NE_DCR_LS       0x08    // Loopback select
#define NE_DCR_ARM      0x10    // Auto-initialize remote
#define NE_DCR_FT0      0x20    // FIFO threshold 0
#define NE_DCR_FT1      0x40    // FIFO threshold 1

// Memory layout
#define NE_PAGE_SIZE    256
#define NE_TX_START     0x40    // TX buffer start page
#define NE_RX_START     0x46    // RX buffer start page
#define NE_RX_STOP      0x80    // RX buffer stop page

#define MAX_ETH_FRAME   1518

static const uint16_t COM1_PORT = 0x3F8;

typedef struct {
    uint8_t status;
    uint8_t next_page;
    uint16_t count;
} __attribute__((packed)) ne2000_rx_header_t;

typedef struct {
    uint16_t io_base;
    uint8_t irq;
    uint8_t mac[6];
    
    uint8_t current_page;
    uint8_t next_packet;
    
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_errors;
    uint32_t tx_errors;
    
    int link_up;
    const sqrm_kernel_api_t *api;
} ne2000_device_t;

static ne2000_device_t *g_ne_dev = NULL;

// Helper functions
static inline uint8_t ne_read(ne2000_device_t *dev, uint8_t reg) {
    return dev->api->inb(dev->io_base + reg);
}

static inline void ne_write(ne2000_device_t *dev, uint8_t reg, uint8_t val) {
    dev->api->outb(dev->io_base + reg, val);
}

static void ne_select_page(ne2000_device_t *dev, uint8_t page) {
    uint8_t cmd = ne_read(dev, NE_CMD) & ~(NE_CMD_PS0 | NE_CMD_PS1);
    ne_write(dev, NE_CMD, cmd | page);
}

static void ne_reset_chip(ne2000_device_t *dev) {
    // Read reset port
    ne_read(dev, NE_RESET);
    
    if (dev->api->sleep_ms) {
        dev->api->sleep_ms(10);
    }
    
    // Wait for reset to complete
    int timeout = 1000;
    while (timeout-- > 0) {
        if (ne_read(dev, NE_ISR) & NE_ISR_RST) break;
        if (dev->api->sleep_ms) dev->api->sleep_ms(1);
    }
    
    // Clear ISR
    ne_write(dev, NE_ISR, 0xFF);
}

static void ne_block_input(ne2000_device_t *dev, uint16_t page, 
                          uint16_t offset, void *buf, uint16_t len) {
    // Stop NIC
    ne_write(dev, NE_CMD, NE_CMD_STA | NE_CMD_RD2 | NE_CMD_PAGE0);
    
    // Clear remote byte count
    ne_write(dev, NE_RBCR0, len & 0xFF);
    ne_write(dev, NE_RBCR1, (len >> 8) & 0xFF);
    
    // Set remote start address
    uint16_t addr = page * NE_PAGE_SIZE + offset;
    ne_write(dev, NE_RSAR0, addr & 0xFF);
    ne_write(dev, NE_RSAR1, (addr >> 8) & 0xFF);
    
    // Start remote DMA read
    ne_write(dev, NE_CMD, NE_CMD_STA | NE_CMD_RD0 | NE_CMD_PAGE0);
    
    // Read data
    uint8_t *ptr = (uint8_t*)buf;
    for (uint16_t i = 0; i < len; i++) {
        ptr[i] = dev->api->inb(dev->io_base + NE_DATAPORT);
    }
    
    // Wait for remote DMA complete
    int timeout = 1000;
    while (timeout-- > 0) {
        if (ne_read(dev, NE_ISR) & NE_ISR_RDC) break;
    }
    
    ne_write(dev, NE_ISR, NE_ISR_RDC);
}

static void ne_block_output(ne2000_device_t *dev, uint16_t page,
                           const void *buf, uint16_t len) {
    // Stop NIC
    ne_write(dev, NE_CMD, NE_CMD_STA | NE_CMD_RD2 | NE_CMD_PAGE0);
    
    // Clear ISR
    ne_write(dev, NE_ISR, NE_ISR_RDC);
    
    // Set remote byte count
    ne_write(dev, NE_RBCR0, len & 0xFF);
    ne_write(dev, NE_RBCR1, (len >> 8) & 0xFF);
    
    // Set remote start address
    uint16_t addr = page * NE_PAGE_SIZE;
    ne_write(dev, NE_RSAR0, addr & 0xFF);
    ne_write(dev, NE_RSAR1, (addr >> 8) & 0xFF);
    
    // Start remote DMA write
    ne_write(dev, NE_CMD, NE_CMD_STA | NE_CMD_RD1 | NE_CMD_PAGE0);
    
    // Write data
    const uint8_t *ptr = (const uint8_t*)buf;
    for (uint16_t i = 0; i < len; i++) {
        dev->api->outb(dev->io_base + NE_DATAPORT, ptr[i]);
    }
    
    // Wait for remote DMA complete
    int timeout = 1000;
    while (timeout-- > 0) {
        if (ne_read(dev, NE_ISR) & NE_ISR_RDC) break;
    }
    
    ne_write(dev, NE_ISR, NE_ISR_RDC);
}

static void ne2000_irq_handler(void) {
    if (!g_ne_dev) return;
    
    ne2000_device_t *dev = g_ne_dev;
    
    // Read interrupt status
    ne_select_page(dev, NE_CMD_PAGE0);
    uint8_t isr = ne_read(dev, NE_ISR);
    
    // Acknowledge interrupts
    ne_write(dev, NE_ISR, isr);
    
    if (isr & NE_ISR_PRX) {
        dev->rx_packets++;
    }
    
    if (isr & NE_ISR_PTX) {
        dev->tx_packets++;
    }
    
    if (isr & (NE_ISR_RXE | NE_ISR_TXE | NE_ISR_OVW)) {
        if (isr & NE_ISR_RXE) dev->rx_errors++;
        if (isr & NE_ISR_TXE) dev->tx_errors++;
    }
    
    if (dev->api->pic_send_eoi) {
        dev->api->pic_send_eoi(dev->irq);
    }
}

static int ne2000_init_device(ne2000_device_t *dev) {
    // Reset chip
    ne_reset_chip(dev);
    
    // Stop NIC
    ne_write(dev, NE_CMD, NE_CMD_STP | NE_CMD_RD2 | NE_CMD_PAGE0);
    
    // Configure data transfer
    ne_write(dev, NE_DCR, NE_DCR_FT1 | NE_DCR_LS | NE_DCR_WTS);
    
    // Clear remote byte count
    ne_write(dev, NE_RBCR0, 0);
    ne_write(dev, NE_RBCR1, 0);
    
    // Set receive configuration (accept broadcast)
    ne_write(dev, NE_RCR, NE_RCR_AB);
    
    // Set transmit configuration (normal operation)
    ne_write(dev, NE_TCR, 0x00);
    
    // Set receive buffer ring
    ne_write(dev, NE_PSTART, NE_RX_START);
    ne_write(dev, NE_PSTOP, NE_RX_STOP);
    ne_write(dev, NE_BOUNDARY, NE_RX_START);
    
    // Clear ISR
    ne_write(dev, NE_ISR, 0xFF);
    
    // Disable interrupts during initialization
    ne_write(dev, NE_IMR, 0x00);
    
    // Switch to page 1
    ne_select_page(dev, NE_CMD_PAGE1);
    
    // Read MAC address from page 1 registers
    for (int i = 0; i < 6; i++) {
        dev->mac[i] = ne_read(dev, 0x01 + i);
    }
    
    // Set current page
    dev->current_page = NE_RX_START + 1;
    ne_write(dev, 0x07, dev->current_page);
    
    // Back to page 0
    ne_select_page(dev, NE_CMD_PAGE0);
    
    // Start NIC
    ne_write(dev, NE_CMD, NE_CMD_STA | NE_CMD_RD2 | NE_CMD_PAGE0);
    
    // Set transmit configuration (normal)
    ne_write(dev, NE_TCR, 0x00);
    
    dev->link_up = 1;
    dev->next_packet = NE_RX_START + 1;
    
    return 0;
}

// Network API implementation
static int net_get_link_up(void) {
    return g_ne_dev ? g_ne_dev->link_up : 0;
}

static int net_get_mtu(uint32_t *out) {
    if (!out) return -22;
    *out = 1500;
    return 0;
}

static int net_get_mac(uint8_t out_mac[6]) {
    if (!out_mac || !g_ne_dev) return -22;
    for (int i = 0; i < 6; i++) {
        out_mac[i] = g_ne_dev->mac[i];
    }
    return 0;
}

static int net_tx_frame(const void *frame, size_t len) {
    if (!g_ne_dev || !frame || len == 0 || len > MAX_ETH_FRAME) {
        return -22;
    }
    
    ne2000_device_t *dev = g_ne_dev;
    
    // Pad to minimum size
    uint16_t send_len = (len < 60) ? 60 : len;
    
    // Stop NIC
    ne_write(dev, NE_CMD, NE_CMD_STA | NE_CMD_RD2 | NE_CMD_PAGE0);
    
    // Write packet to TX buffer
    ne_block_output(dev, NE_TX_START, frame, len);
    
    // If padding needed, write zeros
    if (send_len > len) {
        uint8_t zero = 0;
        for (uint16_t i = len; i < send_len; i++) {
            dev->api->outb(dev->io_base + NE_DATAPORT, zero);
        }
    }
    
    // Set transmit page and count
    ne_write(dev, NE_TPSR, NE_TX_START);
    ne_write(dev, NE_TBCR0, send_len & 0xFF);
    ne_write(dev, NE_TBCR1, (send_len >> 8) & 0xFF);
    
    // Trigger transmission
    ne_write(dev, NE_CMD, NE_CMD_STA | NE_CMD_TXP | NE_CMD_RD2);
    
    return 0;
}

static int net_rx_poll(void *out_frame, size_t out_cap, size_t *out_len) {
    if (!g_ne_dev || !out_frame || !out_len) return -22;
    
    ne2000_device_t *dev = g_ne_dev;
    
    // Get current page
    ne_select_page(dev, NE_CMD_PAGE1);
    uint8_t current = ne_read(dev, 0x07);
    ne_select_page(dev, NE_CMD_PAGE0);
    
    // Check if packet available
    if (dev->next_packet == current) {
        *out_len = 0;
        return 0;
    }
    
    // Read header
    ne2000_rx_header_t header;
    ne_block_input(dev, dev->next_packet, 0, &header, sizeof(header));
    
    // Get packet length (minus header and CRC)
    uint16_t pkt_len = header.count - sizeof(header);
    if (pkt_len < 4) pkt_len = 0;
    else pkt_len -= 4;
    
    if (pkt_len > out_cap) {
        *out_len = pkt_len;
        return -90;
    }
    
    // Read packet data
    ne_block_input(dev, dev->next_packet, sizeof(header), out_frame, pkt_len);
    
    *out_len = pkt_len;
    
    // Save next packet location for consume
    dev->next_packet = header.next_page;
    
    return 1;
}

static int net_rx_consume(void) {
    if (!g_ne_dev) return -22;
    
    ne2000_device_t *dev = g_ne_dev;
    
    // Update boundary pointer
    uint8_t boundary = (dev->next_packet == NE_RX_START) ? 
                       (NE_RX_STOP - 1) : (dev->next_packet - 1);
    
    ne_select_page(dev, NE_CMD_PAGE0);
    ne_write(dev, NE_BOUNDARY, boundary);
    
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
        api->com_write_string(COM1_PORT, "[ne2000] NE2000 driver initializing\n");
    }
    
    // Check capabilities
    if (!api->inb || !api->outb || !api->kmalloc) {
        if (api->com_write_string) {
            api->com_write_string(COM1_PORT, "[ne2000] Missing required capabilities\n");
        }
        return -1;
    }
    
    // Allocate device
    ne2000_device_t *dev = api->kmalloc(sizeof(ne2000_device_t));
    if (!dev) return -1;
    
    dev->api = api;
    dev->io_base = 0x300;  // Standard ISA address
    dev->irq = 3;          // Standard IRQ
    dev->rx_packets = 0;
    dev->tx_packets = 0;
    dev->rx_errors = 0;
    dev->tx_errors = 0;
    
    g_ne_dev = dev;
    
    // Initialize device
    if (ne2000_init_device(dev) != 0) {
        if (api->com_write_string) {
            api->com_write_string(COM1_PORT, "[ne2000] Device initialization failed\n");
        }
        api->kfree(dev);
        g_ne_dev = NULL;
        return -1;
    }
    
    // Install IRQ handler
    if (api->irq_install_handler) {
        api->irq_install_handler(dev->irq, ne2000_irq_handler);
    }
    
    // Now enable interrupts after handler is installed
    ne_write(dev, NE_ISR, 0xFF);  // Clear any pending interrupts
    ne_write(dev, NE_IMR, NE_ISR_PRX | NE_ISR_PTX | NE_ISR_RXE | 
                          NE_ISR_TXE | NE_ISR_OVW);
    
    // Register service
    if (api->sqrm_service_register) {
        api->sqrm_service_register("net", &g_net_api, sizeof(g_net_api));
    }
    
    if (api->com_write_string) {
        api->com_write_string(COM1_PORT, "[ne2000] Driver loaded successfully\n");
    }
    
    return 0;
}