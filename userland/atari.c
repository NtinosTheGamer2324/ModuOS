#include "libc.h"
#include "string.h"
#include "gfx2d.h"
#include "lib_8bit.h"
#include "lib_a2600.h"
#include "../include/moduos/kernel/events/events.h"

#define CORE_ROM_PATH "/appdata/atari/core/maze_craze.bin"

static int poll_input(int efd, int32_t *mx, int32_t *my, uint8_t *btn) {
    if (efd < 0) return 0;
    
    int want_exit = 0;
    
    for (;;) {
        Event e;
        ssize_t n = read(efd, &e, sizeof(e));
        if (n != (ssize_t)sizeof(e)) break;
        
        if (e.type == EVENT_KEY_PRESSED && e.data.keyboard.keycode == KEY_ESCAPE) {
            want_exit = 1;
        } else if (e.type == EVENT_MOUSE_MOVE) {
            *mx += e.data.mouse.delta_x;
            *my += e.data.mouse.delta_y;
            
            /* Clamp to 160x192 */
            if (*mx < 0) *mx = 0;
            if (*mx > 159) *mx = 159;
            if (*my < 0) *my = 0;
            if (*my > 191) *my = 191;
            
            if (e.data.mouse.x || e.data.mouse.y) {
                *mx = e.data.mouse.x * 160 / 640;  /* Scale to native res */
                *my = e.data.mouse.y * 192 / 480;
            }
        } else if (e.type == EVENT_MOUSE_BUTTON) {
            *btn = e.data.mouse.buttons;
        }
    }
    
    return want_exit;
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    gfx2d_t g;
    g.fd = -1;
    int rc = gfx2d_open(&g);
    if (rc != 0) {
        printf("Atari.sqr: gfx2d_open rc=%d\n", rc);
        return 1;
    }

    gfx2d_info_t info;
    memset(&info, 0, sizeof(info));
    rc = gfx2d_get_info(&g, &info);
    if (rc != 0) {
        printf("Atari.sqr: gfx2d_get_info rc=%d\n", rc);
        gfx2d_close(&g);
        return 1;
    }

    uint32_t handle = 0, pitch = 0;
    uint32_t buf_size = info.width * info.height * 4u;
    rc = gfx2d_alloc_buf(&g, buf_size, (uint32_t)MD64API_GRP_FMT_XRGB8888, &handle, &pitch);
    if (rc != 0 || handle == 0) {
        printf("Atari.sqr: alloc_buf rc=%d handle=%u\n", rc, handle);
        gfx2d_close(&g);
        return 1;
    }

    void *bb = NULL;
    uint32_t bb_size = 0, bb_pitch = 0, bb_fmt = 0;
    rc = gfx2d_map_buf(&g, handle, &bb, &bb_size, &bb_pitch, &bb_fmt);
    if (rc != 0 || !bb) {
        printf("Atari.sqr: map_buf rc=%d\n", rc);
        gfx2d_close(&g);
        return 1;
    }

    /* Load ROM */
    int fd = open(CORE_ROM_PATH, O_RDONLY, 0);
    if (fd < 0) {
        printf("Atari.sqr: missing ROM: %s\n", CORE_ROM_PATH);
        gfx2d_close(&g);
        return 2;
    }

    size_t cap = 2 * 1024 * 1024;
    uint8_t *rom = (uint8_t*)malloc(cap);
    if (!rom) { close(fd); gfx2d_close(&g); return 3; }

    ssize_t n = read(fd, rom, cap);
    close(fd);
    if (n <= 0) { free(rom); gfx2d_close(&g); return 4; }

    a2600_t emu;
    rc = a2600_load_cart(&emu, rom, (size_t)n);
    if (rc != 0) {
        printf("Atari.sqr: bad ROM rc=%d size=%d\n", rc, (int)n);
        free(rom);
        gfx2d_close(&g);
        return 5;
    }

    /* Native resolution buffers */
    const uint32_t native_w = 160;
    const uint32_t native_h = 192;

    uint32_t pitch_bytes = bb_pitch ? bb_pitch : (info.width * 4u);
    uint32_t sx = (info.width >= native_w) ? (info.width / native_w) : 1u;
    uint32_t sy = (info.height >= native_h) ? (info.height / native_h) : 1u;
    uint32_t scale = (sx < sy) ? sx : sy;
    if (scale < 1u) scale = 1u;

    uint32_t out_w = native_w * scale;
    uint32_t out_h = native_h * scale;
    uint32_t dst_x = (info.width > out_w) ? ((info.width - out_w) / 2u) : 0u;
    uint32_t dst_y = (info.height > out_h) ? ((info.height - out_h) / 2u) : 0u;

    uint32_t *native = (uint32_t*)malloc((size_t)native_w * (size_t)native_h * 4u);
    if (!native) {
        printf("Atari.sqr: OOM native buffer\n");
        gfx2d_close(&g);
        return 6;
    }

    emu_frame_t frame;
    frame.width = native_w;
    frame.height = native_h;
    frame.pitch_bytes = native_w * 4u;
    frame.fmt = (uint32_t)MD64API_GRP_FMT_XRGB8888;
    frame.pixels = (uint8_t*)native;

    /* Open input device */
    int efd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    if (efd < 0) {
        printf("Atari.sqr: warning: cannot open input (mouse disabled)\n");
    }

    int32_t mouse_x = 80;
    int32_t mouse_y = 96;
    uint8_t mouse_btn = 0;

    printf("Atari.sqr: Running with FULL TIA + RIOT timer + Mouse input!\n");
    printf("  - Move mouse to control joystick\n");
    printf("  - Left click = fire button\n");
    printf("  - Press ESC to exit\n");

    uint32_t frame_no = 0;
    uint64_t next_ms = (uint64_t)time_ms();

    for (;;) {
        /* Poll input */
        if (poll_input(efd, &mouse_x, &mouse_y, &mouse_btn)) break;

        /* Update input state */
        a2600_update_input(&emu, mouse_x, mouse_y, mouse_btn);

        /* Execute one frame */
        a2600_step_frame(&emu);

        if (emu.faulted && frame_no == 0) {
            printf("Atari.sqr: CPU fault at PC=0x%x opcode=0x%x\n",
                   (unsigned)emu.fault_pc, (unsigned)emu.fault_opcode);
        }

        /* Clear backbuffer */
        memset(bb, 0, buf_size);

        /* Render with full TIA features */
        a2600_render_full(&emu, &frame);
        frame_no++;

        /* Scale to screen */
        for (uint32_t y = 0; y < out_h; y++) {
            uint32_t src_y = y / scale;
            uint32_t *dst_row = (uint32_t*)((uint8_t*)bb + (uint64_t)(dst_y + y) * pitch_bytes) + dst_x;
            uint32_t *src_row = (uint32_t*)((uint8_t*)native + (uint64_t)src_y * (uint64_t)(native_w * 4u));
            for (uint32_t x = 0; x < out_w; x++) {
                uint32_t src_x = x / scale;
                dst_row[x] = src_row[src_x];
            }
        }

        (void)gfx2d_blit_buf(&g, handle, 0, 0, 0, 0, info.width, info.height, 
                             pitch_bytes, (uint32_t)MD64API_GRP_FMT_XRGB8888);
        (void)gfx2d_flush(&g, 0, 0, 0, 0);

        /* 50 FPS timing */
        next_ms += 20;
        uint64_t now;
        while ((now = (uint64_t)time_ms()) < next_ms) {
            
        }
        if (now > next_ms + 100) {
            next_ms = now;
        }
    }

    if (efd >= 0) close(efd);
    free(native);
    free(rom);
    gfx2d_close(&g);
    return 0;
}