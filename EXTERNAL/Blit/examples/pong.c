// Pong - BlitEngine Example Game
// Classic two-player pong (player vs CPU)

#include <Blit/Blit.h>
#include <libc.h>

typedef struct {
    Entity *ball;
    Entity *player_paddle;
    Entity *cpu_paddle;
    int player_score;
    int cpu_score;
    int ball_vx;
    int ball_vy;
} Game;

void init_game(Game *game, BlitEngine *engine) {
    memset(game, 0, sizeof(Game));
    
    // Create ball
    Sprite *ball_sprite = blit_sprite_create_circle(engine, 8, 0xFFFFFFFF);
    game->ball = blit_entity_create(
        engine->screen_width / 2 - 8,
        engine->screen_height / 2 - 8,
        16, 16,
        ball_sprite
    );
    game->ball_vx = 4;
    game->ball_vy = 3;
    
    // Create paddles
    Sprite *paddle_sprite = blit_sprite_create_color(engine, 12, 80, 0xFFFFFFFF);
    
    game->player_paddle = blit_entity_create(
        20,
        engine->screen_height / 2 - 40,
        12, 80,
        paddle_sprite
    );
    
    game->cpu_paddle = blit_entity_create(
        engine->screen_width - 32,
        engine->screen_height / 2 - 40,
        12, 80,
        paddle_sprite
    );
}

void reset_ball(Game *game, BlitEngine *engine) {
    game->ball->x = engine->screen_width / 2 - 8;
    game->ball->y = engine->screen_height / 2 - 8;
    game->ball_vx = (game->ball_vx > 0) ? -4 : 4;
    game->ball_vy = (blit_random_int(0, 1) ? 3 : -3);
}

int md_main(long argc, char **argv) {
    BlitEngine engine;
    
    if (blit_init(&engine) != 0) {
        printf("Failed to initialize BlitEngine!\n");
        return 1;
    }
    
    printf("Pong!\n");
    printf("Move mouse up/down to control your paddle\n");
    printf("First to 5 wins!\n");
    printf("ESC to quit\n\n");
    
    Game game;
    init_game(&game, &engine);
    
    while (blit_is_running(&engine)) {
        blit_update_input(&engine);
        
        if (blit_key_pressed(&engine, BLIT_KEY_ESC)) {
            break;
        }
        
        // Check for winner
        if (game.player_score >= 5 || game.cpu_score >= 5) {
            blit_begin_frame(&engine, 0xFF000000);
            
            if (game.player_score >= 5) {
                blit_draw_text(&engine, "YOU WIN!", 
                    engine.screen_width / 2 - 35, engine.screen_height / 2, 0xFF00FF00);
            } else {
                blit_draw_text(&engine, "CPU WINS!", 
                    engine.screen_width / 2 - 40, engine.screen_height / 2, 0xFFFF0000);
            }
            
            blit_draw_text(&engine, "Press ESC to exit",
                engine.screen_width / 2 - 70, engine.screen_height / 2 + 30, 0xFF888888);
            
            blit_end_frame(&engine);
            continue;
        }
        
        // UPDATE
        
        // Player paddle follows mouse Y
        game.player_paddle->y = engine.input.mouse_y - 40;
        blit_entity_clamp_to_screen(&engine, game.player_paddle);
        
        // CPU AI - simple follow the ball
        if (game.ball->y < game.cpu_paddle->y + 30) {
            game.cpu_paddle->y -= 3;
        }
        if (game.ball->y > game.cpu_paddle->y + 50) {
            game.cpu_paddle->y += 3;
        }
        blit_entity_clamp_to_screen(&engine, game.cpu_paddle);
        
        // Move ball
        game.ball->x += game.ball_vx;
        game.ball->y += game.ball_vy;
        
        // Ball bounces off top/bottom
        if (game.ball->y <= 0 || game.ball->y >= engine.screen_height - 16) {
            game.ball_vy = -game.ball_vy;
        }
        
        // Ball hits player paddle
        if (blit_entity_collides(game.ball, game.player_paddle)) {
            game.ball_vx = (game.ball_vx > 0) ? game.ball_vx : -game.ball_vx;
            game.ball->x = game.player_paddle->x + 12;
            
            // Add spin based on where it hit the paddle
            int hit_pos = (game.ball->y + 8) - (game.player_paddle->y + 40);
            game.ball_vy = hit_pos / 8;
        }
        
        // Ball hits CPU paddle
        if (blit_entity_collides(game.ball, game.cpu_paddle)) {
            game.ball_vx = (game.ball_vx < 0) ? game.ball_vx : -game.ball_vx;
            game.ball->x = game.cpu_paddle->x - 16;
            
            int hit_pos = (game.ball->y + 8) - (game.cpu_paddle->y + 40);
            game.ball_vy = hit_pos / 8;
        }
        
        // Ball goes off left side (CPU scores)
        if (game.ball->x < -16) {
            game.cpu_score++;
            reset_ball(&game, &engine);
        }
        
        // Ball goes off right side (Player scores)
        if (game.ball->x > engine.screen_width) {
            game.player_score++;
            reset_ball(&game, &engine);
        }
        
        // DRAW
        blit_begin_frame(&engine, 0xFF001010);
        
        // Draw center line
        for (int y = 0; y < engine.screen_height; y += 20) {
            blit_draw_rect(&engine, engine.screen_width / 2 - 2, y, 4, 10, 0xFF404040);
        }
        
        // Draw paddles and ball
        blit_entity_draw(&engine, game.player_paddle);
        blit_entity_draw(&engine, game.cpu_paddle);
        blit_entity_draw(&engine, game.ball);
        
        // Draw scores
        char score_text[64];
        snprintf(score_text, sizeof(score_text), "%d - %d", 
                 game.player_score, game.cpu_score);
        blit_draw_text(&engine, score_text,
            engine.screen_width / 2 - 25, 20, 0xFFFFFFFF);
        
        blit_end_frame(&engine);
    }
    
    printf("\nFinal Score: %d - %d\n", game.player_score, game.cpu_score);
    
    blit_shutdown(&engine);
    return 0;
}
