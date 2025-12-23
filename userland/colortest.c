#include "libc.h"
#include "string.h"

/*
 * colortest.sqr
 *
 * Quick verification for:
 *  1) Legacy ModuOS color escapes (backslash codes parsed by VGA driver)
 *  2) ANSI SGR (ESC[...m) parsed by VGA driver
 *  3) Direct VGA syscalls (vga_set_color / vga_reset_color)
 */

static void title(const char *t) {
    puts_raw("\n\x1b[95m");
    puts_raw(t);
    puts_raw("\x1b[0m\n");
}

static void show_legacy(void) {
    title("1) Legacy ModuOS backslash color codes");

    puts_raw("(These are parsed by VGA_Write/VGA_WriteN)\n\n");

    /* Note: need to escape backslash in C strings */
    puts_raw("\\crRed  \\cgGreen  \\cyYellow  \\cbBlue  \\cpPurple  \\ccCyan  \\cwWhite  \\ckBlack\\rr\n");
    puts_raw("\\clrBrightRed  \\clgBrightGreen  \\clyBrightYellow  \\clbBrightBlue  \\clpBrightPurple  \\clcBrightCyan  \\clwBrightWhite\\rr\n");

    puts_raw("\nBackground demo: ");
    puts_raw("\\bk"); puts_raw(" BG:black ");
    puts_raw("\\br"); puts_raw(" BG:red ");
    puts_raw("\\bg"); puts_raw(" BG:green ");
    puts_raw("\\by"); puts_raw(" BG:yellow ");
    puts_raw("\\bb"); puts_raw(" BG:blue ");
    puts_raw("\\bp"); puts_raw(" BG:purple ");
    puts_raw("\\bc"); puts_raw(" BG:cyan ");
    puts_raw("\\bw"); puts_raw(" BG:white ");
    puts_raw("\\rr\n");
}

static void show_ansi(void) {
    title("2) ANSI SGR (ESC[...m)");

    puts_raw("\x1b[31mRed\x1b[0m ");
    puts_raw("\x1b[32mGreen\x1b[0m ");
    puts_raw("\x1b[33mYellow\x1b[0m ");
    puts_raw("\x1b[34mBlue\x1b[0m ");
    puts_raw("\x1b[35mMagenta\x1b[0m ");
    puts_raw("\x1b[36mCyan\x1b[0m ");
    puts_raw("\x1b[37mWhite\x1b[0m\n");

    puts_raw("\x1b[91mBrightRed\x1b[0m ");
    puts_raw("\x1b[92mBrightGreen\x1b[0m ");
    puts_raw("\x1b[93mBrightYellow\x1b[0m ");
    puts_raw("\x1b[94mBrightBlue\x1b[0m ");
    puts_raw("\x1b[95mBrightMagenta\x1b[0m ");
    puts_raw("\x1b[96mBrightCyan\x1b[0m ");
    puts_raw("\x1b[97mBrightWhite\x1b[0m\n");

    puts_raw("\nBackground demo: ");
    puts_raw("\x1b[40m BG:black \x1b[0m");
    puts_raw("\x1b[41m BG:red \x1b[0m");
    puts_raw("\x1b[42m BG:green \x1b[0m");
    puts_raw("\x1b[43m BG:yellow \x1b[0m");
    puts_raw("\x1b[44m BG:blue \x1b[0m");
    puts_raw("\x1b[45m BG:magenta \x1b[0m");
    puts_raw("\x1b[46m BG:cyan \x1b[0m");
    puts_raw("\x1b[47m BG:white \x1b[0m");
    puts_raw("\n");
}

static void show_syscalls(void) {
    title("3) Direct VGA syscalls");

    puts_raw("Current attr (bg<<4|fg): 0x");
    printf("%x\n", (unsigned)vga_get_color());

    puts_raw("\nSetting colors via vga_set_color(fg,bg)...\n");
    puts_raw("(Tip: if this scrolls too fast in QEMU, colortest now pauses between rows.)\n\n");

    for (int bg = 0; bg < 8; bg++) {
        for (int fg = 0; fg < 16; fg++) {
            vga_set_color((uint8_t)fg, (uint8_t)bg);
            printf("%X", fg);
        }
        vga_reset_color();
        puts_raw("  bg=");
        printf("%d\n", bg);

        /* SYS_SLEEP is seconds-based; keep it obvious/visible in emulators */
    }

    vga_reset_color();
    puts_raw("\nReset done.\n");
}

static void flash_demo(void) {
    title("4) Flashing demo (emulator-visible)");

    puts_raw("This alternates background colors once per second for ~10 seconds.\n");
    puts_raw("If you still don't see it, try running QEMU with -vga std or -vga virtio.\n\n");

    for (int i = 0; i < 10; i++) {
        /* Alternate between two high-contrast schemes */
        if (i & 1) vga_set_color(15, 4); /* white on red */
        else       vga_set_color(0, 14); /* black on yellow */

        puts_raw(" FLASH ");
        puts_raw(" (should be obvious) \n");
    }

    vga_reset_color();
    puts_raw("\nFlashing demo finished.\n");
}

int md_main(long argc, char **argv) {
    (void)argc;
    (void)argv;

    puts_raw("colortest - ModuOS console colors\n");
    puts_raw("================================\n");

    show_legacy();
    show_ansi();
    show_syscalls();
    flash_demo();

    return 0;
}
