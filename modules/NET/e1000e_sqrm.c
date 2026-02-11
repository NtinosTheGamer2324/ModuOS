//
// e1000e_sqrm.c - Intel E1000E Network Card Driver
//
// Part of ModuOS kernel - SQRM network module
//

#include "moduos/kernel/sqrm.h"

/*
 * Intel E1000E NIC Driver
 *
 * Supports Intel 82571/82572/82573/82574/82583/ICH8/ICH9/ICH10 families
 * Common in modern Intel motherboards and enterprise hardware
 *
 * PCI Device IDs (partial list):
 *   0x10D3 - 82574L Gigabit Network Connection
 *   0x10CC - 82567LM-2 Gigabit Network Connection
 *   0x10CD - 82567LF-2 Gigabit Network Connection
 *   0x105E - 82571EB Gigabit Ethernet Controller
 *   0x107D - 82572EI Gigabit Ethernet Controller
 */

SQRM_DEFINE_MODULE(SQRM_TYPE_NET, "e1000e");

// E1000E uses similar registers to E1000 but with extensions
#define E1000E_REG_CTRL      0x0000
#define E1000E_REG_STATUS    0x0008
#define E1000E_REG_EECD      0x0010
#define E1000E_REG_EERD      0x0014
#define E1000E_REG_CTRL_EXT  0x0018
#define E1000E_REG_ICR       0x00C0
#define E1000E_REG_IMS       0x00D0
#define E1000E_REG_IMC       0x00D8
#define E1000E_REG_RCTL      0x0100
#define E1000E_REG_TCTL      0x0400
#define E1000E_REG_RDBAL     0x2800
#define E1000E_REG_RDBAH     0x2804
#define E1000E_REG_RDLEN     0x2808
#define E1000E_REG_RDH       0x2810
#define E1000E_REG_RDT       0x2818
#define E1000E_REG_TDBAL     0x3800
#define E1000E_REG_TDBAH     0x3804
#define E1000E_REG_TDLEN     0x3808
#define E1000E_REG_TDH       0x3810
#define E1000E_REG_TDT       0x3818
#define E1000E_REG_RAL       0x5400
#define E1000E_REG_RAH       0x5404

#define E1000E_CTRL_RST      (1 << 26)
#define E1000E_CTRL_SLU      (1 << 6)
#define E1000E_RCTL_EN       (1 << 1)
#define E1000E_RCTL_BAM      (1 << 15)
#define E1000E_RCTL_BSIZE_2K (0 << 16)
#define E1000E_RCTL_SECRC    (1 << 26)
#define E1000E_TCTL_EN       (1 << 1)
#define E1000E_TCTL_PSP      (1 << 3)

#define E1000E_NUM_RX_DESC   32
#define E1000E_NUM_TX_DESC   32
#define E1000E_BUFFER_SIZE   2048

#define E1000E_VENDOR_ID     0x8086

// Common E1000E device IDs
static const uint16_t e1000e_devices[] = {
    0x10D3, 0x10CC, 0x10CD, 0x105E, 0x107D, 0x107E, 0x107F,
    0x108B, 0x108C, 0x109A, 0x10A4, 0x10A5, 0x10B9, 0x10BA,
    0x10BB, 0x10BC, 0x10BD, 0x10BF, 0x10C0, 0x10C2, 0x10C3,
    0x10C4, 0x10C5, 0x10CB, 0x10CE, 0x10CF, 0x10D0, 0x10D1,
    0x10D2, 0x10D5, 0x10D9, 0x10DA, 0x10DE, 0x10DF, 0x10E5,
    0x10EA, 0x10EB, 0x10EF, 0x10F0, 0x10F5, 0x1501, 0x1502,
    0x1503
};

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed)) e1000e_desc_t;

typedef struct {
    volatile uint8_t *mmio_base;
    uint8_t mac_addr[6];
    e1000e_desc_t *rx_descs;
    uint8_t *rx_buffers[E1000E_NUM_RX_DESC];
    uint32_t rx_tail;
    e1000e_desc_t *tx_descs;
    uint8_t *tx_buffers[E1000E_NUM_TX_DESC];
    uint32_t tx_tail;
    int link_up;
} e1000e_state_t;

static e1000e_state_t g_e1000e;
static const sqrm_kernel_api_t *g_api;
static const uint16_t COM1_PORT = 0x3F8;

static inline void e1000e_write_reg(uint32_t reg, uint32_t value) {
    *((volatile uint32_t *)(g_e1000e.mmio_base + reg)) = value;
}

static inline uint32_t e1000e_read_reg(uint32_t reg) {
    return *((volatile uint32_t *)(g_e1000e.mmio_base + reg));
}

static int e1000e_read_mac_addr(uint8_t mac_out[6]) {
    if (!mac_out) return -22;
    
    uint32_t ral = e1000e_read_reg(E1000E_REG_RAL);
    uint32_t rah = e1000e_read_reg(E1000E_REG_RAH);
    
    mac_out[0] = ral & 0xFF;
    mac_out[1] = (ral >> 8) & 0xFF;
    mac_out[2] = (ral >> 16) & 0xFF;
    mac_out[3] = (ral >> 24) & 0xFF;
    mac_out[4] = rah & 0xFF;
    mac_out[5] = (rah >> 8) & 0xFF;
    
    return 0;
}

static int e1000e_init_rx(void) {
    g_e1000e.rx_descs = (e1000e_desc_t *)g_api->kmalloc(
        sizeof(e1000e_desc_t) * E1000E_NUM_RX_DESC);
    if (!g_e1000e.rx_descs) return -4;
    
    for (int i = 0; i < E1000E_NUM_RX_DESC; i++) {
        g_e1000e.rx_buffers[i] = (uint8_t *)g_api->kmalloc(E1000E_BUFFER_SIZE);
        if (!g_e1000e.rx_buffers[i]) return -4;
        
        uint64_t phys = g_api->virt_to_phys((uint64_t)g_e1000e.rx_buffers[i]);
        g_e1000e.rx_descs[i].addr = phys;
        g_e1000e.rx_descs[i].status = 0;
    }
    
    uint64_t rx_phys = g_api->virt_to_phys((uint64_t)g_e1000e.rx_descs);
    e1000e_write_reg(E1000E_REG_RDBAL, rx_phys & 0xFFFFFFFF);
    e1000e_write_reg(E1000E_REG_RDBAH, (rx_phys >> 32) & 0xFFFFFFFF);
    e1000e_write_reg(E1000E_REG_RDLEN, E1000E_NUM_RX_DESC * sizeof(e1000e_desc_t));
    e1000e_write_reg(E1000E_REG_RDH, 0);
    e1000e_write_reg(E1000E_REG_RDT, E1000E_NUM_RX_DESC - 1);
    
    g_e1000e.rx_tail = 0;
    
    uint32_t rctl = E1000E_RCTL_EN | E1000E_RCTL_BAM | 
                    E1000E_RCTL_BSIZE_2K | E1000E_RCTL_SECRC;
    e1000e_write_reg(E1000E_REG_RCTL, rctl);
    
    return 0;
}

static int e1000e_init_tx(void) {
    g_e1000e.tx_descs = (e1000e_desc_t *)g_api->kmalloc(
        sizeof(e1000e_desc_t) * E1000E_NUM_TX_DESC);
    if (!g_e1000e.tx_descs) return -4;
    
    for (int i = 0; i < E1000E_NUM_TX_DESC; i++) {
        g_e1000e.tx_buffers[i] = (uint8_t *)g_api->kmalloc(E1000E_BUFFER_SIZE);
        if (!g_e1000e.tx_buffers[i]) return -4;
        g_e1000e.tx_descs[i].status = 1;
    }
    
    uint64_t tx_phys = g_api->virt_to_phys((uint64_t)g_e1000e.tx_descs);
    e1000e_write_reg(E1000E_REG_TDBAL, tx_phys & 0xFFFFFFFF);
    e1000e_write_reg(E1000E_REG_TDBAH, (tx_phys >> 32) & 0xFFFFFFFF);
    e1000e_write_reg(E1000E_REG_TDLEN, E1000E_NUM_TX_DESC * sizeof(e1000e_desc_t));
    e1000e_write_reg(E1000E_REG_TDH, 0);
    e1000e_write_reg(E1000E_REG_TDT, 0);
    
    g_e1000e.tx_tail = 0;
    
    uint32_t tctl = E1000E_TCTL_EN | E1000E_TCTL_PSP | (0x0F << 4) | (0x40 << 12);
    e1000e_write_reg(E1000E_REG_TCTL, tctl);
    
    return 0;
}

static int e1000e_detect_and_init(void) {
    if (!g_api->pci_find_device) return -38;
    
    uint32_t bus = 0, slot = 0, func = 0;
    int found = 0;
    
    for (bus = 0; bus < 256 && !found; bus++) {
        for (slot = 0; slot < 32 && !found; slot++) {
            for (func = 0; func < 8; func++) {
                uint32_t vd = g_api->pci_cfg_read32(bus, slot, func, 0);
                uint16_t vendor = vd & 0xFFFF;
                uint16_t device = (vd >> 16) & 0xFFFF;
                
                if (vendor == E1000E_VENDOR_ID) {
                    for (int i = 0; i < sizeof(e1000e_devices) / sizeof(uint16_t); i++) {
                        if (device == e1000e_devices[i]) {
                            found = 1;
                            break;
                        }
                    }
                    if (found) break;
                }
            }
        }
    }
    
    if (!found) return -1;
    
    bus--; slot--;
    
    if (g_api->com_write_string) {
        g_api->com_write_string(COM1_PORT, "[e1000e] Found Intel E1000E NIC\n");
    }
    
    uint32_t bar0 = g_api->pci_cfg_read32(bus, slot, func, 0x10);
    uint64_t mmio_addr = bar0 & 0xFFFFFFF0;
    
    uint32_t cmd = g_api->pci_cfg_read32(bus, slot, func, 0x04);
    cmd |= 0x6;
    g_api->pci_cfg_write32(bus, slot, func, 0x04, cmd);
    
    g_e1000e.mmio_base = (volatile uint8_t *)g_api->ioremap(mmio_addr, 0x20000);
    if (!g_e1000e.mmio_base) return -4;
    
    e1000e_write_reg(E1000E_REG_CTRL, E1000E_CTRL_RST);
    for (volatile int i = 0; i < 10000; i++);
    
    e1000e_write_reg(E1000E_REG_IMC, 0xFFFFFFFF);
    e1000e_read_reg(E1000E_REG_ICR);
    
    e1000e_read_mac_addr(g_e1000e.mac_addr);
    
    int ret = e1000e_init_rx();
    if (ret < 0) return ret;
    
    ret = e1000e_init_tx();
    if (ret < 0) return ret;
    
    uint32_t ctrl = e1000e_read_reg(E1000E_REG_CTRL);
    ctrl |= E1000E_CTRL_SLU;
    e1000e_write_reg(E1000E_REG_CTRL, ctrl);
    
    uint32_t status = e1000e_read_reg(E1000E_REG_STATUS);
    g_e1000e.link_up = (status & (1 << 1)) ? 1 : 0;
    
    return 0;
}

static int net_get_link_up(void) {
    uint32_t status = e1000e_read_reg(E1000E_REG_STATUS);
    return (status & (1 << 1)) ? 1 : 0;
}

static int net_get_mtu(uint32_t *out) {
    if (!out) return -22;
    *out = 1500;
    return 0;
}

static int net_get_mac(uint8_t out_mac[6]) {
    if (!out_mac) return -22;
    for (int i = 0; i < 6; i++) {
        out_mac[i] = g_e1000e.mac_addr[i];
    }
    return 0;
}

static int net_tx_frame(const void *frame, size_t len) {
    if (!frame || len == 0 || len > E1000E_BUFFER_SIZE) return -22;
    
    uint32_t tail = g_e1000e.tx_tail;
    e1000e_desc_t *desc = &g_e1000e.tx_descs[tail];
    
    if (!(desc->status & 1)) return -11;
    
    for (size_t i = 0; i < len; i++) {
        g_e1000e.tx_buffers[tail][i] = ((const uint8_t *)frame)[i];
    }
    
    uint64_t phys = g_api->virt_to_phys((uint64_t)g_e1000e.tx_buffers[tail]);
    desc->addr = phys;
    desc->length = len;
    desc->status = 0;
    desc->checksum = 0;
    desc->special = 0;
    *((uint8_t *)desc + 11) = 0x09;
    
    tail = (tail + 1) % E1000E_NUM_TX_DESC;
    g_e1000e.tx_tail = tail;
    e1000e_write_reg(E1000E_REG_TDT, tail);
    
    return 0;
}

static int net_rx_poll(void *out_frame, size_t out_cap, size_t *out_len) {
    if (!out_frame || !out_len) return -22;
    
    uint32_t tail = g_e1000e.rx_tail;
    e1000e_desc_t *desc = &g_e1000e.rx_descs[tail];
    
    if (!(desc->status & 1)) {
        *out_len = 0;
        return 0;
    }
    
    uint16_t len = desc->length;
    if (len > out_cap) {
        *out_len = 0;
        return -28;
    }
    
    for (uint16_t i = 0; i < len; i++) {
        ((uint8_t *)out_frame)[i] = g_e1000e.rx_buffers[tail][i];
    }
    
    *out_len = len;
    return 0;
}

static int net_rx_consume(void) {
    uint32_t tail = g_e1000e.rx_tail;
    g_e1000e.rx_descs[tail].status = 0;
    tail = (tail + 1) % E1000E_NUM_RX_DESC;
    g_e1000e.rx_tail = tail;
    e1000e_write_reg(E1000E_REG_RDT, (tail - 1) % E1000E_NUM_RX_DESC);
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
    
    g_api = api;
    
    if (g_api->com_write_string) {
        g_api->com_write_string(COM1_PORT, "[e1000e] Intel E1000E driver initializing\n");
    }
    
    int ret = e1000e_detect_and_init();
    if (ret < 0) {
        if (g_api->com_write_string) {
            g_api->com_write_string(COM1_PORT, "[e1000e] No E1000E device found\n");
        }
        return -1;
    }
    
    if (g_api->sqrm_service_register) {
        ret = g_api->sqrm_service_register("net", &g_net_api, sizeof(g_net_api));
        if (ret < 0) return -1;
    }
    
    if (g_api->com_write_string) {
        g_api->com_write_string(COM1_PORT, "[e1000e] Driver initialized successfully\n");
    }
    
    return 0;
}