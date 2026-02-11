// lib_8bit.c
#include "libc.h"
#include "lib_8bit.h"

uint64_t emu_get_ticks_ms(void) {
    return (uint64_t)time_ms();
}
