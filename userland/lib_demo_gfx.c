#include "libc.h"

/* Example userland shared library (.sqrl).
 * MVP: keep it trivial and avoid calling into other libs.
 */

int demo_add(int a, int b) {
    return a + b;
}
