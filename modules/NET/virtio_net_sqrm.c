//
// virtio_net_sqrm.c - VirtIO Network Device Driver
//
// Part of ModuOS kernel - SQRM network module
//

#include "moduos/kernel/sqrm.h"

/*
 * VirtIO Network Driver
 *
 * VirtIO is the standard virtualization interface used by:
 *   - KVM/QEMU
 *   - VirtualBox (when VirtIO drivers are enabled)
 *   - VMware (with VirtIO support)
 *   - Cloud providers (AWS, GCP, Azure)
 *
 * This is the most important driver for cloud and virtualized environments.
 */

SQRM_DEFINE_MODULE(SQRM_TYPE_NET, "virtio_net");

// VirtIO PCI Vendor/Device IDs
#define VIRTIO_VENDOR_ID        0x1AF4
#define VIRTIO_DEVICE_NET_OLD   0x1000  // Legacy
#define VIRTIO_DEVICE_NET       0x1041  // Modern

// VirtIO Network Feature Bits
#define VIRTIO_NET_F_MAC        (1 << 5)  // MAC address available

// VirtIO Common Configuration Offsets
#define VIRTIO_PCI_CAP_COMMON_CFG   1
#define VIRTIO_PCI_CAP_NOTIFY_CFG   2
#define VIRTIO_PCI_CAP_ISR_CFG      3
#define VIRTIO_PCI_CAP_DEVICE_CFG   4

// Device Status Bits
#define VIRTIO_STATUS_ACKNOWLEDGE   1
#define VIRTIO_STATUS_DRIVER        2
#define VIRTIO_STATUS_DRIVER_OK     4
#define VIRTIO_STATUS_FEATURES_OK   8
#define VIRTIO_STATUS_FAILED        128

// Queue Configuration
#define VIRTIO_QUEUE_RX         0
#define VIRTIO_QUEUE_TX         1
#define VIRTIO_QUEUE_SIZE       128

// VirtIO Ring Descriptor Flags
#define VIRTQ_DESC_F_NEXT       1
#define VIRTQ_DESC_F_WRITE      2

// VirtIO Header (prepended to each packet)
typedef struct {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
} __attribute__((packed)) virtio_net_hdr_t;

// VirtQueue Descriptor
typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) virtq_desc_t;

// VirtQueue Available Ring
typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTIO_QUEUE_SIZE];
    uint16_t used_event;
} __attribute__((packed)) virtq_avail_t;

// VirtQueue Used Element
typedef struct {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) virtq_used_elem_t;

// VirtQueue Used Ring
typedef struct {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[VIRTIO_QUEUE_SIZE];
    uint16_t avail_event;
} __attribute__((packed)) virtq_used_t;

// VirtQueue
typedef struct {
    virtq_desc_t *desc;
    virtq_avail_t *avail;
    virtq_used_t *used;
    
    uint16_t queue_size;
    uint16_t last_used_idx;
    uint16_t free_head;
    uint16_t num_free;
    
    uint8_t **buffers;
} virtqueue_t;

// Driver State
typedef struct {
    volatile uint8_t *common_cfg;
    volatile uint8_t *notify_base;
    volatile uint8_t *isr_cfg;
    volatile uint8_t *device_cfg;
    
    uint32_t notify_off_multiplier;
    
    uint8_t mac_addr[6];
    
    virtqueue_t rx_queue;
    virtqueue_t tx_queue;
    
    int link_up;
} virtio_net_state_t;

static virtio_net_state_t g_virtio;
static const sqrm_kernel_api_t *g_api;
static const uint16_t COM1_PORT = 0x3F8;

// Common Configuration Register Offsets
#define VIRTIO_CFG_DEVICE_FEATURE_SELECT    0x00
#define VIRTIO_CFG_DEVICE_FEATURE           0x04
#define VIRTIO_CFG_DRIVER_FEATURE_SELECT    0x08
#define VIRTIO_CFG_DRIVER_FEATURE           0x0C
#define VIRTIO_CFG_MSIX_CONFIG              0x10
#define VIRTIO_CFG_NUM_QUEUES               0x12
#define VIRTIO_CFG_DEVICE_STATUS            0x14
#define VIRTIO_CFG_CONFIG_GENERATION        0x15
#define VIRTIO_CFG_QUEUE_SELECT             0x16
#define VIRTIO_CFG_QUEUE_SIZE               0x18
#define VIRTIO_CFG_QUEUE_MSIX_VECTOR        0x1A
#define VIRTIO_CFG_QUEUE_ENABLE             0x1C
#define VIRTIO_CFG_QUEUE_NOTIFY_OFF         0x1E
#define VIRTIO_CFG_QUEUE_DESC               0x20
#define VIRTIO_CFG_QUEUE_DRIVER             0x28
#define VIRTIO_CFG_QUEUE_DEVICE             0x30

static inline void virtio_write8(volatile uint8_t *base, uint32_t offset, uint8_t val) {
    *((volatile uint8_t *)(base + offset)) = val;
}

static inline void virtio_write16(volatile uint8_t *base, uint32_t offset, uint16_t val) {
    *((volatile uint16_t *)(base + offset)) = val;
}

static inline void virtio_write32(volatile uint8_t *base, uint32_t offset, uint32_t val) {
    *((volatile uint32_t *)(base + offset)) = val;
}

static inline void virtio_write64(volatile uint8_t *base, uint32_t offset, uint64_t val) {
    *((volatile uint64_t *)(base + offset)) = val;
}

static inline uint8_t virtio_read8(volatile uint8_t *base, uint32_t offset) {
    return *((volatile uint8_t *)(base + offset));
}

static inline uint16_t virtio_read16(volatile uint8_t *base, uint32_t offset) {
    return *((volatile uint16_t *)(base + offset));
}

static inline uint32_t virtio_read32(volatile uint8_t *base, uint32_t offset) {
    return *((volatile uint32_t *)(base + offset));
}

/**
 * Initialize a VirtQueue
 * 
 * Allocates and sets up descriptor, available, and used rings.
 * 
 * @param vq VirtQueue to initialize
 * @param queue_idx Queue index (0 for RX, 1 for TX)
 * @return 0 on success, negative on error
 */
static int virtio_init_queue(virtqueue_t *vq, uint16_t queue_idx) {
    vq->queue_size = VIRTIO_QUEUE_SIZE;
    vq->last_used_idx = 0;
    vq->free_head = 0;
    vq->num_free = VIRTIO_QUEUE_SIZE;
    
    // Allocate descriptor table
    size_t desc_size = sizeof(virtq_desc_t) * VIRTIO_QUEUE_SIZE;
    vq->desc = (virtq_desc_t *)g_api->kmalloc(desc_size);
    if (!vq->desc) return -4;
    
    // Allocate available ring
    size_t avail_size = sizeof(virtq_avail_t);
    vq->avail = (virtq_avail_t *)g_api->kmalloc(avail_size);
    if (!vq->avail) return -4;
    
    // Allocate used ring
    size_t used_size = sizeof(virtq_used_t);
    vq->used = (virtq_used_t *)g_api->kmalloc(used_size);
    if (!vq->used) return -4;
    
    // Initialize descriptor chain
    for (int i = 0; i < VIRTIO_QUEUE_SIZE; i++) {
        vq->desc[i].next = (i + 1) % VIRTIO_QUEUE_SIZE;
        vq->desc[i].flags = 0;
    }
    
    // Allocate buffers array
    vq->buffers = (uint8_t **)g_api->kmalloc(sizeof(uint8_t *) * VIRTIO_QUEUE_SIZE);
    if (!vq->buffers) return -4;
    
    for (int i = 0; i < VIRTIO_QUEUE_SIZE; i++) {
        vq->buffers[i] = NULL;
    }
    
    // Select queue and configure it
    virtio_write16(g_virtio.common_cfg, VIRTIO_CFG_QUEUE_SELECT, queue_idx);
    virtio_write16(g_virtio.common_cfg, VIRTIO_CFG_QUEUE_SIZE, VIRTIO_QUEUE_SIZE);
    
    // Set queue addresses
    uint64_t desc_phys = g_api->virt_to_phys((uint64_t)vq->desc);
    uint64_t avail_phys = g_api->virt_to_phys((uint64_t)vq->avail);
    uint64_t used_phys = g_api->virt_to_phys((uint64_t)vq->used);
    
    virtio_write64(g_virtio.common_cfg, VIRTIO_CFG_QUEUE_DESC, desc_phys);
    virtio_write64(g_virtio.common_cfg, VIRTIO_CFG_QUEUE_DRIVER, avail_phys);
    virtio_write64(g_virtio.common_cfg, VIRTIO_CFG_QUEUE_DEVICE, used_phys);
    
    // Enable queue
    virtio_write16(g_virtio.common_cfg, VIRTIO_CFG_QUEUE_ENABLE, 1);
    
    return 0;
}

/**
 * Fill RX queue with buffers
 * 
 * Pre-populates the receive queue with available buffers.
 * 
 * @return 0 on success, negative on error
 */
static int virtio_fill_rx_queue(void) {
    for (int i = 0; i < VIRTIO_QUEUE_SIZE - 1; i++) {
        // Allocate buffer (header + packet data)
        uint8_t *buf = (uint8_t *)g_api->kmalloc(sizeof(virtio_net_hdr_t) + 1514);
        if (!buf) return -4;
        
        g_virtio.rx_queue.buffers[i] = buf;
        
        // Set up descriptor
        uint64_t buf_phys = g_api->virt_to_phys((uint64_t)buf);
        g_virtio.rx_queue.desc[i].addr = buf_phys;
        g_virtio.rx_queue.desc[i].len = sizeof(virtio_net_hdr_t) + 1514;
        g_virtio.rx_queue.desc[i].flags = VIRTQ_DESC_F_WRITE;
        
        // Add to available ring
        uint16_t avail_idx = g_virtio.rx_queue.avail->idx;
        g_virtio.rx_queue.avail->ring[avail_idx % VIRTIO_QUEUE_SIZE] = i;
        g_virtio.rx_queue.avail->idx = avail_idx + 1;
    }
    
    return 0;
}

/**
 * Detect and initialize VirtIO network device
 * 
 * @return 0 on success, negative on error
 */
static int virtio_detect_and_init(void) {
    if (!g_api->pci_find_device) return -38;
    
    uint32_t bus = 0, slot = 0, func = 0;
    int found = 0;
    
    for (bus = 0; bus < 256 && !found; bus++) {
        for (slot = 0; slot < 32 && !found; slot++) {
            for (func = 0; func < 8; func++) {
                uint32_t vd = g_api->pci_cfg_read32(bus, slot, func, 0);
                uint16_t vendor = vd & 0xFFFF;
                uint16_t device = (vd >> 16) & 0xFFFF;
                
                if (vendor == VIRTIO_VENDOR_ID && 
                    (device == VIRTIO_DEVICE_NET || device == VIRTIO_DEVICE_NET_OLD)) {
                    found = 1;
                    break;
                }
            }
        }
    }
    
    if (!found) return -1;
    
    bus--; slot--;
    
    if (g_api->com_write_string) {
        g_api->com_write_string(COM1_PORT, "[virtio_net] Found VirtIO network device\n");
    }
    
    // Enable bus mastering and memory space
    uint32_t cmd = g_api->pci_cfg_read32(bus, slot, func, 0x04);
    cmd |= 0x6;
    g_api->pci_cfg_write32(bus, slot, func, 0x04, cmd);
    
    // Find capability structures
    uint8_t cap_ptr = g_api->pci_cfg_read32(bus, slot, func, 0x34) & 0xFF;
    
    while (cap_ptr != 0) {
        uint32_t cap_data = g_api->pci_cfg_read32(bus, slot, func, cap_ptr);
        uint8_t cap_id = cap_data & 0xFF;
        uint8_t cap_next = (cap_data >> 8) & 0xFF;
        
        if (cap_id == 0x09) {  // Vendor-specific capability
            uint32_t cfg_type = g_api->pci_cfg_read32(bus, slot, func, cap_ptr + 4);
            uint8_t type = cfg_type & 0xFF;
            uint8_t bar = (cfg_type >> 8) & 0xFF;
            uint32_t offset = g_api->pci_cfg_read32(bus, slot, func, cap_ptr + 8);
            
            uint32_t bar_reg = g_api->pci_cfg_read32(bus, slot, func, 0x10 + bar * 4);
            uint64_t bar_addr = bar_reg & 0xFFFFFFF0;
            
            volatile uint8_t *mapped = (volatile uint8_t *)g_api->ioremap(bar_addr, 0x1000);
            if (!mapped) return -4;
            
            if (type == VIRTIO_PCI_CAP_COMMON_CFG) {
                g_virtio.common_cfg = mapped + offset;
            } else if (type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                g_virtio.notify_base = mapped + offset;
                uint32_t mult = g_api->pci_cfg_read32(bus, slot, func, cap_ptr + 16);
                g_virtio.notify_off_multiplier = mult;
            } else if (type == VIRTIO_PCI_CAP_ISR_CFG) {
                g_virtio.isr_cfg = mapped + offset;
            } else if (type == VIRTIO_PCI_CAP_DEVICE_CFG) {
                g_virtio.device_cfg = mapped + offset;
            }
        }
        
        cap_ptr = cap_next;
    }
    
    if (!g_virtio.common_cfg) return -1;
    
    // Reset device
    virtio_write8(g_virtio.common_cfg, VIRTIO_CFG_DEVICE_STATUS, 0);
    
    // Set ACKNOWLEDGE bit
    uint8_t status = VIRTIO_STATUS_ACKNOWLEDGE;
    virtio_write8(g_virtio.common_cfg, VIRTIO_CFG_DEVICE_STATUS, status);
    
    // Set DRIVER bit
    status |= VIRTIO_STATUS_DRIVER;
    virtio_write8(g_virtio.common_cfg, VIRTIO_CFG_DEVICE_STATUS, status);
    
    // Negotiate features
    virtio_write32(g_virtio.common_cfg, VIRTIO_CFG_DRIVER_FEATURE_SELECT, 0);
    virtio_write32(g_virtio.common_cfg, VIRTIO_CFG_DRIVER_FEATURE, VIRTIO_NET_F_MAC);
    
    status |= VIRTIO_STATUS_FEATURES_OK;
    virtio_write8(g_virtio.common_cfg, VIRTIO_CFG_DEVICE_STATUS, status);
    
    // Read MAC address from device config
    if (g_virtio.device_cfg) {
        for (int i = 0; i < 6; i++) {
            g_virtio.mac_addr[i] = virtio_read8(g_virtio.device_cfg, i);
        }
    } else {
        // Default MAC
        g_virtio.mac_addr[0] = 0x52;
        g_virtio.mac_addr[1] = 0x54;
        g_virtio.mac_addr[2] = 0x00;
        g_virtio.mac_addr[3] = 0x12;
        g_virtio.mac_addr[4] = 0x34;
        g_virtio.mac_addr[5] = 0x56;
    }
    
    // Initialize queues
    int ret = virtio_init_queue(&g_virtio.rx_queue, VIRTIO_QUEUE_RX);
    if (ret < 0) return ret;
    
    ret = virtio_init_queue(&g_virtio.tx_queue, VIRTIO_QUEUE_TX);
    if (ret < 0) return ret;
    
    // Fill RX queue with buffers
    ret = virtio_fill_rx_queue();
    if (ret < 0) return ret;
    
    // Set DRIVER_OK
    status |= VIRTIO_STATUS_DRIVER_OK;
    virtio_write8(g_virtio.common_cfg, VIRTIO_CFG_DEVICE_STATUS, status);
    
    g_virtio.link_up = 1;
    
    return 0;
}

static int net_get_link_up(void) {
    return g_virtio.link_up;
}

static int net_get_mtu(uint32_t *out) {
    if (!out) return -22;
    *out = 1500;
    return 0;
}

static int net_get_mac(uint8_t out_mac[6]) {
    if (!out_mac) return -22;
    for (int i = 0; i < 6; i++) {
        out_mac[i] = g_virtio.mac_addr[i];
    }
    return 0;
}

static int net_tx_frame(const void *frame, size_t len) {
    if (!frame || len == 0 || len > 1514) return -22;
    
    if (g_virtio.tx_queue.num_free < 1) return -11;
    
    // Allocate buffer with VirtIO header
    uint8_t *buf = (uint8_t *)g_api->kmalloc(sizeof(virtio_net_hdr_t) + len);
    if (!buf) return -4;
    
    // Zero header
    virtio_net_hdr_t *hdr = (virtio_net_hdr_t *)buf;
    for (size_t i = 0; i < sizeof(virtio_net_hdr_t); i++) {
        buf[i] = 0;
    }
    
    // Copy frame data
    for (size_t i = 0; i < len; i++) {
        buf[sizeof(virtio_net_hdr_t) + i] = ((const uint8_t *)frame)[i];
    }
    
    // Get free descriptor
    uint16_t desc_idx = g_virtio.tx_queue.free_head;
    g_virtio.tx_queue.free_head = g_virtio.tx_queue.desc[desc_idx].next;
    g_virtio.tx_queue.num_free--;
    
    // Set up descriptor
    uint64_t buf_phys = g_api->virt_to_phys((uint64_t)buf);
    g_virtio.tx_queue.desc[desc_idx].addr = buf_phys;
    g_virtio.tx_queue.desc[desc_idx].len = sizeof(virtio_net_hdr_t) + len;
    g_virtio.tx_queue.desc[desc_idx].flags = 0;
    
    g_virtio.tx_queue.buffers[desc_idx] = buf;
    
    // Add to available ring
    uint16_t avail_idx = g_virtio.tx_queue.avail->idx;
    g_virtio.tx_queue.avail->ring[avail_idx % VIRTIO_QUEUE_SIZE] = desc_idx;
    g_virtio.tx_queue.avail->idx = avail_idx + 1;
    
    // Notify device (kick)
    if (g_virtio.notify_base) {
        virtio_write16(g_virtio.notify_base, 0, VIRTIO_QUEUE_TX);
    }
    
    return 0;
}

static int net_rx_poll(void *out_frame, size_t out_cap, size_t *out_len) {
    if (!out_frame || !out_len) return -22;
    
    uint16_t last_idx = g_virtio.rx_queue.last_used_idx;
    uint16_t used_idx = g_virtio.rx_queue.used->idx;
    
    if (last_idx == used_idx) {
        *out_len = 0;
        return 0;
    }
    
    virtq_used_elem_t *elem = &g_virtio.rx_queue.used->ring[last_idx % VIRTIO_QUEUE_SIZE];
    uint16_t desc_idx = elem->id;
    uint32_t total_len = elem->len;
    
    if (total_len < sizeof(virtio_net_hdr_t)) {
        *out_len = 0;
        return 0;
    }
    
    uint32_t data_len = total_len - sizeof(virtio_net_hdr_t);
    if (data_len > out_cap) {
        *out_len = 0;
        return -28;
    }
    
    uint8_t *buf = g_virtio.rx_queue.buffers[desc_idx];
    for (uint32_t i = 0; i < data_len; i++) {
        ((uint8_t *)out_frame)[i] = buf[sizeof(virtio_net_hdr_t) + i];
    }
    
    *out_len = data_len;
    return 0;
}

static int net_rx_consume(void) {
    uint16_t last_idx = g_virtio.rx_queue.last_used_idx;
    virtq_used_elem_t *elem = &g_virtio.rx_queue.used->ring[last_idx % VIRTIO_QUEUE_SIZE];
    uint16_t desc_idx = elem->id;
    
    // Re-add buffer to available ring
    uint16_t avail_idx = g_virtio.rx_queue.avail->idx;
    g_virtio.rx_queue.avail->ring[avail_idx % VIRTIO_QUEUE_SIZE] = desc_idx;
    g_virtio.rx_queue.avail->idx = avail_idx + 1;
    
    g_virtio.rx_queue.last_used_idx++;
    
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
        g_api->com_write_string(COM1_PORT, "[virtio_net] VirtIO network driver initializing\n");
    }
    
    int ret = virtio_detect_and_init();
    if (ret < 0) {
        if (g_api->com_write_string) {
            g_api->com_write_string(COM1_PORT, "[virtio_net] No VirtIO device found\n");
        }
        return -1;
    }
    
    if (g_api->sqrm_service_register) {
        ret = g_api->sqrm_service_register("net", &g_net_api, sizeof(g_net_api));
        if (ret < 0) return -1;
    }
    
    if (g_api->com_write_string) {
        g_api->com_write_string(COM1_PORT, "[virtio_net] Driver initialized successfully\n");
    }
    
    return 0;
}