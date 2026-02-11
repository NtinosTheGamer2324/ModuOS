#include "../sqrm_sdk.h"

/*
 * GenericSQRM module template.
 *
 * SQRM_TYPE_GENERIC is for modules that don't fit cleanly into a single subsystem.
 * Examples: diagnostics, debug tools, small services, policy modules, shims.
 *
 * By default this template uses ABI v1 (no dependency loading).
 * If you want dependencies, switch to SQRM_DEFINE_MODULE_V2 and set SQRM_ABI_VERSION accordingly.
 */

SQRM_DEFINE_MODULE(SQRM_TYPE_GENERIC, "generic");

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    if (!api || api->abi_version != SQRM_ABI_VERSION) return -1;

    if (api->com_write_string) {
        api->com_write_string(0x3F8, "[generic] GenericSQRM module loaded (template)\n");
    }

    // Put your module logic here.

    return 0;
}
