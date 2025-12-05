#include "moduos/kernel/events/events.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/games/RaycasterFPS.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// Game constants
#define MAP_WIDTH 24
#define MAP_HEIGHT 24
#define SCREEN_WIDTH 40
#define SCREEN_HEIGHT 20
#define FOV 60
#define MAX_DEPTH 20
#define MOVE_SPEED 20
#define ROT_SPEED 10

// Enemy constants
#define MAX_ENEMIES 10
#define ENEMY_DAMAGE 5

// Simple RNG
static uint32_t rng_seed = 12345;

void srand_doom(uint32_t seed) {
    rng_seed = seed;
}

uint32_t rand_doom(void) {
    rng_seed = (rng_seed * 1103515245 + 12345) & 0x7fffffff;
    return rng_seed;
}

// Map (1 = wall, 2 = door, 0 = empty)
#define MAP_WIDTH 24
#define MAP_HEIGHT 24

static int map[MAP_HEIGHT][MAP_WIDTH] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,1},
    {1,0,1,1,1,0,1,0,1,0,1,1,1,0,1,0,1,1,1,0,1,1,0,1},
    {1,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,1,0,1},
    {1,0,1,0,2,0,1,1,1,0,1,0,1,1,1,0,2,0,1,0,2,1,0,1},
    {1,0,0,0,0,0,0,0,1,0,0,0,0,0,1,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,0,1,1,0,1,1,1,1,0,1,1,1,0,1,1,1,0,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,1,1,1,1,0,1,1,1,1,1,1,1,0,1,1,1,1,1,0,1,1,1},
    {1,0,1,0,0,0,0,1,0,0,0,0,0,1,0,0,0,0,0,1,0,0,0,1},
    {1,0,1,0,2,1,0,1,0,2,0,2,0,1,0,1,2,0,1,0,2,1,0,1},
    {1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,0,1,1,1,0,1,1,1,1,1,1,0,1,1,1,0,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,1,1,1,0,1,0,1,1,1,1,1,1,1,0,1,0,1,1,1,0,1,1},
    {1,0,1,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1},
    {1,0,1,0,2,0,1,1,1,0,1,0,1,1,1,0,1,2,1,0,2,1,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,1,1,1,1,0,1,1,1,1,1,1,1,1,0,1,1,1,1,0,1,1,1},
    {1,0,1,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1},
    {1,0,1,0,2,1,0,1,0,2,0,2,0,1,0,1,2,0,1,0,2,1,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}
};


// Simple sine table (0-360 degrees, scaled by 100)
static const int sin_table[361] = {
    0, 2, 3, 5, 7, 9, 10, 12, 14, 16, 17, 19, 21, 22, 24, 26, 28, 29, 31, 33,
    34, 36, 37, 39, 41, 42, 44, 45, 47, 48, 50, 52, 53, 54, 56, 57, 59, 60, 62, 63,
    64, 66, 67, 68, 69, 71, 72, 73, 74, 75, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86,
    87, 87, 88, 89, 90, 91, 91, 92, 93, 93, 94, 95, 95, 96, 96, 97, 97, 97, 98, 98,
    98, 99, 99, 99, 99, 99, 100, 100, 100, 100, 100, 100, 100, 99, 99, 99, 99, 99, 98, 98,
    98, 97, 97, 97, 96, 96, 95, 95, 94, 93, 93, 92, 91, 91, 90, 89, 88, 87, 87, 86,
    85, 84, 83, 82, 81, 80, 79, 78, 77, 75, 74, 73, 72, 71, 69, 68, 67, 66, 64, 63,
    62, 60, 59, 57, 56, 54, 53, 52, 50, 48, 47, 45, 44, 42, 41, 39, 37, 36, 34, 33,
    31, 29, 28, 26, 24, 22, 21, 19, 17, 16, 14, 12, 10, 9, 7, 5, 3, 2, 0, -2,
    -3, -5, -7, -9, -10, -12, -14, -16, -17, -19, -21, -22, -24, -26, -28, -29, -31, -33, -34, -36,
    -37, -39, -41, -42, -44, -45, -47, -48, -50, -52, -53, -54, -56, -57, -59, -60, -62, -63, -64, -66,
    -67, -68, -69, -71, -72, -73, -74, -75, -77, -78, -79, -80, -81, -82, -83, -84, -85, -86, -87, -87,
    -88, -89, -90, -91, -91, -92, -93, -93, -94, -95, -95, -96, -96, -97, -97, -97, -98, -98, -98, -99,
    -99, -99, -99, -99, -100, -100, -100, -100, -100, -100, -100, -99, -99, -99, -99, -99, -98, -98, -98, -97,
    -97, -97, -96, -96, -95, -95, -94, -93, -93, -92, -91, -91, -90, -89, -88, -87, -87, -86, -85, -84,
    -83, -82, -81, -80, -79, -78, -77, -75, -74, -73, -72, -71, -69, -68, -67, -66, -64, -63, -62, -60,
    -59, -57, -56, -54, -53, -52, -50, -48, -47, -45, -44, -42, -41, -39, -37, -36, -34, -33, -31, -29,
    -28, -26, -24, -22, -21, -19, -17, -16, -14, -12, -10, -9, -7, -5, -3, -2, 0
};

// Integer sin/cos
int isin(int angle) {
    while (angle < 0) angle += 360;
    while (angle >= 360) angle -= 360;
    return sin_table[angle];
}

int icos(int angle) {
    return isin(angle + 90);
}

// Enemy structure
typedef struct {
    int x;
    int y;
    int health;
    bool alive;
    int attack_timer;
} Enemy;

// Player structure
typedef struct {
    int x;
    int y;
    int angle;
    int health;
    int ammo;
    int kills;
} Player;

// Game state
typedef struct {
    Player player;
    Enemy enemies[MAX_ENEMIES];
    int enemy_count;
    bool quit;
    int shot_flash;
    int damage_flash;
} GameState;

// Initialize game
void doom_game_init(GameState *game) {
    game->player.x = 150;
    game->player.y = 150; 
    game->player.angle = 0; 
    game->player.health = 100;
    game->player.ammo = 50;
    game->player.kills = 0;
    game->quit = false;
    game->shot_flash = 0;
    game->damage_flash = 0;
    
    // Spawn enemies
    game->enemy_count = 0;
    int spawn_points[][2] = {
        {500, 500}, {1800, 500}, {500, 1800}, {1800, 1800},
        {1200, 1200}, {600, 1200}, {1200, 600}, {1500, 1500}
    };
    
    for (int i = 0; i < 8 && i < MAX_ENEMIES; i++) {
        game->enemies[i].x = spawn_points[i][0];
        game->enemies[i].y = spawn_points[i][1];
        game->enemies[i].health = 30;
        game->enemies[i].alive = true;
        game->enemies[i].attack_timer = 0;
        game->enemy_count++;
    }
}

// Cast a ray
int cast_ray(int px, int py, int angle, int *hit_type) {
    int dx = icos(angle);
    int dy = isin(angle);
    
    for (int dist = 0; dist < MAX_DEPTH * 10; dist += 2) {
        int test_x = (px + (dx * dist / 100)) / 100;
        int test_y = (py + (dy * dist / 100)) / 100;
        
        if (test_x < 0 || test_x >= MAP_WIDTH || 
            test_y < 0 || test_y >= MAP_HEIGHT) {
            *hit_type = 1;
            return dist;
        }
        
        if (map[test_y][test_x] != 0) {
            *hit_type = map[test_y][test_x];
            return dist;
        }
    }
    
    *hit_type = 0;
    return MAX_DEPTH * 10;
}

// Get shade character
char get_shade(int distance, int hit_type) {
    if (hit_type == 2) return '|'; // Door
    
    if (distance < 20) return '#';
    if (distance < 40) return '%';
    if (distance < 60) return '+';
    if (distance < 80) return '=';
    if (distance < 100) return '-';
    if (distance < 120) return '.';
    return ' ';
}

// Get color
const char* get_color(int distance, int hit_type) {
    if (hit_type == 2) return "\\cy"; // Yellow doors
    
    if (distance < 30) return "\\clr";
    if (distance < 50) return "\\cr";
    if (distance < 70) return "\\clm";
    if (distance < 90) return "\\cg";
    if (distance < 110) return "\\cc";
    return "\\cb";
}

// Check if enemy is visible
bool is_enemy_visible(GameState *game, int enemy_idx, int screen_x, int screen_y) {
    Enemy *e = &game->enemies[enemy_idx];
    if (!e->alive) return false;
    
    // Calculate enemy position relative to player
    int dx = e->x - game->player.x;
    int dy = e->y - game->player.y;
    
    // Calculate angle to enemy
    int angle_to_enemy = 0;
    if (dx != 0) {
        // Simple angle approximation
        angle_to_enemy = (dy * 100) / (dx + 1);
    }
    
    // Check if in front of player
    int player_dx = icos(game->player.angle);
    int player_dy = isin(game->player.angle);
    int dot = (dx * player_dx + dy * player_dy);
    
    if (dot <= 0) return false; // Behind player
    
    // Calculate distance
    int dist_sq = (dx * dx + dy * dy) / 10000;
    if (dist_sq > 100) return false; // Too far
    
    // Simple screen position check
    int enemy_screen_pos = SCREEN_WIDTH / 2 + (angle_to_enemy / 10);
    if (enemy_screen_pos < 0 || enemy_screen_pos >= SCREEN_WIDTH) return false;
    
    // Check if this screen position matches
    if (screen_x >= enemy_screen_pos - 2 && screen_x <= enemy_screen_pos + 2 &&
        screen_y >= SCREEN_HEIGHT / 2 - 3 && screen_y <= SCREEN_HEIGHT / 2 + 3) {
        return true;
    }
    
    return false;
}

// Render view
void doom_render_view(GameState *game) {
    VGA_Clear();
    
    int distances[SCREEN_WIDTH];
    int hit_types[SCREEN_WIDTH];
    
    int angle_step = FOV * 10 / SCREEN_WIDTH;
    int start_angle = game->player.angle - (FOV / 2);
    
    // Cast all rays
    for (int x = 0; x < SCREEN_WIDTH; x++) {
        int ray_angle = start_angle + (x * angle_step / 10);
        while (ray_angle < 0) ray_angle += 360;
        while (ray_angle >= 360) ray_angle -= 360;
        
        distances[x] = cast_ray(game->player.x, game->player.y, ray_angle, &hit_types[x]);
    }
    
    // Render scene
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            int distance = distances[x];
            int hit_type = hit_types[x];
            
            int wall_height = (SCREEN_HEIGHT * 100) / (distance + 10);
            int wall_top = (SCREEN_HEIGHT / 2) - (wall_height / 2);
            int wall_bottom = wall_top + wall_height;
            
            // Check for enemies
            bool drew_enemy = false;
            for (int e = 0; e < game->enemy_count; e++) {
                if (is_enemy_visible(game, e, x, y)) {
                    VGA_Write("\\crE\\rr");
                    drew_enemy = true;
                    break;
                }
            }
            
            if (!drew_enemy) {
                if (y < wall_top) {
                    VGA_Write("\\bc \\rr");
                } else if (y >= wall_top && y < wall_bottom && hit_type != 0) {
                    VGA_Write(get_color(distance, hit_type));
                    VGA_WriteChar(get_shade(distance, hit_type));
                    VGA_Write("\\rr");
                } else {
                    VGA_Write("\\bg \\rr");
                }
            }
        }
        VGA_WriteChar('\n');
    }
    
    // HUD
    VGA_Write("\\rr");
    if (game->damage_flash > 0) {
        VGA_Write("\\cr* OUCH! *\\rr ");
        game->damage_flash--;
    }
    if (game->shot_flash > 0) {
        VGA_Write("\\cy* BANG! *\\rr ");
        game->shot_flash--;
    }
    VGA_Write("HP:\\cr");
    if (game->player.health >= 100) VGA_WriteChar('1');
    if (game->player.health >= 10) VGA_WriteChar('0' + ((game->player.health % 100) / 10));
    VGA_WriteChar('0' + (game->player.health % 10));
    VGA_Write("\\rr Ammo:\\cy");
    if (game->player.ammo >= 10) VGA_WriteChar('0' + (game->player.ammo / 10));
    VGA_WriteChar('0' + (game->player.ammo % 10));
    VGA_Write("\\rr Kills:\\cg");
    VGA_WriteChar('0' + game->player.kills);
    VGA_Write("\\rr | [WASD]Move [Arrows]Turn [SPACE]Shoot [ESC]Menu");
}

// Update game logic
void doom_update_game(GameState *game) {
    // Enemy AI
    for (int i = 0; i < game->enemy_count; i++) {
        if (!game->enemies[i].alive) continue;
        
        int dx = game->player.x - game->enemies[i].x;
        int dy = game->player.y - game->enemies[i].y;
        int dist_sq = (dx * dx + dy * dy) / 10000;
        
        // Attack player if close
        if (dist_sq < 100) {
            game->enemies[i].attack_timer++;
            if (game->enemies[i].attack_timer > 50) {
                game->player.health -= ENEMY_DAMAGE;
                game->damage_flash = 3;
                game->enemies[i].attack_timer = 0;
            }
        } else {
            game->enemies[i].attack_timer = 0;
        }
    }
}

// Handle input
void doom_handle_input(GameState *game, Event *event) {
    if (event->type != EVENT_KEY_PRESSED) return;
    
    char key = event->data.keyboard.ascii;
    int dx = icos(game->player.angle);
    int dy = isin(game->player.angle);
    
    int new_x = game->player.x;
    int new_y = game->player.y;
    
    // Movement
    if (key == 'w' || key == 'W') {
        new_x += (dx * MOVE_SPEED) / 10;
        new_y += (dy * MOVE_SPEED) / 10;
    } else if (key == 's' || key == 'S') {
        new_x -= (dx * MOVE_SPEED) / 10;
        new_y -= (dy * MOVE_SPEED) / 10;
    } else if (key == 'a' || key == 'A') {
        new_x += (dy * MOVE_SPEED) / 10;
        new_y -= (dx * MOVE_SPEED) / 10;
    } else if (key == 'd' || key == 'D') {
        new_x -= (dy * MOVE_SPEED) / 10;
        new_y += (dx * MOVE_SPEED) / 10;
    }
    
    // Shooting
    if (key == ' ' && game->player.ammo > 0) {
        game->player.ammo--;
        game->shot_flash = 2;
        
        // Find closest enemy in front
        int closest_idx = -1;
        int closest_dist = 999999;
        
        for (int e = 0; e < game->enemy_count; e++) {
            if (!game->enemies[e].alive) continue;
            
            int ex_dx = game->enemies[e].x - game->player.x;
            int ey_dy = game->enemies[e].y - game->player.y;
            
            // Check if in front
            int dot = (ex_dx * dx + ey_dy * dy);
            if (dot > 0) {
                int dist = ex_dx * ex_dx + ey_dy * ey_dy;
                if (dist < closest_dist && dist < 1000000) {
                    closest_dist = dist;
                    closest_idx = e;
                }
            }
        }
        
        if (closest_idx >= 0) {
            game->enemies[closest_idx].health -= 15;
            if (game->enemies[closest_idx].health <= 0) {
                game->enemies[closest_idx].alive = false;
                game->player.kills++;
                game->player.ammo += 10;
            }
        }
    }
    
    // Rotation
    if (key == 'q' || key == 'Q' || event->data.keyboard.keycode == KEY_ARROW_LEFT) {
        game->player.angle -= ROT_SPEED;
        if (game->player.angle < 0) game->player.angle += 360;
    } else if (key == 'e' || key == 'E' || event->data.keyboard.keycode == KEY_ARROW_RIGHT) {
        game->player.angle += ROT_SPEED;
        if (game->player.angle >= 360) game->player.angle -= 360;
    }
    
    // Collision
    int map_x = new_x / 100;
    int map_y = new_y / 100;
    
    if (map_x >= 0 && map_x < MAP_WIDTH && 
        map_y >= 0 && map_y < MAP_HEIGHT &&
        map[map_y][map_x] == 0) {
        game->player.x = new_x;
        game->player.y = new_y;
    }
}

// Main menu
int show_doom_menu(void) {
    int selected = 0;
    Event event;
    bool done = false;
    
    const char* menu_items[] = {
        "Start New Game",
        "Instructions",
        "Exit to Shell"
    };
    
    while (!done) {
        VGA_Clear();
        VGA_Write("\n\n");
        VGA_Write("  \\cr####\\cg####\\cy####\\cm####\\cb####\\clr####\\clg####\\clb####\\rr\n");
        VGA_Write("  \\cr##\\rr                                    \\cr##\\rr\n");
        VGA_Write("  \\cr##\\rr    \\clrD O O M\\rr  -  \\cyRAYCASTER FPS\\rr    \\cr##\\rr\n");
        VGA_Write("  \\cr##\\rr                                    \\cr##\\rr\n");
        VGA_Write("  \\cr####\\cg####\\cy####\\cm####\\cb####\\clr####\\clg####\\clb####\\rr\n\n");
        
        for (int i = 0; i < 3; i++) {
            if (i == selected) {
                VGA_Write("         \\clb> ");
                VGA_Write(menu_items[i]);
                VGA_Write(" <\\rr\n");
            } else {
                VGA_Write("           ");
                VGA_Write(menu_items[i]);
                VGA_Write("\n");
            }
        }
        
        VGA_Write("\n  Use ARROWS to navigate | ENTER to select\n");
        
        event = event_wait();
        
        if (event.type == EVENT_KEY_PRESSED) {
            switch (event.data.keyboard.keycode) {
                case KEY_ARROW_UP:
                    if (selected > 0) selected--;
                    break;
                    
                case KEY_ARROW_DOWN:
                    if (selected < 2) selected++;
                    break;
                    
                case KEY_ENTER:
                    done = true;
                    break;
                    
                case KEY_ESCAPE:
                    return 3;
            }
        }
    }
    
    return selected + 1;
}

void show_instructions(void) {
    VGA_Clear();
    VGA_Write("\n\\clg=== DOOM INSTRUCTIONS ===\\rr\n\n");
    VGA_Write("\\cyOBJECTIVE:\\rr\n");
    VGA_Write("  Eliminate all \\crdemons\\rr in the facility!\n\n");
    VGA_Write("\\cyCONTROLS:\\rr\n");
    VGA_Write("  \\clgW/A/S/D\\rr    - Move forward/strafe\n");
    VGA_Write("  \\clgArrow Keys\\rr - Turn left/right (or Q/E)\n");
    VGA_Write("  \\clgSPACE\\rr      - Shoot weapon\n");
    VGA_Write("  \\clgESC\\rr        - Return to menu\n\n");
    VGA_Write("\\cyGAMEPLAY:\\rr\n");
    VGA_Write("  - Watch your \\crhealth\\rr and \\cyammo\\rr\n");
    VGA_Write("  - \\crRed 'E'\\rr = Enemy demon\n");
    VGA_Write("  - \\cyYellow |\\rr = Doors\n");
    VGA_Write("  - Killing enemies gives +10 bonus ammo\n");
    VGA_Write("  - Enemies attack when close!\n\n");
    VGA_Write("Press any key to return...");
    event_wait();
}

// Main game loop
void play_doom_game(void) {
    VGA_EnableScrolling(false);
    VGA_HideCursor();
    
    srand_doom(12345);
    
    while (1) {
        int choice = show_doom_menu();
        
        if (choice == 2) {
            show_instructions();
            continue;
        }
        
        if (choice == 3) {
            break;
        }
        
        // Start game
        GameState game;
        Event event;
        
        doom_game_init(&game);
        event_clear();
        
        // Initial render
        doom_render_view(&game);
        
        int update_counter = 0;
        
        // Main game loop
        while (!game.quit && game.player.health > 0) {
            // Process ALL input events without rendering
            bool had_input = false;
            while (event_poll(&event)) {
                if (event.type == EVENT_KEY_PRESSED) {
                    if (event.data.keyboard.keycode == KEY_ESCAPE) {
                        game.quit = true;
                        break;
                    }
                    doom_handle_input(&game, &event);
                    had_input = true;
                }
            }
            
            if (game.quit) break;
            
            // Update game logic periodically
            update_counter++;
            if (update_counter > 20) {
                doom_update_game(&game);
                update_counter = 0;
                had_input = true; // Force render after update
            }
            
            // Only render if something happened
            if (had_input) {
                doom_render_view(&game);
            }
            
            // Small delay
            for (volatile int i = 0; i < 80000; i++);
            
            // Check win
            bool all_dead = true;
            for (int i = 0; i < game.enemy_count; i++) {
                if (game.enemies[i].alive) {
                    all_dead = false;
                    break;
                }
            }
            
            if (all_dead) {
                VGA_Clear();
                VGA_Write("\n\n");
                VGA_Write("  \\clg##################################\\rr\n");
                VGA_Write("  \\clg##                              ##\\rr\n");
                VGA_Write("  \\clg##\\rr  \\cyYOU WIN! ALL DEMONS DEAD!\\rr  \\clg##\\rr\n");
                VGA_Write("  \\clg##                              ##\\rr\n");
                VGA_Write("  \\clg##################################\\rr\n\n");
                VGA_Write("  Final Stats:\n");
                VGA_Write("  Kills: \\cg");
                VGA_WriteChar('0' + game.player.kills);
                VGA_Write("\\rr\n");
                VGA_Write("  Health: \\cr");
                if (game.player.health >= 100) VGA_WriteChar('1');
                if (game.player.health >= 10) VGA_WriteChar('0' + ((game.player.health % 100) / 10));
                VGA_WriteChar('0' + (game.player.health % 10));
                VGA_Write("\\rr\n\n  Press any key...");
                event_wait();
                break;
            }
        }
        
        // Game over check
        if (game.player.health <= 0) {
            VGA_Clear();
            VGA_Write("\n\n");
            VGA_Write("  \\cr##################################\\rr\n");
            VGA_Write("  \\cr##                              ##\\rr\n");
            VGA_Write("  \\cr##\\rr      \\crYOU DIED! GAME OVER!\\rr      \\cr##\\rr\n");
            VGA_Write("  \\cr##                              ##\\rr\n");
            VGA_Write("  \\cr##################################\\rr\n\n");
            VGA_Write("  Demons killed: \\cg");
            VGA_WriteChar('0' + game.player.kills);
            VGA_Write("\\rr\n\n  Press any key...");
            event_wait();
        }
    }
    
    VGA_Clear();
    VGA_EnableScrolling(true);
    VGA_ShowCursor();
}