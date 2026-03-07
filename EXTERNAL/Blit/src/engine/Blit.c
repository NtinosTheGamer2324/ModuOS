// Blit Engine - Implementation
// A 2D game framework for ModuOS built on NodGL
// NO _start here - this is a library
#define LIBC_NO_START
#include <Blit/Blit.h>
#include <libc.h>
#include <moduos/kernel/events/events.h>
#include <gfx2d.h>

/* ============================================================
 * Engine Core
 * ============================================================ */

int blit_init(BlitEngine *engine) {
    memset(engine, 0, sizeof(BlitEngine));
    
    if (NodGL_CreateDevice(NodGL_FEATURE_LEVEL_1_0, &engine->device, &engine->ctx, NULL) != NodGL_OK) {
        return -1;
    }
    
    NodGL_GetScreenResolution(engine->device, &engine->screen_width, &engine->screen_height);
    
    engine->event_queue_fd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    if (engine->event_queue_fd < 0) {
        NodGL_ReleaseDevice(engine->device);
        return -1;
    }
    
    // Set up a simple arrow cursor (16x16)
    uint32_t cursor_pixels[16 * 16];
    memset(cursor_pixels, 0, sizeof(cursor_pixels));
    
    // Draw a simple white arrow cursor
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            uint32_t color = 0x00000000; // Transparent
            
            // Simple arrow shape
            if (x <= y && x < 10 && y < 16) {
                if (x == y || x == 0 || y == 15) {
                    color = 0xFFFFFFFF; // White outline
                } else if (x < y) {
                    color = 0xFFFFFFFF; // White fill
                }
            }
            
            cursor_pixels[y * 16 + x] = color;
        }
    }
    
    // Try to set hardware cursor (will fail silently if not supported)
    int cursor_fd = open("$/dev/graphics/video0", O_RDWR, 0);
    if (cursor_fd >= 0) {
        gfx2d_t gfx;
        gfx.fd = cursor_fd;
        gfx2d_cursor_set(&gfx, 16, 16, 0, 0, cursor_pixels);
        gfx2d_cursor_show(&gfx, 1);
        close(cursor_fd);
    }
    
    engine->running = 1;
    engine->frame_count = 0;
    
    return 0;
}

void blit_shutdown(BlitEngine *engine) {
    if (engine->event_queue_fd >= 0) {
        close(engine->event_queue_fd);
    }
    NodGL_ReleaseDevice(engine->device);
    engine->running = 0;
}

void blit_update_input(BlitEngine *engine) {
    Event ev;
    
    /* Clear "just pressed" states */
    memset(engine->input.keys_pressed, 0, sizeof(engine->input.keys_pressed));
    engine->input.mouse_clicked = 0;
    
    int prev_buttons = engine->input.mouse_buttons;
    
    /* Read all pending events */
    while (read(engine->event_queue_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EVENT_MOUSE_MOVE) {
            engine->input.mouse_x = ev.data.mouse.x;
            engine->input.mouse_y = ev.data.mouse.y;
        }
        if (ev.type == EVENT_MOUSE_BUTTON) {
            engine->input.mouse_buttons = ev.data.mouse.buttons;
        }
        if (ev.type == EVENT_KEY_PRESSED) {
            int keycode = ev.data.keyboard.scancode;
            if (keycode >= 0 && keycode < 256) {
                engine->input.keys[keycode] = 1;
                engine->input.keys_pressed[keycode] = 1;
            }
        }
        if (ev.type == EVENT_KEY_RELEASED) {
            int keycode = ev.data.keyboard.scancode;
            if (keycode >= 0 && keycode < 256) {
                engine->input.keys[keycode] = 0;
            }
        }
    }
    
    /* Detect mouse click */
    if ((engine->input.mouse_buttons & 1) && !(prev_buttons & 1)) {
        engine->input.mouse_clicked = 1;
    }
}

int blit_is_running(BlitEngine *engine) {
    return engine->running;
}

void blit_begin_frame(BlitEngine *engine, uint32_t clear_color) {
    NodGL_ClearContext(engine->ctx, NodGL_CLEAR_COLOR, clear_color, 1.0f, 0);
}

void blit_end_frame(BlitEngine *engine) {
    NodGL_PresentContext(engine->ctx, 0);  // vsync=0 for better performance
    engine->frame_count++;
}

/* ============================================================
 * Sprite Functions
 * ============================================================ */

Sprite* blit_sprite_create(BlitEngine *engine, int width, int height, uint32_t *pixels) {
    Sprite *sprite = malloc(sizeof(Sprite));
    if (!sprite) return NULL;
    
    sprite->width = width;
    sprite->height = height;
    
    NodGL_TextureDesc desc = {0};
    desc.width = width;
    desc.height = height;
    desc.format = NodGL_FORMAT_R8G8B8A8_UNORM;
    desc.mip_levels = 1;
    desc.initial_data = pixels;
    desc.initial_data_size = width * height * 4;
    
    if (NodGL_CreateTexture(engine->device, &desc, &sprite->texture) != NodGL_OK) {
        free(sprite);
        return NULL;
    }
    
    return sprite;
}

Sprite* blit_sprite_create_color(BlitEngine *engine, int width, int height, uint32_t color) {
    uint32_t *pixels = malloc(width * height * sizeof(uint32_t));
    if (!pixels) return NULL;
    
    for (int i = 0; i < width * height; i++) {
        pixels[i] = color;
    }
    
    Sprite *sprite = blit_sprite_create(engine, width, height, pixels);
    free(pixels);
    return sprite;
}

Sprite* blit_sprite_create_circle(BlitEngine *engine, int radius, uint32_t color) {
    int size = radius * 2;
    uint32_t *pixels = malloc(size * size * sizeof(uint32_t));
    if (!pixels) return NULL;
    
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int dx = x - radius;
            int dy = y - radius;
            if (dx * dx + dy * dy <= radius * radius) {
                pixels[y * size + x] = color;
            } else {
                pixels[y * size + x] = 0x00000000;  /* Transparent */
            }
        }
    }
    
    Sprite *sprite = blit_sprite_create(engine, size, size, pixels);
    free(pixels);
    return sprite;
}

void blit_sprite_free(BlitEngine *engine, Sprite *sprite) {
    if (sprite) {
        NodGL_ReleaseResource(engine->device, sprite->texture);
        free(sprite);
    }
}

void blit_sprite_draw(BlitEngine *engine, Sprite *sprite, int x, int y) {
    NodGL_DrawTexture(engine->ctx, sprite->texture, 0, 0, x, y, sprite->width, sprite->height);
}

void blit_sprite_draw_tinted(BlitEngine *engine, Sprite *sprite, int x, int y, uint32_t tint) {
    /* TODO: Implement tinting - for now just draw normally */
    blit_sprite_draw(engine, sprite, x, y);
}

/* ============================================================
 * Entity Functions
 * ============================================================ */

Entity* blit_entity_create(float x, float y, int width, int height, Sprite *sprite) {
    Entity *entity = malloc(sizeof(Entity));
    if (!entity) return NULL;
    
    entity->x = x;
    entity->y = y;
    entity->vx = 0;
    entity->vy = 0;
    entity->width = width;
    entity->height = height;
    entity->sprite = sprite;
    entity->active = 1;
    entity->user_data = NULL;
    
    return entity;
}

void blit_entity_free(Entity *entity) {
    free(entity);
}

void blit_entity_update(Entity *entity) {
    entity->x += entity->vx;
    entity->y += entity->vy;
}

void blit_entity_draw(BlitEngine *engine, Entity *entity) {
    if (entity->active && entity->sprite) {
        blit_sprite_draw(engine, entity->sprite, (int)entity->x, (int)entity->y);
    }
}

int blit_entity_collides(Entity *a, Entity *b) {
    return blit_rects_overlap(
        (int)a->x, (int)a->y, a->width, a->height,
        (int)b->x, (int)b->y, b->width, b->height
    );
}

int blit_entity_on_screen(BlitEngine *engine, Entity *entity) {
    return entity->x + entity->width >= 0 &&
           entity->x < engine->screen_width &&
           entity->y + entity->height >= 0 &&
           entity->y < engine->screen_height;
}

void blit_entity_clamp_to_screen(BlitEngine *engine, Entity *entity) {
    if (entity->x < 0) entity->x = 0;
    if (entity->y < 0) entity->y = 0;
    if (entity->x + entity->width > engine->screen_width) {
        entity->x = engine->screen_width - entity->width;
    }
    if (entity->y + entity->height > engine->screen_height) {
        entity->y = engine->screen_height - entity->height;
    }
}

/* ============================================================
 * Drawing Functions
 * ============================================================ */

void blit_draw_rect(BlitEngine *engine, int x, int y, int w, int h, uint32_t color) {
    NodGL_FillRectContext(engine->ctx, x, y, w, h, color);
}

void blit_draw_rect_outline(BlitEngine *engine, int x, int y, int w, int h, uint32_t color, int thickness) {
    // Optimized: draw 4 rects directly instead of using line functions
    NodGL_FillRectContext(engine->ctx, x, y, w, thickness, color);                    // Top
    NodGL_FillRectContext(engine->ctx, x, y + h - thickness, w, thickness, color);    // Bottom
    NodGL_FillRectContext(engine->ctx, x, y, thickness, h, color);                    // Left
    NodGL_FillRectContext(engine->ctx, x + w - thickness, y, thickness, h, color);    // Right
}

void blit_draw_line(BlitEngine *engine, int x0, int y0, int x1, int y1, uint32_t color, int thickness) {
    NodGL_DrawLineContext(engine->ctx, x0, y0, x1, y1, color, thickness);
}

void blit_draw_circle(BlitEngine *engine, int cx, int cy, int radius, uint32_t color) {
    // Draw circle using horizontal spans for much better performance
    for (int y = -radius; y <= radius; y++) {
        int width = 0;
        for (int x = -radius; x <= radius; x++) {
            if (x * x + y * y <= radius * radius) {
                width++;
            } else if (width > 0) {
                NodGL_FillRectContext(engine->ctx, cx + x - width, cy + y, width, 1, color);
                width = 0;
            }
        }
        if (width > 0) {
            NodGL_FillRectContext(engine->ctx, cx + radius - width + 1, cy + y, width, 1, color);
        }
    }
}

void blit_draw_text(BlitEngine *engine, const char *text, int x, int y, uint32_t color) {
    /* Simple bitmap font - each char is 8x12 pixels */
    for (int i = 0; text[i]; i++) {
        blit_draw_rect(engine, x + i * 8, y, 6, 12, color);
    }
}

/* ============================================================
 * Collision & Math
 * ============================================================ */

int blit_rects_overlap(int x1, int y1, int w1, int h1, int x2, int y2, int w2, int h2) {
    return x1 < x2 + w2 &&
           x1 + w1 > x2 &&
           y1 < y2 + h2 &&
           y1 + h1 > y2;
}

int blit_point_in_rect(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw &&
           py >= ry && py < ry + rh;
}

float blit_distance(float x1, float y1, float x2, float y2) {
    float dx = x2 - x1;
    float dy = y2 - y1;
    /* Simple approximation */
    float ax = dx < 0 ? -dx : dx;
    float ay = dy < 0 ? -dy : dy;
    return ax + ay;  /* Manhattan distance */
}

float blit_clamp(float value, float min, float max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

int blit_random_int(int min, int max) {
    return min + (rand() % (max - min + 1));
}

float blit_random_float(void) {
    return (float)(rand() % 1000) / 1000.0f;
}

/* ============================================================
 * Input Helpers
 * ============================================================ */

int blit_key_down(BlitEngine *engine, int keycode) {
    if (keycode < 0 || keycode >= 256) return 0;
    return engine->input.keys[keycode];
}

int blit_key_pressed(BlitEngine *engine, int keycode) {
    if (keycode < 0 || keycode >= 256) return 0;
    return engine->input.keys_pressed[keycode];
}

int blit_mouse_down(BlitEngine *engine, int button) {
    return engine->input.mouse_buttons & button;
}

int blit_mouse_clicked(BlitEngine *engine, int button) {
    return engine->input.mouse_clicked && (engine->input.mouse_buttons & button);
}
