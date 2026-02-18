// process_exit_stub.c - Callable wrapper for process_exit
// The inline function in the header can't be called from assembly

#include "moduos/kernel/process/process_new.h"

// Callable version for assembly code
void process_exit(int status) {
    do_exit(status);
}
