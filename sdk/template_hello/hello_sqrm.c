#include "../sqrm_sdk.h"

/*
 * Minimal SQRM module example.
 *
 * Build: see template_hello/build.sh
 */

SQRM_DEFINE_MODULE(SQRM_TYPE_USB, "hello");

static const uint16_t COM1_PORT = 0x3F8;

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    if (!api || api->abi_version != SQRM_ABI_VERSION) return -1;

    if (api->com_write_string) {
        api->com_write_string(COM1_PORT, "[hello_sqrm] hello from third-party module!\n");
    }

    return 0;
}
