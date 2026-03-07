#include "libc.h"
#include "string.h"
#include "NodGL.h"
#include <stdint.h>
#include "lib_fnt.h"

#define SEEK_SET 0
#define SEEK_END 2

// Signal handler - TTY managers NEVER exit
void signal_handler(int sig) {
    (void)sig;
    // Ignore all signals - we're a system service!
}

// Render string using LibFNT with NodGL (clean and fast)
void render_string(fnt_font_t *font, const char *str, int x, int y, uint32_t color) {
    if (!font || !str) return;
    
    int cx = x;
    
    while (*str) {
        fnt_glyph_t *glyph = fnt_get_glyph(font, (uint32_t)(unsigned char)*str);
        if (glyph) {
            // Render glyph bitmap using NodGL
            for (int dy = 0; dy < glyph->bitmap_height; dy++) {
                for (int dx = 0; dx < glyph->bitmap_width; dx++) {
                    if (fnt_get_pixel(glyph, dx, dy)) {
                        NodGL_FillRect(cx + dx, y + dy, 1, 1, color);
                    }
                }
            }
            
            cx += glyph->width;
        }
        str++;
    }
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    printf("ttyman - Terminal Manager (System Service)\n");
    
    // Ignore signals - TTY managers never exit
    signal(SIGINT, SIG_IGN);    // Ignore Ctrl+C
    signal(SIGTERM, SIG_IGN);   // Ignore termination
    signal(SIGHUP, SIG_IGN);    // Ignore hangup
    signal(SIGQUIT, SIG_IGN);   // Ignore quit
    
    int fd = open("/ModuOS/shared/usr/assets/fonts/Unicode.fnt", O_RDONLY, 0);
    if (fd < 0) {
        printf("Error: Could not open font file!\n");
        return 1;
    }
    
    long font_size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    
    if (font_size <= 0) {
        printf("Error: Invalid font file size!\n");
        close(fd);
        return 1;
    }
    
    void *font_data = malloc((size_t)font_size);
    if (!font_data) {
        printf("Error: Could not allocate memory for font!\n");
        close(fd);
        return 1;
    }
    
    ssize_t bytes_read = read(fd, font_data, (size_t)font_size);
    close(fd);
    
    if (bytes_read != font_size) {
        printf("Error: Could not read font file!\n");
        free(font_data);
        return 1;
    }
    
    fnt_font_t *font = fnt_load_font(font_data, font_size);
    if (!font) {
        printf("Error: Could not parse font!\n");
        free(font_data);
        return 1;
    }
    
    printf("Font loaded successfully: %s\n", font->header.name);
    printf("Glyph size: %ux%u\n", font->header.glyph_width, font->header.glyph_height);
    
    if (NodGL_Init() != 0) {
        printf("Error: Could not initialize NodGL!\n");
        fnt_free_font(font);
        free(font_data);
        return 1;
    }
    
    // Clear screen to black
    NodGL_Clear(0xFF000000);
    
    render_string(font, "Hello from ttyman!", 100, 100, 0xFFFFFFFF);
    render_string(font, "Unicode font loaded successfully!", 100, 120, 0xFF00FF00);
    render_string(font, "Press any key to exit...", 100, 140, 0xFF888888);
    
    // Present to screen
    NodGL_Present();
    
    // Main loop - TTY managers run FOREVER
    printf("TTY manager running. Cannot be killed.\n");
    
    while (1) {
        // TODO: Handle TTY input/output
        // TODO: Process escape sequences
        // TODO: Handle scrolling
        // TODO: Manage cursor
        
        sleep(1);  // For now, just idle
    }
    
    // NEVER REACHED - TTY managers don't exit
    return 0;
}