#include "libc.h"

/* FPU/SSE multitasking test.
 * Run two copies in background:
 *   exec /Apps/fputest.sqr &
 *   exec /Apps/fputest.sqr &
 * If FPU state is NOT saved/restored per process, outputs will become inconsistent.
 */

static double step(double x) {
    /* some non-trivial floating point ops to exercise XMM regs */
    return (x * 1.0000001) + 0.0000003;
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    double x = 0.1;
    for (unsigned long iter = 1;; iter++) {
        x = step(x);
        x = step(x);
        x = step(x);

        if ((iter % 5000000UL) == 0) {
            /* Print a stable-ish value without needing float formatting support.
             * We print x scaled to micro-units as an integer.
             */
            long long scaled = (long long)(x * 1000000.0);
            printf("fputest: iter=%lu x*1e6=%lld\n", iter, scaled);
        }
    }

    return 0;
}
