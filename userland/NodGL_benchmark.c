// NodGL GPU Benchmark - Measure 2D/3D Performance
#include "NodGL.h"
#include "libc.h"

#define BENCH_ITERATIONS 1000

typedef struct {
    const char *name;
    uint64_t cycles;
    uint32_t ops;
} benchmark_result_t;

static uint32_t g_screen_w = 800;
static uint32_t g_screen_h = 600;

static uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void bench_fill_rect(NodGL_Context ctx, benchmark_result_t *result) {
    uint64_t start = rdtsc();
    
    for (uint32_t i = 0; i < BENCH_ITERATIONS; i++) {
        uint32_t x = (i * 73) % (g_screen_w - 100);
        uint32_t y = (i * 37) % (g_screen_h - 100);
        uint32_t color = 0xFF000000 | ((i * 1234567) & 0xFFFFFF);
        NodGL_FillRectContext(ctx, x, y, 100, 100, color);
    }
    
    uint64_t end = rdtsc();
    result->cycles = end - start;
    result->ops = BENCH_ITERATIONS;
}

static void bench_draw_line(NodGL_Context ctx, benchmark_result_t *result) {
    uint64_t start = rdtsc();
    
    for (uint32_t i = 0; i < BENCH_ITERATIONS; i++) {
        int32_t x0 = (i * 13) % g_screen_w;
        int32_t y0 = (i * 17) % g_screen_h;
        int32_t x1 = ((i + 100) * 19) % g_screen_w;
        int32_t y1 = ((i + 100) * 23) % g_screen_h;
        uint32_t color = 0xFF000000 | ((i * 987654) & 0xFFFFFF);
        NodGL_DrawLineContext(ctx, x0, y0, x1, y1, color, 2);
    }
    
    uint64_t end = rdtsc();
    result->cycles = end - start;
    result->ops = BENCH_ITERATIONS;
}

static void bench_blit_texture(NodGL_Device device, NodGL_Context ctx, benchmark_result_t *result) {
    /* Create a test texture */
    NodGL_TextureDesc desc = {0};
    desc.width = 64;
    desc.height = 64;
    desc.format = NodGL_FORMAT_R8G8B8A8_UNORM;
    desc.mip_levels = 1;
    
    uint32_t *pixels = malloc(64 * 64 * 4);
    if (!pixels) {
        result->cycles = 0;
        result->ops = 0;
        return;
    }
    
    for (uint32_t i = 0; i < 64 * 64; i++) {
        pixels[i] = 0xFF000000 | ((i * 12345) & 0xFFFFFF);
    }
    
    desc.initial_data = pixels;
    desc.initial_data_size = 64 * 64 * 4;
    
    NodGL_Texture tex;
    if (NodGL_CreateTexture(device, &desc, &tex) != NodGL_OK) {
        free(pixels);
        result->cycles = 0;
        result->ops = 0;
        return;
    }
    
    uint64_t start = rdtsc();
    
    for (uint32_t i = 0; i < BENCH_ITERATIONS; i++) {
        int32_t x = (i * 41) % (g_screen_w - 64);
        int32_t y = (i * 47) % (g_screen_h - 64);
        NodGL_DrawTexture(ctx, tex, 0, 0, x, y, 64, 64);
    }
    
    uint64_t end = rdtsc();
    result->cycles = end - start;
    result->ops = BENCH_ITERATIONS;
    
    NodGL_ReleaseResource(device, tex);
    free(pixels);
}

static void bench_clear_screen(NodGL_Context ctx, benchmark_result_t *result) {
    uint64_t start = rdtsc();
    
    for (uint32_t i = 0; i < BENCH_ITERATIONS / 10; i++) {
        uint32_t color = 0xFF000000 | ((i * 111111) & 0xFFFFFF);
        NodGL_ClearContext(ctx, NodGL_CLEAR_COLOR, color, 1.0f, 0);
    }
    
    uint64_t end = rdtsc();
    result->cycles = end - start;
    result->ops = BENCH_ITERATIONS / 10;
}

static void bench_present(NodGL_Context ctx, benchmark_result_t *result) {
    uint64_t start = rdtsc();
    
    for (uint32_t i = 0; i < BENCH_ITERATIONS / 10; i++) {
        NodGL_PresentContext(ctx, 0); /* No vsync for benchmark */
    }
    
    uint64_t end = rdtsc();
    result->cycles = end - start;
    result->ops = BENCH_ITERATIONS / 10;
}

static void print_result(const char *test_name, benchmark_result_t *result) {
    if (result->ops == 0) {
        printf("  %-25s FAILED\n", test_name);
        return;
    }
    
    uint64_t cycles_per_op = result->cycles / result->ops;
    double ops_per_sec = 0.0;
    
    /* Assume 2.5 GHz CPU for rough estimate */
    if (cycles_per_op > 0) {
        ops_per_sec = (2500000000.0 / cycles_per_op);
    }
    
    printf("  %-25s %10lu cycles/op | ~%.0f ops/sec\n", 
           test_name, cycles_per_op, ops_per_sec);
}

int md_main(long argc, char **argv) {
    printf("=== NodGL GPU Benchmark ===\n\n");
    
    NodGL_Device device;
    NodGL_Context ctx;
    NodGL_FeatureLevel level;
    
    if (NodGL_CreateDevice(NodGL_FEATURE_LEVEL_1_0, &device, &ctx, &level) != NodGL_OK) {
        printf("Failed to create NodGL device\n");
        return 1;
    }
    
    NodGL_DeviceCaps caps;
    NodGL_GetDeviceCaps(device, &caps);
    
    printf("GPU Information:\n");
    printf("  Driver:       %s\n", caps.adapter_name);
    printf("  Feature Level: 0x%04x\n", caps.feature_level);
    g_screen_w = caps.screen_width;
    g_screen_h = caps.screen_height;
    printf("  Resolution:   %ux%u @ %ubpp\n", g_screen_w, g_screen_h, 32);
    printf("  Video Memory: %u MB\n", caps.video_memory_mb);
    printf("\nCapabilities:\n");
    if (caps.capabilities & NodGL_CAP_HARDWARE_ACCEL)
        printf("  [X] Hardware 2D Acceleration\n");
    else
        printf("  [ ] Hardware 2D Acceleration\n");
    if (caps.capabilities & NodGL_CAP_ALPHA_BLEND)
        printf("  [X] Alpha Blending\n");
    else
        printf("  [ ] Alpha Blending\n");
    if (caps.capabilities & NodGL_CAP_3D_PIPELINE)
        printf("  [X] 3D Pipeline\n");
    else
        printf("  [ ] 3D Pipeline\n");

    printf("\n=== Running Benchmarks (%u iterations) ===\n\n", BENCH_ITERATIONS);
    
    NodGL_Viewport vp = {0, 0, (float)g_screen_w, (float)g_screen_h, 0.0f, 1.0f};
    NodGL_SetViewport(ctx, &vp);
    
    benchmark_result_t result;
    
    printf("2D Operations:\n");
    
    bench_fill_rect(ctx, &result);
    print_result("FillRect (100x100)", &result);
    
    bench_draw_line(ctx, &result);
    print_result("DrawLine", &result);
    
    bench_blit_texture(device, ctx, &result);
    print_result("Blit Texture (64x64)", &result);
    
    bench_clear_screen(ctx, &result);
    print_result("Clear Screen", &result);
    
    bench_present(ctx, &result);
    print_result("Present (no vsync)", &result);
    
    printf("\n=== Benchmark Complete ===\n");
    printf("\nNotes:\n");
    printf("  - Lower cycles/op = faster\n");
    printf("  - Higher ops/sec = better\n");
    printf("  - Results depend on GPU driver and hardware\n");
    printf("  - VMSVGA performance fix should show similar speeds to QXL\n");
    
    NodGL_ReleaseDevice(device);
    return 0;
}


