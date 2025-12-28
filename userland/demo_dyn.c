#include "libc.h"

/* Demo: call into a shared library function.
 * Requires ld-moduos symbol relocations (GLOB_DAT/JUMP_SLOT/64).
 */
extern int demo_add(int a, int b);

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;
    printf("demo_dyn: demo_add(2,3)=%d\n", demo_add(2,3));
    return 0;
}
