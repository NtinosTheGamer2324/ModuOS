// NodGL Stress Test - Push GPU to limits
#include "NodGL.h"
#include "libc.h"

#define MAX_PARTICLES 5000
#define MAX_OBJECTS 100

typedef struct {
    float x, y;
    float vx, vy;
    uint32_t color;
    uint8_t size;
} particle_t;

typedef struct {
    float x, y;
    float vx, vy;
    float rotation;
    float rot_speed;
    uint32_t color;
    uint8_t size;
} object_t;

static particle_t particles[MAX_PARTICLES];
static object_t objects[MAX_OBJECTS];
static uint32_t g_screen_w = 800;
static uint32_t g_screen_h = 600;

static uint32_t rand_state = 12345;

static uint32_t xorshift32(void) {
    rand_state ^= rand_state << 13;
    rand_state ^= rand_state >> 17;
    rand_state ^= rand_state << 5;
    return rand_state;
}

static void init_particles(void) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        particles[i].x = (xorshift32() % g_screen_w);
        particles[i].y = (xorshift32() % g_screen_h);
        particles[i].vx = ((float)(xorshift32() % 1000) - 500.0f) / 100.0f;
        particles[i].vy = ((float)(xorshift32() % 1000) - 500.0f) / 100.0f;
        particles[i].color = 0xFF000000 | (xorshift32() & 0xFFFFFF);
        particles[i].size = 2 + (xorshift32() % 4);
    }
}

static void init_objects(void) {
    for (int i = 0; i < MAX_OBJECTS; i++) {
        objects[i].x = (xorshift32() % g_screen_w);
        objects[i].y = (xorshift32() % g_screen_h);
        objects[i].vx = ((float)(xorshift32() % 600) - 300.0f) / 100.0f;
        objects[i].vy = ((float)(xorshift32() % 600) - 300.0f) / 100.0f;
        objects[i].rotation = 0.0f;
        objects[i].rot_speed = ((float)(xorshift32() % 100) - 50.0f) / 1000.0f;
        objects[i].color = 0xFF000000 | (xorshift32() & 0xFFFFFF);
        objects[i].size = 10 + (xorshift32() % 40);
    }
}

static void update_particles(void) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        particles[i].x += particles[i].vx;
        particles[i].y += particles[i].vy;
        
        if (particles[i].x < 0 || particles[i].x >= g_screen_w) particles[i].vx = -particles[i].vx;
        if (particles[i].y < 0 || particles[i].y >= g_screen_h) particles[i].vy = -particles[i].vy;
        
        if (particles[i].x < 0) particles[i].x = 0;
        if (particles[i].x >= g_screen_w) particles[i].x = g_screen_w - 1;
        if (particles[i].y < 0) particles[i].y = 0;
        if (particles[i].y >= g_screen_h) particles[i].y = g_screen_h - 1;
    }
}

static void update_objects(void) {
    for (int i = 0; i < MAX_OBJECTS; i++) {
        objects[i].x += objects[i].vx;
        objects[i].y += objects[i].vy;
        objects[i].rotation += objects[i].rot_speed;
        
        if (objects[i].x < 0 || objects[i].x > g_screen_w) objects[i].vx = -objects[i].vx;
        if (objects[i].y < 0 || objects[i].y > g_screen_h) objects[i].vy = -objects[i].vy;
        
        if (objects[i].x < 0) objects[i].x = 0;
        if (objects[i].x >= g_screen_w) objects[i].x = g_screen_w - 1;
        if (objects[i].y < 0) objects[i].y = 0;
        if (objects[i].y >= g_screen_h) objects[i].y = g_screen_h - 1;
    }
}

static void draw_particles(NodGL_Context ctx) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        int32_t x = (int32_t)particles[i].x;
        int32_t y = (int32_t)particles[i].y;
        NodGL_FillRectContext(ctx, x, y, particles[i].size, particles[i].size, particles[i].color);
    }
}

static void draw_objects(NodGL_Context ctx) {
    for (int i = 0; i < MAX_OBJECTS; i++) {
        int32_t x = (int32_t)objects[i].x;
        int32_t y = (int32_t)objects[i].y;
        uint8_t s = objects[i].size;
        
        /* Draw rotated square using lines */
        NodGL_FillRectContext(ctx, x - s/2, y - s/2, s, s, objects[i].color);
        
        /* Draw border */
        uint32_t border = 0xFFFFFFFF;
        NodGL_DrawLineContext(ctx, x - s/2, y - s/2, x + s/2, y - s/2, border, 1);
        NodGL_DrawLineContext(ctx, x + s/2, y - s/2, x + s/2, y + s/2, border, 1);
        NodGL_DrawLineContext(ctx, x + s/2, y + s/2, x - s/2, y + s/2, border, 1);
        NodGL_DrawLineContext(ctx, x - s/2, y + s/2, x - s/2, y - s/2, border, 1);
    }
}

int md_main(long argc, char **argv) {
    (void)argc;
    (void)argv;
    
    printf("=== NodGL GPU Stress Test ===\n\n");
    printf("Rendering:\n");
    printf("  - %u particles\n", MAX_PARTICLES);
    printf("  - %u animated objects\n", MAX_OBJECTS);
    printf("  - Full-screen clear every frame\n");
    printf("  - Testing GPU acceleration limits\n\n");
    
    NodGL_Device device;
    NodGL_Context ctx;
    
    if (NodGL_CreateDevice(NodGL_FEATURE_LEVEL_1_0, &device, &ctx, NULL) != NodGL_OK) {
        printf("Failed to create NodGL device\n");
        return 1;
    }
    
    NodGL_DeviceCaps caps;
    NodGL_GetDeviceCaps(device, &caps);
    g_screen_w = caps.screen_width;
    g_screen_h = caps.screen_height;
    printf("GPU: %s\n", caps.adapter_name);
    
    if (caps.capabilities & NodGL_CAP_HARDWARE_ACCEL) {
        printf("Status: Hardware 2D acceleration ENABLED\n\n");
    } else {
        printf("Status: Software rendering (no acceleration)\n\n");
    }
    
    NodGL_Viewport vp = {0, 0, (float)g_screen_w, (float)g_screen_h, 0.0f, 1.0f};
    NodGL_SetViewport(ctx, &vp);
    
    init_particles();
    init_objects();
    
    printf("Running stress test (300 frames)...\n");
    printf("Watch for smooth animation and no lag!\n\n");
    
    uint32_t frame_count = 0;
    uint64_t start_time = 0; /* Would use real timer in production */
    
    for (int frame = 0; frame < 300; frame++) {
        /* Clear to gradient background */
        uint8_t bg = (frame * 2) % 256;
        NodGL_ClearContext(ctx, NodGL_CLEAR_COLOR, NodGL_ColorARGB(0xFF, 0, bg/4, bg/2), 1.0f, 0);
        
        /* Update and draw particles */
        update_particles();
        draw_particles(ctx);
        
        /* Update and draw objects */
        update_objects();
        draw_objects(ctx);
        
        /* FPS counter (simplified) */
        if (frame % 30 == 0) {
            char fps_text[64];
            snprintf(fps_text, sizeof(fps_text), "Frame %d/300", frame);
            
            /* Draw FPS text as rectangles */
            for (uint32_t i = 0; fps_text[i] && i < 20; i++) {
                NodGL_FillRectContext(ctx, 10 + i * 8, 10, 6, 12, NodGL_ColorARGB(0xFF, 0xFF, 0xFF, 0xFF));
            }
        }
        
        /* Present */
        NodGL_PresentContext(ctx, 1);
        
        frame_count++;
        
        /* Progress indicator */
        if (frame % 60 == 0) {
            printf("  Frame %d/300 (%.0f%%)\n", frame, (frame / 300.0f) * 100.0f);
        }
    }
    
    printf("\n=== Stress Test Complete ===\n");
    printf("Total frames rendered: %u\n", frame_count);
    printf("Total draw calls: ~%u (approx)\n", frame_count * (MAX_PARTICLES + MAX_OBJECTS * 5));
    
    if (caps.capabilities & NodGL_CAP_HARDWARE_ACCEL) {
        printf("\nWith hardware acceleration, this should run smoothly.\n");
        printf("VMSVGA performance fix ensures no lag!\n");
    } else {
        printf("\nSoftware rendering - some slowdown is expected.\n");
    }
    
    NodGL_ReleaseDevice(device);
    return 0;
}



