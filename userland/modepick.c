#include "libc.h"
#include "string.h"

#include "../include/moduos/drivers/graphics/videoctl.h"
#include "../include/moduos/drivers/graphics/gfx_mode.h"

#define VIDEO0_PATH "$/dev/graphics/video0"

static int videoctl_get_modes(gfx_mode_t *out, uint32_t max_modes, uint32_t *out_count) {
    if (!out || max_modes == 0) return -1;
    if (out_count) *out_count = 0;

    int fd = open(VIDEO0_PATH, O_WRONLY, 0);
    if (fd < 0) return -2;

    videoctl_req_t req;
    memset(&req, 0, sizeof(req));
    req.magic = VIDEOCTL_MAGIC;
    req.cmd = VIDEOCTL_CMD_GET_MODES;
    req.out_modes_user = (uint64_t)(uintptr_t)out;
    req.max_modes = max_modes;

    ssize_t wr = write(fd, &req, sizeof(req));
    close(fd);

    if (wr < 0) return -3;
    if ((size_t)wr % sizeof(gfx_mode_t) != 0) return -4;

    uint32_t count = (uint32_t)((size_t)wr / sizeof(gfx_mode_t));
    if (out_count) *out_count = count;
    return 0;
}

static int videoctl_set_mode(uint32_t w, uint32_t h, uint32_t bpp) {
    int fd = open(VIDEO0_PATH, O_WRONLY, 0);
    if (fd < 0) return -1;

    videoctl_req_t req;
    memset(&req, 0, sizeof(req));
    req.magic = VIDEOCTL_MAGIC;
    req.cmd = VIDEOCTL_CMD_SET_MODE;
    req.width = w;
    req.height = h;
    req.bpp = bpp;

    ssize_t wr = write(fd, &req, sizeof(req));
    close(fd);
    return (wr == (ssize_t)sizeof(req)) ? 0 : -2;
}

static void print_modes(const gfx_mode_t *modes, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        printf("%u) %ux%u %ubpp\n",
               (unsigned)i,
               (unsigned)modes[i].width,
               (unsigned)modes[i].height,
               (unsigned)modes[i].bpp);
    }
}

int md_main(long argc, char **argv) {
    (void)argc;
    (void)argv;

    md64api_grp_video_info_t orig;
    memset(&orig, 0, sizeof(orig));
    if (md64api_grp_get_video0_info(&orig) != 0) {
        puts_raw("modepick: failed to read current video0 info\n");
        return 1;
    }

    gfx_mode_t modes[128];
    uint32_t mode_count = 0;
    if (videoctl_get_modes(modes, (uint32_t)(sizeof(modes) / sizeof(modes[0])), &mode_count) != 0 || mode_count == 0) {
        puts_raw("modepick: no mode list available (driver doesn't support enumerate_modes?)\n");
        return 2;
    }

    puts_raw("Available modes:\n");
    print_modes(modes, mode_count);

    printf("\nCurrent: %ux%u %ubpp\n",
           (unsigned)orig.width,
           (unsigned)orig.height,
           (unsigned)orig.bpp);

    puts_raw("\nType the mode number to test (or 'q' to quit): ");

    char *s = input();
    if (!s || s[0] == 0) return 0;
    if (s[0] == 'q') return 0;

    int sel = atoi(s);
    if (sel < 0 || (uint32_t)sel >= mode_count) {
        puts_raw("Invalid selection\n");
        return 3;
    }

    gfx_mode_t chosen = modes[(uint32_t)sel];

    printf("Switching to %ux%u %ubpp...\n",
           (unsigned)chosen.width,
           (unsigned)chosen.height,
           (unsigned)chosen.bpp);

    if (videoctl_set_mode(chosen.width, chosen.height, chosen.bpp) != 0) {
        puts_raw("modepick: set_mode failed\n");
        return 4;
    }

    puts_raw("\nIf you can see this, type 'ok' within 5 seconds to keep this mode.\n");
    puts_raw("Otherwise it will revert automatically.\n\n");

    // poll kbd0 nonblocking for up to 5 seconds
    int kfd = open("$/dev/input/kbd0", O_RDONLY | O_NONBLOCK, 0);
    if (kfd < 0) {
        // If we can't do nonblocking input, fall back to blocking and *don't* auto-revert.
        puts_raw("(warning) could not open kbd0 nonblocking; cannot auto-revert safely\n");
        char *line = input();
        if (line && strcmp(line, "ok") == 0) return 0;
        // best effort revert
        (void)videoctl_set_mode(orig.width, orig.height, orig.bpp);
        return 0;
    }

    char buf[64];
    size_t used = 0;
    int keep = 0;

    const uint64_t start_ms = time_ms();
    const uint64_t deadline_ms = start_ms + 5000ULL;

    int revert_now = 0;

    while (time_ms() < deadline_ms) {
        char ch;
        ssize_t r = read(kfd, &ch, 1);
        if (r == 1) {
            // ESC cancels immediately (safety if the new mode is unusable).
            if ((unsigned char)ch == 0x1B) {
                revert_now = 1;
                break;
            }

            if (ch == '\n' || ch == '\r') {
                buf[used] = 0;
                if (strcmp(buf, "ok") == 0) {
                    keep = 1;
                    break;
                }
                // reset buffer on newline
                used = 0;
                buf[0] = 0;
            } else if (ch == '\b') {
                if (used > 0) used--;
            } else {
                if (used + 1 < sizeof(buf)) {
                    buf[used++] = ch;
                    buf[used] = 0;
                }
            }
        }

        // Don't busy-spin too hard.
        yield();
    }

    close(kfd);

    if (!keep) {
        if (revert_now) {
            puts_raw("Reverting (ESC pressed)...\n");
        } else {
            puts_raw("Reverting...\n");
        }
        (void)videoctl_set_mode(orig.width, orig.height, orig.bpp);
    } else {
        puts_raw("Keeping new mode.\n");
    }

    return 0;
}
