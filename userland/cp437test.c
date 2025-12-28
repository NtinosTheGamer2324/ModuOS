#include "libc.h"

/*
 * CP437 calibration tool.
 * Prints bytes 0x80..0xFF (as single bytes) so we can see what the active
 * framebuffer console font contains at each index.
 */
int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    printf("CP437 test (bytes 0x80..0xFF)\n");
    printf("If you see '?' then that glyph is missing in the active font.\n\n");

    for (int row = 0; row < 8; row++) {
        int base = 0x80 + row * 16;
        printf("%02x: ", base);
        for (int col = 0; col < 16; col++) {
            char ch = (char)(base + col);
            write(STDOUT_FILENO, &ch, 1);
        }
        printf("\n");
    }

    printf("\nCommon CP437 box drawing indices: \n");
    const unsigned char demo[] = {
        0xDA,0xC4,0xC4,0xBF,'\n',
        0xB3,'h','i',0xB3,'\n',
        0xC0,0xC4,0xC4,0xD9,'\n',
        0
    };
    for (int i = 0; demo[i]; i++) {
        char ch = (char)demo[i];
        write(STDOUT_FILENO, &ch, 1);
    }

    return 0;
}
