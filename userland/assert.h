#pragma once

/*
 * Minimal freestanding assert.h for ModuOS userland (-nostdlib).
 *
 * This is only intended to satisfy thirdparty code that uses assert().
 * There is no stderr/printf here, so we just trap.
 */

#ifdef NDEBUG
#define assert(x) ((void)0)
#else
static inline void __moduos_assert_fail(void) {
    /* Cause an invalid instruction trap (best effort). */
    __builtin_trap();
    for (;;) { }
}

#define assert(x) ((x) ? (void)0 : __moduos_assert_fail())
#endif
