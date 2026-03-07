// BlitStudio - Visual Game Editor Application
// Create games without code!

#include <Blit/BlitStudio.h>
#include <libc.h>

int md_main(long argc, char **argv) {
    BlitEngine engine;
    
    if (blit_init(&engine) != 0) {
        printf("Failed to initialize Blit Engine!\n");
        return 1;
    }
    
    printf("===========================================\n");
    printf("  BlitStudio - Visual Game Creator\n");
    printf("  Powered by Blit Engine\n");
    printf("===========================================\n");
    printf("\n");
    printf("Controls:\n");
    printf("  TAB       - Switch modes\n");
    printf("  F1        - Toggle help\n");
    printf("  G         - Toggle grid\n");
    printf("  Ctrl+S    - Save level\n");
    printf("  Ctrl+O    - Load level\n");
    printf("  ESC       - Exit\n");
    printf("\n");
    printf("Modes:\n");
    printf("  SELECT    - Select and move objects\n");
    printf("  PAINT     - Draw sprites\n");
    printf("  ENTITY    - Place game entities\n");
    printf("  COLLISION - Add collision boxes\n");
    printf("  TEST      - Test your level\n");
    printf("\n");
    printf("Starting BlitStudio...\n\n");
    
    BlitStudioState editor;
    if (blitstudio_init(&editor, &engine) != 0) {
        printf("Failed to initialize BlitStudio!\n");
        blit_shutdown(&engine);
        return 1;
    }
    
    // Main editor loop
    while (blit_is_running(&engine)) {
        blit_update_input(&engine);
        
        // ESC to exit
        if (blit_key_pressed(&engine, BLIT_KEY_ESC)) {
            printf("Exiting BlitStudio...\n");
            break;
        }
        
        blitstudio_update(&editor);
        blitstudio_draw(&editor);
    }
    
    printf("\nEditor statistics:\n");
    printf("  Objects created: %d\n", editor.level.num_objects);
    printf("  Level name: %s\n", editor.level.name);
    
    blitstudio_shutdown(&editor);
    blit_shutdown(&engine);
    
    printf("\nThank you for using BlitStudio!\n");
    return 0;
}
