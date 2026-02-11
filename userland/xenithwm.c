#include "libc.h"
#include "string.h"
#include "../include/moduos/kernel/events/events.h"
#include "xenith26_proto.h"

/*
 * xenithwm.sqr
 *
 * Xenith26 window manager (MVP):
 *  - connects to $/dev/gui0
 *  - listens for window-created broadcasts + global mouse
 *  - click-to-focus/raise
 *  - drag-to-move
 */

static int send_msg(int fd, uint16_t type, const void *payload, uint32_t payload_len) {
    uint8_t buf[512];
    if (payload_len > (sizeof(buf) - sizeof(x26_hdr_t))) return -1;

    x26_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.magic = X26_MAGIC;
    h.type = type;
    h.size = (uint32_t)(sizeof(x26_hdr_t) + payload_len);

    memcpy(buf, &h, sizeof(h));
    if (payload_len) memcpy(buf + sizeof(h), payload, payload_len);

    return (write(fd, buf, h.size) == (ssize_t)h.size) ? 0 : -1;
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    int fd = open("$/dev/gui0", O_RDWR | O_NONBLOCK, 0);
    if (fd < 0) {
        puts_raw("xenithwm: cannot open $/dev/gui0\n");
        return 1;
    }

    /* register as WM */
    {
        x26_hdr_t h;
        memset(&h, 0, sizeof(h));
        h.magic = X26_MAGIC;
        h.type = X26_MSG_REGISTER_WM;
        h.size = (uint32_t)sizeof(h);
        (void)write(fd, &h, sizeof(h));
    }

    struct Win {
        uint32_t id;
        int32_t x,y;
        uint32_t w,h;
        int used;
    } wins[32];
    memset(wins, 0, sizeof(wins));

    int32_t mx = 0, my = 0;
    uint16_t buttons = 0, prev_buttons = 0;

    int dragging = 0;
    uint32_t drag_id = 0;
    int32_t drag_off_x = 0;
    int32_t drag_off_y = 0;

    puts_raw("xenithwm: running (click to raise, drag to move).\n");

    for (;;) {
        uint8_t buf[256];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0 && n >= (ssize_t)sizeof(x26_hdr_t)) {
            x26_hdr_t *h = (x26_hdr_t*)buf;
            if (h->magic == X26_MAGIC) {
                uint32_t payload_len = h->size - (uint32_t)sizeof(x26_hdr_t);
                uint8_t *payload = buf + sizeof(x26_hdr_t);

                if (h->type == X26_MSG_WINDOW_CREATED && payload_len >= sizeof(x26_window_created_t)) {
                    x26_window_created_t *wc = (x26_window_created_t*)payload;
                    for (int i = 0; i < 32; i++) {
                        if (!wins[i].used) {
                            wins[i].used = 1;
                            wins[i].id = wc->id;
                            wins[i].w = wc->w;
                            wins[i].h = wc->h;
                            wins[i].x = 50 + i*20;
                            wins[i].y = 50 + i*20;
                            /* Ask server to map window at our chosen pos */
                            x26_map_window_t mw;
                            mw.id = wc->id;
                            mw.x = (int16_t)wins[i].x;
                            mw.y = (int16_t)wins[i].y;
                            (void)send_msg(fd, X26_MSG_MAP_WINDOW, &mw, sizeof(mw));
                            break;
                        }
                    }
                } else if (h->type == X26_MSG_INPUT_EVENT && payload_len >= sizeof(x26_input_event_t)) {
                    x26_input_event_t *ie = (x26_input_event_t*)payload;
                    mx = ie->x;
                    my = ie->y;
                    prev_buttons = buttons;
                    buttons = ie->buttons;

                    int left_now = (buttons & 1) != 0;
                    int left_prev = (prev_buttons & 1) != 0;
                    int left_pressed = left_now && !left_prev;
                    int left_released = !left_now && left_prev;

                    if (left_pressed) {
                        /* hit-test topmost window = highest index used */
                        int hit = -1;
                        for (int i = 31; i >= 0; i--) {
                            if (!wins[i].used) continue;
                            if (mx >= wins[i].x && my >= wins[i].y && mx < (wins[i].x + (int32_t)wins[i].w) && my < (wins[i].y + (int32_t)wins[i].h)) {
                                hit = i;
                                break;
                            }
                        }
                        if (hit >= 0) {
                            x26_window_id_t rid;
                            rid.id = wins[hit].id;
                            (void)send_msg(fd, X26_MSG_RAISE_WINDOW, &rid, sizeof(rid));

                            dragging = 1;
                            drag_id = wins[hit].id;
                            drag_off_x = mx - wins[hit].x;
                            drag_off_y = my - wins[hit].y;
                        }
                    }

                    if (dragging && left_now) {
                        int32_t nx = mx - drag_off_x;
                        int32_t ny = my - drag_off_y;
                        x26_move_window_t mv;
                        mv.id = drag_id;
                        mv.x = (int16_t)nx;
                        mv.y = (int16_t)ny;
                        (void)send_msg(fd, X26_MSG_MOVE_WINDOW, &mv, sizeof(mv));

                        for (int i = 0; i < 32; i++) {
                            if (wins[i].used && wins[i].id == drag_id) {
                                wins[i].x = nx;
                                wins[i].y = ny;
                            }
                        }
                    }

                    if (left_released) {
                        dragging = 0;
                        drag_id = 0;
                    }
                }
            }
        }

        yield();
    }
}
