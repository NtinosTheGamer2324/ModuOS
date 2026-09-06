#include "moduos/kernel/sqrm.h"
#include "usbmaster/usbmaster_hc_api.h"
#include "usbmaster/usb_types.h"
#include "stdarg.h"

static const char *usbmaster_deps[] = { "? uhci" };

SQRM_DEFINE_MODULE_V2(SQRM_TYPE_USB, "usbmaster", 2, 0, 1, usbmaster_deps);

/* Globals */
const sqrm_kernel_api_t *g_api;
const uhci_hc_api_t *g_uhci_api;

uint8_t g_next_address = 1;

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

/* Helpers */
static void kprint(const char *s) {
    g_api->com_write_string(0x3F8, s);
}

static void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    while (*fmt != '\0') {
        if (*fmt == '%') {
            fmt++;
            if (*fmt == 'd' || *fmt == 'i') {
                int val = va_arg(args, int);
                char numbuf[32];
                itoa(val, numbuf, 10);
                kprint(numbuf);   
            } else if (*fmt == 's') {
                const char *s = va_arg(args, const char *);
                kprint(s ? s : "(null)");
            } else if (*fmt == 'x') {
                int val = va_arg(args, int);
                char numbuf[32];
                
                itoa(val, numbuf, 16); 
                kprint("0x");
                kprint(numbuf);
            } else if (*fmt == '%') {
                kprint("%");
            }
        } else {
            char next_char[2] = { *fmt, '\0' };
            kprint(next_char);
        }
        fmt++;
    }
    va_end(args);
}

static void wait_ms(uint64_t ms) {

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

/* USB Functions */
static int usbmaster_set_address(uint8_t new_address) {
    usb_setup_packet_t set_address = {
        .RequestType = USB_DIR_OUT,
        .Request = SET_ADDRESS,
        .Value = new_address,
        .Index = 0, /* Unused */
        .Length =0 /* Unused */
    };

    int r = g_uhci_api->send_control(0, set_address, NULL, 0);

    return r;
}

static void usbmaster_enumerate_devices(void) {
    for (int i = 0; i < g_uhci_api->get_port_count(); i++) {
        int connected = g_uhci_api->get_port_connected(i);

        if (connected == pconnected) {
            kprintf("[USBMASTER] USB device detected on port: %d\n", i);

            /* USB spec says wait 100ms, that's what we are going to do*/
            wait_ms(100);

            int rc = g_uhci_api->enable_port(i);
            if (rc == esuccess) {
                kprintf("[USBMASTER] Successfuly enabled port: %d\n", i);
                int urc =usbmaster_set_address(g_next_address);

                if (urc == esuccess) {
                    kprintf("[USBMASTER] Successfully set address for port: %d as: %d\n", i, g_next_address);
                    g_next_address++;
                } else {
                    kprintf("[USBMASTER] FAILED to set address for port: %d\n", i);
                }
            } else {
                kprintf("[USBMASTER] FAILED to enable port for: %d\n", i);
            }
        }

    }
}

/* Entry */
int sqrm_module_init(const sqrm_kernel_api_t *api) {

    g_api = api;

    /* Get the UHCI service */
    size_t uhci_hc_size = 0;
    const uhci_hc_api_t *uhci_hc = (const uhci_hc_api_t *)api->sqrm_service_get("uhci_hc", &uhci_hc_size);

    if (!uhci_hc) {
        kprint("[USBMASTER]: No UHCI controller service found.\n");
    } else if (uhci_hc_size != sizeof(uhci_hc_api_t)) {
        kprint("[USBMASTER]: UHCI service size mismatch! Refusing to use it.\n");
    } else {
        kprint("[USBMASTER]: UHCI controller service found!\n");

        g_uhci_api = uhci_hc;

        usbmaster_enumerate_devices();

    }

    return 0;
}