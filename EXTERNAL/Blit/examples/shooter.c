// Space Shooter - BlitEngine Example Game
// Move with WASD, shoot with SPACE, avoid enemies!

#include <Blit/Blit.h>
#include <libc.h>

#define MAX_BULLETS 30
#define MAX_ENEMIES 15

typedef struct {
    Entity *player;
    Entity *bullets[MAX_BULLETS];
    Entity *enemies[MAX_ENEMIES];
    int score;
    int game_over;
} Game;

void init_game(Game *game, BlitEngine *engine) {
    memset(game, 0, sizeof(Game));
    
    // Create player sprite (green triangle-ish)
    Sprite *player_sprite = blit_sprite_create_color(engine, 32, 32, 0xFF00FF00);
    game->player = blit_entity_create(
        engine->screen_width / 2 - 16,
        engine->screen_height - 80,
        32, 32,
        player_sprite
    );
    
    // Create bullet sprite (yellow small rectangles)
    Sprite *bullet_sprite = blit_sprite_create_color(engine, 4, 12, 0xFFFFFF00);
    for (int i = 0; i < MAX_BULLETS; i++) {
        game->bullets[i] = blit_entity_create(0, 0, 4, 12, bullet_sprite);
        game->bullets[i]->active = 0;
    }
    
    // Create enemy sprite (red squares)
    Sprite *enemy_sprite = blit_sprite_create_color(engine, 28, 28, 0xFFFF0000);
    for (int i = 0; i < MAX_ENEMIES; i++) {
        game->enemies[i] = blit_entity_create(
            blit_random_int(0, engine->screen_width - 28),
            -28 - blit_random_int(0, 300),
            28, 28,
            enemy_sprite
        );
        game->enemies[i]->vy = 1 + blit_random_float() * 2;
    }
}

void fire_bullet(Game *game, BlitEngine *engine) {
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!game->bullets[i]->active) {
            game->bullets[i]->active = 1;
            game->bullets[i]->x = game->player->x + 14;
            game->bullets[i]->y = game->player->y;
            game->bullets[i]->vy = -10;
            break;
        }
    }
}

int md_main(long argc, char **argv) {
    BlitEngine engine;
    
    if (blit_init(&engine) != 0) {
        printf("Failed to initialize BlitEngine!\n");
        return 1;
    }
    
    printf("Space Shooter!\n");
    printf("WASD to move, SPACE to shoot\n");
    printf("Don't let enemies reach the bottom!\n");
    printf("ESC to quit\n\n");
    
    Game game;
    init_game(&game, &engine);
    
    while (blit_is_running(&engine)) {
        blit_update_input(&engine);
        
        if (blit_key_pressed(&engine, BLIT_KEY_ESC)) {
            break;
        }
        
        if (game.game_over) {
            // Draw game over screen
            blit_begin_frame(&engine, 0xFF000000);
            blit_draw_text(&engine, "GAME OVER", 
                engine.screen_width / 2 - 40, engine.screen_height / 2 - 6, 0xFFFF0000);
            
            char score_text[64];
            snprintf(score_text, sizeof(score_text), "Final Score: %d", game.score);
            blit_draw_text(&engine, score_text,
                engine.screen_width / 2 - 60, engine.screen_height / 2 + 20, 0xFFFFFFFF);
            
            blit_draw_text(&engine, "Press ESC to exit",
                engine.screen_width / 2 - 70, engine.screen_height / 2 + 40, 0xFF888888);
            
            blit_end_frame(&engine);
            continue;
        }
        
        // UPDATE
        
        // Player movement
        if (blit_key_down(&engine, BLIT_KEY_A)) game.player->x -= 4;
        if (blit_key_down(&engine, BLIT_KEY_D)) game.player->x += 4;
        if (blit_key_down(&engine, BLIT_KEY_W)) game.player->y -= 3;
        if (blit_key_down(&engine, BLIT_KEY_S)) game.player->y += 3;
        
        blit_entity_clamp_to_screen(&engine, game.player);
        
        // Shooting
        if (blit_key_pressed(&engine, BLIT_KEY_SPACE)) {
            fire_bullet(&game, &engine);
        }
        
        // Update bullets
        for (int i = 0; i < MAX_BULLETS; i++) {
            if (game.bullets[i]->active) {
                blit_entity_update(game.bullets[i]);
                
                if (!blit_entity_on_screen(&engine, game.bullets[i])) {
                    game.bullets[i]->active = 0;
                }
            }
        }
        
        // Update enemies
        for (int i = 0; i < MAX_ENEMIES; i++) {
            blit_entity_update(game.enemies[i]);
            
            // Check if enemy reached bottom (game over!)
            if (game.enemies[i]->y > engine.screen_height) {
                game.game_over = 1;
            }
        }
        
        // Check bullet vs enemy collisions
        for (int i = 0; i < MAX_BULLETS; i++) {
            if (!game.bullets[i]->active) continue;
            
            for (int j = 0; j < MAX_ENEMIES; j++) {
                if (blit_entity_collides(game.bullets[i], game.enemies[j])) {
                    // Hit!
                    game.bullets[i]->active = 0;
                    game.enemies[j]->x = blit_random_int(0, engine.screen_width - 28);
                    game.enemies[j]->y = -28;
                    game.score += 10;
                    break;
                }
            }
        }
        
        // Check player vs enemy collisions
        for (int i = 0; i < MAX_ENEMIES; i++) {
            if (blit_entity_collides(game.player, game.enemies[i])) {
                game.game_over = 1;
            }
        }
        
        // DRAW
        blit_begin_frame(&engine, 0xFF000020);
        
        // Draw player
        blit_entity_draw(&engine, game.player);
        
        // Draw bullets
        for (int i = 0; i < MAX_BULLETS; i++) {
            if (game.bullets[i]->active) {
                blit_entity_draw(&engine, game.bullets[i]);
            }
        }
        
        // Draw enemies
        for (int i = 0; i < MAX_ENEMIES; i++) {
            blit_entity_draw(&engine, game.enemies[i]);
        }
        
        // Draw HUD
        char hud[64];
        snprintf(hud, sizeof(hud), "Score: %d", game.score);
        blit_draw_text(&engine, hud, 10, 10, 0xFFFFFFFF);
        
        blit_end_frame(&engine);
    }
    
    printf("\nGame finished! Score: %d\n", game.score);
    
    blit_shutdown(&engine);
    return 0;
}
