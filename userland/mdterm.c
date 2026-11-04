/*
 * mdterm.c — terminal emulator client for mdw, backed by a real child
 * process (zenith5.1.sqr) over pipes. No PTY/line-discipline layer exists
 * on ModuOS yet, so this is plain pipe I/O: stdin/stdout are redirected
 * onto pipe ends via dup2() before execve(). Good enough for a
 * line-oriented shell; no raw-mode/job-control semantics.
 *
 * Copyright © 2026 New Technologies Software — GPL v2.0
 */

#include "libc.h"
#include "wm_protocol.h"
#include "lib_fnt.h"

#define FONT_PATH   "/ModuOS/shared/assets/fonts/Terminus.fnt"
#define SHELL_PATH  "/Apps/zenith5.1.sqr"
#define CELL_W      8
#define CELL_H      16
#define TERM_COLS   80
#define TERM_ROWS   25
#define WIN_W       (TERM_COLS * CELL_W)
#define WIN_H       (TERM_ROWS * CELL_H)

#define COL_BG      0xFF0C0F14
#define COL_FG      0xFFD8DEEA
#define COL_CURSOR  0xFF2F6FE0

/* fcntl commands (see include/moduos/fs/fd.h) */
#define F_GETFL 3
#define F_SETFL 4

static void copy_str_local(char *dst, size_t dst_size, const char *src) {
    if (!dst_size) return;
    size_t i = 0;
    if (src) while (src[i] && i + 1 < dst_size) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

typedef struct { char ch; } term_cell_t;

static term_cell_t g_grid[TERM_ROWS][TERM_COLS];
static int g_cur_row = 0, g_cur_col = 0;

static wm_shm_t     *g_mailbox;
static uint32_t      *g_pixels;
static wm_key_ring_t *g_keys;
static int32_t        g_win_id;
static fnt_font_t    *g_font;

/* Child shell process + pipe fds (parent/mdterm side) */
static int g_child_pid = -1;
static int g_to_child_write = -1;   /* mdterm writes here -> child's stdin  */
static int g_from_child_read = -1;  /* mdterm reads here <- child's stdout  */

static int g_should_quit = 0;

/* ---------------- terminal grid ---------------- */

static void term_clear(void) {
    for (int r = 0; r < TERM_ROWS; r++)
        for (int c = 0; c < TERM_COLS; c++)
            g_grid[r][c].ch = ' ';
    g_cur_row = 0; g_cur_col = 0;
}

static void term_scroll(void) {
    for (int r = 1; r < TERM_ROWS; r++)
        for (int c = 0; c < TERM_COLS; c++)
            g_grid[r - 1][c] = g_grid[r][c];
    for (int c = 0; c < TERM_COLS; c++)
        g_grid[TERM_ROWS - 1][c].ch = ' ';
}

static void term_newline(void) {
    g_cur_col = 0;
    g_cur_row++;
    if (g_cur_row >= TERM_ROWS) { term_scroll(); g_cur_row = TERM_ROWS - 1; }
}

static void term_putc(char c) {
    if (c == '\n') { term_newline(); return; }
    if (c == '\r') return; /* normalize CRLF from the child, if any */
    if (c == '\b') {
        if (g_cur_col > 0) { g_cur_col--; g_grid[g_cur_row][g_cur_col].ch = ' '; }
        return;
    }
    g_grid[g_cur_row][g_cur_col].ch = c;
    g_cur_col++;
    if (g_cur_col >= TERM_COLS) term_newline();
}

static void term_puts(const char *s) { while (*s) term_putc(*s++); }

static void term_render_and_send_damage(void) {
    for (int i = 0; i < WIN_W * WIN_H; i++) g_pixels[i] = COL_BG;

    for (int r = 0; r < TERM_ROWS; r++) {
        for (int c = 0; c < TERM_COLS; c++) {
            char ch = g_grid[r][c].ch;
            if (ch == ' ' || !g_font) continue;
            fnt_glyph_t *gl = fnt_get_glyph(g_font, (uint32_t)(unsigned char)ch);
            if (!gl) continue;
            int px0 = c * CELL_W, py0 = r * CELL_H;
            for (int dy = 0; dy < gl->bitmap_height && dy < CELL_H; dy++)
                for (int dx = 0; dx < gl->bitmap_width && dx < CELL_W; dx++) {
                    if (!fnt_get_pixel(gl, dx, dy)) continue;
                    int px = px0 + dx, py = py0 + dy;
                    if (px >= 0 && px < WIN_W && py >= 0 && py < WIN_H)
                        g_pixels[py * WIN_W + px] = COL_FG;
                }
        }
    }

    int cx0 = g_cur_col * CELL_W, cy0 = g_cur_row * CELL_H;
    for (int dy = CELL_H - 2; dy < CELL_H; dy++)
        for (int dx = 0; dx < CELL_W; dx++) {
            int px = cx0 + dx, py = cy0 + dy;
            if (px >= 0 && px < WIN_W && py >= 0 && py < WIN_H)
                g_pixels[py * WIN_W + px] = COL_CURSOR;
        }

    wm_request_t dmg; memset(&dmg, 0, sizeof(dmg));
    dmg.cmd = WM_CMD_DAMAGE;
    dmg.win_id = g_win_id;
    dmg.x = 0; dmg.y = 0; dmg.w = WIN_W; dmg.h = WIN_H;
    wm_response_t resp;
    wm_ipc_call(g_mailbox, &dmg, &resp, 2000);
}

/* ---------------- child shell (pipes) ---------------- */

/* Spawns SHELL_PATH with its stdin/stdout redirected onto pipe ends.
 * Two pipes:
 *   in_pipe[2]  : mdterm writes in_pipe[1]  -> child reads in_pipe[0] (its stdin)
 *   out_pipe[2] : child writes out_pipe[1]  -> mdterm reads out_pipe[0] (its stdout)
 * In the child: dup2 the read end of in_pipe onto fd 0, the write end of
 * out_pipe onto fd 1 (and 2), close everything else, then execve. Parent
 * closes the ends it doesn't use and keeps the other two for the I/O loop. */
static int spawn_shell(void) {
    int in_pipe[2]  = { -1, -1 };
    int out_pipe[2] = { -1, -1 };

    if (pipe(in_pipe) != 0)  { printf("mdterm: pipe(in) failed\n");  return -1; }
    if (pipe(out_pipe) != 0) { printf("mdterm: pipe(out) failed\n"); close(in_pipe[0]); close(in_pipe[1]); return -1; }

    int pid = fork();
    if (pid < 0) {
        printf("mdterm: fork failed\n");
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        /* child */
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);

        /* originals no longer needed once dup2'd onto 0/1/2 */
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);

        char *argv[] = { (char *)SHELL_PATH, NULL };
        char *envp[] = { NULL };
        execve(SHELL_PATH, argv, envp);

        /* execve only returns on failure */
        puts("mdterm: execve(zenith5.1.sqr) failed\n");
        exit(127);
    }

    /* parent */
    close(in_pipe[0]);   /* we don't read our own input pipe */
    close(out_pipe[1]);  /* we don't write the child's output pipe */

    g_child_pid = pid;
    g_to_child_write = in_pipe[1];
    g_from_child_read = out_pipe[0];

    /* Make the read side non-blocking so pump_child_output() never stalls
     * the loop waiting on a shell that has nothing to say yet. Confirmed
     * against fd_fcntl() in src/fs/fd.c: F_SETFL only acts on the
     * O_NONBLOCK bit (0x0800), everything else is ignored, so this is
     * exactly the supported path. */
    if (fcntl(g_from_child_read, F_SETFL, O_NONBLOCK) < 0) {
        printf("mdterm: warning: couldn't set O_NONBLOCK on shell stdout pipe\n");
    }

    return 0;
}

/* Drain whatever the child has written since last poll. g_from_child_read
 * is O_NONBLOCK (set in spawn_shell), so read() returns immediately with
 * <=0 when there's nothing pending instead of blocking the loop. */
static int pump_child_output(void) {
    char buf[256];
    int got_any = 0;
    for (;;) {
        ssize_t n = read(g_from_child_read, buf, sizeof(buf));
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) term_putc(buf[i]);
        got_any = 1;
    }
    return got_any;
}

static int child_is_alive(void) {
    if (g_child_pid < 0) return 0;
    int status = 0;
    int r = waitpid(g_child_pid, &status, WNOHANG);
    if (r == g_child_pid) { g_child_pid = -1; return 0; } /* reaped: exited */
    if (r < 0) return (g_child_pid >= 0);                 /* WNOHANG "no change yet" on some impls returns -1 */
    return 1;
}

/* ---------------- keyboard -> child stdin ---------------- */

#define LINE_BUF_SIZE 256
static char g_line_buf[LINE_BUF_SIZE];
static int  g_line_len = 0;

static void term_handle_key(const KeyboardEventData *k) {
    if (k->keycode == KEY_ENTER) {
        term_putc('\n');
        g_line_buf[g_line_len] = '\n';
        write(g_to_child_write, g_line_buf, (size_t)g_line_len + 1);
        g_line_len = 0;
        return;
    }
    if (k->keycode == KEY_BACKSPACE) {
        if (g_line_len > 0) { g_line_len--; term_putc('\b'); }
        return;
    }
    if (k->ascii >= 32 && k->ascii < 127 && g_line_len < LINE_BUF_SIZE - 1) {
        g_line_buf[g_line_len++] = k->ascii;
        term_putc(k->ascii);
    }
}

/* ---------------- entry point ---------------- */

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    g_font = NULL;
    int fd = open(FONT_PATH, O_RDONLY, 0);
    if (fd >= 0) {
        long fsz = lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
        if (fsz > 0 && fsz < 4 * 1024 * 1024) {
            void *data = malloc((size_t)fsz);
            if (data) {
                size_t got = 0;
                while (got < (size_t)fsz) {
                    ssize_t r = read(fd, (uint8_t *)data + got, (size_t)fsz - got);
                    if (r <= 0) break;
                    got += (size_t)r;
                }
                if (got == (size_t)fsz) g_font = fnt_load_font(data, got);
                free(data);
            }
        }
        close(fd);
    }
    if (!g_font) printf("mdterm: warning: couldn't load font, glyphs disabled\n");

    g_mailbox = wm_ipc_connect(3000);
    if (!g_mailbox) { printf("mdterm: couldn't connect to mdw (is it running?)\n"); return 1; }

    wm_request_t req; memset(&req, 0, sizeof(req));
    req.cmd = WM_CMD_CREATE;
    req.w = WIN_W; req.h = WIN_H;
    req.flags = 0;
    req.pid = (uint32_t)getpid();
    copy_str_local(req.title, sizeof(req.title), "Terminal");

    wm_response_t resp;
    if (wm_ipc_call(g_mailbox, &req, &resp, 3000) != 0 || !resp.success) {
        printf("mdterm: WM_CMD_CREATE failed: %s\n", resp.message[0] ? resp.message : "(no response)");
        return 1;
    }
    g_win_id = resp.win_id;

    int pixel_fd = shm_open(resp.shm_name, O_RDWR, 0, 0);
    if (pixel_fd < 0) { printf("mdterm: shm_open(pixels) failed\n"); return 1; }
    g_pixels = (uint32_t *)mmap(NULL, resp.shm_size, PROT_R | PROT_W, MAP_SHARED, pixel_fd);
    if (g_pixels == MAP_FAILED) { printf("mdterm: mmap(pixels) failed\n"); return 1; }

    int key_fd = shm_open(resp.key_shm_name, O_RDWR, 0, 0);
    if (key_fd < 0) { printf("mdterm: shm_open(keys) failed\n"); return 1; }
    g_keys = (wm_key_ring_t *)mmap(NULL, resp.key_shm_size, PROT_R | PROT_W, MAP_SHARED, key_fd);
    if (g_keys == MAP_FAILED) { printf("mdterm: mmap(keys) failed\n"); return 1; }

    term_clear();
    term_puts("mdterm: launching " SHELL_PATH "...\n");
    term_render_and_send_damage();

    if (spawn_shell() != 0) {
        term_puts("mdterm: failed to start shell\n");
        term_render_and_send_damage();
    }

    while (!g_should_quit) {
        int dirty = 0;

        KeyboardEventData k;
        while (wm_key_ring_pop(g_keys, &k)) { term_handle_key(&k); dirty = 1; }

        if (g_child_pid >= 0) {
            if (pump_child_output()) dirty = 1;
            if (!child_is_alive()) {
                term_puts("\n[shell exited]\n");
                dirty = 1;
            }
        }

        if (dirty) term_render_and_send_damage();
        yield();
    }

    if (g_to_child_write >= 0) close(g_to_child_write);
    if (g_from_child_read >= 0) close(g_from_child_read);
    if (g_child_pid >= 0) kill(g_child_pid, SIGTERM);

    wm_request_t close_req; memset(&close_req, 0, sizeof(close_req));
    close_req.cmd = WM_CMD_CLOSE;
    close_req.win_id = g_win_id;
    wm_response_t close_resp;
    wm_ipc_call(g_mailbox, &close_req, &close_resp, 2000);

    munmap(g_keys, resp.key_shm_size);
    munmap(g_pixels, resp.shm_size);
    return 0;
}