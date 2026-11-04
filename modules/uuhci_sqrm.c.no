#include "moduos/kernel/sqrm.h"
#include <stdint.h>

#include "usbmaster/usbmaster_hc_api.h"
#include "usbmaster/usb_types.h"

/* Globals */
const sqrm_kernel_api_t *g_api;
pci_device_t *g_uhci_dev;
uint16_t g_uhci_io_base;
uint32_t *g_frame_list;
int g_uhci_ports;

/* kprint */
static void kprint(const char *string) {
    g_api->com_write_string(0x3F8, string);
}

SQRM_DEFINE_MODULE_V2(SQRM_TYPE_USB, "uhci", 1, 0, 0, NULL);

/* Structs */
typedef struct __attribute__((packed)) {
    uint32_t ptrHorizontal;
    uint32_t ptrVertical;
} uhci_qh_t;

typedef struct __attribute__((packed)) {
    uint32_t nextDescriptor;
    uint32_t Status;
    uint32_t packetHeader;
    uint32_t bufAddress;
    uint32_t sysuse[4];
} uhci_td_t;

/* I/O Registers */
#define USBCMD   0x00
#define USBSTS   0x02
#define USBINTR  0x04
#define FRNUM    0x06
#define FRBASEADD 0x08
#define SOFMOD 0x0C
#define PORTSC1 0x10
#define PORTSC2 0x12

/* MISC */
#define UHCI_MAX_PORTS 16

/* UHCI Helpers */

    /* 16bit I/O */
static void uhci_write16(uint16_t offset, uint16_t value) {
    g_api->outw(g_uhci_io_base + offset, value);
}

static uint16_t uhci_read16(uint16_t offset) {
    return g_api->inw(g_uhci_io_base + offset);
}

    /* 32bit I/O */
static void uhci_write32(uint16_t offset, uint32_t value) {
    g_api->outl(g_uhci_io_base + offset, value);
}

static uint32_t uhci_read32(uint16_t offset) {
    return g_api->inl(g_uhci_io_base + offset);
}

    /* timing */
static void uhci_wait_ms(uint64_t ms) {

    int start_ticks = g_api->get_system_ticks();
    int start_ms = g_api->ticks_to_ms(start_ticks);

    while (1) {
        int current_ticks = g_api->get_system_ticks();
        int current_ms = g_api->ticks_to_ms(current_ticks);

        if ((current_ms - start_ms) >= ms) {
            break;
        }
    }
}

/* Memory Helpers */
static const char *itoa(int value, char *str, int base) {
    if (base < 2 || base > 36) {
        *str = '\0';
        return str;
    }

    const char *digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    char *ptr = str, *ptr1 = str, tmp_char;
    int sign = 0;

    // Handle negative sign for base 10
    if (value < 0 && base == 10) {
        sign = 1;
    }

    // Use unsigned int for safe handling of INT_MIN
    unsigned int uvalue = (sign) ? -value : value;

    do {
        int remainder = uvalue % base;
        *ptr++ = digits[remainder];
        uvalue /= base;
    } while (uvalue);

    if (sign) {
        *ptr++ = '-';
    }

    *ptr-- = '\0';

    // Reverse the string
    while (ptr1 < ptr) {
        tmp_char = *ptr;
        *ptr-- = *ptr1;
        *ptr1++ = tmp_char;
    }

    return str;
}

static void *memcpy(void *dest, const void *src, size_t len) {
    const unsigned char *s = src;
    unsigned char *d = dest;
    while (len--) {
        *d++ = *s++;
    }
    return dest;
}

/* UHCI Functions */

void uhci_irq() {
    uint16_t status = uhci_read16(USBSTS);

    /* Since IRQ 11 is SHARED we have to check if it is actually us and not someone elses Interrupt*/
    if ((status & 0x0003) == 0) {
        /* NOT US */
        g_api->pic_send_eoi(g_uhci_dev->interrupt_line);
        return;
    }
    /* Past this point it is US */

    uhci_write16(USBSTS, status & 0x0003);

    g_api->pic_send_eoi(g_uhci_dev->interrupt_line);
}

static int uhci_enable_port(int port) {

    /* Reset the port */
    uhci_write16(PORTSC1 + (port * 2), (1 << 9));

    uhci_wait_ms(100);
    uhci_write16(PORTSC1 + (port * 2), 0x00);
    kprint("[UHCI]:  Clear Reset bit done! \n");

    /* Enable the port */
    uhci_wait_ms(50);

    uhci_write16(PORTSC1 + (port * 2), (1 << 2));
    kprint("[UHCI]:  Enable bit set. \n");

    /* Poll until Device Enable bit reads back as set */
    int start_poll_ticks = g_api->get_system_ticks();
    int start_poll_ms = g_api->ticks_to_ms(start_poll_ticks);

    while (1) {
        int current_poll_ticks = g_api->get_system_ticks();
        int current_poll_ms = g_api->ticks_to_ms(current_poll_ticks);


        if ((uhci_read16(PORTSC1 + (port * 2)) & (1 << 2)) != 0) {
            kprint("[UHCI]:  Device respond\n");
            return esuccess;
        }

        if ((current_poll_ms - start_poll_ms) >= 100) {
            kprint("[UHCI]: Device Timeout.\n");
            kprint("[UHCI]:  Is the UHCI Device faulty?\n");
            return -etimeout;
        }
    }
}

static int uhci_send_setup_packet(uint8_t device_address, usb_setup_packet_t packet, void *out_buffer, size_t response_len) {

    int result = esuccess;

    /* Figure out how many data TDs we need. */
    int data_td_count = (response_len + 8 - 1) / 8;

    dma_buffer_t qh = {0};
    dma_buffer_t setup_td = {0};
    dma_buffer_t status_td = {0};
    dma_buffer_t buf = {0};
    dma_buffer_t data_td[data_td_count];
    dma_buffer_t data_buf[data_td_count];

    for (int i = 0; i < data_td_count; i++) {
        data_td[i].virt = NULL;
        data_buf[i].virt = NULL;
    }

    /* Allocate Memory */

        /* Allocate QH */
    int qh_rc = g_api->dma_alloc(&qh, sizeof(uhci_qh_t), 16);
    if (qh_rc != esuccess) {
        result = -eallocfail;
        goto setup_cleanup;
    }

        /* Allocate Setup TD */
    int setup_td_rc = g_api->dma_alloc(&setup_td, sizeof(uhci_td_t), 16);
    if (setup_td_rc != esuccess) {
        result = -eallocfail;
        goto setup_cleanup;
    }

        /* Allocate Data TDs */
    for (int i = 0; i < data_td_count; i++) {
        int data_td_rc = g_api->dma_alloc(&data_td[i], sizeof(uhci_td_t), 16);
        if (data_td_rc != 0) {
            result = -eallocfail;
            goto setup_cleanup;
        }
    }
        /* Allocate Status TD */
    int status_td_rc = g_api->dma_alloc(&status_td, sizeof(uhci_td_t), 16);
    if (status_td_rc != esuccess) {
        result = -eallocfail;
        goto setup_cleanup;
    }

    /* Fill the Setup TD */
    int status_buf_rc = g_api->dma_alloc(&buf, 8, 16);
    if (status_buf_rc != esuccess) {
        result = -eallocfail;
        goto setup_cleanup;
    }

    memcpy(buf.virt, &packet, sizeof(packet));

    uhci_td_t *setup_td_ptr = (uhci_td_t *)setup_td.virt;
    setup_td_ptr->bufAddress = buf.phys;

    /* Packet Header */
    uint32_t packet_header = (0x2D) | (device_address << 8) | (0 << 15) | (0 << 19) | (7 << 21);
    setup_td_ptr->packetHeader = packet_header;

    /* Status */
    uint32_t status = (1 << 23) | (1 << 26) | (1 << 24) | (3 << 27);
    setup_td_ptr->Status = status;

    /* Data (PAIN) */
    for (int i = 0; i < data_td_count; i++) {
        int data_buf_rc = g_api->dma_alloc(&data_buf[i], 8, 16);
        if (data_buf_rc != 0) {
            result = -eallocfail;
            goto setup_cleanup;
        }

        uhci_td_t *td_ptr = (uhci_td_t *)data_td[i].virt;
        td_ptr->bufAddress = data_buf[i].phys;

        int bytes_remaining = response_len - (i * 8);
        int this_len = (bytes_remaining > 8) ? 8 : bytes_remaining;

        int toggle = 1 - (i % 2);

        uint32_t d_packet_header =
            (0x69) |                          /* Packet Type = IN */
            (device_address << 8) |           /* Device */
            (0 << 15) |                       /* Endpoint 0 */
            (toggle << 19) |                  /* Data Toggle */
            ((this_len - 1) << 21);           /* Maximum Length = len - 1 */

        td_ptr->packetHeader = d_packet_header;

        uint32_t d_status = (1 << 23) | (1 << 26) | (1 << 24) | (3 << 27);
        td_ptr->Status = d_status;
    }

    /* STATUS TD */
    uhci_td_t *status_td_ptr = (uhci_td_t *)status_td.virt;
    status_td_ptr->bufAddress = 0;

    uint32_t status_max_len = (0 - 1) & 0x7FF; /* zero-length quirk = 0x7FF */

    /* Status stage direction is the OPPOSITE of the data stage direction.
     * If there is no data stage at all (data_td_count == 0, e.g. SET_ADDRESS),
     * the status stage is always IN. */
    uint32_t status_pid = (data_td_count > 0) ? 0xE1 /* OUT */ : 0x69 /* IN */;

    uint32_t status_packet_header =
        (status_pid) |                    /* Packet Type = OUT or IN, see above */
        (device_address << 8) |           /* Device */
        (0 << 15) |                       /* Endpoint 0 */
        (1 << 19) |                       /* Data Toggle = 1, always for STATUS */
        (status_max_len << 21);           /* Maximum Length = 0x7FF (zero-length) */

    status_td_ptr->packetHeader = status_packet_header;

    uint32_t status_status = (1 << 23) | (1 << 26) | (1 << 24) | (3 << 27);
    status_td_ptr->Status = status_status;

    /* Chain: setup -> data[0], or setup -> status directly if there's no data stage
     * (e.g. SET_ADDRESS, which has response_len == 0 / data_td_count == 0). */
    if (data_td_count > 0) {
        setup_td_ptr->nextDescriptor = data_td[0].phys | (1 << 2);
    } else {
        setup_td_ptr->nextDescriptor = status_td.phys | (1 << 2);
    }

    /* Chain: data[i] -> data[i+1], last data -> status */
    for (int i = 0; i < data_td_count; i++) {
        uhci_td_t *td_ptr = (uhci_td_t *)data_td[i].virt;

        if (i < data_td_count - 1) {
            td_ptr->nextDescriptor = data_td[i + 1].phys | (1 << 2);
        } else {
            td_ptr->nextDescriptor = status_td.phys | (1 << 2);
        }
    }

    /* Status TD terminates the chain */
    status_td_ptr->nextDescriptor = 0x1;

    /* Wire QH to point at the Setup TD */
    uhci_qh_t *qh_ptr = (uhci_qh_t *)qh.virt;
    qh_ptr->ptrVertical = setup_td.phys;
    qh_ptr->ptrHorizontal = 0x1;

    /* Insert QH into the Frame List */
    g_frame_list[0] = qh.phys | (1 << 1);

    /* Poll for completion */
    int start_ticks = g_api->get_system_ticks();
    int start_ms = g_api->ticks_to_ms(start_ticks);

    while (1) {
        if ((status_td_ptr->Status & (1 << 23)) == 0) {
            kprint("[UHCI]: Transfer complete!\n");
            break;
        }

        int current_ticks = g_api->get_system_ticks();
        int current_ms = g_api->ticks_to_ms(current_ticks);
        if ((current_ms - start_ms) >= 500) {
            kprint("[UHCI]: Transfer TIMEOUT.\n");

            result = -etimeout;
            goto setup_cleanup;
        }
    }

    /* Copy response data out to caller's buffer */
    for (int i = 0; i < data_td_count; i++) {
        int bytes_remaining = response_len - (i * 8);
        int this_len = (bytes_remaining > 8) ? 8 : bytes_remaining;

        memcpy((uint8_t *)out_buffer + (i * 8), data_buf[i].virt, this_len);
    }

setup_cleanup:
    if (qh.virt) g_api->dma_free(&qh);
    if (setup_td.virt) g_api->dma_free(&setup_td);
    if (status_td.virt) g_api->dma_free(&status_td);
    if (buf.virt) g_api->dma_free(&buf);
    for (int i = 0; i < data_td_count; i++) {
        if (data_td[i].virt) g_api->dma_free(&data_td[i]);
        if (data_buf[i].virt) g_api->dma_free(&data_buf[i]);
    }
    return result;
}

static int uhci_get_port_count(void) {
    return g_uhci_ports;
}

static int uhci_get_port_connected(int port) {
    uint16_t portsc1 = uhci_read16(PORTSC1 + (port * 2));

    if ((portsc1 & 0x01) == 1) {
        return pconnected;
    }

    return pdisconnected;
}

/* Entry */
int sqrm_module_init(const sqrm_kernel_api_t *api) {

    g_api = api;

    kprint("[UHCI]:  Start.\n");

    /* find the UHCI PCI device */

    int index = 0;
    int max = 0;

    max = api->pci_get_device_count();

    while (index < max) {
        pci_device_t *randdev = api->pci_get_device(index);

        if (!randdev) {
            kprint("[UHCI]:  pci_get_device returned NULL.in this index\n");
            break;
        }
        
        if (randdev->class_code == 0x0C && randdev->subclass == 0x03 && randdev->prog_if == 0x00) {
            kprint("[UHCI]:  Found UHCI controller.\n");
            g_uhci_dev = randdev;
            break;
        }
        index++;
    }

    if (!g_uhci_dev) {
        kprint("[UHCI]:  No UHCI controller found.\n");
        return -enotfound;
    }

    /* allow I/O and Memory access to the UHCI device */
    api->pci_enable_io_space(g_uhci_dev);
    api->pci_enable_bus_mastering(g_uhci_dev);

    if (g_uhci_dev->bar_type[4] != 1) {
        kprint("[UHCI]:  BAR4 is not I/O space?!\n");
        return -ebarnotio;
    }
    g_uhci_io_base = (uint16_t)g_uhci_dev->bar[4];
    

    api->pci_cfg_write32(g_uhci_dev->bus, g_uhci_dev->device, g_uhci_dev->function, 0xC0, 0x2000); /* Disable Legacy Support */

    uhci_write16(USBCMD, 0x02); 

    for (;;) {
        uint16_t status = uhci_read16(USBCMD);
        if ((status & (1 << 1)) == 0) {
            kprint("[UHCI]:  HCRESET done! \n");
            break;
        }
    }

    uhci_write16(USBCMD, 0x04);

    /* Wait for 10 ms */
    /* Why not use sleep_ms? because from past experience, it doesn't work, at all. */
    int start_ticks = api->get_system_ticks();
    int start_ms = api->ticks_to_ms(start_ticks);

    while (1) {
        int current_ticks = api->get_system_ticks();
        int current_ms = api->ticks_to_ms(current_ticks);

        if ((current_ms - start_ms) >= 10) {
            uhci_write16(USBCMD, 0x00);
            kprint("[UHCI]:  GRESET done! \n");
            break;
        }
    }

    /* Frame List */
    dma_buffer_t frame_list_buffer;
    int result = api->dma_alloc(&frame_list_buffer, 1024 * sizeof(uint32_t), 4096);

    if (result < 0) {
        kprint("Failed to allocate frame list buffer\n");
        return -eallocfail;
    }

    g_frame_list = (uint32_t *)frame_list_buffer.virt;

    for (int index = 0; index < 1024; index++) {
        g_frame_list[index] = (1 << 0);
    };

    uhci_write32(FRBASEADD, frame_list_buffer.phys);

    /* Enable Interrupts */
    uhci_write16(USBINTR, (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3));

    /* Register Interrupt Handler */
    api->irq_install_handler(g_uhci_dev->interrupt_line, uhci_irq);

    /* Set Frame Number to 0 */
    uhci_write16(FRNUM, (uint16_t)0);

    /* Start UHCI Controller */
    uhci_write16(USBCMD, (1 << 0) | (1 << 7));

    /* Find ports */
    for (int n = 0; n < UHCI_MAX_PORTS; n++) {
        uint16_t portsc1 = uhci_read16(PORTSC1 + (n * 2));

        if (portsc1 == 0xFFFF || (portsc1 & 0x80) == 0) {
            g_uhci_ports = n;
            break;
        }

    }

    static uhci_hc_api_t uhci_api;

    /* Providing UHCI functions to the API struct */
    uhci_api.enable_port = uhci_enable_port;
    uhci_api.send_control = uhci_send_setup_packet;
    uhci_api.get_port_count = uhci_get_port_count;
    uhci_api.get_port_connected = uhci_get_port_connected;

    /* Register the UHCI Hardware Controller API */
    api->sqrm_service_register("uhci_hc", &uhci_api, sizeof(uhci_api));

    /* From now on, UHCI driver is done, the USBMASTER will handle all USB specific stuff */

    return esuccess;
}
