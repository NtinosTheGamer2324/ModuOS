#include "moduos/kernel/events/events.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/games/avoidthemine.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIDTH 10
#define HEIGHT 10
#define NUM_MINES 10

// Cell states
typedef enum {
    CELL_HIDDEN,
    CELL_REVEALED,
    CELL_FLAGGED
} CellState;

// Cell structure
typedef struct {
    bool mine;
    int adjacent;
    CellState state;
} Cell;

// Game state
typedef struct {
    Cell board[HEIGHT][WIDTH];
    int cursor_x;
    int cursor_y;
    bool game_over;
    bool win;
    bool paused;
    bool first_move;
} Minesweeper;

// Simple RNG
static uint32_t rng_seed = 12345;
void srand2(uint32_t s) { rng_seed = s; }
uint32_t rand2(void) {
    rng_seed = (rng_seed * 1103515245 + 12345) & 0x7fffffff;
    return rng_seed;
}

static void place_mines(Minesweeper *g, int safe_x, int safe_y) {
    int placed = 0;
    while (placed < NUM_MINES) {
        int x = rand2() % WIDTH;
        int y = rand2() % HEIGHT;
        if ((x == safe_x && y == safe_y) || g->board[y][x].mine) continue;
        g->board[y][x].mine = true;
        placed++;
    }
    // Compute adjacent counts
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            if (g->board[y][x].mine) continue;
            int count = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && nx < WIDTH && ny >= 0 && ny < HEIGHT) {
                        if (g->board[ny][nx].mine) count++;
                    }
                }
            }
            g->board[y][x].adjacent = count;
        }
    }
}

static void game_init(Minesweeper *g) {
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            g->board[y][x].mine = false;
            g->board[y][x].adjacent = 0;
            g->board[y][x].state = CELL_HIDDEN;
        }
    }
    g->cursor_x = WIDTH / 2;
    g->cursor_y = HEIGHT / 2;
    g->game_over = false;
    g->paused = false;
    g->win = false;
    g->first_move = true;
}

static void reveal_cell(Minesweeper *g, int x, int y) {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
    Cell *c = &g->board[y][x];
    if (c->state != CELL_HIDDEN) return;

    c->state = CELL_REVEALED;

    if (c->mine) {
        g->game_over = true;
        g->win = false;
        return;
    }

    if (c->adjacent == 0) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (dx != 0 || dy != 0) reveal_cell(g, x + dx, y + dy);
            }
        }
    }
}

static bool check_win(Minesweeper *g) {
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            Cell *c = &g->board[y][x];
            if (!c->mine && c->state != CELL_REVEALED) return false;
        }
    }
    return true;
}

static void draw_game(Minesweeper *g) {
    VGA_Write("+");
    for (int i = 0; i < WIDTH; i++) VGA_Write("-");
    VGA_Write("+\n");

    for (int y = 0; y < HEIGHT; y++) {
        VGA_Write("|");
        for (int x = 0; x < WIDTH; x++) {
            Cell *c = &g->board[y][x];
            if (x == g->cursor_x && y == g->cursor_y) {
                VGA_Write("\\clg[\\rr");
                if (c->state == CELL_HIDDEN) VGA_Write(".");
                else if (c->state == CELL_FLAGGED) VGA_Write("\\crF\\rr");
                else if (c->mine && g->game_over) VGA_Write("\\cr*\\rr");
                else if (c->adjacent > 0) {
                    VGA_WriteChar('0' + c->adjacent);
                } else VGA_Write(" ");
                VGA_Write("\\clg]\\rr");
            } else {
                if (c->state == CELL_HIDDEN) VGA_Write(".");
                else if (c->state == CELL_FLAGGED) VGA_Write("\\crF\\rr");
                else if (c->mine && g->game_over) VGA_Write("\\cr*\\rr");
                else if (c->adjacent > 0) {
                    VGA_WriteChar('0' + c->adjacent);
                } else VGA_Write(" ");
            }
        }
        VGA_Write("|\n");
    }

    VGA_Write("+");
    for (int i = 0; i < WIDTH; i++) VGA_Write("-");
    VGA_Write("+\n");

    if (g->paused) VGA_Write("*** PAUSED ***\n");
    if (g->game_over) {
        if (g->win) VGA_Write("\\clg*** YOU WIN! ***\\rr\n");
        else VGA_Write("\\cr*** GAME OVER ***\\rr\n");
        VGA_Write("Press ENTER to play again or ESC to quit\n");
    } else {
        VGA_Write("Arrows: Move | Space: Reveal | F: Flag | P: Pause | ESC: Quit\n");
    }
}

static void handle_input(Minesweeper *g, Event *e) {
    if (e->type != EVENT_KEY_PRESSED) return;

    if (g->game_over) {
        if (e->data.keyboard.keycode == KEY_ENTER) {
            game_init(g);
        }
        return;
    }

    switch (e->data.keyboard.keycode) {
        case KEY_ARROW_UP: if (g->cursor_y > 0) g->cursor_y--; break;
        case KEY_ARROW_DOWN: if (g->cursor_y < HEIGHT - 1) g->cursor_y++; break;
        case KEY_ARROW_LEFT: if (g->cursor_x > 0) g->cursor_x--; break;
        case KEY_ARROW_RIGHT: if (g->cursor_x < WIDTH - 1) g->cursor_x++; break;
        case KEY_ESCAPE: g->game_over = true; break;
        default:
            if (e->data.keyboard.ascii == 'p' || e->data.keyboard.ascii == 'P') {
                g->paused = !g->paused;
            } else if (e->data.keyboard.ascii == 'f' || e->data.keyboard.ascii == 'F') {
                Cell *c = &g->board[g->cursor_y][g->cursor_x];
                if (c->state == CELL_HIDDEN) c->state = CELL_FLAGGED;
                else if (c->state == CELL_FLAGGED) c->state = CELL_HIDDEN;
            } else if (e->data.keyboard.ascii == ' ' || e->data.keyboard.ascii == '\r') {
                if (g->first_move) {
                    place_mines(g, g->cursor_x, g->cursor_y);
                    g->first_move = false;
                }
                reveal_cell(g, g->cursor_x, g->cursor_y);
                if (!g->game_over && check_win(g)) {
                    g->game_over = true;
                    g->win = true;
                }
            }
            break;
    }
}

void play_minesweeper_game(void) {
    VGA_EnableScrolling(false);
    VGA_HideCursor();

    Minesweeper game;
    Event e;
    bool quit = false;

    srand2(12345);
    game_init(&game);

    VGA_Clear();
    VGA_Write("Starting Minesweeper... Press any key to begin\n");
    event_wait();

    VGA_Clear();
    draw_game(&game);

    while (!quit) {
        while (event_poll(&e)) {
            if (e.type == EVENT_KEY_PRESSED) {
                if (e.data.keyboard.keycode == KEY_ESCAPE) { quit = true; break; }
                handle_input(&game, &e);
            }
        }
        if (quit) break;

        VGA_Clear();
        draw_game(&game);
        for (volatile int i = 0; i < 900000; i++);
    }

    VGA_Clear();
    VGA_Write("\n========================================\n");
    VGA_Write("      THANKS FOR PLAYING MINESWEEPER!     \n");
    VGA_Write("========================================\n\n");

    VGA_EnableScrolling(true);
    VGA_ShowCursor();
    event_wait();
}
