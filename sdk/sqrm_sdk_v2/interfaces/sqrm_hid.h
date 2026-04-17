#pragma once
/*
 * sqrm_hid.h — SQRM HID (Human Interface Device) service ABI (v1).
 *
 * Available to: HID modules.
 *   - input_push_event in sqrm_kernel_api_t is NULL for all other types.
 *
 * Service name convention: "sqrm_hid_v1"
 * Register via api->sqrm_service_register("sqrm_hid_v1", ...).
 * Retrieve via api->sqrm_service_get("sqrm_hid_v1", ...).
 *
 * All functions return 0 (absent) or 1 (present), or negative errno on failure.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int (*get_keyboard_present)(void);
    int (*get_mouse_present)   (void);
} sqrm_hid_api_v1_t;

#ifdef __cplusplus
}
#endif