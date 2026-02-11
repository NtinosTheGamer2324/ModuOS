#include "libc.h"

// ANSI Utility Macros
#define CLS          "\x1b[2J"
#define GOTO(r,c)    printf("\x1b[%d;%dH", r, c)
#define COLOR(f,b)   printf("\x1b[%d;%dm", f, b)
#define RESET        printf("\x1b[0m")

// Colors (ANSI Standards)
#define FG_WHITE     37
#define BG_BLUE      44
#define FG_YELLOW    33
#define BG_CYAN      46
#define FG_BLACK     30

void draw_window(int r, int c, int w, int h, const char* title) {
    // Background Fill
    for (int i = 0; i < h; i++) {
        GOTO(r + i, c);
        COLOR(FG_WHITE, BG_BLUE);
        for (int j = 0; j < w; j++) putc(' ');
    }

    // Header bar
    GOTO(r, c);
    COLOR(FG_BLACK, BG_CYAN);
    for (int i = 0; i < w; i++) putc(' ');
    GOTO(r, c + (w/2) - (strlen(title)/2));
    printf("%s", title);

    // Box Borders (Using your printf's ability to print characters)
    COLOR(FG_WHITE, BG_BLUE);
    for (int i = 1; i < h-1; i++) {
        GOTO(r + i, c); printf("│");
        GOTO(r + i, c + w - 1); printf("│");
    }
}

void draw_ui(int sel) {
    RESET;
    printf(CLS);

    // Sidebar Window
    draw_window(2, 2, 30, 8, "System Info");
    GOTO(4, 4); printf("OS:  ModuOS 64");
    GOTO(6, 4); printf("Arc: AMD64");

    // Main Selection Window
    draw_window(12, 40, 50, 10, "Hardware Setup");
    GOTO(14, 42); printf("Select your GPU driver:");

    const char* options[] = {"VGA Generic", "QXL Virtual", "VMSVGA VMware"};
    for (int i = 0; i < 3; i++) {
        GOTO(16 + i, 44);
        if (i == sel) {
            COLOR(FG_BLACK, BG_CYAN); // Selection highlight
            printf(" > [X] %s ", options[i]);
        } else {
            COLOR(FG_WHITE, BG_BLUE);
            printf("   [ ] %s ", options[i]);
        }
    }

    // Status line
    GOTO(48, 1);
    COLOR(FG_BLACK, 47); // Black on Light Gray
    for(int i=0; i<128; i++) putc(' ');
    GOTO(48, 2);
    printf("W/S: Move | ENTER: Confirm | ModuOS OOBE Phase");
    RESET;
}

int md_main(long argc, char** argv) {
    int sel = 0;
    while(1) {
        draw_ui(sel);
        
        char* k = input(); // Blocking read
        if (strcmp(k, "s") == 0) sel = (sel + 1) % 3;
        else if (strcmp(k, "w") == 0) sel = (sel + 2) % 3;
        else if (strlen(k) == 0) break; // Enter
    }

    printf(CLS);
    GOTO(24, 50);
    printf("Saving & Cleaning up...");

    sleep(1);
    exit(0);
    return 0;
}