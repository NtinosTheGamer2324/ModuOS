// win_demo.c - windows.sqrl hello world: bouncing ball, click to move it, q to quit
#include "libc/libc.h"
#include "windows.h"

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;
    win_t *w = win_open("windows.sqrl demo", 320, 200);
    if (!w) { printf("win_demo: mdw not running\n"); return 1; }

    int x = 160, y = 100, vx = 3, vy = 2, mx = -1, my = -1;
    char buf[48];
    win_event_t e;
    while (win_alive(w)) {
        while (win_poll(w, &e)) {
            if (e.type == WIN_EV_KEY && e.ascii == 'q') win_close(w);
            if (e.type == WIN_EV_MOUSE_DOWN) { x = e.x; y = e.y; }
            if (e.type == WIN_EV_MOUSE_MOVE) { mx = e.x; my = e.y; }
        }
        if (!win_alive(w)) break;
        x += vx; y += vy;
        if (x < 10 || x > win_width(w) - 10)  vx = -vx;
        if (y < 10 || y > win_height(w) - 10) vy = -vy;

        win_clear(w, WIN_RGB(18, 20, 32));
        win_rect(w, 0, 0, win_width(w), win_height(w), WIN_GRAY);
        win_fill_circle(w, x, y, 10, WIN_RGB(255, 140, 0));
        win_line(w, 0, 0, x, y, WIN_BLUE);
        win_text(w, 8, 8, "windows.sqrl: click = move ball, q = quit", WIN_WHITE);
        sprintf(buf, "mouse %d,%d", mx, my);
        win_text(w, 8, win_height(w) - 20, buf, WIN_GREEN);
        win_present(w);
    }
    return 0;
}