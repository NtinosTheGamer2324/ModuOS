#include "libc.h"
#include "string.h"
#include "../include/moduos/kernel/events/events.h"

#define MAX_BULLETS 10
#define MAX_WALLS 30
#define TANK_SIZE 16

typedef struct { float x, y, angle; int health, score; uint32_t color; } Tank;
typedef struct { float x, y, vx, vy; int active, owner; } Bullet;
typedef struct { int x, y, w, h; } Wall;

static Tank p1, p2;
static Bullet bullets[MAX_BULLETS];
static Wall walls[MAX_WALLS];
static int num_walls = 0, game_over = 0;
static uint32_t rng = 1;
static uint32_t rnd() { rng = rng * 1103515245u + 12345u; return rng; }

static float cosf_approx(float x) { return 1.0f - x*x/2.0f; }
static float sinf_approx(float x) { return x; }

static void move_tank(Tank *t, float dist, uint32_t w, uint32_t h);
static void shoot(Tank *t, int owner);
static void update_game(uint32_t w, uint32_t h);
static void create_map(uint32_t w, uint32_t h);
static void draw_tank_xrgb(uint8_t *fb, uint32_t pitch, uint32_t w, uint32_t h, Tank *t);
static void draw_pixel_tank_xrgb(uint8_t *fb, uint32_t pitch, uint32_t w, uint32_t h, Tank *t);
static void draw_tank_rgb565(uint8_t *fb, uint32_t pitch, uint32_t w, uint32_t h, Tank *t);

static uint32_t pack_xrgb8888(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static uint16_t pack_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t rr = (uint16_t)((r * 31u) / 255u);
    uint16_t gg = (uint16_t)((g * 63u) / 255u);
    uint16_t bb = (uint16_t)((b * 31u) / 255u);
    return (uint16_t)((rr << 11) | (gg << 5) | (bb));
}

static void draw_rect_xrgb8888(uint8_t *fb, uint32_t pitch, uint32_t w, uint32_t h,
                               uint32_t x, uint32_t y, uint32_t rw, uint32_t rh,
                               uint32_t color) {
    if (!fb) return;
    if (x >= w || y >= h) return;
    if (x + rw > w) rw = w - x;
    if (y + rh > h) rh = h - y;

    for (uint32_t yy = 0; yy < rh; yy++) {
        uint32_t *row = (uint32_t *)(fb + (uint64_t)(y + yy) * pitch);
        for (uint32_t xx = 0; xx < rw; xx++) {
            row[x + xx] = color;
        }
    }
}

static void draw_rect_rgb565(uint8_t *fb, uint32_t pitch, uint32_t w, uint32_t h,
                             uint32_t x, uint32_t y, uint32_t rw, uint32_t rh,
                             uint16_t color) {
    if (!fb) return;
    if (x >= w || y >= h) return;
    if (x + rw > w) rw = w - x;
    if (y + rh > h) rh = h - y;

    for (uint32_t yy = 0; yy < rh; yy++) {
        uint16_t *row = (uint16_t *)(fb + (uint64_t)(y + yy) * pitch);
        for (uint32_t xx = 0; xx < rw; xx++) {
            row[x + xx] = color;
        }
    }
}

static void draw_gradient_xrgb8888(uint8_t *fb, uint32_t pitch, uint32_t w, uint32_t h) {
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(fb + (uint64_t)y * pitch);
        for (uint32_t x = 0; x < w; x++) {
            uint8_t r = (uint8_t)((x * 255u) / (w ? w : 1));
            uint8_t g = (uint8_t)((y * 255u) / (h ? h : 1));
            uint8_t b = (uint8_t)(((x ^ y) & 0xFF));
            row[x] = pack_xrgb8888(r, g, b);
        }
    }
}

static void draw_gradient_rgb565(uint8_t *fb, uint32_t pitch, uint32_t w, uint32_t h) {
    for (uint32_t y = 0; y < h; y++) {
        uint16_t *row = (uint16_t *)(fb + (uint64_t)y * pitch);
        for (uint32_t x = 0; x < w; x++) {
            uint8_t r = (uint8_t)((x * 255u) / (w ? w : 1));
            uint8_t g = (uint8_t)((y * 255u) / (h ? h : 1));
            uint8_t b = (uint8_t)(((x ^ y) & 0xFF));
            row[x] = pack_rgb565(r, g, b);
        }
    }
}

int md_main(long argc, char **argv) {
    (void)argc;
    (void)argv;

    puts_raw("=============================\n");
    puts_raw("    NEON TANK ARENA\n");
    puts_raw("=============================\n");

    int fd = open(MD64API_GRP_DEFAULT_DEVICE, O_RDONLY, 0);
    if (fd < 0) {
        printf("gfxtest: cannot open %s\n", MD64API_GRP_DEFAULT_DEVICE);
        return 1;
    }

    md64api_grp_video_info_t info;
    memset(&info, 0, sizeof(info));

    ssize_t n = read(fd, &info, sizeof(info));
    close(fd);

    if (n < (ssize_t)sizeof(info)) {
        printf("gfxtest: read video info failed (n=%ld)\n", (long)n);
        return 1;
    }

    printf("mode=%u fmt=%u bpp=%u\n", (unsigned)info.mode, (unsigned)info.fmt, (unsigned)info.bpp);
    printf("w=%u h=%u pitch=%u\n", (unsigned)info.width, (unsigned)info.height, (unsigned)info.pitch);
    printf("fb_addr=0x%llx\n", (unsigned long long)info.fb_addr);

    if (info.mode != MD64API_GRP_MODE_GRAPHICS || info.width == 0 || info.height == 0) {
        puts_raw("Neon Tank Arena: not in graphics mode\n");
        return 0;
    }

    int efd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    if (efd < 0) {
        puts_raw("Neon Tank Arena: cannot open input\n");
        return 2;
    }

    rng = (uint32_t)time_ms();
    puts_raw("Initializing game...\n");

    /*
     * Be tolerant: some kernels may report fmt=UNKNOWN (0) even though bpp is known.
     * In that case, infer from bpp.
     */
    uint8_t fmt = info.fmt;
    if (fmt == MD64API_GRP_FMT_UNKNOWN) {
        if (info.bpp == 32) fmt = MD64API_GRP_FMT_XRGB8888;
        else if (info.bpp == 16) fmt = MD64API_GRP_FMT_RGB565;
    }

    uint32_t bpp_bytes = (fmt == MD64API_GRP_FMT_RGB565) ? 2u : 4u;

    uint32_t game_w = info.width;
    uint32_t game_h = info.height;
    uint32_t pitch = game_w * bpp_bytes;
    uint32_t buf_size = pitch * game_h;

    uint8_t *bb = (uint8_t*)malloc(buf_size);
    if (!bb) return 3;
    
    p1 = (Tank){50, 50, 0, 3, 0, 0xFF00FFFF};
    p2 = (Tank){(float)game_w-50, (float)game_h-50, 180, 3, 0, 0xFFFF00FF};
    for(int i=0; i<MAX_BULLETS; i++) bullets[i].active=0;
    
    create_map(game_w, game_h);
    
    puts_raw("Game ready! Controls:\n");
    puts_raw("  Player 1: WASD to move, SPACE to shoot\n");
    puts_raw("  Player 2: Arrow keys to move, 0 to shoot\n");
    puts_raw("  R to restart, ESC to quit\n");
    
    int quit = 0;
    uint64_t last_update = time_ms();
    
    while (!quit) {
        Event ev;
        while (read(efd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (ev.type == EVENT_KEY_PRESSED) {
                KeyCode kc = ev.data.keyboard.keycode;
                char ch = ev.data.keyboard.ascii;
                if (kc == KEY_ESCAPE || ch == 0x1b) { quit = 1; break; }
                if (ch == 'w' || ch == 'W') move_tank(&p1, 3, game_w, game_h);
                if (ch == 's' || ch == 'S') move_tank(&p1, -2, game_w, game_h);
                if (ch == 'a' || ch == 'A') p1.angle -= 8;
                if (ch == 'd' || ch == 'D') p1.angle += 8;
                if (ch == ' ') shoot(&p1, 0);
                if (kc == KEY_ARROW_UP) move_tank(&p2, 3, game_w, game_h);
                if (kc == KEY_ARROW_DOWN) move_tank(&p2, -2, game_w, game_h);
                if (kc == KEY_ARROW_LEFT) p2.angle -= 8;
                if (kc == KEY_ARROW_RIGHT) p2.angle += 8;
                if (ch == '0' || ch == ')') shoot(&p2, 1); // 0 key or Numpad 0
                if (ch == 'r' || ch == 'R') {
                    p1.x = 50; p1.y = 50; p1.angle = 0; p1.health = 3; p1.score = 0;
                    p2.x = game_w-50; p2.y = game_h-50; p2.angle = 180; p2.health = 3; p2.score = 0;
                    for(int i=0; i<MAX_BULLETS; i++) bullets[i].active=0;
                }
            }
        }
        
        if (quit) break;
        
        uint64_t now = time_ms();
        if (now - last_update >= 16) {
            update_game(game_w, game_h);
            last_update = now;
        }
        
        // Draw background gradient
        if (fmt == MD64API_GRP_FMT_XRGB8888) {
            draw_gradient_xrgb8888(bb, pitch, game_w, game_h);
        } else {
            draw_gradient_rgb565(bb, pitch, game_w, game_h);
        }
        
        // Draw walls
        for(int i=0; i<num_walls; i++) {
            if (fmt == MD64API_GRP_FMT_XRGB8888) {
                draw_rect_xrgb8888(bb, pitch, game_w, game_h, 
                                   walls[i].x, walls[i].y, walls[i].w, walls[i].h, 0xFF808080);
            } else {
                draw_rect_rgb565(bb, pitch, game_w, game_h, 
                                 walls[i].x, walls[i].y, walls[i].w, walls[i].h, 
                                 pack_rgb565(128, 128, 128));
            }
        }
        
        // Draw bullets
        for(int i=0; i<MAX_BULLETS; i++) {
            if(bullets[i].active) {
                int bx = (int)bullets[i].x;
                int by = (int)bullets[i].y;
                uint32_t bullet_color = bullets[i].owner == 0 ? 0xFFFFFF00 : 0xFFFF00FF;
                if (fmt == MD64API_GRP_FMT_XRGB8888) {
                    draw_rect_xrgb8888(bb, pitch, game_w, game_h, bx-2, by-2, 5, 5, bullet_color);
                } else {
                    uint16_t bc = pack_rgb565((bullet_color>>16)&0xFF, (bullet_color>>8)&0xFF, bullet_color&0xFF);
                    draw_rect_rgb565(bb, pitch, game_w, game_h, bx-2, by-2, 5, 5, bc);
                }
            }
        }
        
        // Draw tanks
        if (fmt == MD64API_GRP_FMT_XRGB8888) {
            draw_pixel_tank_xrgb(bb, pitch, game_w, game_h, &p1);
            draw_pixel_tank_xrgb(bb, pitch, game_w, game_h, &p2);
        } else {
            draw_tank_rgb565(bb, pitch, game_w, game_h, &p1);
            draw_tank_rgb565(bb, pitch, game_w, game_h, &p2);
        }
        
        // Draw UI - Health bars
        if (fmt == MD64API_GRP_FMT_XRGB8888) {
            // Player 1 UI (top-left)
            draw_rect_xrgb8888(bb, pitch, game_w, game_h, 10, 10, 150, 50, 0xC0000000);
            draw_rect_xrgb8888(bb, pitch, game_w, game_h, 12, 12, 146, 46, 0xFF1A1A1A);
            
            // P1 Health bar
            int p1_health_width = (p1.health * 130) / 3;
            if(p1_health_width > 0) {
                draw_rect_xrgb8888(bb, pitch, game_w, game_h, 20, 20, p1_health_width, 12, 0xFF00FF00);
            }
            
            // P1 Score
            draw_rect_xrgb8888(bb, pitch, game_w, game_h, 20, 40, 120, 15, 0xFF0A0A0A);
            
            // Player 2 UI (top-right)
            draw_rect_xrgb8888(bb, pitch, game_w, game_h, game_w-160, 10, 150, 50, 0xC0000000);
            draw_rect_xrgb8888(bb, pitch, game_w, game_h, game_w-158, 12, 146, 46, 0xFF1A1A1A);
            
            // P2 Health bar
            int p2_health_width = (p2.health * 130) / 3;
            if(p2_health_width > 0) {
                draw_rect_xrgb8888(bb, pitch, game_w, game_h, game_w-150, 20, p2_health_width, 12, 0xFF00FF00);
            }
            
            // P2 Score
            draw_rect_xrgb8888(bb, pitch, game_w, game_h, game_w-150, 40, 120, 15, 0xFF0A0A0A);
        }
        
        // Present to screen
        gfx_blit(bb, (uint16_t)game_w, (uint16_t)game_h, 0, 0, (uint16_t)pitch, (uint16_t)fmt);
        
        yield();
    }
    
    puts_raw("\nNeon Tank Arena - Exiting...\n");
    
    free(bb);
    close(efd);
    return 0;
}

static void move_tank(Tank *t, float dist, uint32_t w, uint32_t h) {
    float rad = t->angle * 3.14159f / 180.0f;
    float nx = t->x + cosf_approx(rad) * dist;
    float ny = t->y + sinf_approx(rad) * dist;
    
    // Check bounds
    if (nx < TANK_SIZE || nx > w-TANK_SIZE || ny < TANK_SIZE || ny > h-TANK_SIZE) {
        return;
    }
    
    // Check wall collisions
    for(int i=0; i<num_walls; i++) {
        if(nx+TANK_SIZE/2 >= walls[i].x && nx-TANK_SIZE/2 <= walls[i].x + walls[i].w &&
           ny+TANK_SIZE/2 >= walls[i].y && ny-TANK_SIZE/2 <= walls[i].y + walls[i].h) {
            return;
        }
    }
    
    t->x = nx; 
    t->y = ny;
}
static void shoot(Tank *t, int owner) {
    for(int i=0; i<MAX_BULLETS; i++) {
        if(!bullets[i].active) {
            float rad = t->angle * 3.14159f / 180.0f;
            bullets[i] = (Bullet){t->x, t->y, cosf_approx(rad)*4, sinf_approx(rad)*4, 1, owner};
            break;
        }
    }
}
static void update_game(uint32_t w, uint32_t h) {
    for(int i=0; i<MAX_BULLETS; i++) {
        if(bullets[i].active) {
            bullets[i].x += bullets[i].vx;
            bullets[i].y += bullets[i].vy;
            
            // Check bounds
            if(bullets[i].x<0||bullets[i].x>w||bullets[i].y<0||bullets[i].y>h) {
                bullets[i].active=0;
                continue;
            }
            
            // Check wall collisions
            for(int wi=0; wi<num_walls; wi++) {
                if(bullets[i].x >= walls[wi].x && bullets[i].x <= walls[wi].x + walls[wi].w &&
                   bullets[i].y >= walls[wi].y && bullets[i].y <= walls[wi].y + walls[wi].h) {
                    bullets[i].active=0;
                    break;
                }
            }
            
            // Check tank collisions
            if(bullets[i].active) {
                Tank *target = bullets[i].owner == 0 ? &p2 : &p1;
                float dx = bullets[i].x - target->x;
                float dy = bullets[i].y - target->y;
                if(dx*dx + dy*dy < TANK_SIZE*TANK_SIZE) {
                    bullets[i].active = 0;
                    target->health--;
                    if(bullets[i].owner == 0) p1.score += 10;
                    else p2.score += 10;
                    if(target->health <= 0) {
                        target->health = 3;
                        target->x = target == &p1 ? 50 : w-50;
                        target->y = target == &p1 ? 50 : (float)h-50;
                    }
                }
            }
        }
    }
}
static void draw_tank_xrgb(uint8_t *fb, uint32_t pitch, uint32_t w, uint32_t h, Tank *t) {
    int x=(int)t->x, y=(int)t->y;
    for(int dy=-TANK_SIZE/2; dy<=TANK_SIZE/2; dy++) {
        for(int dx=-TANK_SIZE/2; dx<=TANK_SIZE/2; dx++) {
            int px=x+dx, py=y+dy;
            if(px>=0&&px<(int)w&&py>=0&&py<(int)h) {
                uint32_t *row = (uint32_t*)(fb + (uint64_t)py*pitch);
                row[px] = t->color;
            }
        }
    }
    float rad = t->angle * 3.14159f / 180.0f;
    int bx = x + (int)(cosf_approx(rad)*TANK_SIZE);
    int by = y + (int)(sinf_approx(rad)*TANK_SIZE);
    if(bx>=0&&bx<(int)w&&by>=0&&by<(int)h) {
        uint32_t *row = (uint32_t*)(fb + (uint64_t)by*pitch);
        row[bx] = 0xFFFFFFFF;
    }
}

static void draw_pixel_tank_xrgb(uint8_t *fb, uint32_t pitch, uint32_t w, uint32_t h, Tank *t) {
    int cx = (int)t->x, cy = (int)t->y;
    float rad = t->angle * 3.14159f / 180.0f;
    float cos_a = cosf_approx(rad);
    float sin_a = sinf_approx(rad);
    
    // Scan screen space in a bounding box around the tank
    for(int py = cy - 20; py <= cy + 20; py++) {
        for(int px = cx - 20; px <= cx + 20; px++) {
            if(px < 0 || px >= (int)w || py < 0 || py >= (int)h) continue;
            
            // Transform screen pixel to tank-local coordinates (inverse rotation)
            float dx = (float)(px - cx);
            float dy = (float)(py - cy);
            float local_x = dx * cos_a - dy * sin_a;
            float local_y = dx * sin_a + dy * cos_a;
            
            uint32_t *row = (uint32_t*)(fb + (uint64_t)py * pitch);
            
            // Check if inside tank body (12x12 square centered at origin)
            if(local_x >= -6.0f && local_x <= 6.0f && local_y >= -6.0f && local_y <= 6.0f) {
                // Draw border (1 pixel thick)
                if(local_x <= -5.0f || local_x >= 5.0f || local_y <= -5.0f || local_y >= 5.0f) {
                    row[px] = 0xFF000000; // Black border
                } else {
                    row[px] = t->color; // Tank color
                }
            }
            // Check if inside cannon (16x5 rectangle extending forward)
            else if(local_x >= 6.0f && local_x <= 22.0f && local_y >= -2.0f && local_y <= 2.0f) {
                row[px] = 0xFF404040; // Dark gray cannon
            }
        }
    }
}

static void draw_tank_rgb565(uint8_t *fb, uint32_t pitch, uint32_t w, uint32_t h, Tank *t) {
    int cx = (int)t->x, cy = (int)t->y;
    float rad = t->angle * 3.14159f / 180.0f;
    float cos_a = cosf_approx(rad);
    float sin_a = sinf_approx(rad);
    uint16_t tank_color = pack_rgb565((t->color>>16)&0xFF, (t->color>>8)&0xFF, t->color&0xFF);
    
    // Scan screen space in a bounding box around the tank
    for(int py = cy - 20; py <= cy + 20; py++) {
        for(int px = cx - 20; px <= cx + 20; px++) {
            if(px < 0 || px >= (int)w || py < 0 || py >= (int)h) continue;
            
            // Transform screen pixel to tank-local coordinates (inverse rotation)
            float dx = (float)(px - cx);
            float dy = (float)(py - cy);
            float local_x = dx * cos_a - dy * sin_a;
            float local_y = dx * sin_a + dy * cos_a;
            
            uint16_t *row = (uint16_t*)(fb + (uint64_t)py * pitch);
            
            // Check if inside tank body (12x12 square centered at origin)
            if(local_x >= -6.0f && local_x <= 6.0f && local_y >= -6.0f && local_y <= 6.0f) {
                // Draw border (1 pixel thick)
                if(local_x <= -5.0f || local_x >= 5.0f || local_y <= -5.0f || local_y >= 5.0f) {
                    row[px] = pack_rgb565(0, 0, 0); // Black border
                } else {
                    row[px] = tank_color; // Tank color
                }
            }
            // Check if inside cannon (16x5 rectangle extending forward)
            else if(local_x >= 6.0f && local_x <= 22.0f && local_y >= -2.0f && local_y <= 2.0f) {
                row[px] = pack_rgb565(64, 64, 64); // Dark gray cannon
            }
        }
    }
}

static void create_map(uint32_t w, uint32_t h) {
    num_walls = 0;
    
    walls[num_walls++] = (Wall){(int)(w*0.25), (int)(h*0.25), 60, 20};
    walls[num_walls++] = (Wall){(int)(w*0.70), (int)(h*0.25), 60, 20};
    walls[num_walls++] = (Wall){(int)(w*0.25), (int)(h*0.70), 60, 20};
    walls[num_walls++] = (Wall){(int)(w*0.70), (int)(h*0.70), 60, 20};
    walls[num_walls++] = (Wall){(int)(w*0.45), (int)(h*0.45), 30, 30};
    
    for(int i=0; i<3; i++) {
        walls[num_walls++] = (Wall){20, 100+i*100, 15, 80};
        walls[num_walls++] = (Wall){(int)w-35, 100+i*100, 15, 80};
    }
}

