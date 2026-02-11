#include "../sqrm_sdk.h"

SQRM_DEFINE_MODULE_V2(SQRM_TYPE_USB, "ehci", 2, 0, 0, NULL);

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    if (!api || api->abi_version != SQRM_ABI_VERSION) return -1;
    if (api->com_write_string) api->com_write_string(0x3F8, "[ehci] loaded (skeleton)\n");
    return 0;
}
