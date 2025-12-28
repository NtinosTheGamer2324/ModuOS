#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Main kernel initialization sequence.
// This performs early init and then starts the shell/process scheduler.
void mdinit_run(uint64_t mb2_ptr);

#ifdef __cplusplus
}
#endif
