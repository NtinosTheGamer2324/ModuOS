#include "moduos/kernel/events/events.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/games/eatfruit.h"
#include <stddef.h>
#include <stdbool.h>

// Game constants
#define GAME_WIDTH 40
#define GAME_HEIGHT 20
#define MAX_SNAKE_LENGTH 800

// Direction enum
typedef enum {
    DIR_UP = 0,
    DIR_DOWN = 1,
    DIR_LEFT = 2,
    DIR_RIGHT = 3
} Direction;

// Point structure
typedef struct {
    int x;
    int y;
} Point;

// Snake structure
typedef struct {
    Point body[MAX_SNAKE_LENGTH];
    int length;
    Direction direction;
} Snake;

// Game state
typedef struct {
    Snake snake;
    Point food;
    int score;
    bool game_over;
    bool paused;
} GameState;

// Simple random number generator (Linear Congruential Generator)
static uint32_t rng_seed = 12345;

void srand(uint32_t seed) {
    rng_seed = seed;
}

uint32_t rand(void) {
    rng_seed = (rng_seed * 1103515245 + 12345) & 0x7fffffff;
    return rng_seed;
}

// Initialize game state
void game_init(GameState *game) {
    // Initialize snake in the middle
    game->snake.length = 3;
    game->snake.direction = DIR_RIGHT;
    
    int start_x = GAME_WIDTH / 2;
    int start_y = GAME_HEIGHT / 2;
    
    game->snake.body[0].x = start_x;
    game->snake.body[0].y = start_y;
    game->snake.body[1].x = start_x - 1;
    game->snake.body[1].y = start_y;
    game->snake.body[2].x = start_x - 2;
    game->snake.body[2].y = start_y;
    
    // Initialize food
    game->food.x = rand() % GAME_WIDTH;
    game->food.y = rand() % GAME_HEIGHT;
    
    game->score = 0;
    game->game_over = false;
    game->paused = false;
}

// Check if point collides with snake
bool check_snake_collision(Snake *snake, Point p) {
    for (int i = 0; i < snake->length; i++) {
        if (snake->body[i].x == p.x && snake->body[i].y == p.y) {
            return true;
        }
    }
    return false;
}

// Spawn new food
void spawn_food(GameState *game) {
    do {
        game->food.x = rand() % GAME_WIDTH;
        game->food.y = rand() % GAME_HEIGHT;
    } while (check_snake_collision(&game->snake, game->food));
}

// Update game logic
void game_update(GameState *game) {
    if (game->game_over || game->paused) return;
    
    // Calculate new head position
    Point new_head = game->snake.body[0];
    
    switch (game->snake.direction) {
        case DIR_UP:
            new_head.y--;
            break;
        case DIR_DOWN:
            new_head.y++;
            break;
        case DIR_LEFT:
            new_head.x--;
            break;
        case DIR_RIGHT:
            new_head.x++;
            break;
    }
    
    // Check wall collision
    if (new_head.x < 0 || new_head.x >= GAME_WIDTH ||
        new_head.y < 0 || new_head.y >= GAME_HEIGHT) {
        game->game_over = true;
        return;
    }
    
    // Check self collision
    if (check_snake_collision(&game->snake, new_head)) {
        game->game_over = true;
        return;
    }
    
    // Check food collision
    bool ate_food = (new_head.x == game->food.x && new_head.y == game->food.y);
    
    if (ate_food) {
        game->score += 10;
        spawn_food(game);
        
        // Grow snake
        if (game->snake.length < MAX_SNAKE_LENGTH) {
            game->snake.length++;
        }
    }
    
    // Move snake body
    for (int i = game->snake.length - 1; i > 0; i--) {
        game->snake.body[i] = game->snake.body[i - 1];
    }
    
    // Update head
    game->snake.body[0] = new_head;
}

// Draw game (only call this after update)
void game_draw(GameState *game) {
    // Draw top border
    VGA_Write("+");
    for (int i = 0; i < GAME_WIDTH; i++) {
        VGA_Write("-");
    }
    VGA_Write("+\n");
    
    // Draw game area
    for (int y = 0; y < GAME_HEIGHT; y++) {
        VGA_Write("|");
        
        for (int x = 0; x < GAME_WIDTH; x++) {
            Point p = {x, y};
            
            // Check if this is the snake head
            if (x == game->snake.body[0].x && y == game->snake.body[0].y) {
                VGA_Write("\\clgO\\rr");
            }
            // Check if this is snake body
            else if (check_snake_collision(&game->snake, p)) {
                VGA_Write("\\cgo\\rr");
            }
            // Check if this is food
            else if (x == game->food.x && y == game->food.y) {
                VGA_Write("\\cr@\\rr");
            }
            // Empty space
            else {
                VGA_Write(" ");
            }
        }
        
        VGA_Write("|\n");
    }
    
    // Draw bottom border
    VGA_Write("+");
    for (int i = 0; i < GAME_WIDTH; i++) {
        VGA_Write("-");
    }
    VGA_Write("+\n");
    
    // Draw score and controls
    VGA_Write("Score: ");
    // Simple integer to string conversion
    if (game->score == 0) {
        VGA_Write("0");
    } else {
        char score_str[16];
        int score = game->score;
        int idx = 0;
        while (score > 0) {
            score_str[idx++] = '0' + (score % 10);
            score /= 10;
        }
        // Reverse the string
        for (int i = idx - 1; i >= 0; i--) {
            VGA_WriteChar(score_str[i]);
        }
    }
    
    VGA_Write(" | Length: ");
    if (game->snake.length < 10) {
        VGA_WriteChar('0' + game->snake.length);
    } else {
        char len_str[16];
        int len = game->snake.length;
        int idx = 0;
        while (len > 0) {
            len_str[idx++] = '0' + (len % 10);
            len /= 10;
        }
        for (int i = idx - 1; i >= 0; i--) {
            VGA_WriteChar(len_str[i]);
        }
    }
    
    VGA_Write("\nArrows: Move | P: \\cyPause\\rr | ESC: \\crQuit\\rr\n");
    
    if (game->paused) {
        VGA_Write("\n*** PAUSED ***\n");
    }
    
    if (game->game_over) {
        VGA_Write("\n\\cr*** GAME OVER ***\\rr\n");
        VGA_Write("Press ENTER to \\clgplay again\\rr or ESC to \\crquit\\rr\n");
    }
}

// Handle input
void game_handle_input(GameState *game, Event *event) {
    if (event->type != EVENT_KEY_PRESSED) return;
    
    if (game->game_over) {
        if (event->data.keyboard.keycode == KEY_ENTER) {
            game_init(game);
        }
        return;
    }
    
    switch (event->data.keyboard.keycode) {
        case KEY_ARROW_UP:
            if (game->snake.direction != DIR_DOWN) {
                game->snake.direction = DIR_UP;
            }
            break;
            
        case KEY_ARROW_DOWN:
            if (game->snake.direction != DIR_UP) {
                game->snake.direction = DIR_DOWN;
            }
            break;
            
        case KEY_ARROW_LEFT:
            if (game->snake.direction != DIR_RIGHT) {
                game->snake.direction = DIR_LEFT;
            }
            break;
            
        case KEY_ARROW_RIGHT:
            if (game->snake.direction != DIR_LEFT) {
                game->snake.direction = DIR_RIGHT;
            }
            break;
            
        default:
            // Check for 'p' key to pause
            if (event->data.keyboard.ascii == 'p' || event->data.keyboard.ascii == 'P') {
                game->paused = !game->paused;
            }
            break;
    }
}

// Main game loop - OPTIMIZED AND SMOOTH
void play_snake_game(void) {
    VGA_EnableScrolling(false);
    VGA_HideCursor();
    GameState game;
    Event event;
    bool quit = false;
    int frame_counter = 0;
    int game_speed = 200; // Game updates every 8 frames (adjust for speed)
    
    // Seed RNG with a pseudo-random value
    srand(12345);
    
    // Initialize game
    game_init(&game);
    
    // Clear event queue
    event_clear();
    
    VGA_Clear();
    VGA_Write("Starting Snake Game...\n");
    VGA_Write("Press any key to begin!\n");
    
    // Wait for keypress to start
    event_wait();
    
    // Draw initial state
    VGA_Clear();
    game_draw(&game);
    
    // Main game loop
    while (!quit) {
        // Process ALL pending input events FIRST (responsive input)
        while (event_poll(&event)) {
            if (event.type == EVENT_KEY_PRESSED) {
                if (event.data.keyboard.keycode == KEY_ESCAPE) {
                    quit = true;
                    break;
                }
                game_handle_input(&game, &event);
            }
        }
        
        if (quit) break;
        
        // Update game at fixed rate
        frame_counter++;
        if (frame_counter >= game_speed) {
            frame_counter = 0;
            
            // Update game state
            game_update(&game);
            
            // Clear and redraw ONLY when we update
            VGA_Clear();
            game_draw(&game);
        }
        
        // Frame delay for consistent timing
        for (volatile int i = 0; i < 270000; i++);
    }
    
    // Game over screen
    VGA_Clear();
    VGA_Write("\n");
    VGA_Write("========================================\n");
    VGA_Write("       THANKS FOR PLAYING SNAKE!       \n");
    VGA_Write("========================================\n\n");
    VGA_Write("Final Score: ");
    
    // Print final score
    if (game.score == 0) {
        VGA_Write("0");
    } else {
        char score_str[16];
        int score = game.score;
        int idx = 0;
        while (score > 0) {
            score_str[idx++] = '0' + (score % 10);
            score /= 10;
        }
        for (int i = idx - 1; i >= 0; i--) {
            VGA_WriteChar(score_str[i]);
        }
    }
    
    VGA_Write("\nSnake Length: ");
    if (game.snake.length < 10) {
        VGA_WriteChar('0' + game.snake.length);
    } else {
        char len_str[16];
        int len = game.snake.length;
        int idx = 0;
        while (len > 0) {
            len_str[idx++] = '0' + (len % 10);
            len /= 10;
        }
        for (int i = idx - 1; i >= 0; i--) {
            VGA_WriteChar(len_str[i]);
        }
    }
    VGA_WriteChar('\n');
    VGA_EnableScrolling(true);
    VGA_ShowCursor();
    event_wait();
}