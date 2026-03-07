// FlareWM - Reference Window Manager for FlareX
// Handles window decorations, placement, stacking, and focus
// Like TWM/IceWM for X11

#define LIBC_NO_START
#include <libc.h>
#include "../lib/libFlareX.h"
#include "../server/xapi_proto.h"

#define MAX_MANAGED_WINDOWS 32

typedef struct {
    uint32_t window_id;
    int16_t x, y;
    uint16_t width, height;
    uint8_t decorated;
    uint8_t focused;
    char title[128];
} wm_window_t;

static wm_window_t g_managed[MAX_MANAGED_WINDOWS];
static int g_running = 1;
static uint32_t g_focused_window = 0;

/* Find managed window */
static wm_window_t *find_managed(uint32_t id) {
    for (int i = 0; i < MAX_MANAGED_WINDOWS; i++) {
        if (g_managed[i].window_id == id) {
            return &g_managed[i];
        }
    }
    return NULL;
}

/* Add window to management */
static void manage_window(uint32_t window_id) {
    /* Find free slot */
    wm_window_t *win = NULL;
    for (int i = 0; i < MAX_MANAGED_WINDOWS; i++) {
        if (g_managed[i].window_id == 0) {
            win = &g_managed[i];
            break;
        }
    }
    
    if (!win) return;
    
    win->window_id = window_id;
    win->decorated = 1;
    win->focused = 0;
    
    printf("[FlareWM] Now managing window %u\n", window_id);
    
    /* Set default decorations */
    xapi_win_set_decorations_t dec;
    memset(&dec, 0, sizeof(dec));
    dec.hdr.type = XAPI_WIN_SET_DECORATIONS;
    dec.hdr.size = sizeof(dec);
    dec.hdr.window_id = window_id;
    
    dec.style.title_bar = 1;
    dec.style.border = 1;
    dec.style.resize_handles = 1;
    dec.style.close_button = 1;
    dec.style.maximize_button = 1;
    dec.style.minimize_button = 1;
    dec.style.title_bg_color = 0xFF2196F3;  /* Material Blue */
    dec.style.title_fg_color = 0xFFFFFFFF;  /* White */
    dec.style.border_color = 0xFF1976D2;    /* Dark Blue */
    dec.style.border_width = 2;
    dec.style.shadow = 1;
    dec.style.shadow_size = 8;
    
    /* TODO: Send to FlareXd via UserFS */
}

/* Remove window from management */
static void unmanage_window(uint32_t window_id) {
    wm_window_t *win = find_managed(window_id);
    if (!win) return;
    
    printf("[FlareWM] Stopped managing window %u\n", window_id);
    memset(win, 0, sizeof(wm_window_t));
}

/* Focus window */
static void focus_window(uint32_t window_id) {
    if (g_focused_window == window_id) return;
    
    /* Unfocus old window */
    if (g_focused_window != 0) {
        wm_window_t *old = find_managed(g_focused_window);
        if (old) {
            old->focused = 0;
            
            /* Update decorations - inactive color */
            xapi_win_set_decorations_t dec;
            memset(&dec, 0, sizeof(dec));
            dec.hdr.type = XAPI_WIN_SET_DECORATIONS;
            dec.hdr.window_id = g_focused_window;
            dec.style.title_bar = 1;
            dec.style.title_bg_color = 0xFF757575;  /* Gray */
            /* TODO: Send to FlareXd */
        }
    }
    
    /* Focus new window */
    wm_window_t *win = find_managed(window_id);
    if (win) {
        win->focused = 1;
        g_focused_window = window_id;
        
        /* Update decorations - active color */
        xapi_win_set_decorations_t dec;
        memset(&dec, 0, sizeof(dec));
        dec.hdr.type = XAPI_WIN_SET_DECORATIONS;
        dec.hdr.window_id = window_id;
        dec.style.title_bar = 1;
        dec.style.title_bg_color = 0xFF2196F3;  /* Blue */
        /* TODO: Send to FlareXd */
        
        /* Raise to top */
        xapi_win_restack_t restack;
        memset(&restack, 0, sizeof(restack));
        restack.hdr.type = XAPI_WIN_RESTACK;
        restack.hdr.window_id = window_id;
        restack.operation = XAPI_STACK_RAISE;
        /* TODO: Send to FlareXd */
        
        printf("[FlareWM] Focused window %u\n", window_id);
    }
}

/* Handle window placement (simple tiling/cascade) */
static void place_window(uint32_t window_id, uint16_t *x, uint16_t *y) {
    /* Simple cascade placement */
    static int cascade_offset = 0;
    
    *x = 50 + (cascade_offset * 30);
    *y = 50 + (cascade_offset * 30);
    
    cascade_offset = (cascade_offset + 1) % 10;
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;
    
    printf("=== FlareWM - Window Manager ===\n");
    
    /* Initialize */
    memset(g_managed, 0, sizeof(g_managed));
    
    /* TODO: Connect to FlareXd via UserFS */
    /* TODO: Subscribe to window events */
    
    printf("[FlareWM] Window manager ready!\n");
    printf("[FlareWM] Waiting for windows to manage...\n");
    
    /* Main event loop */
    while (g_running) {
        /* TODO: Handle events from FlareXd:
         * - XAPI_EVENT_WINDOW_CREATE - manage new window
         * - XAPI_EVENT_WINDOW_DESTROY - unmanage window
         * - XAPI_EVENT_MOUSE_PRESS - focus window
         * - XAPI_EVENT_KEY_PRESS - handle keyboard shortcuts
         */
        
        /* Example keyboard shortcuts:
         * - Alt+F4: Close focused window (send WM_DELETE_WINDOW)
         * - Alt+Tab: Cycle through windows
         * - Alt+F10: Maximize focused window
         * - Alt+F9: Minimize focused window
         */
        
    }
    
    return 0;
}
