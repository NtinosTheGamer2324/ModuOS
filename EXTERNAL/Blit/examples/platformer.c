// Simple Platformer - BlitEngine Example
// Jump and run! Collect coins!

#include <Blit/Blit.h>
#include <libc.h>

#define MAX_PLATFORMS 20
#define MAX_COINS 30

typedef struct {
    int x, y, width, height;
} Platform;

typedef struct {
    Entity *player;
    Platform platforms[MAX_PLATFORMS];
    Entity *coins[MAX_COINS];
    int score;
    int on_ground;
    float camera_y;
} Game;

void init_game(Game *game, BlitEngine *engine) {
    memset(game, 0, sizeof(Game));
    
    // Create player
    Sprite *player_sprite = blit_sprite_create_color(engine, 24, 32, 0xFF00AAFF);
    game->player = blit_entity_create(100, 100, 24, 32, player_sprite);
    
    // Create platforms
    game->platforms[0].x = 0;
    game->platforms[0].y = engine->screen_height - 40;
    game->platforms[0].width = engine->screen_width;
    game->platforms[0].height = 40;
    
    int platform_y = 500;
    for (int i = 1; i < MAX_PLATFORMS; i++) {
        game->platforms[i].width = 80 + blit_random_int(0, 120);
        game->platforms[i].height = 20;
        game->platforms[i].x = blit_random_int(50, engine->screen_width - game->platforms[i].width - 50);
        game->platforms[i].y = platform_y;
        platform_y -= blit_random_int(60, 100);
    }
    
    // Create coins
    Sprite *coin_sprite = blit_sprite_create_circle(engine, 8, 0xFFFFD700);
    for (int i = 0; i < MAX_COINS; i++) {
        int plat = 1 + (i % (MAX_PLATFORMS - 1));
        game->coins[i] = blit_entity_create(
            game->platforms[plat].x + blit_random_int(10, game->platforms[plat].width - 20),
            game->platforms[plat].y - 40,
            16, 16,
            coin_sprite
        );
    }
}

int md_main(long argc, char **argv) {
    BlitEngine engine;
    
    if (blit_init(&engine) != 0) {
        printf("Failed to initialize BlitEngine!\n");
        return 1;
    }
    
    printf("Platformer Game!\n");
    printf("A/D to move, SPACE to jump\n");
    printf("Collect coins! ESC to quit\n\n");
    
    Game game;
    init_game(&game, &engine);
    
    while (blit_is_running(&engine)) {
        blit_update_input(&engine);
        
        if (blit_key_pressed(&engine, BLIT_KEY_ESC)) {
            break;
        }
        
        // Movement
        if (blit_key_down(&engine, BLIT_KEY_A)) {
            game.player->vx = -4;
        } else if (blit_key_down(&engine, BLIT_KEY_D)) {
            game.player->vx = 4;
        } else {
            game.player->vx = 0;
        }
        
        // Jump
        if (blit_key_pressed(&engine, BLIT_KEY_SPACE) && game.on_ground) {
            game.player->vy = -12;
            game.on_ground = 0;
        }
        
        // Gravity
        game.player->vy += 0.5f;
        if (game.player->vy > 10) game.player->vy = 10;
        
        // Update position
        game.player->x += game.player->vx;
        game.player->y += game.player->vy;
        
        // Keep on screen horizontally
        if (game.player->x < 0) game.player->x = 0;
        if (game.player->x > engine.screen_width - 24) {
            game.player->x = engine.screen_width - 24;
        }
        
        // Platform collision
        game.on_ground = 0;
        for (int i = 0; i < MAX_PLATFORMS; i++) {
            Platform *plat = &game.platforms[i];
            
            if (game.player->vy >= 0 &&
                game.player->x + 24 > plat->x &&
                game.player->x < plat->x + plat->width &&
                game.player->y + 32 >= plat->y &&
                game.player->y + 32 <= plat->y + plat->height) {
                
                game.player->y = plat->y - 32;
                game.player->vy = 0;
                game.on_ground = 1;
                break;
            }
        }
        
        // Collect coins
        for (int i = 0; i < MAX_COINS; i++) {
            if (game.coins[i]->active) {
                if (blit_entity_collides(game.player, game.coins[i])) {
                    game.coins[i]->active = 0;
                    game.score += 10;
                }
            }
        }
        
        // Camera follows player
        float target_camera = game.player->y - engine.screen_height / 2;
        game.camera_y += (target_camera - game.camera_y) * 0.1f;
        
        // Draw
        blit_begin_frame(&engine, 0xFF87CEEB);
        
        // Draw platforms
        for (int i = 0; i < MAX_PLATFORMS; i++) {
            int screen_y = game.platforms[i].y - (int)game.camera_y;
            blit_draw_rect(&engine, 
                game.platforms[i].x, screen_y,
                game.platforms[i].width, game.platforms[i].height,
                0xFF228B22);
        }
        
        // Draw coins
        for (int i = 0; i < MAX_COINS; i++) {
            if (game.coins[i]->active) {
                int coin_y = (int)game.coins[i]->y - (int)game.camera_y;
                blit_sprite_draw(&engine, game.coins[i]->sprite, 
                    (int)game.coins[i]->x, coin_y);
            }
        }
        
        // Draw player
        int player_y = (int)game.player->y - (int)game.camera_y;
        blit_sprite_draw(&engine, game.player->sprite, 
            (int)game.player->x, player_y);
        
        // HUD
        char hud[64];
        snprintf(hud, sizeof(hud), "Score: %d  Height: %d", 
                 game.score, -(int)game.player->y / 10);
        blit_draw_text(&engine, hud, 10, 10, 0xFF000000);
        
        blit_end_frame(&engine);
    }
    
    printf("\nFinal Score: %d\n", game.score);
    
    blit_shutdown(&engine);
    return 0;
}
