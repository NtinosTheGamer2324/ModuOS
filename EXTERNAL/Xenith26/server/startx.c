#include "libc.h"
#include "string.h"
#include "userfs.h"
#include "nodes.h"
#include "xapi_proto.h"

/* Display server state */
typedef struct {
    int fb_fd;                  /* $/dev/graphics/video0 */
    uint32_t *fb;               /* Framebuffer pointer */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t bpp;
} xserver_display_t;

typedef struct {
    uint32_t id;
    uint32_t owner_pid;
    int16_t x, y;
    uint16_t width, height;
    uint8_t type;               /* xapi_win_type_t */
    uint8_t mapped;             /* Visible? */
    char title[128];
    uint32_t *buffer;           /* Window contents */
} xserver_window_t;

#define MAX_WINDOWS 64
static xserver_window_t g_windows[MAX_WINDOWS];
static uint32_t g_next_window_id = 1;
static xserver_display_t g_display;

/* Initialize framebuffer */
static int init_display() {
    printf("[xenith26d] Opening graphics device...\n");
    
    g_display.fb_fd = open("$/dev/graphics/video0", O_RDWR);
    if (g_display.fb_fd < 0) {
        printf("[xenith26d] ERROR: Could not open $/dev/graphics/video0\n");
        return -1;
    }
    
    /* Read video info */
    uint8_t info_buf[64];
    ssize_t n = read(g_display.fb_fd, info_buf, sizeof(info_buf));
    if (n < (ssize_t)sizeof(md64api_grp_video_info_t)) {
        printf("[xenith26d] ERROR: Could not read video info\n");
        close(g_display.fb_fd);
        return -1;
    }
    
    md64api_grp_video_info_t *info = (md64api_grp_video_info_t *)info_buf;
    
    if (info->mode != MD64API_GRP_MODE_GRAPHICS) {
        printf("[xenith26d] ERROR: Not in graphics mode\n");
        close(g_display.fb_fd);
        return -1;
    }
    
    g_display.fb = (uint32_t *)info->fb_addr;
    g_display.width = info->width;
    g_display.height = info->height;
    g_display.pitch = info->pitch;
    g_display.bpp = info->bpp;
    
    printf("[xenith26d] Display initialized: %ux%u @ %u bpp\n", 
           g_display.width, g_display.height, g_display.bpp);
    printf("[xenith26d] Framebuffer at: 0x%lx\n", (uint64_t)g_display.fb);
    
    /* Clear screen to dark blue */
    for (uint32_t y = 0; y < g_display.height; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)g_display.fb + y * g_display.pitch);
        for (uint32_t x = 0; x < g_display.width; x++) {
            row[x] = 0xFF1A1A2E; /* Dark blue background */
        }
    }
    
    return 0;
}

/* Find free window slot */
static xserver_window_t *alloc_window() {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_windows[i].id == 0) {
            memset(&g_windows[i], 0, sizeof(xserver_window_t));
            g_windows[i].id = g_next_window_id++;
            return &g_windows[i];
        }
    }
    return NULL;
}

/* Find window by ID */
static xserver_window_t *find_window(uint32_t id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_windows[i].id == id) {
            return &g_windows[i];
        }
    }
    return NULL;
}

/* Draw rectangle to framebuffer */
static void draw_rect(int16_t x, int16_t y, uint16_t w, uint16_t h, uint32_t color) {
    if (x < 0 || y < 0) return;
    if (x + w > g_display.width) w = g_display.width - x;
    if (y + h > g_display.height) h = g_display.height - y;
    
    for (uint16_t dy = 0; dy < h; dy++) {
        uint32_t *row = (uint32_t *)((uint8_t *)g_display.fb + (y + dy) * g_display.pitch);
        for (uint16_t dx = 0; dx < w; dx++) {
            row[x + dx] = color;
        }
    }
}

/* Composite window to framebuffer */
static void composite_window(xserver_window_t *win) {
    if (!win->mapped || !win->buffer) return;
    
    for (uint16_t dy = 0; dy < win->height; dy++) {
        int16_t screen_y = win->y + dy;
        if (screen_y < 0 || screen_y >= (int16_t)g_display.height) continue;
        
        uint32_t *fb_row = (uint32_t *)((uint8_t *)g_display.fb + screen_y * g_display.pitch);
        uint32_t *win_row = win->buffer + dy * win->width;
        
        for (uint16_t dx = 0; dx < win->width; dx++) {
            int16_t screen_x = win->x + dx;
            if (screen_x < 0 || screen_x >= (int16_t)g_display.width) continue;
            
            fb_row[screen_x] = win_row[dx];
        }
    }
}

/* Handle window create */
static void handle_create_window(const xapi_win_create_t *msg, uint32_t client_pid) {
    printf("[xenith26d] CREATE_WINDOW from PID %u: %ux%u\n", 
           client_pid, msg->width, msg->height);
    
    xserver_window_t *win = alloc_window();
    if (!win) {
        printf("[xenith26d] ERROR: No free window slots\n");
        return;
    }
    
    win->owner_pid = client_pid;
    win->width = msg->width;
    win->height = msg->height;
    win->type = msg->win_type;
    win->x = 100;  /* Default position */
    win->y = 100;
    win->mapped = 0;
    
    /* Allocate window buffer */
    size_t buf_size = msg->width * msg->height * 4;
    win->buffer = (uint32_t *)malloc(buf_size);
    if (!win->buffer) {
        printf("[xenith26d] ERROR: Could not allocate window buffer\n");
        win->id = 0;
        return;
    }
    
    /* Clear to white */
    for (uint32_t i = 0; i < msg->width * msg->height; i++) {
        win->buffer[i] = 0xFFFFFFFF;
    }
    
    printf("[xenith26d] Created window ID=%u\n", win->id);
}

/* Handle window map */
static void handle_map_window(const xapi_win_geometry_t *msg) {
    xserver_window_t *win = find_window(msg->hdr.window_id);
    if (!win) {
        printf("[xenith26d] ERROR: Window %u not found\n", msg->hdr.window_id);
        return;
    }
    
    win->x = msg->x;
    win->y = msg->y;
    win->mapped = 1;
    
    printf("[xenith26d] Mapped window %u at (%d, %d)\n", 
           win->id, win->x, win->y);
    
    composite_window(win);
}

/* Handle draw rectangle */
static void handle_draw_rect(const xapi_cmd_rect_t *msg) {
    xserver_window_t *win = find_window(msg->hdr.window_id);
    if (!win || !win->buffer) return;
    
    /* Draw to window buffer */
    for (int16_t dy = 0; dy < msg->h; dy++) {
        if (msg->y + dy < 0 || msg->y + dy >= win->height) continue;
        uint32_t *row = win->buffer + (msg->y + dy) * win->width;
        
        for (int16_t dx = 0; dx < msg->w; dx++) {
            if (msg->x + dx < 0 || msg->x + dx >= win->width) continue;
            row[msg->x + dx] = msg->color;
        }
    }
}

/* Handle commit (present window) */
static void handle_commit(const xapi_cmd_commit_t *msg) {
    xserver_window_t *win = find_window(msg->hdr.window_id);
    if (!win) return;
    
    composite_window(win);
}

/* Process message from userfs node */
static void process_message(const void *buf, size_t len, uint32_t client_pid) {
    if (len < sizeof(xapi_msg_hdr_t)) return;
    
    const xapi_msg_hdr_t *hdr = (const xapi_msg_hdr_t *)buf;
    
    switch (hdr->type) {
        case XAPI_WIN_CREATE:
            handle_create_window((const xapi_win_create_t *)buf, client_pid);
            break;
        case XAPI_WIN_MAP:
            handle_map_window((const xapi_win_geometry_t *)buf);
            break;
        case XAPI_CMD_RECT:
            handle_draw_rect((const xapi_cmd_rect_t *)buf);
            break;
        case XAPI_CMD_COMMIT:
            handle_commit((const xapi_cmd_commit_t *)buf);
            break;
        default:
            printf("[xenith26d] Unknown message type: %u\n", hdr->type);
            break;
    }
}

/* Initialize UserFS nodes */
static int init_userfs_nodes() {
    printf("[xenith26d] Registering UserFS nodes...\n");
    
    if (userfs_register_node(NODE_GFX) < 0) {
        printf("[xenith26d] ERROR: Could not register %s\n", NODE_DEV_GFX);
        return -1;
    }
    
    if (userfs_register_node(NODE_EVENT) < 0) {
        printf("[xenith26d] ERROR: Could not register %s\n", NODE_DEV_EVENT);
        return -1;
    }
    
    if (userfs_register_node(NODE_WINDOWS) < 0) {
        printf("[xenith26d] ERROR: Could not register %s\n", NODE_DEV_WINDOWS);
        return -1;
    }
    
    printf("[xenith26d] UserFS nodes registered successfully\n");
    return 0;
}

int md_main(long argc, char** argv) {
    (void)argc; (void)argv;

    printf("=== Xenith26 Display Server ===\n");
    
    /* Initialize display */
    if (init_display() < 0) {
        return 1;
    }
    
    /* Initialize UserFS communication */
    if (init_userfs_nodes() < 0) {
        close(g_display.fb_fd);
        return 1;
    }
    
    printf("[xenith26d] Server ready and waiting for clients...\n");
    
    /* Main event loop */
    uint8_t msg_buf[4096];
    while (1) {
        /* Poll for messages from clients */
        /* TODO: Implement actual message reading from UserFS nodes */
        /* For now, just sleep */
        sleep(1);
    }
    
    return 0;
}