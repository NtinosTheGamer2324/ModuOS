#include "libc.h"

#define TestPath1 "$/user/test1"
#define TestPath2 "$/user/test2"
#define TestPath3 "$/user/test3"

int md_main(long argc, char** argv) {
    (void)argc; (void)argv;

    userfs_register_path(TestPath1, USERFS_PERM_READ_WRITE);
    userfs_register_path(TestPath2, USERFS_PERM_READ_WRITE);
    userfs_register_path(TestPath3, USERFS_PERM_READ_WRITE);
}