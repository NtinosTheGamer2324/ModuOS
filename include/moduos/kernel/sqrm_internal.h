#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Internal helper: current SQRM module name during module init/callbacks.
const char *sqrm_get_current_module_name(void);

#ifdef __cplusplus
}
#endif
