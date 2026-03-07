// BlitStudio - Implementation
// NO _start here - main.c provides it
#define LIBC_NO_START
#include <Blit/BlitStudio.h>
#include <libc.h>
#include <moduos/kernel/events/events.h>

/* Color palette */
const uint32_t BLITSTUDIO_PALETTE[16] = {
    0xFF000000, 0xFFFFFFFF, 0xFF808080, 0xFFC0C0C0,
    0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00,
    0xFFFF00FF, 0xFF00FFFF, 0xFF800000, 0xFF008000,
    0xFF000080, 0xFF808000, 0xFF800080, 0xFF008080
};

/* ============================================================
 * Editor Core
 * ============================================================ */

int blitstudio_init(BlitStudioState *editor, BlitEngine *engine) {
    memset(editor, 0, sizeof(BlitStudioState));
    
    editor->engine = engine;
    editor->mode = EDITOR_MODE_SELECT;
    editor->tool = TOOL_PENCIL;
    
    editor->current_color = 0xFFFF0000;
    editor->brush_size = 1;
    
    editor->grid_enabled = 1;
    editor->grid_size = 32;
    
    editor->show_objects_panel = 1;
    
    blitstudio_new_level(editor);
    blitstudio_set_status(editor, "BlitStudio loaded - Press F1 for help");
    
    return 0;
}

void blitstudio_shutdown(BlitStudioState *editor) {
    if (editor->sprite_editor.pixels) {
        free(editor->sprite_editor.pixels);
    }
}

void blitstudio_update(BlitStudioState *editor) {
    blitstudio_handle_input(editor);
    
    if (editor->status_timer > 0) {
        editor->status_timer--;
    }
}

void blitstudio_draw(BlitStudioState *editor) {
    BlitEngine *engine = editor->engine;
    
    // Draw background
    blit_begin_frame(engine, editor->level.background_color);
    
    // Draw grid
    if (editor->grid_enabled && editor->mode != EDITOR_MODE_TEST) {
        blitstudio_draw_grid(editor);
    }
    
    // Draw level objects
    for (int i = 0; i < editor->level.num_objects; i++) {
        EditorObject *obj = &editor->level.objects[i];
        if (!obj->active) continue;
        
        uint32_t color = obj->color;
        if (obj->selected) {
            color = 0xFF00FFFF; // Cyan for selected
        }
        
        blit_draw_rect_outline(engine, obj->x, obj->y, obj->width, obj->height, color, 2);
        
        // Draw object type indicator
        const char *type_str = "?";
        switch (obj->type) {
            case OBJ_SPRITE: type_str = "S"; break;
            case OBJ_ENTITY: type_str = "E"; break;
            case OBJ_COLLISION_BOX: type_str = "C"; break;
            case OBJ_TRIGGER: type_str = "T"; break;
            case OBJ_SPAWN_POINT: type_str = "P"; break;
        }
        blit_draw_text(engine, type_str, obj->x + 4, obj->y + 4, color);
    }
    
    // Draw UI
    if (editor->mode != EDITOR_MODE_TEST) {
        blitstudio_draw_toolbar(editor);
        blitstudio_draw_status_bar(editor);
        
        if (editor->show_palette) {
            blitstudio_draw_palette(editor);
        }
        
        if (editor->show_objects_panel) {
            blitstudio_draw_objects_panel(editor);
        }
        
        if (editor->show_help) {
            blitstudio_draw_help(editor);
        }
    }
    
    blit_end_frame(engine);
}

void blitstudio_handle_input(BlitStudioState *editor) {
    BlitEngine *engine = editor->engine;
    
    // F1 - Toggle help
    if (blit_key_pressed(engine, BLIT_KEY_1) && blit_key_down(engine, BLIT_KEY_ESC)) {
        editor->show_help = !editor->show_help;
    }
    
    // Tab - Cycle modes
    if (blit_key_pressed(engine, 15)) { // Tab key
        editor->mode = (editor->mode + 1) % 5;
        const char *mode_names[] = {"SELECT", "PAINT", "ENTITY", "COLLISION", "TEST"};
        char msg[64];
        snprintf(msg, sizeof(msg), "Mode: %s", mode_names[editor->mode]);
        blitstudio_set_status(editor, msg);
    }
    
    // Grid toggle (G key)
    if (blit_key_pressed(engine, 34)) { // G key scancode
        editor->grid_enabled = !editor->grid_enabled;
    }
    
    // Mouse handling
    int mx = engine->input.mouse_x;
    int my = engine->input.mouse_y;
    
    if (editor->mode == EDITOR_MODE_SELECT) {
        if (blit_mouse_clicked(engine, MOUSE_LEFT)) {
            int obj_idx = blitstudio_get_object_at(editor, mx, my);
            if (obj_idx >= 0) {
                // Deselect all
                for (int i = 0; i < editor->level.num_objects; i++) {
                    editor->level.objects[i].selected = 0;
                }
                // Select this one
                editor->level.objects[obj_idx].selected = 1;
                editor->selected_object = obj_idx;
                editor->dragging = 1;
                editor->drag_start_x = mx;
                editor->drag_start_y = my;
            }
        }
        
        if (editor->dragging && blit_mouse_down(engine, MOUSE_LEFT)) {
            int dx = mx - editor->drag_start_x;
            int dy = my - editor->drag_start_y;
            blitstudio_move_selected(editor, dx, dy);
            editor->drag_start_x = mx;
            editor->drag_start_y = my;
        }
        
        if (!blit_mouse_down(engine, MOUSE_LEFT)) {
            editor->dragging = 0;
        }
        
        // Delete key
        if (blit_key_pressed(engine, 83)) { // Delete key
            blitstudio_delete_object(editor);
        }
    }
    
    if (editor->mode == EDITOR_MODE_ENTITY) {
        if (blit_mouse_clicked(engine, MOUSE_LEFT)) {
            blitstudio_add_object(editor, OBJ_ENTITY, mx, my);
        }
    }
    
    if (editor->mode == EDITOR_MODE_COLLISION) {
        if (blit_mouse_clicked(engine, MOUSE_LEFT)) {
            blitstudio_add_object(editor, OBJ_COLLISION_BOX, mx, my);
        }
    }
    
    // Save/Load
    if (blit_key_down(engine, 29) && blit_key_pressed(engine, BLIT_KEY_S)) { // Ctrl+S
        blitstudio_save_level(editor, "level.dat");
        blitstudio_set_status(editor, "Level saved!");
    }
    
    if (blit_key_down(engine, 29) && blit_key_pressed(engine, 24)) { // Ctrl+O
        if (blitstudio_load_level(editor, "level.dat") == 0) {
            blitstudio_set_status(editor, "Level loaded!");
        }
    }
}

/* ============================================================
 * Level Functions
 * ============================================================ */

void blitstudio_new_level(BlitStudioState *editor) {
    memset(&editor->level, 0, sizeof(Level));
    editor->level.background_color = 0xFF001020;
    editor->level.width = 800;
    editor->level.height = 600;
    snprintf(editor->level.name, sizeof(editor->level.name), "Untitled Level");
}

int blitstudio_save_level(BlitStudioState *editor, const char *filename) {
    // Simple binary save for now
    int fd = open(filename, 1, 0644); // Write mode
    if (fd < 0) return -1;
    
    write(fd, &editor->level, sizeof(Level));
    close(fd);
    return 0;
}

int blitstudio_load_level(BlitStudioState *editor, const char *filename) {
    int fd = open(filename, 0, 0); // Read mode
    if (fd < 0) return -1;
    
    read(fd, &editor->level, sizeof(Level));
    close(fd);
    return 0;
}

int blitstudio_export_code(BlitStudioState *editor, const char *filename) {
    // Export level as C code
    int fd = open(filename, 1, 0644);
    if (fd < 0) return -1;
    
    char buffer[256];
    
    // Header
    snprintf(buffer, sizeof(buffer), "// Generated by BlitStudio\n");
    write(fd, buffer, strlen(buffer));
    
    snprintf(buffer, sizeof(buffer), "// Level: %s\n\n", editor->level.name);
    write(fd, buffer, strlen(buffer));
    
    snprintf(buffer, sizeof(buffer), "void load_level() {\n");
    write(fd, buffer, strlen(buffer));
    
    // Export each object
    for (int i = 0; i < editor->level.num_objects; i++) {
        EditorObject *obj = &editor->level.objects[i];
        if (!obj->active) continue;
        
        snprintf(buffer, sizeof(buffer), 
            "    // %s\n    add_object(%d, %d, %d, %d, 0x%08X);\n",
            obj->name, obj->x, obj->y, obj->width, obj->height, obj->color);
        write(fd, buffer, strlen(buffer));
    }
    
    snprintf(buffer, sizeof(buffer), "}\n");
    write(fd, buffer, strlen(buffer));
    
    close(fd);
    return 0;
}

/* ============================================================
 * Object Functions
 * ============================================================ */

int blitstudio_add_object(BlitStudioState *editor, ObjectType type, int x, int y) {
    if (editor->level.num_objects >= 256) return -1;
    
    if (editor->grid_enabled) {
        blitstudio_snap_to_grid(editor, &x, &y);
    }
    
    EditorObject *obj = &editor->level.objects[editor->level.num_objects];
    obj->type = type;
    obj->x = x;
    obj->y = y;
    obj->width = editor->grid_size;
    obj->height = editor->grid_size;
    obj->color = editor->current_color;
    obj->active = 1;
    
    snprintf(obj->name, sizeof(obj->name), "Object_%d", editor->level.num_objects);
    
    editor->level.num_objects++;
    
    char msg[64];
    snprintf(msg, sizeof(msg), "Added object at (%d, %d)", x, y);
    blitstudio_set_status(editor, msg);
    
    return editor->level.num_objects - 1;
}

void blitstudio_delete_object(BlitStudioState *editor) {
    if (editor->selected_object < 0) return;
    
    editor->level.objects[editor->selected_object].active = 0;
    editor->selected_object = -1;
    
    blitstudio_set_status(editor, "Object deleted");
}

int blitstudio_select_object(BlitStudioState *editor, int x, int y) {
    for (int i = editor->level.num_objects - 1; i >= 0; i--) {
        EditorObject *obj = &editor->level.objects[i];
        if (!obj->active) continue;
        
        if (blit_point_in_rect(x, y, obj->x, obj->y, obj->width, obj->height)) {
            return i;
        }
    }
    return -1;
}

void blitstudio_move_selected(BlitStudioState *editor, int dx, int dy) {
    if (editor->selected_object < 0) return;
    
    EditorObject *obj = &editor->level.objects[editor->selected_object];
    obj->x += dx;
    obj->y += dy;
}

/* ============================================================
 * UI Functions
 * ============================================================ */

void blitstudio_draw_toolbar(BlitStudioState *editor) {
    BlitEngine *engine = editor->engine;
    
    // Toolbar background
    blit_draw_rect(engine, 0, 0, engine->screen_width, 30, 0xFF202020);
    
    // Mode indicator
    const char *mode_str = "SELECT";
    switch (editor->mode) {
        case EDITOR_MODE_SELECT: mode_str = "SELECT"; break;
        case EDITOR_MODE_PAINT: mode_str = "PAINT"; break;
        case EDITOR_MODE_ENTITY: mode_str = "ENTITY"; break;
        case EDITOR_MODE_COLLISION: mode_str = "COLLISION"; break;
        case EDITOR_MODE_TEST: mode_str = "TEST"; break;
    }
    
    char toolbar_text[128];
    snprintf(toolbar_text, sizeof(toolbar_text), "Mode: %s | Objects: %d | Grid: %s",
        mode_str, editor->level.num_objects, editor->grid_enabled ? "ON" : "OFF");
    
    blit_draw_text(engine, toolbar_text, 10, 10, 0xFFFFFFFF);
}

void blitstudio_draw_status_bar(BlitStudioState *editor) {
    BlitEngine *engine = editor->engine;
    
    int y = engine->screen_height - 20;
    
    // Status bar background
    blit_draw_rect(engine, 0, y, engine->screen_width, 20, 0xFF202020);
    
    // Mouse position
    char mouse_text[64];
    snprintf(mouse_text, sizeof(mouse_text), "X:%d Y:%d", 
        engine->input.mouse_x, engine->input.mouse_y);
    blit_draw_text(engine, mouse_text, 10, y + 5, 0xFF00FF00);
    
    // Status message
    if (editor->status_timer > 0) {
        blit_draw_text(engine, editor->status_message, 200, y + 5, 0xFFFFFF00);
    }
}

void blitstudio_draw_grid(BlitStudioState *editor) {
    BlitEngine *engine = editor->engine;
    uint32_t grid_color = 0x40FFFFFF;
    
    for (int x = 0; x < engine->screen_width; x += editor->grid_size) {
        blit_draw_line(engine, x, 30, x, engine->screen_height - 20, grid_color, 1);
    }
    
    for (int y = 30; y < engine->screen_height - 20; y += editor->grid_size) {
        blit_draw_line(engine, 0, y, engine->screen_width, y, grid_color, 1);
    }
}

void blitstudio_draw_palette(BlitStudioState *editor) {
    BlitEngine *engine = editor->engine;
    
    int x = engine->screen_width - 200;
    int y = 40;
    
    blit_draw_rect(engine, x, y, 180, 200, 0xCC000000);
    blit_draw_text(engine, "Color Palette", x + 10, y + 10, 0xFFFFFFFF);
    
    for (int i = 0; i < 16; i++) {
        int px = x + 10 + (i % 4) * 40;
        int py = y + 30 + (i / 4) * 40;
        
        blit_draw_rect(engine, px, py, 35, 35, BLITSTUDIO_PALETTE[i]);
        
        if (editor->current_color == BLITSTUDIO_PALETTE[i]) {
            blit_draw_rect_outline(engine, px, py, 35, 35, 0xFFFFFFFF, 2);
        }
    }
}

void blitstudio_draw_objects_panel(BlitStudioState *editor) {
    BlitEngine *engine = editor->engine;
    
    blit_draw_rect(engine, 10, 40, 200, 300, 0xCC000000);
    blit_draw_text(engine, "Objects", 20, 50, 0xFFFFFFFF);
    
    int y = 70;
    int count = 0;
    for (int i = 0; i < editor->level.num_objects && count < 10; i++) {
        if (!editor->level.objects[i].active) continue;
        
        char obj_text[64];
        snprintf(obj_text, sizeof(obj_text), "%s", editor->level.objects[i].name);
        
        uint32_t text_color = editor->level.objects[i].selected ? 0xFF00FFFF : 0xFFCCCCCC;
        blit_draw_text(engine, obj_text, 20, y, text_color);
        
        y += 15;
        count++;
    }
}

void blitstudio_draw_help(BlitStudioState *editor) {
    BlitEngine *engine = editor->engine;
    
    int w = 400;
    int h = 350;
    int x = (engine->screen_width - w) / 2;
    int y = (engine->screen_height - h) / 2;
    
    blit_draw_rect(engine, x, y, w, h, 0xEE000000);
    blit_draw_rect_outline(engine, x, y, w, h, 0xFFFFFFFF, 2);
    
    blit_draw_text(engine, "BlitStudio Help", x + 130, y + 10, 0xFFFFFF00);
    
    const char *help_text[] = {
        "TAB       - Cycle modes",
        "F1        - Toggle help",
        "G         - Toggle grid",
        "",
        "SELECT MODE:",
        "  Click   - Select object",
        "  Drag    - Move object",
        "  DELETE  - Delete selected",
        "",
        "ENTITY MODE:",
        "  Click   - Place entity",
        "",
        "COLLISION MODE:",
        "  Click   - Place collision box",
        "",
        "Ctrl+S    - Save level",
        "Ctrl+O    - Load level",
        "ESC       - Exit editor",
        "",
        "Press F1 to close help"
    };
    
    int ty = y + 40;
    for (int i = 0; i < 20; i++) {
        blit_draw_text(engine, help_text[i], x + 20, ty, 0xFFFFFFFF);
        ty += 15;
    }
}

void blitstudio_set_status(BlitStudioState *editor, const char *message) {
    snprintf(editor->status_message, sizeof(editor->status_message), "%s", message);
    editor->status_timer = 180; // 3 seconds at 60fps
}

/* ============================================================
 * Utility Functions
 * ============================================================ */

void blitstudio_snap_to_grid(BlitStudioState *editor, int *x, int *y) {
    *x = (*x / editor->grid_size) * editor->grid_size;
    *y = (*y / editor->grid_size) * editor->grid_size;
}

int blitstudio_get_object_at(BlitStudioState *editor, int x, int y) {
    return blitstudio_select_object(editor, x, y);
}
