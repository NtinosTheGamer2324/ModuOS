#include "sqrm_sdk.h"

sqrm_kernel_api_t *g_api;

#define COM1_PORT 0x3F8

/*
    This macro is one of the MOST IMPORTANT CODE in your module
    Without this, the Kernel WONT crash, BUT, it will fail to load your module
    Because you DID NOT provide ANY DETAILS for the kernel.
*/
SQRM_DEFINE_MODULE(SQRM_TYPE_GENERIC, "test");

/*
    If you want dependencies you should use 
    SQRM_DEFINE_MODULE_V2()
    It takes:
    TYPE, NAME, CLASS ID, SUBCLASS ID, DEPENDENCY COUNT, DEPENDENCY POINTER

    Example:
    static const char * const g_usb_deps[] = {
        "uhci",
    };

    SQRM_DEFINE_MODULE_V2(SQRM_TYPE_USB, "usb", 1, 0, (uint16_t)(sizeof(g_usb_deps)/sizeof(g_usb_deps[0])), g_usb_deps);
*/


/* ↓ important! Need to be LOCAL in the function table.*/
static void this_will_not_crash_sys() {
    g_api->com_write_string(COM1_PORT, "Sys safe!");
}

/* 
    IF YOU CALL THIS FUNCTION, YOU WILL OPCODE FAULT
    EXPLANATION:
    The kernel, when it sees G in the table, it will think: Oh this func is in me and trying to call it!
    But when it doesn't find it, it will fault
    NOW, if it was called something the kernel already has, it would work, but the cpu would execute THAT function.
*/
void this_WILL_crash_sys() {
    g_api->com_write_string(COM1_PORT, "oops!");
}

/* This will always be ran BEFORE anything else in your module*/
int sqrm_module_init(const sqrm_kernel_api_t *api) {
    /* api is NEVER NULL, the kernel ALWAYS fills it, and check in the SDK header what your SQRM_TYPE is available to.*/
    /* This is great practice but not very needed, but it is good practice:*/
    if (!api) return -1;
    g_api = api;
    
    this_will_not_crash_sys();
    g_api->com_write_string(COM1_PORT, "please use the static keyword so the system does not fault!\n");

    return 0;
}