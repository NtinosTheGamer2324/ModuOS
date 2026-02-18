// exec_stubs.c - Stub implementations for old exec functions
// These are compatibility stubs for old code that still calls exec_run

#include "moduos/kernel/exec.h"
#include "moduos/kernel/COM/com.h"

// Old exec_run - replaced by new POSIX fork/exec model
int exec_run(const char *path, int wait_for_exit) {
    (void)path;
    (void)wait_for_exit;
    com_write_string(COM1_PORT, "[EXEC_STUB] exec_run() called - not supported in new process system\n");
    return -1;  // Not supported
}
