// SPDX-License-Identifier: GPL-2.0-only
//
// ntosiux_ttyman.c - NTOSIUX TTY Manager (New Technologies Operating Systen Interface - Unix eXtended, Teletype Manager)
// UserFS-based virtual terminal multiplexer

#include "libc.h"

#define MAX_TTYS 6
#define MAX_TTY_NAME 32

typedef struct {
    char name[MAX_TTY_NAME];
    uint64_t security_key;
    int active;
    
    // UserFS node paths
    char stdin_path[128];
    char stdout_path[128];
    char stderr_path[128];
    
    // Ring buffers for I/O
    uint8_t *stdin_buf;   // Keyboard input waiting to be read by process
    uint8_t *stdout_buf;  // Process output waiting to be displayed
    uint8_t *stderr_buf;  // Process errors waiting to be displayed
    
    uint32_t stdin_r, stdin_w, stdin_count, stdin_cap;
    uint32_t stdout_r, stdout_w, stdout_count, stdout_cap;
    uint32_t stderr_r, stderr_w, stderr_count, stderr_cap;
    
    // Child process PID running on this TTY
    int shell_pid;
} vtty_t;

static vtty_t ttys[MAX_TTYS];
static int current_tty = 0;
static uint64_t next_security_key = 1;
static int kbd_fd = -1;

#define RING_BUF_SIZE 4096

// Ring buffer helpers
static void ring_push(uint8_t *buf, uint32_t *r, uint32_t *w, uint32_t *count, uint32_t cap,
                      const uint8_t *data, size_t len) {
    if (!buf) return;
    for (size_t i = 0; i < len; i++) {
        if (*count >= cap) break;
        buf[*w] = data[i];
        *w = (*w + 1) % cap;
        (*count)++;
    }
}

static size_t ring_pop(uint8_t *buf, uint32_t *r, uint32_t *w, uint32_t *count, uint32_t cap,
                       uint8_t *out, size_t maxlen) {
    if (!buf) return 0;
    size_t n = 0;
    while (n < maxlen && *count > 0) {
        out[n++] = buf[*r];
        *r = (*r + 1) % cap;
        (*count)--;
    }
    return n;
}

// Forward declarations
static int create_vtty(const char *name, uint64_t *out_key);
static int destroy_vtty(const char *name, uint64_t key);
static int find_tty_by_name(const char *name);
static void handle_keyboard_input(void);
static void update_display(void);
static int spawn_shell_on_tty(int tty_index);

// UserFS callbacks (invoked by kernel when processes read/write nodes)
static ssize_t tty_stdin_read(void *ctx, void *buf, size_t count);
static ssize_t tty_stdout_write(void *ctx, const void *buf, size_t count);
static ssize_t tty_stderr_write(void *ctx, const void *buf, size_t count);

int md_main(long argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("[NTOSIUX-TTY] Starting TTY Manager\n");

    // Initialize all TTYs as inactive
    for (int i = 0; i < MAX_TTYS; i++) {
        ttys[i].active = 0;
        ttys[i].shell_pid = -1;
        ttys[i].stdin_buf = NULL;
        ttys[i].stdout_buf = NULL;
        ttys[i].stderr_buf = NULL;
    }

    // Open keyboard device
    kbd_fd = open("$/dev/input/kbd0", 0, 0);
    if (kbd_fd < 0) {
        printf("[NTOSIUX-TTY] Failed to open keyboard device\n");
        return 1;
    }

    // Create default TTYs (tty1-tty6)
    uint64_t key;
    for (int i = 1; i <= MAX_TTYS; i++) {
        char tty_name[16];
        // Simple itoa instead of sprintf
        tty_name[0] = 't';
        tty_name[1] = 't';
        tty_name[2] = 'y';
        tty_name[3] = '0' + (char)i;
        tty_name[4] = '\0';
        
        if (create_vtty(tty_name, &key) < 0) {
            printf("[NTOSIUX-TTY] Failed to create %s\n", tty_name);
            return 1;
        }
        
        printf("[NTOSIUX-TTY] Created %s (key=0x%lx)\n", tty_name, key);
    }

    // Spawn login shell on tty1
    current_tty = 0;
    if (spawn_shell_on_tty(0) < 0) {
        printf("[NTOSIUX-TTY] Failed to spawn shell on tty1\n");
        return 1;
    }

    printf("[NTOSIUX-TTY] TTY Manager ready. Press Alt+F1-F6 to switch TTYs.\n");
    printf("[NTOSIUX-TTY] Current TTY: tty%d\n", current_tty + 1);

    // Main loop
    while (1) {
        // Read keyboard input and route to current TTY's stdin
        handle_keyboard_input();
        
        // Display current TTY's stdout/stderr
        update_display();
        
        // Check for dead shells and respawn
        for (int i = 0; i < MAX_TTYS; i++) {
            if (ttys[i].active && ttys[i].shell_pid > 0) {
                int status = 0;
                int result = waitpid(ttys[i].shell_pid, &status, WNOHANG);
                if (result > 0) {
                    // Shell exited, respawn it
                    printf("[NTOSIUX-TTY] Shell on tty%d exited, respawning...\n", i + 1);
                    ttys[i].shell_pid = -1;
                    spawn_shell_on_tty(i);
                }
            }
        };
    }

    return 0;
}

// UserFS callback: Process reads from stdin (we provide keyboard input)
static ssize_t tty_stdin_read(void *ctx, void *buf, size_t count) {
    vtty_t *tty = (vtty_t *)ctx;
    if (!tty || !tty->active) return 0;
    
    return (ssize_t)ring_pop(tty->stdin_buf, &tty->stdin_r, &tty->stdin_w,
                             &tty->stdin_count, tty->stdin_cap,
                             (uint8_t *)buf, count);
}

// UserFS callback: Process writes to stdout (we capture and display)
static ssize_t tty_stdout_write(void *ctx, const void *buf, size_t count) {
    vtty_t *tty = (vtty_t *)ctx;
    if (!tty || !tty->active) return 0;
    
    ring_push(tty->stdout_buf, &tty->stdout_r, &tty->stdout_w,
              &tty->stdout_count, tty->stdout_cap,
              (const uint8_t *)buf, count);
    return (ssize_t)count;
}

// UserFS callback: Process writes to stderr (we capture and display)
static ssize_t tty_stderr_write(void *ctx, const void *buf, size_t count) {
    vtty_t *tty = (vtty_t *)ctx;
    if (!tty || !tty->active) return 0;
    
    ring_push(tty->stderr_buf, &tty->stderr_r, &tty->stderr_w,
              &tty->stderr_count, tty->stderr_cap,
              (const uint8_t *)buf, count);
    return (ssize_t)count;
}

static int create_vtty(const char *name, uint64_t *out_key) {
    // Find free slot
    int slot = -1;
    for (int i = 0; i < MAX_TTYS; i++) {
        if (!ttys[i].active) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        return -1;  // No free slots
    }
    
    vtty_t *tty = &ttys[slot];
    
    // Initialize TTY
    strncpy(tty->name, name, MAX_TTY_NAME - 1);
    tty->security_key = next_security_key++;
    tty->active = 1;
    tty->shell_pid = -1;
    
    // Allocate ring buffers
    tty->stdin_buf = (uint8_t *)malloc(RING_BUF_SIZE);
    tty->stdout_buf = (uint8_t *)malloc(RING_BUF_SIZE);
    tty->stderr_buf = (uint8_t *)malloc(RING_BUF_SIZE);
    
    if (!tty->stdin_buf || !tty->stdout_buf || !tty->stderr_buf) {
        printf("[NTOSIUX-TTY] Failed to allocate ring buffers\n");
        if (tty->stdin_buf) free(tty->stdin_buf);
        if (tty->stdout_buf) free(tty->stdout_buf);
        if (tty->stderr_buf) free(tty->stderr_buf);
        return -2;
    }
    
    tty->stdin_cap = RING_BUF_SIZE;
    tty->stdout_cap = RING_BUF_SIZE;
    tty->stderr_cap = RING_BUF_SIZE;
    tty->stdin_r = tty->stdin_w = tty->stdin_count = 0;
    tty->stdout_r = tty->stdout_w = tty->stdout_count = 0;
    tty->stderr_r = tty->stderr_w = tty->stderr_count = 0;
    
    // Build UserFS node paths (relative to $/user)
    sprintf(tty->stdin_path, "ttyman/%s/stdin", name);
    sprintf(tty->stdout_path, "ttyman/%s/stdout", name);
    sprintf(tty->stderr_path, "ttyman/%s/stderr", name);
    
    // Register UserFS nodes with callbacks
    // stdin = READ_ONLY for processes (they read keyboard input via tty_stdin_read)
    // stdout = WRITE_ONLY for processes (they write output via tty_stdout_write)
    // stderr = WRITE_ONLY for processes (they write errors via tty_stderr_write)
    userfs_user_node_t stdin_node = {
        .path = tty->stdin_path,
        .owner_id = "ttyman",
        .perms = USERFS_PERM_READ_ONLY,
        .ops = { .read = tty_stdin_read, .write = NULL },
        .ctx = tty
    };
    
    userfs_user_node_t stdout_node = {
        .path = tty->stdout_path,
        .owner_id = "ttyman",
        .perms = USERFS_PERM_WRITE_ONLY,
        .ops = { .read = NULL, .write = tty_stdout_write },
        .ctx = tty
    };
    
    userfs_user_node_t stderr_node = {
        .path = tty->stderr_path,
        .owner_id = "ttyman",
        .perms = USERFS_PERM_WRITE_ONLY,
        .ops = { .read = NULL, .write = tty_stderr_write },
        .ctx = tty
    };
    
    if (userfs_register(&stdin_node) < 0) {
        printf("[NTOSIUX-TTY] Failed to register %s\n", tty->stdin_path);
        free(tty->stdin_buf);
        free(tty->stdout_buf);
        free(tty->stderr_buf);
        return -3;
    }
    
    if (userfs_register(&stdout_node) < 0) {
        printf("[NTOSIUX-TTY] Failed to register %s\n", tty->stdout_path);
        free(tty->stdin_buf);
        free(tty->stdout_buf);
        free(tty->stderr_buf);
        return -4;
    }
    
    if (userfs_register(&stderr_node) < 0) {
        printf("[NTOSIUX-TTY] Failed to register %s\n", tty->stderr_path);
        free(tty->stdin_buf);
        free(tty->stdout_buf);
        free(tty->stderr_buf);
        return -5;
    }
    
    if (out_key) {
        *out_key = tty->security_key;
    }
    
    return slot;
}

static int destroy_vtty(const char *name, uint64_t key) {
    int idx = find_tty_by_name(name);
    if (idx < 0) {
        return -1;  // Not found
    }
    
    vtty_t *tty = &ttys[idx];
    
    // Verify security key
    if (tty->security_key != key) {
        return -2;  // Authentication failed
    }
    
    // Kill shell if running
    if (tty->shell_pid > 0) {
        // Signal the shell to terminate gracefully
        // Since we don't have kill() yet, just wait for it to exit
        int status;
        waitpid(tty->shell_pid, &status, 0);
        tty->shell_pid = -1;
    }
    
    // Unregister UserFS nodes
    // Note: UserFS nodes will be automatically cleaned up when the process exits
    // If userfs_unregister_node() becomes available, call it here
    
    // Mark as inactive
    tty->active = 0;
    
    return 0;
}

static int find_tty_by_name(const char *name) {
    for (int i = 0; i < MAX_TTYS; i++) {
        if (ttys[i].active && strcmp(ttys[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void handle_keyboard_input(void) {
    if (kbd_fd < 0) return;
    
    vtty_t *tty = &ttys[current_tty];
    if (!tty->active || !tty->stdin_buf) return;
    
    // Read from keyboard
    uint8_t buf[256];
    int n = read(kbd_fd, buf, sizeof(buf));
    if (n > 0) {
        // Check for TTY switch hotkeys (Alt+F1-F6)
        for (int i = 0; i < n; i++) {
            // Simple hotkey detection: F1-F6 keys (scan codes)
            // Alt+F1 = 0x3B, Alt+F2 = 0x3C, etc. (simplified detection)
            if (buf[i] >= 0x3B && buf[i] <= 0x40) {
                int new_tty = buf[i] - 0x3B;
                if (new_tty < MAX_TTYS && ttys[new_tty].active) {
                    current_tty = new_tty;
                    printf("[NTOSIUX-TTY] Switched to tty%d\n", current_tty + 1);
                    return; // Don't push hotkey to stdin buffer
                }
            }
        }
        
        // Push keyboard input into stdin ring buffer
        // (processes will read via tty_stdin_read callback)
        ring_push(tty->stdin_buf, &tty->stdin_r, &tty->stdin_w,
                  &tty->stdin_count, tty->stdin_cap, buf, (size_t)n);
    }
}

static void update_display(void) {
    vtty_t *tty = &ttys[current_tty];
    if (!tty->active || !tty->stdout_buf) return;
    
    // Pop from stdout ring buffer (populated by tty_stdout_write callback)
    // and display to physical console
    uint8_t buf[256];
    size_t n = ring_pop(tty->stdout_buf, &tty->stdout_r, &tty->stdout_w,
                        &tty->stdout_count, tty->stdout_cap, buf, sizeof(buf));
    if (n > 0) {
        write(1, buf, n);  // Write to physical console
    }
    
    // Also pop from stderr ring buffer if any
    if (tty->stderr_buf) {
        n = ring_pop(tty->stderr_buf, &tty->stderr_r, &tty->stderr_w,
                     &tty->stderr_count, tty->stderr_cap, buf, sizeof(buf));
        if (n > 0) {
            write(2, buf, n);  // Write errors to physical stderr
        }
    }
}

static int spawn_shell_on_tty(int tty_index) {
    if (tty_index < 0 || tty_index >= MAX_TTYS) {
        return -1;
    }
    
    vtty_t *tty = &ttys[tty_index];
    if (!tty->active) {
        return -2;
    }
    
    int pid = fork();
    if (pid < 0) {
        return -3;
    }
    
    if (pid == 0) {
        // Child process
        
        // Build full paths for UserFS nodes
        char full_stdin[256], full_stdout[256], full_stderr[256];
        sprintf(full_stdin, "$/user/%s", tty->stdin_path);
        sprintf(full_stdout, "$/user/%s", tty->stdout_path);
        sprintf(full_stderr, "$/user/%s", tty->stderr_path);
        
        // Open TTY nodes from process perspective:
        // - stdin is READ_ONLY (process reads keyboard input)
        // - stdout is WRITE_ONLY (process writes output)
        // - stderr is WRITE_ONLY (process writes errors)
        int stdin_fd = open(full_stdin, 0, 0);   // Read from stdin node
        int stdout_fd = open(full_stdout, 1, 0);  // Write to stdout node
        int stderr_fd = open(full_stderr, 1, 0);  // Write to stderr node
        
        if (stdin_fd < 0 || stdout_fd < 0 || stderr_fd < 0) {
            printf("[NTOSIUX-TTY] Child failed to open TTY nodes: stdin=%d stdout=%d stderr=%d\n",
                   stdin_fd, stdout_fd, stderr_fd);
            exit(1);
        }
        
        // Dup to FDs 0, 1, 2
        if (stdin_fd != 0) dup2(stdin_fd, 0);
        if (stdout_fd != 1) dup2(stdout_fd, 1);
        if (stderr_fd != 2) dup2(stderr_fd, 2);
        
        // Close original FDs if > 2
        if (stdin_fd > 2) close(stdin_fd);
        if (stdout_fd > 2) close(stdout_fd);
        if (stderr_fd > 2) close(stderr_fd);
        
        // Exec shell
        char *argv[] = { "/ModuOS/System64/sh.sqr", NULL };
        char *env[] = {
            "PATH=/ModuOS/System64:/ModuOS/System64/sutils",
            "HOME=/",
            "TERM=ntosiux",
            NULL
        };
        
        execve("/ModuOS/System64/sh.sqr", argv, env);
        
        // If we get here, exec failed
        printf("[NTOSIUX-TTY] execve failed\n");
        exit(1);
    }
    
    // Parent process
    tty->shell_pid = pid;
    printf("[NTOSIUX-TTY] Spawned shell PID %d on %s\n", pid, tty->name);
    return pid;
}
