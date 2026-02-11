#include "../sqrm_sdk.h"

static const char * const g_hid_deps[] = { "usb" };
SQRM_DEFINE_MODULE_V2(SQRM_TYPE_HID, "hid", 1, 0, 1, g_hid_deps);

static int hid_get_keyboard_present(void) { return 0; }
static int hid_get_mouse_present(void) { return 0; }

static const sqrm_hid_api_v1_t g_hid_api = {
    .get_keyboard_present = hid_get_keyboard_present,
    .get_mouse_present = hid_get_mouse_present,
};

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    if (!api || api->abi_version != SQRM_ABI_VERSION) return -1;
    if (api->com_write_string) api->com_write_string(0x3F8, "[hid] loaded (skeleton)\n");

    if (api->sqrm_service_register) {
        (void)api->sqrm_service_register("hid", &g_hid_api, sizeof(g_hid_api));
    }

    return 0;
}
