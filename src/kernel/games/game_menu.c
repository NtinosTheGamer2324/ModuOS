#include "moduos/kernel/events/events.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/games/eatfruit.h"
#include "moduos/kernel/games/verticalpingpong.h"
#include "moduos/kernel/games/avoidthemine.h"
#include "moduos/kernel/games/stackblocks.h"
#include "moduos/kernel/games/RaycasterFPS.h"

// =========================
// ===== ENUMS & STATE =====
// =========================
typedef enum {
    MENU_ITEM_SNAKE,
    MENU_ITEM_PONG,
    MENU_ITEM_MINE_SWEEP,
    MENU_ITEM_TETRIS,
    MENU_ITEM_DOOM,       // <-- Added DOOM
    MENU_ITEM_OPTIONS,
    MENU_ITEM_COUNT
} MenuItem;

typedef struct {
    bool sound_enabled;
    int difficulty; // 0 = Easy, 1 = Normal, 2 = Hard
} GameOptions;

static GameOptions options = { .sound_enabled = true, .difficulty = 1 };

// =========================
// ===== OPTIONS MENU ======
// =========================
static void show_options_menu(void) {
    Event event;
    int selected = 0;
    const char* difficulty_names[] = { "Easy", "Normal", "Hard" };
    bool done = false;

    while (!done) {
        VGA_Clear();
        VGA_Write("\n\\clg=== OPTIONS ===\\rr\n\n");

        VGA_Write("Sound: ");
        VGA_Write(options.sound_enabled ? "\\clgOn\\rr" : "\\crOff\\rr");
        VGA_Write("\n");

        VGA_Write("Difficulty: \\clb");
        VGA_Write(difficulty_names[options.difficulty]);
        VGA_Write("\\rr\n");

        VGA_Write("\nUse LEFT/RIGHT to change values\n");
        VGA_Write("ENTER to return\n");

        event = event_wait();

        if (event.type == EVENT_KEY_PRESSED) {
            switch (event.data.keyboard.keycode) {
                case KEY_ARROW_LEFT:
                    if (selected == 0) {
                        options.sound_enabled = !options.sound_enabled;
                    } else if (selected == 1 && options.difficulty > 0) {
                        options.difficulty--;
                    }
                    break;

                case KEY_ARROW_RIGHT:
                    if (selected == 0) {
                        options.sound_enabled = !options.sound_enabled;
                    } else if (selected == 1 && options.difficulty < 2) {
                        options.difficulty++;
                    }
                    break;

                case KEY_ARROW_UP:
                case KEY_ARROW_DOWN:
                    selected = !selected; // toggle between sound/difficulty
                    break;

                case KEY_ENTER:
                case KEY_ESCAPE:
                    done = true;
                    break;
            }
        }
    }
}

// =========================
// ===== MAIN MENU =========
// =========================
static int show_menu(void) {
    MenuItem selected = MENU_ITEM_SNAKE;
    Event event;
    bool done = false;

    const char* menu_items[] = {
        "Eat Fruit as a Snake",
        "Vertical Ping Pong",
        "Avoid The Boom Mine",
        "Stack Blocks",
        "RaycasterFPS",              // <-- Added DOOM to the list
        "Options"
    };

    while (!done) {
        VGA_Clear();
        VGA_Write("\n\\clg=== GAME SELECT MENU ===\\rr\n\n");

        for (int i = 0; i < MENU_ITEM_COUNT; i++) {
            if (i == selected) VGA_Write("\\clb> ");
            else VGA_Write("  ");
            VGA_Write(menu_items[i]);
            if (i == selected) VGA_Write(" <\\rr");
            VGA_WriteChar('\n');
        }

        VGA_Write("\nUse ARROWS to navigate | ENTER to select | ESC to quit\n");

        event = event_wait();
        if (event.type == EVENT_KEY_PRESSED) {
            switch (event.data.keyboard.keycode) {
                case KEY_ARROW_UP:
                    if (selected > 0) selected--;
                    break;

                case KEY_ARROW_DOWN:
                    if (selected < MENU_ITEM_COUNT - 1) selected++;
                    break;

                case KEY_ENTER:
                    done = true;
                    break;

                case KEY_ESCAPE:
                    return -1; // Quit
            }
        }
    }

    return selected;
}

// =========================
// ===== MENU HANDLER ======
// =========================
void Menu(void) {
    VGA_Clear();
    int slct = show_menu();

    if (slct == -1) {
        VGA_Clear();
        VGA_Write("\\crExiting Menu...\\rr\n");
        return;
    }

    switch (slct) {
        case MENU_ITEM_SNAKE:
            play_snake_game();
            break;
    
        case MENU_ITEM_PONG:
            play_pong_game();
            break;
    
        case MENU_ITEM_MINE_SWEEP:
            play_minesweeper_game();
            break;
    
        case MENU_ITEM_TETRIS:
            play_tetris_game();
            break;
    
        case MENU_ITEM_DOOM:           // <-- Added DOOM handler
            play_doom_game();
            break;
    
        case MENU_ITEM_OPTIONS:
            show_options_menu();
            break;
    }


    // Wait for confirmation before returning to menu
    VGA_Write("\n\n\\clgPress ENTER to return to menu...\\rr\n");
    Event e;
    do {
        e = event_wait();
    } while (e.type != EVENT_KEY_PRESSED || e.data.keyboard.keycode != KEY_ENTER);

    Menu(); // reopen main menu
}
