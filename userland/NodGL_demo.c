// NodGL Demo - Showcase modern graphics API for ModuOS
//
// Copyright © 2025-2026 ModuOS Project Contributors
// Licensed under GPL v2.0 - See LICENSE.md

#include "NodGL.h"
#include "libc.h"

static void draw_gradient_background(NodGL_Context ctx, uint32_t width, uint32_t height) {
    for (uint32_t y = 0; y < height; y += 4) {
        uint8_t intensity = (uint8_t)((y * 255) / height);
        uint32_t color = NodGL_ColorARGB(0xFF, 0, intensity / 2, intensity);
        NodGL_FillRectContext(ctx, 0, (int32_t)y, width, 4, color);
    }
}

static void draw_test_pattern(NodGL_Context ctx) {
    NodGL_FillRectContext(ctx, 50, 50, 100, 100, NodGL_ColorARGB(0xFF, 0xFF, 0, 0));
    NodGL_FillRectContext(ctx, 200, 50, 100, 100, NodGL_ColorARGB(0xFF, 0, 0xFF, 0));
    NodGL_FillRectContext(ctx, 350, 50, 100, 100, NodGL_ColorARGB(0xFF, 0, 0, 0xFF));
    
    NodGL_DrawLineContext(ctx, 100, 200, 400, 200, NodGL_ColorARGB(0xFF, 0xFF, 0xFF, 0xFF), 3);
    NodGL_DrawLineContext(ctx, 100, 200, 250, 350, NodGL_ColorARGB(0xFF, 0xFF, 0, 0xFF), 2);
    NodGL_DrawLineContext(ctx, 250, 350, 400, 200, NodGL_ColorARGB(0xFF, 0xFF, 0xFF, 0), 2);
}

static void draw_texture_demo(NodGL_Context ctx, NodGL_Device device) {
    NodGL_TextureDesc tex_desc = {0};
    tex_desc.width = 64;
    tex_desc.height = 64;
    tex_desc.format = NodGL_FORMAT_R8G8B8A8_UNORM;
    tex_desc.mip_levels = 1;

    uint32_t *tex_data = malloc(64 * 64 * 4);
    if (!tex_data) return;

    for (uint32_t y = 0; y < 64; y++) {
        for (uint32_t x = 0; x < 64; x++) {
            uint8_t r = (uint8_t)((x * 255) / 63);
            uint8_t g = (uint8_t)((y * 255) / 63);
            uint8_t b = (uint8_t)(((x + y) * 255) / 126);
            tex_data[y * 64 + x] = NodGL_ColorARGB(0xFF, r, g, b);
        }
    }

    tex_desc.initial_data = tex_data;
    tex_desc.initial_data_size = 64 * 64 * 4;

    NodGL_Texture texture = 0;
    if (NodGL_CreateTexture(device, &tex_desc, &texture) == NodGL_OK) {
        for (int i = 0; i < 5; i++) {
            NodGL_DrawTexture(ctx, texture, 0, 0, 500 + i * 80, 50 + i * 80, 64, 64);
        }
        NodGL_ReleaseResource(device, texture);
    }

    free(tex_data);
}

static void draw_animated_boxes(NodGL_Context ctx, uint32_t frame) {
    for (int i = 0; i < 10; i++) {
        int x = 50 + (int)((frame * 2 + i * 30) % 700);
        int y = 400 + (int)((frame + i * 20) % 100);
        uint32_t color = NodGL_ColorARGB(0xFF, 
            (uint8_t)((i * 25) % 255), 
            (uint8_t)((frame + i * 30) % 255),
            (uint8_t)((255 - i * 20) % 255));
        NodGL_FillRectContext(ctx, x, y, 40, 40, color);
    }
}

static void print_device_info(const NodGL_DeviceCaps *caps) {
    printf("=== NodGL Device Information ===\n");
    printf("Adapter: %s\n", caps->adapter_name);
    printf("Feature Level: 0x%04x\n", caps->feature_level);
    printf("Video Memory: %u MB\n", caps->video_memory_mb);
    printf("Max Texture Size: %ux%u\n", caps->max_texture_width, caps->max_texture_height);
    printf("Max Buffers: %u\n", caps->max_buffers);
    printf("Screen: %ux%u\n", caps->screen_width, caps->screen_height);
    
    printf("\nCapabilities:\n");
    if (caps->capabilities & NodGL_CAP_HARDWARE_ACCEL)
        printf("  - Hardware Acceleration\n");
    if (caps->capabilities & NodGL_CAP_ALPHA_BLEND)
        printf("  - Alpha Blending\n");
    if (caps->capabilities & NodGL_CAP_3D_PIPELINE)
        printf("  - 3D Pipeline\n");
    if (caps->capabilities & NodGL_CAP_SHADER_SUPPORT)
        printf("  - Shader Support\n");
    if (caps->capabilities & NodGL_CAP_VSYNC)
        printf("  - VSync\n");
    printf("\n");
}

int md_main(long argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("NodGL Demo - Modern Graphics API for ModuOS\n\n");

    NodGL_Device device = NULL;
    NodGL_Context context = NULL;
    NodGL_FeatureLevel actual_level = 0;

    int result = NodGL_CreateDevice(NodGL_FEATURE_LEVEL_1_0, &device, &context, &actual_level);
    if (result != NodGL_OK) {
        printf("Failed to create NodGL device: %s\n", NodGL_GetErrorString(result));
        return 1;
    }

    printf("NodGL device created successfully!\n");
    printf("Feature Level: 0x%04x\n\n", actual_level);

    NodGL_DeviceCaps caps;
    if (NodGL_GetDeviceCaps(device, &caps) == NodGL_OK) {
        print_device_info(&caps);
    }

    NodGL_Viewport viewport = {0};
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = (float)caps.screen_width;
    viewport.height = (float)caps.screen_height;
    viewport.min_depth = 0.0f;
    viewport.max_depth = 1.0f;
    NodGL_SetViewport(context, &viewport);

    printf("Running animation (press Ctrl+C to exit)...\n\n");

    for (uint32_t frame = 0; frame < 300; frame++) {
        NodGL_ClearContext(context, NodGL_CLEAR_COLOR, NodGL_ColorARGB(0xFF, 0x00, 0x00, 0x20), 1.0f, 0);

        draw_gradient_background(context, caps.screen_width, caps.screen_height);
        
        draw_test_pattern(context);
        
        if (frame % 60 == 0) {
            draw_texture_demo(context, device);
        }
        
        draw_animated_boxes(context, frame);

        NodGL_DrawLineContext(context, 0, (int32_t)caps.screen_height - 20, (int32_t)caps.screen_width, (int32_t)caps.screen_height - 20, NodGL_ColorARGB(0xFF, 0xFF, 0xFF, 0xFF), 2);
        
        char fps_text[64];
        snprintf(fps_text, sizeof(fps_text), "Frame %u", frame);
        
        for (uint32_t i = 0; fps_text[i]; i++) {
            uint32_t char_x = 10 + i * 8;
            NodGL_FillRectContext(context, (int32_t)char_x, 10, 6, 12, NodGL_ColorARGB(0xFF, 0xFF, 0xFF, 0xFF));
        }

        result = NodGL_PresentContext(context, 1);
        if (result != NodGL_OK) {
            printf("Present failed: %s\n", NodGL_GetErrorString(result));
            break;
        }

        for (volatile int i = 0; i < 100000; i++);
    }

    printf("\nCleaning up...\n");
    NodGL_ReleaseDevice(device);
    
    printf("NodGL demo completed successfully!\n");
    return 0;
}


