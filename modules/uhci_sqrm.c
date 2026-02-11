#include <stdint.h>
#include <stddef.h>

// log helpers (defined later)
static void log_hex16(uint16_t v);

#include "moduos/kernel/sqrm.h"
#include "moduos/kernel/errno.h"

// Debug controls
//  - UHCI_DEBUG enables failure-only dumps (recommended to keep on)
//  - UHCI_DEBUG_VERBOSE enables extra logs
#define UHCI_DEBUG 1
#define UHCI_DEBUG_VERBOSE 0

SQRM_DEFINE_MODULE_V2(SQRM_TYPE_USB, "uhci", 3, 0, 0, NULL);

// --- UHCI register offsets ---
#define UHCI_REG_USBCMD    0x00
#define UHCI_REG_USBSTS    0x02
#define UHCI_REG_USBINTR   0x04
#define UHCI_REG_FRNUM     0x06
#define UHCI_REG_FRBASEADD 0x08
#define UHCI_REG_SOFMOD    0x0C
#define UHCI_REG_PORTSC1   0x10
#define UHCI_REG_PORTSC2   0x12

// USBCMD bits
#define UHCI_CMD_RS      (1u << 0)
#define UHCI_CMD_HCRESET (1u << 1)
#define UHCI_CMD_CF      (1u << 6)
#define UHCI_CMD_MAXP    (1u << 7) /* 0=32-byte, 1=64-byte max packet */

// USBSTS bits
#define UHCI_STS_HCH     (1u << 5)

// USBINTR bits
#define UHCI_INTR_TIMEOUT (1u << 0)
#define UHCI_INTR_RESUME  (1u << 1)
#define UHCI_INTR_IOC     (1u << 2)
#define UHCI_INTR_SP      (1u << 3)

// PORTSC bits
#define UHCI_PORT_CCS   (1u << 0)
#define UHCI_PORT_PED   (1u << 2)
#define UHCI_PORT_LSDA  (1u << 8)
#define UHCI_PORT_PR    (1u << 9)

// Frame list
#define UHCI_FRAMELIST_COUNT 1024

// Link pointer bits
#define UHCI_LINK_TERMINATE (1u << 0)
#define UHCI_LINK_QH        (1u << 1)
#define UHCI_LINK_VF        (1u << 2) // depth-first traversal

// TD status bits
#define UHCI_TD_STATUS_ACTIVE  (1u << 23)
#define UHCI_TD_STATUS_IOC     (1u << 24)
#define UHCI_TD_STATUS_LS      (1u << 26)
#define UHCI_TD_STATUS_SPD     (1u << 29) // short packet detect

// PID
#define UHCI_PID_IN    0x69u
#define UHCI_PID_OUT   0xE1u
#define UHCI_PID_SETUP 0x2Du

typedef struct {
    uint32_t link_ptr;
    uint32_t status;
    uint32_t token;
    uint32_t buffer_ptr;
    uint32_t sw[4];
} __attribute__((aligned(16))) uhci_td_t;

typedef struct {
    uint32_t head_link_ptr;
    uint32_t element_link_ptr;
    uint32_t sw[6];
} __attribute__((aligned(16))) uhci_qh_t;

// API structs (must match sdk/sqrm_sdk.h layout)
typedef enum {
    SQRM_USB_SPEED_LOW  = 1,
    SQRM_USB_SPEED_FULL = 2,
    SQRM_USB_SPEED_HIGH = 3,
} sqrm_usb_speed_t;

typedef enum {
    SQRM_USB_XFER_CONTROL   = 1,
    SQRM_USB_XFER_BULK      = 2,
    SQRM_USB_XFER_INTERRUPT = 3,
} sqrm_usb_xfer_type_t;

typedef struct {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) sqrm_usb_setup_packet_t;

typedef struct {
    uint8_t dev_addr;
    uint8_t endpoint;
    uint8_t speed;
    uint8_t xfer_type;
    sqrm_usb_setup_packet_t setup;
    void   *data;
    uint32_t length;
    uint8_t direction_in;
    int32_t status;
    uint32_t actual_length;
} sqrm_usb_transfer_v1_t;

typedef uint32_t sqrm_usb_xfer_handle_t;
#define SQRM_USB_XFER_INVALID_HANDLE 0u

typedef struct {
    uint8_t bus, device, function;
    uint8_t irq_line;
    uint16_t io_base;
} sqrm_uhci_controller_info_v1_t;

typedef void (*sqrm_usb_xfer_cb_v1_t)(sqrm_usb_xfer_handle_t handle, sqrm_usb_transfer_v1_t *xfer, void *user);

typedef struct {
    int (*get_controller_count)(void);
    int (*get_controller_info)(int index, sqrm_uhci_controller_info_v1_t *out);

    // port ops
    int (*get_port_count)(int controller_index);
    uint16_t (*read_portsc)(int controller_index, int port_index);
    int (*reset_port)(int controller_index, int port_index);

    // transfers
    sqrm_usb_xfer_handle_t (*submit)(int controller_index, sqrm_usb_transfer_v1_t *xfer);
    int (*wait)(sqrm_usb_xfer_handle_t handle, uint32_t timeout_ms);
    int (*cancel)(sqrm_usb_xfer_handle_t handle);

    // async completion (IRQ-driven)
    int (*set_callback)(sqrm_usb_xfer_handle_t handle, sqrm_usb_xfer_cb_v1_t cb, void *user);
} sqrm_usbctl_uhci_api_v1_t;

static const sqrm_kernel_api_t *g_api;

typedef struct {
    int present;
    sqrm_uhci_controller_info_v1_t info;

    dma_buffer_t framelist_dma;
    dma_buffer_t qh_dma;
    dma_buffer_t td_dma;

    uhci_qh_t *qh_interrupt;
    uhci_qh_t *qh_control;
    uhci_qh_t *qh_bulk;
    uhci_td_t *td_pool;
    uint32_t td_pool_count;
    uint32_t td_alloc_cursor;
} uhci_ctrl_t;

static uhci_ctrl_t g_ctrls[8];
static int g_ctrl_count;

typedef struct {
    int in_use;
    int ctrl_idx;
    sqrm_usb_transfer_v1_t *xfer;

    dma_buffer_t setup_dma;
    dma_buffer_t data_dma;

    uhci_td_t *td_first;
    uhci_td_t *td_last;

    // async completion
    sqrm_usb_xfer_cb_v1_t cb;
    void *cb_user;
} uhci_handle_t;

static uhci_handle_t g_handles[64];

static void log_str(const char *s) {
    if (g_api && g_api->com_write_string) g_api->com_write_string(0x3F8, s);
}

static uint16_t io_read16(uint16_t base, uint16_t reg) {
    return g_api->inw((uint16_t)(base + reg));
}
static void io_write16(uint16_t base, uint16_t reg, uint16_t v) {
    g_api->outw((uint16_t)(base + reg), v);
}
static void io_write32(uint16_t base, uint16_t reg, uint32_t v) {
    g_api->outl((uint16_t)(base + reg), v);
}

// Delay helper that does NOT rely on timer interrupts.
// Uses port 0x80 (legacy POST delay port) which QEMU emulates.
static void delay_ms_fallback(uint32_t ms) {
    if (!g_api || !g_api->inb) return;
    for (uint32_t m = 0; m < ms; m++) {
        for (volatile uint32_t i = 0; i < 1000; i++) {
            (void)g_api->inb(0x80);
        }
    }
}

static uint32_t td_token(uint8_t pid, uint8_t addr, uint8_t ep, uint8_t toggle, uint16_t max_len) {
    // UHCI token:
    // [7:0] PID, [14:8] addr, [18:15] ep, [19] toggle, [31:21] max_len-1 (or 0x7FF for null)
    uint32_t t = 0;
    t |= (uint32_t)pid;
    t |= ((uint32_t)(addr & 0x7F) << 8);
    t |= ((uint32_t)(ep & 0x0F) << 15);
    t |= ((uint32_t)(toggle & 1) << 19);
    uint16_t ml = (max_len == 0) ? 0x7FFu : (uint16_t)(max_len - 1);
    t |= ((uint32_t)(ml & 0x7FFu) << 21);
    return t;
}

static uhci_td_t* td_alloc(uhci_ctrl_t *c) {
    if (!c || !c->td_pool || c->td_pool_count == 0) return NULL;
    // very simple linear allocator for bring-up (wraps)
    uhci_td_t *td = &c->td_pool[c->td_alloc_cursor++ % c->td_pool_count];
    // zero td
    td->link_ptr = UHCI_LINK_TERMINATE;
    td->status = 0;
    td->token = 0;
    td->buffer_ptr = 0;
    return td;
}

static uint32_t td_phys(uhci_ctrl_t *c, uhci_td_t *td) {
    uintptr_t off = (uintptr_t)((uint8_t*)td - (uint8_t*)c->td_pool);
    return (uint32_t)(c->td_dma.phys + off);
}

static uint32_t qh_phys(uhci_ctrl_t *c, uhci_qh_t *qh) {
    uintptr_t off = (uintptr_t)((uint8_t*)qh - (uint8_t*)c->qh_interrupt);
    return (uint32_t)(c->qh_dma.phys + off);
}

static int uhci_reset_hw(uhci_ctrl_t *c) {
    log_str("[UHCI] reset hw...\n");
    uint16_t base = c->info.io_base;

    // Stop
    io_write16(base, UHCI_REG_USBCMD, 0);

    // Wait for controller halted (HCH). Don't rely on sleep_ms here; just poll with a timeout.
    for (int i = 0; i < 100000; i++) {
        uint16_t sts = io_read16(base, UHCI_REG_USBSTS);
        if (sts & UHCI_STS_HCH) break;
    }

    // HC reset
    io_write16(base, UHCI_REG_USBCMD, UHCI_CMD_HCRESET);

    // Wait for HCRESET to clear with a hard upper bound. If it never clears, don't hang the boot.
    int cleared = 0;
    for (int i = 0; i < 1000000; i++) {
        uint16_t cmd = io_read16(base, UHCI_REG_USBCMD);
        if ((cmd & UHCI_CMD_HCRESET) == 0) { cleared = 1; break; }
    }
    if (!cleared) {
        log_str("[UHCI] WARN: HCRESET did not clear (continuing)\n");
    }

    // clear status
    io_write16(base, UHCI_REG_USBSTS, 0xFFFF);
    log_str("[UHCI] reset hw done\n");
    return 0;
}

static void uhci_complete_handle(int hidx) {
    uhci_handle_t *h = &g_handles[hidx];
    if (!h->in_use || !h->xfer) return;

    // Completion: copy IN data
    if (h->xfer->length && h->xfer->direction_in && h->data_dma.virt) {
        for (uint32_t i = 0; i < h->xfer->length; i++) ((uint8_t*)h->xfer->data)[i] = ((uint8_t*)h->data_dma.virt)[i];
        h->xfer->actual_length = h->xfer->length;
    }

    // Free DMA bounces
    if (h->xfer->length) g_api->dma_free(&h->data_dma);
    if (h->setup_dma.virt) g_api->dma_free(&h->setup_dma);

    sqrm_usb_xfer_cb_v1_t cb = h->cb;
    void *user = h->cb_user;
    sqrm_usb_transfer_v1_t *xfer = h->xfer;

    h->in_use = 0;
    h->cb = NULL;
    h->cb_user = NULL;
    h->xfer = NULL;

    if (cb) {
        uintptr_t p = (uintptr_t)cb;
        uint64_t top = ((uint64_t)p) >> 48;
        int canonical = (top == 0x0000u || top == 0xFFFFu);
        if (!canonical || p < 0x100000u) {
            if (g_api && g_api->com_write_string) {
                g_api->com_write_string(0x3F8, "[UHCI] refusing to call non-canonical cb\n");
            }
            return;
        }
        cb((sqrm_usb_xfer_handle_t)(hidx + 1), xfer, user);
    }
}

static void uhci_irq_handler(void) {
    if (g_api && g_api->com_write_string) g_api->com_write_string(0x3F8, "[UHCI] irq\n");
    // Shared handler: scan all controllers/handles.
    for (int ci = 0; ci < g_ctrl_count; ci++) {
        uhci_ctrl_t *c = &g_ctrls[ci];
        if (!c->present) continue;

        uint16_t sts = io_read16(c->info.io_base, UHCI_REG_USBSTS);
        if (sts == 0) continue;

        // Ack all bits.
        io_write16(c->info.io_base, UHCI_REG_USBSTS, 0xFFFF);

        // Complete any finished handles on this controller.
        for (int hi = 0; hi < (int)(sizeof(g_handles)/sizeof(g_handles[0])); hi++) {
            uhci_handle_t *h = &g_handles[hi];
            if (!h->in_use) continue;
            if (h->ctrl_idx != ci) continue;
            if (!h->td_last) continue;
            if ((h->td_last->status & UHCI_TD_STATUS_ACTIVE) == 0) {
                uhci_complete_handle(hi);
            }
        }

        // EOI
        if (g_api && g_api->pic_send_eoi) {
            g_api->pic_send_eoi(c->info.irq_line);
        }
    }
}

static int uhci_init_controller(uhci_ctrl_t *c) {
    if (!c || !c->present) return -EINVAL;
    uint16_t base = c->info.io_base;

    log_str("[UHCI] init controller io_base=");
    log_hex16(base);
    log_str("\n");

    if (!g_api->dma_alloc || !g_api->dma_free) return -ENOSYS;

    // Enable PCI IO + bus mastering (needed for DMA)
    if (g_api->pci_find_device) {
        pci_device_t *pdev = g_api->pci_find_device(0, 0); // unused
        (void)pdev;
    }

    (void)uhci_reset_hw(c);

    // Allocate frame list (4k aligned)
    if (g_api->dma_alloc(&c->framelist_dma, UHCI_FRAMELIST_COUNT * sizeof(uint32_t), 4096) != 0) {
        log_str("[UHCI] dma_alloc framelist failed\n");
        return -ENOMEM;
    }

    // Allocate QHs (interrupt, control, bulk) in one DMA block
    if (g_api->dma_alloc(&c->qh_dma, 3 * sizeof(uhci_qh_t), 16) != 0) {
        log_str("[UHCI] dma_alloc qh failed\n");
        return -ENOMEM;
    }

    c->qh_interrupt = (uhci_qh_t*)c->qh_dma.virt;
    c->qh_control = (uhci_qh_t*)((uint8_t*)c->qh_dma.virt + sizeof(uhci_qh_t));
    c->qh_bulk = (uhci_qh_t*)((uint8_t*)c->qh_dma.virt + 2 * sizeof(uhci_qh_t));

    // Allocate TD pool
    c->td_pool_count = 256;
    if (g_api->dma_alloc(&c->td_dma, c->td_pool_count * sizeof(uhci_td_t), 16) != 0) {
        log_str("[UHCI] dma_alloc td pool failed\n");
        return -ENOMEM;
    }
    c->td_pool = (uhci_td_t*)c->td_dma.virt;
    c->td_alloc_cursor = 0;

    // init QHs
    c->qh_interrupt->head_link_ptr = (qh_phys(c, c->qh_control) | UHCI_LINK_QH | UHCI_LINK_VF);
    c->qh_interrupt->element_link_ptr = UHCI_LINK_TERMINATE;

    c->qh_control->head_link_ptr = (qh_phys(c, c->qh_bulk) | UHCI_LINK_QH | UHCI_LINK_VF);
    c->qh_control->element_link_ptr = UHCI_LINK_TERMINATE;

    c->qh_bulk->head_link_ptr = UHCI_LINK_TERMINATE;
    c->qh_bulk->element_link_ptr = UHCI_LINK_TERMINATE;

    // framelist points to interrupt qh
    uint32_t *fl = (uint32_t*)c->framelist_dma.virt;
    uint32_t iqh = qh_phys(c, c->qh_interrupt) | UHCI_LINK_QH | UHCI_LINK_VF;
    for (int i = 0; i < UHCI_FRAMELIST_COUNT; i++) fl[i] = iqh;

    // program hardware
    io_write32(base, UHCI_REG_FRBASEADD, (uint32_t)c->framelist_dma.phys);
    io_write16(base, UHCI_REG_FRNUM, 0);
    io_write16(base, UHCI_REG_SOFMOD, 64);

    // enable interrupts
    io_write16(base, UHCI_REG_USBINTR, (uint16_t)(UHCI_INTR_TIMEOUT | UHCI_INTR_IOC | UHCI_INTR_SP));

    // Install shared IRQ handler
    if (g_api->irq_install_handler) {
        g_api->irq_install_handler((int)c->info.irq_line, uhci_irq_handler);
    }

    // run
    // Set MAXP so the HC uses 64-byte max packet for full-speed control/bulk.
    // Without this, QEMU can fail to complete control transfers during enumeration.
    uint16_t cmd = (uint16_t)(UHCI_CMD_RS | UHCI_CMD_CF | UHCI_CMD_MAXP);
    io_write16(base, UHCI_REG_USBCMD, cmd);

    return 0;
}

static int api_get_controller_count(void) { return g_ctrl_count; }

static int api_get_controller_info(int index, sqrm_uhci_controller_info_v1_t *out) {
    if (!out) return -EINVAL;
    if (index < 0 || index >= g_ctrl_count) return -EINVAL;
    *out = g_ctrls[index].info;
    return 0;
}

static int api_get_port_count(int controller_index) {
    if (controller_index < 0 || controller_index >= g_ctrl_count) return -EINVAL;
    return 2; // UHCI has PORTSC1/2
}

static uint16_t api_read_portsc(int controller_index, int port_index) {
    if (controller_index < 0 || controller_index >= g_ctrl_count) return 0;
    uhci_ctrl_t *c = &g_ctrls[controller_index];
    uint16_t reg = (port_index == 0) ? UHCI_REG_PORTSC1 : UHCI_REG_PORTSC2;
    return io_read16(c->info.io_base, reg);
}

static int api_reset_port(int controller_index, int port_index) {
    if (controller_index < 0 || controller_index >= g_ctrl_count) return -EINVAL;
    uhci_ctrl_t *c = &g_ctrls[controller_index];
    uint16_t base = c->info.io_base;
    uint16_t reg = (port_index == 0) ? UHCI_REG_PORTSC1 : UHCI_REG_PORTSC2;

    uint16_t st = io_read16(base, reg);
    if (!(st & UHCI_PORT_CCS)) return 0;

    // reset pulse ~50ms
    io_write16(base, reg, (uint16_t)(st | UHCI_PORT_PR));
    delay_ms_fallback(50);

    st = io_read16(base, reg);
    io_write16(base, reg, (uint16_t)(st & ~UHCI_PORT_PR));
    delay_ms_fallback(10);

    // enable
    st = io_read16(base, reg);
    io_write16(base, reg, (uint16_t)(st | UHCI_PORT_PED));
    delay_ms_fallback(10);

    return 0;
}

static sqrm_usb_xfer_handle_t api_submit(int controller_index, sqrm_usb_transfer_v1_t *xfer) {
    if (controller_index < 0 || controller_index >= g_ctrl_count) return SQRM_USB_XFER_INVALID_HANDLE;
    if (!xfer) return SQRM_USB_XFER_INVALID_HANDLE;

    uhci_ctrl_t *c = &g_ctrls[controller_index];

    // Support CONTROL and (simple) INTERRUPT IN transfers.
    // INTERRUPT is currently implemented as a single TD (no QH scheduling yet) and is suitable
    // for polling low-bandwidth endpoints (e.g., HID boot keyboard/mouse).
    if (xfer->xfer_type != SQRM_USB_XFER_CONTROL && xfer->xfer_type != SQRM_USB_XFER_INTERRUPT) {
        xfer->status = -ENOSYS;
        return SQRM_USB_XFER_INVALID_HANDLE;
    }

    if (xfer->xfer_type == SQRM_USB_XFER_INTERRUPT) {
        // Interrupt transfers: only IN supported for now.
        if (!xfer->direction_in || xfer->length == 0) {
            xfer->status = -EINVAL;
            return SQRM_USB_XFER_INVALID_HANDLE;
        }
    }

    // allocate handle
    int hidx = -1;
    for (int i = 0; i < (int)(sizeof(g_handles)/sizeof(g_handles[0])); i++) {
        if (!g_handles[i].in_use) { hidx = i; break; }
    }
    if (hidx < 0) {
        xfer->status = -ENOMEM;
        return SQRM_USB_XFER_INVALID_HANDLE;
    }

    uhci_handle_t *h = &g_handles[hidx];
    *h = (uhci_handle_t){0};
    h->in_use = 1;
    h->ctrl_idx = controller_index;
    h->xfer = xfer;

    // bounce setup (CONTROL only)
    if (xfer->xfer_type == SQRM_USB_XFER_CONTROL) {
        if (g_api->dma_alloc(&h->setup_dma, sizeof(sqrm_usb_setup_packet_t), 16) != 0) {
            h->in_use = 0;
            xfer->status = -ENOMEM;
            return SQRM_USB_XFER_INVALID_HANDLE;
        }
        *(sqrm_usb_setup_packet_t*)h->setup_dma.virt = xfer->setup;
    }

    // bounce data if needed
    if (xfer->length) {
        if (g_api->dma_alloc(&h->data_dma, xfer->length, 16) != 0) {
            if (h->setup_dma.virt) g_api->dma_free(&h->setup_dma);
            h->in_use = 0;
            xfer->status = -ENOMEM;
            return SQRM_USB_XFER_INVALID_HANDLE;
        }
        if (!xfer->direction_in) {
            // OUT: copy into DMA buffer
            for (uint32_t i = 0; i < xfer->length; i++) ((uint8_t*)h->data_dma.virt)[i] = ((uint8_t*)xfer->data)[i];
        }
    }

    // CONTROL: Build TD chain: SETUP, optional DATA, STATUS
    // INTERRUPT: Build a single TD.
    if (xfer->xfer_type == SQRM_USB_XFER_INTERRUPT) {
        // bounce data buffer
        if (g_api->dma_alloc(&h->data_dma, xfer->length, 16) != 0) {
            h->in_use = 0;
            xfer->status = -ENOMEM;
            return SQRM_USB_XFER_INVALID_HANDLE;
        }

        // One TD: IN, DATA0 for bring-up (toggle tracking can be added later)
        uhci_td_t *td = td_alloc(c);
        if (!td) {
            g_api->dma_free(&h->data_dma);
            h->in_use = 0;
            xfer->status = -ENOMEM;
            return SQRM_USB_XFER_INVALID_HANDLE;
        }

        uint32_t st = UHCI_TD_STATUS_ACTIVE | (3u << 27) | UHCI_TD_STATUS_SPD | UHCI_TD_STATUS_IOC;
        if (xfer->speed == SQRM_USB_SPEED_LOW) st |= UHCI_TD_STATUS_LS;

        td->status = st;
        td->token = td_token(UHCI_PID_IN, xfer->dev_addr, xfer->endpoint, 0, (uint16_t)xfer->length);
        td->buffer_ptr = (uint32_t)h->data_dma.phys;
        td->link_ptr = UHCI_LINK_TERMINATE;

        h->td_first = td;
        h->td_last = td;

        // Link into control QH for now (works as a generic async queue)
        c->qh_control->element_link_ptr = (td_phys(c, td) & ~0xFu) | UHCI_LINK_VF;

        xfer->status = 0;
        xfer->actual_length = 0;
        return (sqrm_usb_xfer_handle_t)(hidx + 1);
    }

    uhci_td_t *td_setup = td_alloc(c);
    uhci_td_t *td_status = td_alloc(c);

    if (!td_setup || !td_status) {
        if (xfer->length) g_api->dma_free(&h->data_dma);
        g_api->dma_free(&h->setup_dma);
        h->in_use = 0;
        xfer->status = -ENOMEM;
        return SQRM_USB_XFER_INVALID_HANDLE;
    }

    // SETUP (always DATA0)
    uint32_t common_status = UHCI_TD_STATUS_ACTIVE | (3u << 27) | UHCI_TD_STATUS_SPD;
    if (xfer->speed == SQRM_USB_SPEED_LOW) common_status |= UHCI_TD_STATUS_LS;

    td_setup->status = common_status;
    td_setup->token = td_token(UHCI_PID_SETUP, xfer->dev_addr, xfer->endpoint, 0, (uint16_t)sizeof(sqrm_usb_setup_packet_t));
    td_setup->buffer_ptr = (uint32_t)h->setup_dma.phys;

    uhci_td_t *prev = td_setup;

    // DATA stage (DATA1 first). For address 0, devices usually require 8-byte packets.
    if (xfer->length) {
        uint8_t pid = xfer->direction_in ? UHCI_PID_IN : UHCI_PID_OUT;
        uint32_t remaining = xfer->length;
        uint32_t off = 0;
        uint8_t toggle = 1;

        // bring-up: use 8 bytes as a safe default for ep0
        uint32_t mps = 8;

        while (remaining) {
            uint32_t chunk = remaining;
            if (chunk > mps) chunk = mps;

            uhci_td_t *td = td_alloc(c);
            if (!td) {
                if (xfer->length) g_api->dma_free(&h->data_dma);
                g_api->dma_free(&h->setup_dma);
                h->in_use = 0;
                xfer->status = -ENOMEM;
                return SQRM_USB_XFER_INVALID_HANDLE;
            }

            td->status = common_status;
            td->token = td_token(pid, xfer->dev_addr, xfer->endpoint, toggle, (uint16_t)chunk);
            td->buffer_ptr = (uint32_t)(h->data_dma.phys + off);

            // TD link pointers should be 16-byte aligned physical addresses.
            // Set VF (depth-first) so the controller follows the TD chain.
            prev->link_ptr = (td_phys(c, td) & ~0xFu) | UHCI_LINK_VF;
            prev = td;

            remaining -= chunk;
            off += chunk;
            toggle ^= 1;
        }
    }

    // STATUS stage: opposite direction, DATA1
    td_status->status = common_status | UHCI_TD_STATUS_IOC;
    uint8_t spid = xfer->direction_in ? UHCI_PID_OUT : UHCI_PID_IN;
    td_status->token = td_token(spid, xfer->dev_addr, xfer->endpoint, 1, 0);
    td_status->buffer_ptr = 0;
    // Ensure TD chain terminates correctly and uses depth-first traversal.
    prev->link_ptr = (td_phys(c, td_status) & ~0xFu) | UHCI_LINK_VF;

    td_status->link_ptr = UHCI_LINK_TERMINATE;

    h->td_first = td_setup;
    h->td_last = td_status;

    // Link into control QH (simple: if empty, set element pointer)
    // Set VF so the controller follows the TD chain depth-first.
    uint32_t td0_link = (td_phys(c, td_setup) & ~0xFu) | UHCI_LINK_VF;
    if (c->qh_control->element_link_ptr & UHCI_LINK_TERMINATE) {
        c->qh_control->element_link_ptr = td0_link;
    } else {
        // simplistic: overwrite (bring-up)
        c->qh_control->element_link_ptr = td0_link;
    }

    xfer->status = 0;
    xfer->actual_length = 0;
    return (sqrm_usb_xfer_handle_t)(hidx + 1);
}

static void log_hex32(uint32_t v) {
    static const char h[] = "0123456789abcdef";
    char b[11];
    b[0] = '0'; b[1] = 'x';
    b[2] = h[(v >> 28) & 0xF];
    b[3] = h[(v >> 24) & 0xF];
    b[4] = h[(v >> 20) & 0xF];
    b[5] = h[(v >> 16) & 0xF];
    b[6] = h[(v >> 12) & 0xF];
    b[7] = h[(v >> 8) & 0xF];
    b[8] = h[(v >> 4) & 0xF];
    b[9] = h[(v >> 0) & 0xF];
    b[10] = 0;
    log_str(b);
}

static void uhci_irq_handler(void);

static void log_hex16(uint16_t v) {
    static const char h[] = "0123456789abcdef";
    char b[7];
    b[0] = '0'; b[1] = 'x';
    b[2] = h[(v >> 12) & 0xF];
    b[3] = h[(v >> 8) & 0xF];
    b[4] = h[(v >> 4) & 0xF];
    b[5] = h[(v >> 0) & 0xF];
    b[6] = 0;
    log_str(b);
}

#if UHCI_DEBUG
static void uhci_dump_regs(uhci_ctrl_t *c) {
    uint16_t base = c->info.io_base;
    uint16_t cmd = io_read16(base, UHCI_REG_USBCMD);
    uint16_t sts = io_read16(base, UHCI_REG_USBSTS);
    uint16_t intr = io_read16(base, UHCI_REG_USBINTR);
    uint16_t fr = io_read16(base, UHCI_REG_FRNUM);
    uint16_t p1 = io_read16(base, UHCI_REG_PORTSC1);
    uint16_t p2 = io_read16(base, UHCI_REG_PORTSC2);

    log_str("[UHCI-DBG] USBCMD="); log_hex16(cmd);
    log_str(" USBSTS="); log_hex16(sts);
    log_str(" USBINTR="); log_hex16(intr);
    log_str(" FRNUM="); log_hex16(fr);
    log_str(" PORT1="); log_hex16(p1);
    log_str(" PORT2="); log_hex16(p2);
    log_str("\n");
}

static void uhci_dump_td_chain(uhci_ctrl_t *c, uhci_td_t *first) {
    (void)c;
    log_str("[UHCI-DBG] TD chain:\n");
    uhci_td_t *td = first;
    for (int i = 0; i < 32 && td; i++) {
        log_str("  TD status="); log_hex32(td->status);
        log_str(" token="); log_hex32(td->token);
        log_str(" buf="); log_hex32(td->buffer_ptr);
        log_str(" link="); log_hex32(td->link_ptr);
        log_str("\n");

        if (td->link_ptr & UHCI_LINK_TERMINATE) break;
        // next TD pointer is physical; we can approximate by walking within our pool by matching phys.
        uint32_t next_phys = td->link_ptr & ~0xFu;
        uint32_t base_phys = (uint32_t)c->td_dma.phys;
        uint32_t off = next_phys - base_phys;
        if (off >= c->td_dma.size) break;
        td = (uhci_td_t*)((uint8_t*)c->td_dma.virt + off);
    }
}
#endif

static int api_wait(sqrm_usb_xfer_handle_t handle, uint32_t timeout_ms) {
    if (handle == SQRM_USB_XFER_INVALID_HANDLE) return -EINVAL;
    int idx = (int)handle - 1;
    if (idx < 0 || idx >= (int)(sizeof(g_handles)/sizeof(g_handles[0]))) return -EINVAL;

    uhci_handle_t *h = &g_handles[idx];
    if (!h->in_use || !h->xfer) return -EINVAL;

    uint64_t start = g_api->get_system_ticks ? g_api->get_system_ticks() : 0;
    uint64_t deadline = start;
    if (g_api->ms_to_ticks) deadline = start + g_api->ms_to_ticks(timeout_ms);

    // Poll last TD active bit until complete
    while (1) {
        // Clear pending status bits so the controller can keep generating interrupts.
        uhci_ctrl_t *c = &g_ctrls[h->ctrl_idx];
        io_write16(c->info.io_base, UHCI_REG_USBSTS, 0xFFFF);

        if ((h->td_last->status & UHCI_TD_STATUS_ACTIVE) == 0) break;
        if (g_api->get_system_ticks && g_api->ms_to_ticks) {
            if (g_api->get_system_ticks() >= deadline) {
                h->xfer->status = -EAGAIN;
#if UHCI_DEBUG
                uhci_ctrl_t *c = &g_ctrls[h->ctrl_idx];
                uhci_dump_regs(c);
                uhci_dump_td_chain(c, h->td_first);
#endif
                return -EAGAIN;
            }
        }
    }

    // Completion: copy IN data
    if (h->xfer->length && h->xfer->direction_in && h->data_dma.virt) {
        for (uint32_t i = 0; i < h->xfer->length; i++) ((uint8_t*)h->xfer->data)[i] = ((uint8_t*)h->data_dma.virt)[i];
        h->xfer->actual_length = h->xfer->length;
    }

    // Free DMA bounces
    if (h->xfer->length) g_api->dma_free(&h->data_dma);
    // setup_dma is only used by CONTROL transfers
    if (h->setup_dma.virt) g_api->dma_free(&h->setup_dma);

    h->in_use = 0;
    return 0;
}

static int api_cancel(sqrm_usb_xfer_handle_t handle) {
    (void)handle;
    return -ENOSYS;
}

static void uhci_dbg_hex64(uint64_t v, char out[17]) {
    static const char *hx = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) { out[i] = hx[v & 0xF]; v >>= 4; }
    out[16] = 0;
}

static int api_set_callback(sqrm_usb_xfer_handle_t handle, sqrm_usb_xfer_cb_v1_t cb, void *user) {
    if (handle == SQRM_USB_XFER_INVALID_HANDLE) return -EINVAL;
    int idx = (int)handle - 1;
    if (idx < 0 || idx >= (int)(sizeof(g_handles)/sizeof(g_handles[0]))) return -EINVAL;
    uhci_handle_t *h = &g_handles[idx];
    if (!h->in_use) return -EINVAL;
    h->cb = cb;
    h->cb_user = user;

    // Debug: log callback pointer we stored
    if (g_api && g_api->com_write_string) {
        char cbuf[17];
        uhci_dbg_hex64((uint64_t)(uintptr_t)cb, cbuf);
        g_api->com_write_string(0x3F8, "[UHCI] set_callback cb=0x");
        g_api->com_write_string(0x3F8, cbuf);
        g_api->com_write_string(0x3F8, "\n");
    }

    return 0;
}

static const sqrm_usbctl_uhci_api_v1_t g_uhci_api = {
    .get_controller_count = api_get_controller_count,
    .get_controller_info = api_get_controller_info,
    .get_port_count = api_get_port_count,
    .read_portsc = api_read_portsc,
    .reset_port = api_reset_port,
    .submit = api_submit,
    .wait = api_wait,
    .cancel = api_cancel,
    .set_callback = api_set_callback,
};

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    if (!api || api->abi_version != 1) return -1;
    g_api = api;

    log_str("[UHCI] uhci.sqrm init\n");

    if (!api->pci_get_device_count || !api->pci_get_device) {
        log_str("[UHCI] PCI API unavailable; cannot probe controllers\n");
        return -ENOSYS;
    }
    if (!api->inw || !api->outw || !api->outl) {
        log_str("[UHCI] Port I/O API unavailable\n");
        return -ENOSYS;
    }
    if (!api->dma_alloc || !api->dma_free) {
        log_str("[UHCI] DMA API unavailable\n");
        return -ENOSYS;
    }

    // scan PCI
    g_ctrl_count = 0;
    int n = api->pci_get_device_count();
    for (int i = 0; i < n && g_ctrl_count < (int)(sizeof(g_ctrls)/sizeof(g_ctrls[0])); i++) {
        pci_device_t *d = api->pci_get_device(i);
        if (!d) continue;

        if (d->class_code == 0x0C && d->subclass == 0x03 && d->prog_if == 0x00) {
            uint32_t io = 0;
            for (int b = 0; b < 6; b++) {
                // Be tolerant: some PCI enumeration code may not fill bar_type correctly.
                // PCI I/O BARs have bit0 = 1.
                if (d->bar[b] && (d->bar_type[b] == 1 /* I/O */ || (d->bar[b] & 1u))) {
                    io = d->bar[b];
                    break;
                }
            }
            uint16_t iobase = (uint16_t)(io & 0xFFFCu);
            if (iobase == 0) {
                log_str("[UHCI] Found UHCI device but no I/O BAR; skipping\n");
                continue;
            }
            uhci_ctrl_t *c = &g_ctrls[g_ctrl_count];
            *c = (uhci_ctrl_t){0};
            c->present = 1;
            c->info = (sqrm_uhci_controller_info_v1_t){
                .bus = d->bus,
                .device = d->device,
                .function = d->function,
                .irq_line = d->interrupt_line,
                .io_base = iobase,
            };

            // Enable PCI I/O + bus mastering
            if (api->pci_enable_io_space) api->pci_enable_io_space(d);
            if (api->pci_enable_bus_mastering) api->pci_enable_bus_mastering(d);

            if (uhci_init_controller(c) == 0) {
                g_ctrl_count++;
            } else {
                log_str("[UHCI] controller init failed\n");
                c->present = 0;
            }
        }
    }

    if (api->sqrm_service_register) {
        int r = api->sqrm_service_register("usbctl_uhci", &g_uhci_api, sizeof(g_uhci_api));
        log_str(r == 0 ? "[UHCI] exported service: usbctl_uhci\n" : "[UHCI] failed to export service: usbctl_uhci\n");
    }

    if (g_ctrl_count <= 0) {
        log_str("[UHCI] No UHCI controllers found (VM may not expose UHCI; add -device ich9-usb-uhci1 or -device piix3-usb-uhci in QEMU)\n");
        return -ENODEV;
    }
    return 0;
}
