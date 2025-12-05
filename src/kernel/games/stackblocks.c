#include "moduos/kernel/events/events.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/games/stackblocks.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// TETRIS for moduOS - single file implementation
// Playfield
#define FIELD_W 10
#define FIELD_H 20

// Piece definitions (7 tetrominoes, 4 rotation states, 4x4 matrix)
static const uint16_t PIECES[7][4] = {
    // I
    {0x0F00, 0x2222, 0x00F0, 0x4444},
    // J
    {0x8E00, 0x6440, 0x0E20, 0x44C0},
    // L
    {0x2E00, 0x4460, 0x0E80, 0xC440},
    // O
    {0x6600, 0x6600, 0x6600, 0x6600},
    // S
    {0x6C00, 0x4620, 0x06C0, 0x8C40},
    // T
    {0x4E00, 0x4640, 0x0E40, 0x4C40},
    // Z
    {0xC600, 0x2640, 0x0C60, 0x4C80}
};

// Game structures
typedef struct {
    int x, y; // position of piece (top-left of 4x4 block)
    int type; // 0..6
    int rot;  // 0..3
} Piece;

typedef struct {
    uint8_t field[FIELD_H][FIELD_W]; // 0 empty, 1 filled
    Piece cur;
    Piece next;
    int score;
    int level;
    int lines_cleared;
    bool game_over;
    bool paused;
    bool running;
} Tetris;

// RNG
static uint32_t rng_seed = 12345;
void srand3(uint32_t s) { rng_seed = s; }
uint32_t rand3(void) { rng_seed = (rng_seed * 1103515245 + 12345) & 0x7fffffff; return rng_seed; }

// Helper: test if cell (px,py) of piece is occupied
static bool piece_cell(int type, int rot, int px, int py) {
    uint16_t m = PIECES[type][rot & 3];
    int bit = 15 - (py * 4 + px);
    return ((m >> bit) & 1) != 0;
}

// Spawn a random piece
static Piece spawn_piece(void) {
    Piece p;
    p.type = rand3() % 7;
    p.rot = 0;
    p.x = (FIELD_W / 2) - 2; // center 4x4
    p.y = 0;
    return p;
}

// Check collision of piece with field
static bool collides(Tetris *g, Piece *p) {
    for (int py = 0; py < 4; py++) {
        for (int px = 0; px < 4; px++) {
            if (!piece_cell(p->type, p->rot, px, py)) continue;
            int fx = p->x + px;
            int fy = p->y + py;
            if (fx < 0 || fx >= FIELD_W || fy < 0 || fy >= FIELD_H) return true;
            if (g->field[fy][fx]) return true;
        }
    }
    return false;
}

// Lock piece into the field
static void lock_piece(Tetris *g) {
    for (int py = 0; py < 4; py++) {
        for (int px = 0; px < 4; px++) {
            if (!piece_cell(g->cur.type, g->cur.rot, px, py)) continue;
            int fx = g->cur.x + px;
            int fy = g->cur.y + py;
            if (fy >= 0 && fy < FIELD_H && fx >= 0 && fx < FIELD_W) g->field[fy][fx] = 1;
        }
    }
}

// Clear full lines and return how many cleared
static int clear_lines(Tetris *g) {
    int cleared = 0;
    for (int y = FIELD_H - 1; y >= 0; y--) {
        bool full = true;
        for (int x = 0; x < FIELD_W; x++) if (!g->field[y][x]) { full = false; break; }
        if (full) {
            cleared++;
            // move everything above down
            for (int ty = y; ty > 0; ty--) for (int x = 0; x < FIELD_W; x++) g->field[ty][x] = g->field[ty-1][x];
            // clear top row
            for (int x = 0; x < FIELD_W; x++) g->field[0][x] = 0;
            y++; // recheck same line index because we moved rows down
        }
    }
    return cleared;
}

// Scoring: classic-like
static void apply_score(Tetris *g, int lines) {
    if (lines <= 0) return;
    int points = 0;
    switch (lines) {
        case 1: points = 40; break;
        case 2: points = 100; break;
        case 3: points = 300; break;
        case 4: points = 1200; break;
    }
    points *= (g->level + 1);
    g->score += points;
    g->lines_cleared += lines;
    // level up every 10 lines
    while (g->lines_cleared >= 10) { g->lines_cleared -= 10; g->level++; }
}

// Hard drop: drop until collision and lock
static void hard_drop(Tetris *g) {
    while (true) {
        Piece p = g->cur;
        p.y++;
        if (collides(g, &p)) break;
        g->cur.y++;
        g->score += 2; // reward for hard drop
    }
    lock_piece(g);
    int c = clear_lines(g);
    apply_score(g, c);
    g->cur = g->next;
    g->next = spawn_piece();
    if (collides(g, &g->cur)) g->game_over = true;
}

// Soft drop: move down one if possible
static void soft_drop(Tetris *g) {
    Piece p = g->cur;
    p.y++;
    if (!collides(g, &p)) {
        g->cur.y++;
        g->score += 1;
    } else {
        // lock
        lock_piece(g);
        int c = clear_lines(g);
        apply_score(g, c);
        g->cur = g->next;
        g->next = spawn_piece();
        if (collides(g, &g->cur)) g->game_over = true;
    }
}

// Try rotate with basic wall kick: attempt to rotate, if collides try shifts
static void rotate_piece(Tetris *g, int dir) {
    Piece p = g->cur;
    p.rot = (p.rot + dir) & 3;
    if (!collides(g, &p)) { g->cur.rot = p.rot; return; }
    // try left/right kicks
    p.x = g->cur.x - 1; if (!collides(g, &p)) { g->cur.x = p.x; g->cur.rot = p.rot; return; }
    p.x = g->cur.x + 1; if (!collides(g, &p)) { g->cur.x = p.x; g->cur.rot = p.rot; return; }
    // try up
    p.x = g->cur.x; p.y = g->cur.y - 1; if (!collides(g, &p)) { g->cur.y = p.y; g->cur.rot = p.rot; return; }
}

// Draw game
static void draw(Tetris *g) {
    // borders
    VGA_Write("+"); for (int i=0;i<FIELD_W;i++) VGA_Write("-"); VGA_Write("+  Next:\n");
    for (int y=0;y<FIELD_H;y++) {
        VGA_Write("|");
        for (int x=0;x<FIELD_W;x++) {
            // check if current piece occupies this cell
            bool drew = false;
            for (int py=0;py<4;py++) for (int px=0;px<4;px++) {
                if (piece_cell(g->cur.type, g->cur.rot, px, py)) {
                    int fx = g->cur.x + px; int fy = g->cur.y + py;
                    if (fx==x && fy==y) { VGA_Write("@"); drew=true; }
                }
            }
            if (drew) continue;
            VGA_Write(g->field[y][x] ? "#" : " ");
        }
        VGA_Write("|");
        // draw next piece preview on right
        if (y < 4) {
            VGA_Write("  ");
            for (int px=0; px<4; px++) {
                if (piece_cell(g->next.type, g->next.rot, px, y)) VGA_Write("#"); else VGA_Write(" ");
            }
        }
        VGA_Write("\n");
    }
    VGA_Write("+"); for (int i=0;i<FIELD_W;i++) VGA_Write("-"); VGA_Write("+\n");
    // info
    VGA_Write("Score: ");
    if (g->score==0) VGA_Write("0"); else { char s[16]; int idx=0,v=g->score; while(v>0){ s[idx++]= '0'+(v%10); v/=10;} for(int i=idx-1;i>=0;i--) VGA_WriteChar(s[i]); }
    VGA_Write("  Level: "); VGA_WriteChar('0' + (g->level % 10));
    VGA_Write("\nControls: Left/Right: Move | Up: Rotate | Down: Soft drop | Space: Hard drop | P: Pause | ESC: Quit\n");
    if (g->paused) VGA_Write("*** PAUSED ***\n");
    if (g->game_over) VGA_Write("*** GAME OVER - Press ENTER to play again or ESC to quit ***\n");
}

// Handle input
static void handle_input(Tetris *g, Event *e) {
    if (e->type != EVENT_KEY_PRESSED) return;
    if (g->game_over) {
        if (e->data.keyboard.keycode == KEY_ENTER) g->running = true; // will re-init in main
        return;
    }
    if (e->data.keyboard.keycode == KEY_ESCAPE) g->running = false;

    switch (e->data.keyboard.keycode) {
        case KEY_ARROW_LEFT: { Piece p = g->cur; p.x--; if (!collides(g,&p)) g->cur.x--; } break;
        case KEY_ARROW_RIGHT: { Piece p = g->cur; p.x++; if (!collides(g,&p)) g->cur.x++; } break;
        case KEY_ARROW_UP: rotate_piece(g, 1); break;
        case KEY_ARROW_DOWN: soft_drop(g); break;
        default:
            if (e->data.keyboard.ascii == ' ') hard_drop(g);
            else if (e->data.keyboard.ascii == 'p' || e->data.keyboard.ascii == 'P') g->paused = !g->paused;
            break;
    }
}

// Initialize game
static void init(Tetris *g) {
    for (int y=0;y<FIELD_H;y++) for (int x=0;x<FIELD_W;x++) g->field[y][x]=0;
    g->score = 0; g->level = 0; g->lines_cleared = 0; g->game_over=false; g->paused=false; g->running=true;
    g->cur = spawn_piece(); g->next = spawn_piece();
}

// Main loop
void play_tetris_game(void) {
    VGA_EnableScrolling(false);
    VGA_HideCursor();

    Tetris g;
    Event e;
    int frame = 0;
    int speed = 40; // lower is faster

    srand3(12345);
    init(&g);

    VGA_Clear(); VGA_Write("Starting Tetris... Press any key to begin\n"); event_wait();

    VGA_Clear(); draw(&g);

    while (g.running) {
        while (event_poll(&e)) {
            if (e.type == EVENT_KEY_PRESSED) {
                if (e.data.keyboard.keycode == KEY_ESCAPE) { g.running = false; break; }
                handle_input(&g, &e);
            }
        }
        if (!g.running) break;

        if (!g.game_over && !g.paused) {
            frame++;
            int ticks_per_drop = (speed - (g.level * 3)); if (ticks_per_drop < 5) ticks_per_drop = 5;
            if (frame >= ticks_per_drop) {
                frame = 0; // move piece down
                Piece p = g.cur; p.y++;
                if (!collides(&g, &p)) { g.cur.y++; }
                else {
                    lock_piece(&g);
                    int c = clear_lines(&g);
                    apply_score(&g, c);
                    g.cur = g.next; g.next = spawn_piece();
                    if (collides(&g, &g.cur)) g.game_over = true;
                }
            }
        }

        VGA_Clear(); draw(&g);
        for (volatile int i=0;i<900000;i++);
    }

    VGA_Clear(); VGA_Write("\n========================================\n"); VGA_Write("        THANKS FOR PLAYING TETRIS!       \n"); VGA_Write("========================================\n\n"); VGA_Write("Final Score: "); if (g.score==0) VGA_Write("0"); else { char s[32]; int idx=0,v=g.score; while(v>0){ s[idx++]='0'+(v%10); v/=10;} for(int i=idx-1;i>=0;i--) VGA_WriteChar(s[i]); }
    VGA_Write("\n");
    VGA_EnableScrolling(true);
    VGA_ShowCursor();
    event_wait();
}
