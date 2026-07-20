#include "sqrm_sdk.h"

sqrm_kernel_api_t *g_api;

SQRM_DEFINE_MODULE(SQRM_TYPE_GENERIC, "test");
/* ↓ important!*/
static void this_will_not_crash_sys() {
    g_api->com_write_string(0x3F8, "Sys safe!");
}

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    g_api = api;
    
    this_will_not_crash_sys();
    g_api->com_write_string(0x3F8, "please use the static keyword so the system does not fault\n");

    return 0;
}