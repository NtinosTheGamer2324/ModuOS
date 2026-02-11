#pragma once

/*
 * Minimal freestanding stdlib.h for ModuOS userland (-nostdlib).
 *
 * This header is primarily here to satisfy thirdparty libraries (e.g., miniz)
 * which include <stdlib.h> for malloc/free/realloc/calloc.
 */

#include <stddef.h>
#include "libc.h"

/* malloc/free/calloc/realloc are provided as static inline wrappers in libc.h. */

/* No hosted process termination support in the userland libc shim yet.
 * Provide best-effort stubs for common symbols.
 */
static inline void abort(void) {
    __builtin_trap();
    for (;;) { }
}

/* exit() is already provided by libc.h */
