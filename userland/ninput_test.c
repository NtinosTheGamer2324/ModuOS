// n_kbdcat.c — reads structured key events from $/dev/n_input/kbd0
#include "libc.h"
#include "../include/moduos/drivers/input/gscancode.h"

static const char* state_str(key_state_t s) {
    return (s == KEY_PRESSED) ? "DOWN" : "UP";
}

int md_main(long argc, char** argv) {
    (void)argc; (void)argv;

    int fd = open("$/dev/n_input/kbd0", O_RDONLY, 0);
    if (fd < 0) {
        puts("n_kbdcat: failed to open $/dev/n_input/kbd0");
        return 1;
    }

    puts("n_kbdcat: reading structured key events, Ctrl+C to quit");

    key_event_t ev;
    for (;;) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n == (ssize_t)sizeof(ev)) {
            printf("scancode=0x%02x state=%s mods[shiftL=%d shiftR=%d ctrlL=%d ctrlR=%d altL=%d altR=%d super=%d caps=%d]\n",
                   ev.scancode,
                   state_str(ev.state),
                   ev.modifiers.shift_l, ev.modifiers.shift_r,
                   ev.modifiers.ctrl_l,  ev.modifiers.ctrl_r,
                   ev.modifiers.alt_l,   ev.modifiers.alt_r,
                   ev.modifiers.super,   ev.modifiers.caps_lock);
        } else if (n == 0) {
            // O_NONBLOCK would land here with nothing queued; on a blocking
            // fd this shouldn't normally happen, but don't spin on it.
            yield();
        } else {
            puts("n_kbdcat: read error");
            break;
        }
    }

    close(fd);
    return 0;
}