#include <stdint.h>
#include <stddef.h>

#include "moduos/kernel/sqrm.h"

/* OHCI controller skeleton (no real USB yet) */
SQRM_DEFINE_MODULE_V2(SQRM_TYPE_USB, "ohci", 4, 0, 0, NULL);

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    if (!api || api->abi_version != 1) return -1;
    if (api->com_write_string) api->com_write_string(0x3F8, "[OHCI] OHCI controller module loaded (skeleton)\n");
    return 0;
}
