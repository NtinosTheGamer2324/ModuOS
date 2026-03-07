// Breakout - BlitEngine Example
// Classic brick-breaking game!

#include <Blit/Blit.h>
#include <libc.h>

#define BRICK_ROWS 8
#define BRICK_COLS 12
#define BRICK_WIDTH 60
#define BRICK_HEIGHT 20

typedef struct {
    int x, y;
    int active;
    uint32_t color;
} Brick;

typedef struct {
    Entity *paddle;
    Entity *ball;
    Brick bricks[BRICK_ROWS * BRICK_COLS];
    int score;
    int lives;
    int started;
} Game;

void init_game(Game *game, BlitEngine *engine) {
    memset(game, 0, sizeof(Game));
    
    // Create paddle
    Sprite *paddle_sprite = blit_sprite_create_color(engine, 80, 12, 0xFF00AAFF);
    game->paddle = blit_entity_create(
        engine->screen_width / 2 - 40,
        engine->screen_height - 50,
        80, 12, paddle_sprite
    );
    
    // Create ball
    Sprite *ball_sprite = blit_sprite_create_circle(engine, 6, 0xFFFFFFFF);
    game->ball = blit_entity_create(
        engine->screen_width / 2 - 6,
        engine->screen_height - 80,
        12, 12, ball_sprite
    );
    game->ball->vx = 3;
    game->ball->vy = -3;
    
    // Create bricks
    uint32_t colors[] = {
        0xFFFF0000, 0xFFFF7700, 0xFFFFFF00, 0xFF00FF00,
        0xFF0088FF, 0xFF0000FF, 0xFFFF00FF, 0xFFFF0088
    };
    
    int brick_start_y = 80;
    for (int row = 0; row < BRICK_ROWS; row++) {
        for (int col = 0; col < BRICK_COLS; col++) {
            int idx = row * BRICK_COLS + col;
            game->bricks[idx].x = 20 + col * (BRICK_WIDTH + 4);
            game->bricks[idx].y = brick_start_y + row * (BRICK_HEIGHT + 4);
            game->bricks[idx].active = 1;
            game->bricks[idx].color = colors[row];
        }
    }
    
    game->lives = 3;
    game->started = 0;
}

int md_main(long argc, char **argv) {
    BlitEngine engine;
    
    if (blit_init(&engine) != 0) {
        printf("Failed to initialize BlitEngine!\n");
        return 1;
    }
    
    printf("Breakout!\n");
    printf("Move mouse to control paddle\n");
    printf("Click to start\n");
    printf("ESC to quit\n\n");
    
    Game game;
    init_game(&game, &engine);
    
    while (blit_is_running(&engine)) {
        blit_update_input(&engine);
        
        if (blit_key_pressed(&engine, BLIT_KEY_ESC)) {
            break;
        }
        
        // Paddle follows mouse
        game.paddle->x = engine.input.mouse_x - 40;
        blit_entity_clamp_to_screen(&engine, game.paddle);
        
        // Start game on click
        if (!game.started && blit_mouse_clicked(&engine, MOUSE_LEFT)) {
            game.started = 1;
        }
        
        if (game.started) {
            // Update ball
            game.ball->x += game.ball->vx;
            game.ball->y += game.ball->vy;
            
            // Bounce off walls
            if (game.ball->x <= 0 || game.ball->x >= engine.screen_width - 12) {
                game.ball->vx = -game.ball->vx;
            }
            if (game.ball->y <= 0) {
                game.ball->vy = -game.ball->vy;
            }
            
            // Ball falls off bottom
            if (game.ball->y > engine.screen_height) {
                game.lives--;
                if (game.lives > 0) {
                    game.ball->x = engine.screen_width / 2 - 6;
                    game.ball->y = engine.screen_height - 80;
                    game.ball->vx = 3;
                    game.ball->vy = -3;
                    game.started = 0;
                } else {
                    break;
                }
            }
            
            // Paddle collision
            if (blit_entity_collides(game.ball, game.paddle)) {
                game.ball->vy = -game.ball->vy;
                game.ball->y = game.paddle->y - 12;
                
                // Add spin
                int hit_pos = (game.ball->x + 6) - (game.paddle->x + 40);
                game.ball->vx += hit_pos / 10;
                
                if (game.ball->vx > 5) game.ball->vx = 5;
                if (game.ball->vx < -5) game.ball->vx = -5;
            }
            
            // Brick collision
            for (int i = 0; i < BRICK_ROWS * BRICK_COLS; i++) {
                if (!game.bricks[i].active) continue;
                
                if (blit_rects_overlap(
                    game.ball->x, game.ball->y, 12, 12,
                    game.bricks[i].x, game.bricks[i].y, BRICK_WIDTH, BRICK_HEIGHT)) {
                    
                    game.bricks[i].active = 0;
                    game.ball->vy = -game.ball->vy;
                    game.score += 10;
                    break;
                }
            }
        } else {
            // Ball sticks to paddle
            game.ball->x = game.paddle->x + 40 - 6;
            game.ball->y = game.paddle->y - 12;
        }
        
        // Draw
        blit_begin_frame(&engine, 0xFF000020);
        
        // Draw bricks
        for (int i = 0; i < BRICK_ROWS * BRICK_COLS; i++) {
            if (game.bricks[i].active) {
                blit_draw_rect(&engine, 
                    game.bricks[i].x, game.bricks[i].y,
                    BRICK_WIDTH, BRICK_HEIGHT,
                    game.bricks[i].color);
            }
        }
        
        // Draw paddle and ball
        blit_entity_draw(&engine, game.paddle);
        blit_entity_draw(&engine, game.ball);
        
        // HUD
        char hud[64];
        snprintf(hud, sizeof(hud), "Score: %d  Lives: %d", game.score, game.lives);
        blit_draw_text(&engine, hud, 10, 10, 0xFFFFFFFF);
        
        if (!game.started) {
            blit_draw_text(&engine, "Click to start!", 
                engine.screen_width / 2 - 60, engine.screen_height / 2, 0xFFFFFFFF);
        }
        
        blit_end_frame(&engine);
    }
    
    printf("\nGame Over! Final Score: %d\n", game.score);
    
    blit_shutdown(&engine);
    return 0;
}
