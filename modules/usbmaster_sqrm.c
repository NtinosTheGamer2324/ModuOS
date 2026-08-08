#include "moduos/kernel/sqrm.h"
#include "usbmaster/usbmaster_hc_api.h"

/* Globals */
const sqrm_kernel_api_t *g_api;
const uhci_hc_api_t *g_uhci_api;

/* Helpers */
static void kprint(const char *s) {
    g_api->com_write_string(0x3F8, s);
}

static const char *usbmaster_deps[] = { "? uhci" };

SQRM_DEFINE_MODULE_V2(SQRM_TYPE_USB, "usbmaster", 2, 0, 1, usbmaster_deps);

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

    }

    return 0;
}