#pragma once
// Blit Engine - A Simple 2D Game Framework for ModuOS
// Built on top of NodGL graphics API
//
// Blit makes game development easy by providing:
// - Entity/sprite management
// - Input handling
// - Collision detection
// - Game loop helpers
// - Math utilities
//
// Copyright © 2026 ModuOS Project Contributors
// Licensed under GPL v2.0

#include "NodGL.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handles */
typedef struct NodGL_device* NodGL_Device;
typedef struct NodGL_context* NodGL_Context;

/* Sprite - An image that can be drawn on screen */
typedef struct {
    NodGL_Texture texture;
    int width;
    int height;
} Sprite;

/* Entity - A game object with position and sprite */
typedef struct {
    float x, y;              // Position
    float vx, vy;            // Velocity
    int width, height;       // Size for collision
    Sprite *sprite;          // Visual appearance
    int active;              // Is this entity alive?
    void *user_data;         // Custom data for your game
} Entity;

/* Rectangle for UI and collision */
typedef struct {
    int x, y;
    int width, height;
} Rect;

/* Input state */
typedef struct {
    int mouse_x, mouse_y;
    int mouse_buttons;
    int mouse_clicked;       // Was button just pressed?
    int keys[256];           // Which keys are held down
    int keys_pressed[256];   // Which keys were just pressed
} Input;

/* Blit Engine state */
typedef struct {
    NodGL_Device device;
    NodGL_Context ctx;
    uint32_t screen_width;
    uint32_t screen_height;
    int event_queue_fd;
    Input input;
    int running;
    uint32_t frame_count;
} BlitEngine;

/* ============================================================
 * Blit Engine Core Functions
 * ============================================================ */

/* Initialize Blit Engine */
int blit_init(BlitEngine *engine);

/* Shutdown Blit Engine */
void blit_shutdown(BlitEngine *engine);

/* Update input state (call once per frame) */
void blit_update_input(BlitEngine *engine);

/* Check if engine should keep running */
int blit_is_running(BlitEngine *engine);

/* Begin a new frame (clears screen) */
void blit_begin_frame(BlitEngine *engine, uint32_t clear_color);

/* End frame (presents to screen) */
void blit_end_frame(BlitEngine *engine);

/* ============================================================
 * Sprite Functions
 * ============================================================ */

/* Create sprite from pixel data */
Sprite* blit_sprite_create(BlitEngine *engine, int width, int height, uint32_t *pixels);

/* Create sprite with solid color */
Sprite* blit_sprite_create_color(BlitEngine *engine, int width, int height, uint32_t color);

/* Create sprite with a circle */
Sprite* blit_sprite_create_circle(BlitEngine *engine, int radius, uint32_t color);

/* Free sprite */
void blit_sprite_free(BlitEngine *engine, Sprite *sprite);

/* Draw sprite at position */
void blit_sprite_draw(BlitEngine *engine, Sprite *sprite, int x, int y);

/* Draw sprite with tint color */
void blit_sprite_draw_tinted(BlitEngine *engine, Sprite *sprite, int x, int y, uint32_t tint);

/* ============================================================
 * Entity Functions
 * ============================================================ */

/* Create entity */
Entity* blit_entity_create(float x, float y, int width, int height, Sprite *sprite);

/* Free entity */
void blit_entity_free(Entity *entity);

/* Update entity position based on velocity */
void blit_entity_update(Entity *entity);

/* Draw entity */
void blit_entity_draw(BlitEngine *engine, Entity *entity);

/* Check if two entities collide */
int blit_entity_collides(Entity *a, Entity *b);

/* Check if entity is on screen */
int blit_entity_on_screen(BlitEngine *engine, Entity *entity);

/* Keep entity within screen bounds */
void blit_entity_clamp_to_screen(BlitEngine *engine, Entity *entity);

/* ============================================================
 * Drawing Functions
 * ============================================================ */

/* Draw filled rectangle */
void blit_draw_rect(BlitEngine *engine, int x, int y, int w, int h, uint32_t color);

/* Draw rectangle outline */
void blit_draw_rect_outline(BlitEngine *engine, int x, int y, int w, int h, uint32_t color, int thickness);

/* Draw line */
void blit_draw_line(BlitEngine *engine, int x0, int y0, int x1, int y1, uint32_t color, int thickness);

/* Draw circle (filled) */
void blit_draw_circle(BlitEngine *engine, int cx, int cy, int radius, uint32_t color);

/* Draw text (simple bitmap font) */
void blit_draw_text(BlitEngine *engine, const char *text, int x, int y, uint32_t color);

/* ============================================================
 * Collision & Math Functions
 * ============================================================ */

/* Check if rectangles overlap */
int blit_rects_overlap(int x1, int y1, int w1, int h1, int x2, int y2, int w2, int h2);

/* Check if point is in rectangle */
int blit_point_in_rect(int px, int py, int rx, int ry, int rw, int rh);

/* Distance between two points */
float blit_distance(float x1, float y1, float x2, float y2);

/* Clamp value between min and max */
float blit_clamp(float value, float min, float max);

/* Random integer between min and max (inclusive) */
int blit_random_int(int min, int max);

/* Random float between 0 and 1 */
float blit_random_float(void);

/* ============================================================
 * Input Helper Functions
 * ============================================================ */

/* Check if key is currently held down */
int blit_key_down(BlitEngine *engine, int keycode);

/* Check if key was just pressed this frame */
int blit_key_pressed(BlitEngine *engine, int keycode);

/* Check if mouse button is down */
int blit_mouse_down(BlitEngine *engine, int button);

/* Check if mouse button was just clicked */
int blit_mouse_clicked(BlitEngine *engine, int button);

/* ============================================================
 * Common Key Codes
 * ============================================================ */

/* Keyboard scancodes - use BLIT_KEY_ prefix to avoid conflicts */
#define BLIT_KEY_ESC     1
#define BLIT_KEY_1       2
#define BLIT_KEY_2       3
#define BLIT_KEY_3       4
#define BLIT_KEY_SPACE   57
#define BLIT_KEY_RETURN  28   /* Note: Using RETURN instead of ENTER to avoid conflict */
#define BLIT_KEY_UP      72
#define BLIT_KEY_DOWN    80
#define BLIT_KEY_LEFT    75
#define BLIT_KEY_RIGHT   77
#define BLIT_KEY_W       17
#define BLIT_KEY_A       30
#define BLIT_KEY_S       31
#define BLIT_KEY_D       32

#define MOUSE_LEFT   1
#define MOUSE_RIGHT  2
#define MOUSE_MIDDLE 4

#ifdef __cplusplus
}
#endif
