/* chvt.c - switch virtual console:  chvt N   (also SHIFT+SUPER+ALT+F<N>) */
#include "libc/libc.h"
#include "libc/tty.h"

int md_main(long argc, char **argv) {
    if (argc < 2) {
        printf("active VT: %d (usage: chvt N)\n", vt_get_active());
        return 0;
    }
    int n = atoi(argv[1]);
    if (n < 1) { printf("chvt: bad VT number\n"); return 1; }
    if (vt_activate(n) < 0) { printf("chvt: failed\n"); return 1; }
    return 0;
}
