// zenith4_stubs.c - Stub for old zenith4 shell
#include "moduos/kernel/COM/com.h"

void zenith4_start(void) {
    com_write_string(COM1_PORT, "[SHELL] zenith4_start() called - shell disabled in new process system\n");
    for (;;) {
        __asm__ volatile("hlt");
    }
}

void panicer_close_shell4(void) {
    // No-op stub
}
