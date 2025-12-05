#include "moduos/kernel/events/events.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/games/verticalpingpong.h"
#include <stddef.h>
#include <stdbool.h>

// Pong constants
#define GAME_WIDTH 40
#define GAME_HEIGHT 20

// Paddle constants
#define PADDLE_HEIGHT 4
#define LEFT_PADDLE_X 1
#define RIGHT_PADDLE_X (GAME_WIDTH - 2)

// Ball structure
typedef struct {
    int x;
    int y;
    int dx; // -1 or 1
    int dy; // -1, 0 or 1
} Ball;

// Paddle structure
typedef struct {
    int y; // top y coordinate
} Paddle;

// Game state
typedef struct {
    Paddle left;
    Paddle right;
    Ball ball;
    int score_left;
    int score_right;
    bool paused;
    bool running;
} PongGame;

// Initialize game
static void game_init(PongGame *g) {
    g->left.y = (GAME_HEIGHT - PADDLE_HEIGHT) / 2;
    g->right.y = (GAME_HEIGHT - PADDLE_HEIGHT) / 2;

    g->ball.x = GAME_WIDTH / 2;
    g->ball.y = GAME_HEIGHT / 2;
    g->ball.dx = (rand() % 2) ? 1 : -1;
    int r = (int)(rand() % 3) - 1; // -1,0,1
    g->ball.dy = r;

    g->score_left = 0;
    g->score_right = 0;
    g->paused = false;
    g->running = true;
}

// Clamp paddle inside playfield
static void clamp_paddle(Paddle *p) {
    if (p->y < 0) p->y = 0;
    if (p->y > GAME_HEIGHT - PADDLE_HEIGHT) p->y = GAME_HEIGHT - PADDLE_HEIGHT;
}

// Reset ball to center and give it a direction towards last scorer (dir: -1 left, 1 right, 0 random)
static void reset_ball(PongGame *g, int dir) {
    g->ball.x = GAME_WIDTH / 2;
    g->ball.y = GAME_HEIGHT / 2;
    if (dir == 0) {
        g->ball.dx = (rand() % 2) ? 1 : -1;
    } else {
        g->ball.dx = dir;
    }
    g->ball.dy = (int)(rand() % 3) - 1;
}

// Update game logic
static void game_update(PongGame *g) {
    if (!g->running || g->paused) return;

    // Move ball
    g->ball.x += g->ball.dx;
    g->ball.y += g->ball.dy;

    // Top/bottom collision
    if (g->ball.y < 0) {
        g->ball.y = 0;
        g->ball.dy = -g->ball.dy;
    }
    if (g->ball.y >= GAME_HEIGHT) {
        g->ball.y = GAME_HEIGHT - 1;
        g->ball.dy = -g->ball.dy;
    }

    // Left paddle collision
    if (g->ball.x == LEFT_PADDLE_X + 1) {
        if (g->ball.y >= g->left.y && g->ball.y < g->left.y + PADDLE_HEIGHT) {
            g->ball.dx = 1; // bounce to right
            // change dy depending on where it hit paddle
            int center = g->left.y + PADDLE_HEIGHT/2;
            g->ball.dy = (g->ball.y < center) ? -1 : (g->ball.y > center) ? 1 : 0;
        }
    }

    // Right paddle collision
    if (g->ball.x == RIGHT_PADDLE_X - 1) {
        if (g->ball.y >= g->right.y && g->ball.y < g->right.y + PADDLE_HEIGHT) {
            g->ball.dx = -1; // bounce to left
            int center = g->right.y + PADDLE_HEIGHT/2;
            g->ball.dy = (g->ball.y < center) ? -1 : (g->ball.y > center) ? 1 : 0;
        }
    }

    // Score: left misses
    if (g->ball.x < 0) {
        g->score_right++;
        reset_ball(g, -1); // send ball toward left (winner served to left)
    }

    // Score: right misses
    if (g->ball.x > GAME_WIDTH - 1) {
        g->score_left++;
        reset_ball(g, 1); // send ball toward right
    }
}

// Draw game
static void game_draw(PongGame *g) {
    // Top border
    VGA_Write("+");
    for (int i = 0; i < GAME_WIDTH; i++) VGA_Write("-");
    VGA_Write("+\n");

    for (int y = 0; y < GAME_HEIGHT; y++) {
        VGA_Write("|");
        for (int x = 0; x < GAME_WIDTH; x++) {
            // Left paddle
            if (x == LEFT_PADDLE_X && y >= g->left.y && y < g->left.y + PADDLE_HEIGHT) {
                VGA_Write("|");
            }
            // Right paddle
            else if (x == RIGHT_PADDLE_X && y >= g->right.y && y < g->right.y + PADDLE_HEIGHT) {
                VGA_Write("|");
            }
            // Ball
            else if (x == g->ball.x && y == g->ball.y) {
                VGA_Write("@");
            }
            // Middle dashed line
            else if (x == GAME_WIDTH/2 && (y % 2) == 0) {
                VGA_Write(".");
            }
            else {
                VGA_Write(" ");
            }
        }
        VGA_Write("|\n");
    }

    // Bottom border
    VGA_Write("+");
    for (int i = 0; i < GAME_WIDTH; i++) VGA_Write("-");
    VGA_Write("+\n");

    // Scores and controls
    VGA_Write("Left: ");
    // score to string
    if (g->score_left == 0) VGA_Write("0");
    else {
        char s[16]; int idx=0; int v=g->score_left; while (v>0){ s[idx++]= '0' + (v%10); v/=10; } for (int i=idx-1;i>=0;i--) VGA_WriteChar(s[i]);
    }
    VGA_Write("  Right: ");
    if (g->score_right == 0) VGA_Write("0");
    else {
        char s[16]; int idx=0; int v=g->score_right; while (v>0){ s[idx++]= '0' + (v%10); v/=10; } for (int i=idx-1;i>=0;i--) VGA_WriteChar(s[i]);
    }
    VGA_Write("\n");

    VGA_Write("W/S: Move Left | Up/Down: Move Right | P: Pause | ESC: Quit\n");
    if (g->paused) VGA_Write("\n*** PAUSED ***\n");
}

// Handle input
static void handle_input(PongGame *g, Event *e) {
    if (e->type != EVENT_KEY_PRESSED) return;

    // Quit
    if (e->data.keyboard.keycode == KEY_ESCAPE) {
        g->running = false;
        return;
    }

    // Pause
    if (e->data.keyboard.ascii == 'p' || e->data.keyboard.ascii == 'P') {
        g->paused = !g->paused;
        return;
    }

    // Left paddle: W/S
    if (e->data.keyboard.ascii == 'w' || e->data.keyboard.ascii == 'W') {
        g->left.y--;
        clamp_paddle(&g->left);
        return;
    }
    if (e->data.keyboard.ascii == 's' || e->data.keyboard.ascii == 'S') {
        g->left.y++;
        clamp_paddle(&g->left);
        return;
    }

    // Right paddle: arrow keys
    switch (e->data.keyboard.keycode) {
        case KEY_ARROW_UP:
            g->right.y--;
            clamp_paddle(&g->right);
            break;
        case KEY_ARROW_DOWN:
            g->right.y++;
            clamp_paddle(&g->right);
            break;
        default:
            break;
    }
}

// Main game loop
void play_pong_game(void) {
    VGA_EnableScrolling(false);
    VGA_HideCursor();

    PongGame game;
    Event event;
    bool quit = false;
    int frame_counter = 0;
    int game_speed = 120; // lower = faster

    // Seed RNG
    srand(12345);

    game_init(&game);

    event_clear();

    VGA_Clear();
    VGA_Write("Starting Pong... Press any key to begin\n");
    event_wait();

    VGA_Clear();
    game_draw(&game);

    while (!quit) {
        // Process all input events
        while (event_poll(&event)) {
            if (event.type == EVENT_KEY_PRESSED) {
                // ESC handled inside handle_input - also allow immediate quit
                if (event.data.keyboard.keycode == KEY_ESCAPE) { quit = true; break; }
                handle_input(&game, &event);
            }
        }
        if (quit) break;

        // Update at fixed intervals
        frame_counter++;
        if (frame_counter >= game_speed) {
            frame_counter = 0;
            game_update(&game);
            VGA_Clear();
            game_draw(&game);
        }

        // Small busy wait to control CPU usage
        for (volatile int i = 0; i < 180000; i++);
    }

    VGA_Clear();
    VGA_Write("\n========================================\n");
    VGA_Write("         THANKS FOR PLAYING PONG!       \n");
    VGA_Write("========================================\n\n");
    VGA_Write("Final Score - Left: ");
    if (game.score_left == 0) VGA_Write("0"); else { char s[16]; int idx=0; int v=game.score_left; while (v>0){ s[idx++]='0'+(v%10); v/=10;} for (int i=idx-1;i>=0;i--) VGA_WriteChar(s[i]); }
    VGA_Write("  Right: ");
    if (game.score_right == 0) VGA_Write("0"); else { char s[16]; int idx=0; int v=game.score_right; while (v>0){ s[idx++]='0'+(v%10); v/=10;} for (int i=idx-1;i>=0;i--) VGA_WriteChar(s[i]); }

    VGA_WriteChar('\n');

    VGA_EnableScrolling(true);
    VGA_ShowCursor();
    event_wait();
}
