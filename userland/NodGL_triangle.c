// NodGL 3D Triangle Demo - Software 3D Rasterization
#include "NodGL.h"
#include "libc.h"

/* Freestanding math helpers */
static inline float NodGL_sqrtf(float x) {
    /* Fast inverse square root (Quake III) then reciprocal */
    union { float f; uint32_t i; } conv = { .f = x };
    conv.i = 0x5f3759df - (conv.i >> 1);
    float y = conv.f;
    y = y * (1.5f - (x * 0.5f * y * y));  /* Newton iteration */
    return x * y;  /* sqrt(x) = x * rsqrt(x) */
}

static inline float NodGL_sinf(float x) {
    /* Fast sine approximation using Taylor series */
    float x2 = x * x;
    return x * (1.0f - x2 * (0.16666667f - x2 * 0.00833333f));
}

static inline float NodGL_cosf(float x) {
    /* cos(x) = sin(x + π/2) */
    return NodGL_sinf(x + 1.57079632f);
}

static inline float NodGL_tanf(float x) {
    /* tan(x) ≈ sin(x) / cos(x) */
    float s = NodGL_sinf(x);
    float c = NodGL_cosf(x);
    return (c > 0.0001f || c < -0.0001f) ? s / c : 0.0f;
}

#define PI 3.14159265359f
static uint32_t g_screen_w = 800;
static uint32_t g_screen_h = 600;
typedef struct {
    float x, y, z;
} vec3_t;

typedef struct {
    float x, y, z, w;
} vec4_t;

typedef struct {
    vec3_t pos;
    uint32_t color;
} vertex_t;

static void vec3_normalize(vec3_t *v) {
    float len = NodGL_sqrtf(v->x * v->x + v->y * v->y + v->z * v->z);
    if (len > 0.0001f) {
        v->x /= len;
        v->y /= len;
        v->z /= len;
    }
}

static vec3_t vec3_cross(vec3_t a, vec3_t b) {
    vec3_t result;
    result.x = a.y * b.z - a.z * b.y;
    result.y = a.z * b.x - a.x * b.z;
    result.z = a.x * b.y - a.y * b.x;
    return result;
}

static vec4_t project_vertex(vec3_t v, float fov, float aspect, float near, float far) {
    float f = 1.0f / NodGL_tanf(fov * 0.5f);
    vec4_t result;
    result.x = v.x * f / aspect;
    result.y = v.y * f;
    result.z = v.z * (far + near) / (far - near) + (2.0f * far * near) / (far - near);
    result.w = -v.z;
    return result;
}

static void screen_coords(vec4_t *v) {
    if (v->w != 0.0f) {
        v->x /= v->w;
        v->y /= v->w;
        v->z /= v->w;
    }
    v->x = (v->x + 1.0f) * g_screen_w * 0.5f;
    v->y = (1.0f - v->y) * g_screen_h * 0.5f;
}

static void draw_spinning_cube(NodGL_Context ctx, float angle) {
    /* Cube vertices */
    vec3_t cube_verts[8] = {
        {-1, -1, -1}, { 1, -1, -1}, { 1,  1, -1}, {-1,  1, -1},
        {-1, -1,  1}, { 1, -1,  1}, { 1,  1,  1}, {-1,  1,  1}
    };

    /* Rotate around Y axis */
    float c = NodGL_cosf(angle);
    float s = NodGL_sinf(angle);

    vec3_t rotated[8];
    for (int i = 0; i < 8; i++) {
        float x = cube_verts[i].x;
        float z = cube_verts[i].z;
        rotated[i].x = x * c - z * s;
        rotated[i].y = cube_verts[i].y;
        rotated[i].z = x * s + z * c + 5.0f; /* Move back 5 units */
    }

    /* Project to screen */
    vec4_t projected[8];
    for (int i = 0; i < 8; i++) {
        projected[i] = project_vertex(rotated[i], PI / 3.0f, (float)g_screen_w / g_screen_h, 0.1f, 100.0f);
        screen_coords(&projected[i]);
    }

    /* Cube faces (indices) */
    int faces[12][3] = {
        {0, 1, 2}, {0, 2, 3}, /* Front */
        {4, 6, 5}, {4, 7, 6}, /* Back */
        {0, 4, 5}, {0, 5, 1}, /* Bottom */
        {2, 6, 7}, {2, 7, 3}, /* Top */
        {0, 3, 7}, {0, 7, 4}, /* Left */
        {1, 5, 6}, {1, 6, 2}  /* Right */
    };

    uint32_t colors[6] = {
        NodGL_ColorARGB(0xFF, 0xFF, 0, 0),     /* Red */
        NodGL_ColorARGB(0xFF, 0, 0xFF, 0),     /* Green */
        NodGL_ColorARGB(0xFF, 0, 0, 0xFF),     /* Blue */
        NodGL_ColorARGB(0xFF, 0xFF, 0xFF, 0),  /* Yellow */
        NodGL_ColorARGB(0xFF, 0xFF, 0, 0xFF),  /* Magenta */
        NodGL_ColorARGB(0xFF, 0, 0xFF, 0xFF)   /* Cyan */
    };

    /* Draw faces with backface culling */
    for (int f = 0; f < 12; f++) {
        int i0 = faces[f][0];
        int i1 = faces[f][1];
        int i2 = faces[f][2];

        /* Backface culling (check triangle winding) */
        float dx1 = projected[i1].x - projected[i0].x;
        float dy1 = projected[i1].y - projected[i0].y;
        float dx2 = projected[i2].x - projected[i0].x;
        float dy2 = projected[i2].y - projected[i0].y;
        float cross = dx1 * dy2 - dy1 * dx2;

        if (cross < 0) continue; /* Back-facing */

        /* Use syscall for triangle rendering (hw or sw) */
        uint32_t color = colors[f / 2];
        
        /* Simple software rasterization for now */
        int32_t x0 = (int32_t)projected[i0].x;
        int32_t y0 = (int32_t)projected[i0].y;
        int32_t x1 = (int32_t)projected[i1].x;
        int32_t y1 = (int32_t)projected[i1].y;
        int32_t x2 = (int32_t)projected[i2].x;
        int32_t y2 = (int32_t)projected[i2].y;

        /* Draw triangle using NodGL (will use hardware if available) */
        /* For now, draw outline with lines */
        NodGL_DrawLineContext(ctx, x0, y0, x1, y1, color, 2);
        NodGL_DrawLineContext(ctx, x1, y1, x2, y2, color, 2);
        NodGL_DrawLineContext(ctx, x2, y2, x0, y0, color, 2);
    }
}

int md_main(long argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("NodGL 3D Triangle Demo - Spinning Cube\n\n");

    NodGL_Device device;
    NodGL_Context ctx;
    NodGL_FeatureLevel level;

    if (NodGL_CreateDevice(NodGL_FEATURE_LEVEL_1_0, &device, &ctx, &level) != NodGL_OK) {
        printf("Failed to create NodGL device\n");
        return 1;
    }

    NodGL_DeviceCaps caps;
    NodGL_GetDeviceCaps(device, &caps);
    g_screen_w = caps.screen_width;
    g_screen_h = caps.screen_height;
    
    printf("GPU: %s\n", caps.adapter_name);
    printf("Capabilities:\n");
    if (caps.capabilities & NodGL_CAP_HARDWARE_ACCEL) printf("  - 2D Hardware Acceleration\n");
    if (caps.capabilities & NodGL_CAP_3D_PIPELINE) printf("  - 3D Pipeline\n");
    printf("\n");

    NodGL_Viewport vp = {0, 0, (float)g_screen_w, (float)g_screen_h, 0.0f, 1.0f};
    NodGL_SetViewport(ctx, &vp);

    printf("Rendering spinning cube...\n");

    for (int frame = 0; frame < 360; frame++) {
        float angle = frame * PI / 180.0f * 2.0f;

        /* Clear to dark blue */
        NodGL_ClearContext(ctx, NodGL_CLEAR_COLOR, NodGL_ColorARGB(0xFF, 0, 0, 0x20), 1.0f, 0);

        /* Draw spinning cube */
        draw_spinning_cube(ctx, angle);

        /* Present */
        NodGL_PresentContext(ctx, 1);

        /* Simple frame delay */
        for (volatile int i = 0; i < 50000; i++);
    }

    NodGL_ReleaseDevice(device);
    printf("Demo complete!\n");
    return 0;
}



