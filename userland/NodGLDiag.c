#include "NodGL.h"
#include "libc.h"

int md_main(long argc, char **argv) {
    printf("┌────────────────────────────────────────┐\n");
    printf("│    NodGL v1.0 SYSTEM DIAGNOSTIC TOOL   │\n");
    printf("└────────────────────────────────────────┘\n\n");

    NodGL_Device device;
    NodGL_Context ctx;
    NodGL_FeatureLevel level;

    if (NodGL_CreateDevice(NodGL_FEATURE_LEVEL_1_0, &device, &ctx, &level) != NodGL_OK) {
        printf(" ✖ CRITICAL ERROR: Could not initialize NodGL Device\n");
        return 1;
    }

    NodGL_DeviceCaps caps;
    NodGL_GetDeviceCaps(device, &caps);
    uint32_t screen_w = caps.screen_width;
    uint32_t screen_h = caps.screen_height;

    printf(" - ADAPTER INFORMATION\n");
    printf(" ├─ Manufacturer:   %s\n", caps.adapter_name);
    printf(" ├─ Feature Level:  0x%04X (v%d.0)\n", (uint32_t)level, (uint32_t)level >> 12);
    printf(" ├─ VRAM Available: %u MB\n", caps.video_memory_mb);
    printf(" └─ Target Res:     %ux%u @ 32bpp\n\n", screen_w, screen_h);

    printf(" - HARDWARE CAPABILITIES\n");
    
    printf("  %s Hardware 2D Acceleration\n", (caps.capabilities & NodGL_CAP_HARDWARE_ACCEL) ? "✔" : "─");
    printf("  %s Alpha Blending\n",           (caps.capabilities & NodGL_CAP_ALPHA_BLEND)    ? "✔" : "─");
    printf("  %s 3D Pipeline\n",              (caps.capabilities & NodGL_CAP_3D_PIPELINE)   ? "✔" : "─");
    printf("  %s Shader Support\n",            (caps.capabilities & NodGL_CAP_SHADER_SUPPORT) ? "✔" : "─");

    printf("\n-- Diagnostic Complete. All systems operational. --\n");

    NodGL_ReleaseDevice(device);
    return 0;
}