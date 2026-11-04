/* crt0.c - program entry, linked into every ModuOS app (and into ld-moduos).
 *
 * It lives outside libc.sqrl because an executable's entry point has to be in
 * the executable itself. exit() and input_flush() come from libc.sqrl.
 */
#include "stdlib.h"
#include "moduos.h"

__attribute__((noreturn, noinline, used))
void _start(long argc, char **argv) {
    int rc = md_main(argc, argv);
    input_flush();
    exit(rc);
}
