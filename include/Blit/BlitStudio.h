#pragma once
// BlitStudio - Visual Game Editor for Blit Engine
// Create games without writing code!
//
// Copyright © 2026 ModuOS Project Contributors
// Licensed under GPL v2.0

#include <Blit/Blit.h>

/* Editor modes */
typedef enum {
    EDITOR_MODE_SELECT,      // Select and move objects
    EDITOR_MODE_PAINT,       // Paint sprites
    EDITOR_MODE_ENTITY,      // Place entities
    EDITOR_MODE_COLLISION,   // Draw collision boxes
    EDITOR_MODE_TEST         // Test/play the level
} EditorMode;

/* Tool types */
typedef enum {
    TOOL_PENCIL,
    TOOL_ERASER,
    TOOL_FILL,
    TOOL_RECT,
    TOOL_CIRCLE,
    TOOL_LINE
} DrawTool;

/* Editor object types */
typedef enum {
    OBJ_SPRITE,
    OBJ_ENTITY,
    OBJ_COLLISION_BOX,
    OBJ_TRIGGER,
    OBJ_SPAWN_POINT
} ObjectType;

/* Editor object */
typedef struct {
    ObjectType type;
    int x, y;
    int width, height;
    uint32_t color;
    char name[32];
    void *data;
    int selected;
    int active;
} EditorObject;

/* Sprite being edited */
typedef struct {
    uint32_t *pixels;
    int width, height;
    int zoom;
    char name[32];
} SpriteEditor;

/* Level data */
typedef struct {
    EditorObject objects[256];
    int num_objects;
    uint32_t background_color;
    int width, height;
    char name[64];
} Level;

/* Editor state */
typedef struct {
    BlitEngine *engine;
    EditorMode mode;
    DrawTool tool;
    
    Level level;
    SpriteEditor sprite_editor;
    
    int selected_object;
    int dragging;
    int drag_start_x, drag_start_y;
    
    uint32_t current_color;
    int brush_size;
    
    int camera_x, camera_y;
    int grid_enabled;
    int grid_size;
    
    int show_help;
    int show_palette;
    int show_objects_panel;
    
    char status_message[128];
    int status_timer;
} BlitStudioState;

/* ============================================================
 * BlitStudio Core Functions
 * ============================================================ */

/* Initialize editor */
int blitstudio_init(BlitStudioState *editor, BlitEngine *engine);

/* Shutdown editor */
void blitstudio_shutdown(BlitStudioState *editor);

/* Update editor (call once per frame) */
void blitstudio_update(BlitStudioState *editor);

/* Draw editor (call once per frame) */
void blitstudio_draw(BlitStudioState *editor);

/* Handle input */
void blitstudio_handle_input(BlitStudioState *editor);

/* ============================================================
 * Level Functions
 * ============================================================ */

/* Create new level */
void blitstudio_new_level(BlitStudioState *editor);

/* Save level to file */
int blitstudio_save_level(BlitStudioState *editor, const char *filename);

/* Load level from file */
int blitstudio_load_level(BlitStudioState *editor, const char *filename);

/* Export level as C code */
int blitstudio_export_code(BlitStudioState *editor, const char *filename);

/* ============================================================
 * Object Functions
 * ============================================================ */

/* Add object to level */
int blitstudio_add_object(BlitStudioState *editor, ObjectType type, int x, int y);

/* Delete selected object */
void blitstudio_delete_object(BlitStudioState *editor);

/* Select object at position */
int blitstudio_select_object(BlitStudioState *editor, int x, int y);

/* Move selected object */
void blitstudio_move_selected(BlitStudioState *editor, int dx, int dy);

/* ============================================================
 * Sprite Editor Functions
 * ============================================================ */

/* Create new sprite */
void blitstudio_new_sprite(BlitStudioState *editor, int width, int height);

/* Draw pixel in sprite */
void blitstudio_draw_pixel(BlitStudioState *editor, int x, int y, uint32_t color);

/* Save sprite */
int blitstudio_save_sprite(BlitStudioState *editor, const char *filename);

/* Load sprite */
int blitstudio_load_sprite(BlitStudioState *editor, const char *filename);

/* ============================================================
 * UI Functions
 * ============================================================ */

/* Draw toolbar */
void blitstudio_draw_toolbar(BlitStudioState *editor);

/* Draw status bar */
void blitstudio_draw_status_bar(BlitStudioState *editor);

/* Draw grid */
void blitstudio_draw_grid(BlitStudioState *editor);

/* Draw color palette */
void blitstudio_draw_palette(BlitStudioState *editor);

/* Draw objects panel */
void blitstudio_draw_objects_panel(BlitStudioState *editor);

/* Draw help overlay */
void blitstudio_draw_help(BlitStudioState *editor);

/* Set status message */
void blitstudio_set_status(BlitStudioState *editor, const char *message);

/* ============================================================
 * Utility Functions
 * ============================================================ */

/* Snap to grid */
void blitstudio_snap_to_grid(BlitStudioState *editor, int *x, int *y);

/* Get object at position */
int blitstudio_get_object_at(BlitStudioState *editor, int x, int y);

/* Common color palette */
extern const uint32_t BLITSTUDIO_PALETTE[16];
