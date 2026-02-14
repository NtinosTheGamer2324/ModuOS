#include "libc.h"

int md_main(long argc, char** argv) {
    (void)argc; (void)argv;
    

    printf("MDBDTest: ModuOS Background Tasks Test\n");

    for (;;) {
        yield();
    }
}