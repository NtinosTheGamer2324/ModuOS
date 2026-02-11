#include "../sqrm_sdk.h"

static const char * const g_usb_deps[] = { "uhci" };
SQRM_DEFINE_MODULE_V2(SQRM_TYPE_USB, "usb", 1, 0, 1, g_usb_deps);

static int usb_get_controller_count(void) { return 0; }
static int usb_get_device_count(void) { return 0; }
static int usb_enumerate(void) { return -38; /* -ENOSYS */ }

static const sqrm_usb_api_v1_t g_usb_api = {
    .get_controller_count = usb_get_controller_count,
    .get_device_count = usb_get_device_count,
    .enumerate = usb_enumerate,
};

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    if (!api || api->abi_version != SQRM_ABI_VERSION) return -1;
    if (api->com_write_string) {
        api->com_write_string(0x3F8, "[usb] loaded (skeleton)\n");
        api->com_write_string(0x3F8, "[usb] controllers should already be loaded via dependencies\n");
    }

    if (api->sqrm_service_register) {
        (void)api->sqrm_service_register("usb", &g_usb_api, sizeof(g_usb_api));
    }

    return 0;
}
