#include "libc.h"
#include "string.h"
#include "../include/moduos/kernel/events/events.h"
#include "vinefm_cursor.c"

/* VineFM - VineDE graphical file manager for ModuOS.
 * Responsive layout, mouse + keyboard navigation, basic file operations.
 */

#define VINEFM_MAX_ENTRIES 512
#define VINEFM_NAME_MAX 260
#define VINEFM_PATH_MAX 512
#define VINEFM_STATUS_MAX 256
#define VINEFM_INPUT_MAX 128

#define COLOR_BG 0xFFF2F2F2
#define COLOR_PANEL 0xFFFFFFFF
#define COLOR_HEADER 0xFFE6E6E6
#define COLOR_STATUS 0xFFEDEDED
#define COLOR_TEXT 0xFF202020
#define COLOR_TEXT_DIM 0xFF505050
#define COLOR_SELECT 0xFFBBD7FF
#define COLOR_SELECT_DIM 0xFFA5C2EE
#define COLOR_BORDER 0xFFB0B0B0
#define COLOR_WARN 0xFFFFC107
#define COLOR_ERR 0xFFE53935
#define COLOR_OK 0xFF43A047

#define FONT_W 8
#define FONT_H 8

/* Embedded 8x8 font (ASCII 32..127). Source: public domain font8x8. */
static const uint8_t font8x8_basic[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x18,0x3c,0x3c,0x18,0x18,0x00,0x18,0x00},
    {0x36,0x36,0x12,0x00,0x00,0x00,0x00,0x00}, {0x36,0x36,0x7f,0x36,0x7f,0x36,0x36,0x00},
    {0x0c,0x3e,0x03,0x1e,0x30,0x1f,0x0c,0x00}, {0x00,0x63,0x33,0x18,0x0c,0x66,0x63,0x00},
    {0x1c,0x36,0x1c,0x6e,0x3b,0x33,0x6e,0x00}, {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00},
    {0x18,0x0c,0x06,0x06,0x06,0x0c,0x18,0x00}, {0x06,0x0c,0x18,0x18,0x18,0x0c,0x06,0x00},
    {0x00,0x66,0x3c,0xff,0x3c,0x66,0x00,0x00}, {0x00,0x0c,0x0c,0x3f,0x0c,0x0c,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x0c,0x0c,0x06}, {0x00,0x00,0x00,0x3f,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x0c,0x0c,0x00}, {0x60,0x30,0x18,0x0c,0x06,0x03,0x01,0x00},
    {0x3e,0x63,0x73,0x7b,0x6f,0x67,0x3e,0x00}, {0x0c,0x0e,0x0f,0x0c,0x0c,0x0c,0x3f,0x00},
    {0x1e,0x33,0x30,0x1c,0x06,0x33,0x3f,0x00}, {0x1e,0x33,0x30,0x1c,0x30,0x33,0x1e,0x00},
    {0x38,0x3c,0x36,0x33,0x7f,0x30,0x78,0x00}, {0x3f,0x03,0x1f,0x30,0x30,0x33,0x1e,0x00},
    {0x1c,0x06,0x03,0x1f,0x33,0x33,0x1e,0x00}, {0x3f,0x33,0x30,0x18,0x0c,0x0c,0x0c,0x00},
    {0x1e,0x33,0x33,0x1e,0x33,0x33,0x1e,0x00}, {0x1e,0x33,0x33,0x3e,0x30,0x18,0x0e,0x00},
    {0x00,0x0c,0x0c,0x00,0x00,0x0c,0x0c,0x00}, {0x00,0x0c,0x0c,0x00,0x00,0x0c,0x0c,0x06},
    {0x18,0x0c,0x06,0x03,0x06,0x0c,0x18,0x00}, {0x00,0x00,0x3f,0x00,0x00,0x3f,0x00,0x00},
    {0x06,0x0c,0x18,0x30,0x18,0x0c,0x06,0x00}, {0x1e,0x33,0x30,0x18,0x0c,0x00,0x0c,0x00},
    {0x3e,0x63,0x7b,0x7b,0x7b,0x03,0x1e,0x00}, {0x0c,0x1e,0x33,0x33,0x3f,0x33,0x33,0x00},
    {0x3f,0x66,0x66,0x3e,0x66,0x66,0x3f,0x00}, {0x3c,0x66,0x03,0x03,0x03,0x66,0x3c,0x00},
    {0x1f,0x36,0x66,0x66,0x66,0x36,0x1f,0x00}, {0x7f,0x46,0x16,0x1e,0x16,0x46,0x7f,0x00},
    {0x7f,0x46,0x16,0x1e,0x16,0x06,0x0f,0x00}, {0x3c,0x66,0x03,0x03,0x73,0x66,0x7c,0x00},
    {0x33,0x33,0x33,0x3f,0x33,0x33,0x33,0x00}, {0x1e,0x0c,0x0c,0x0c,0x0c,0x0c,0x1e,0x00},
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1e,0x00}, {0x67,0x66,0x36,0x1e,0x36,0x66,0x67,0x00},
    {0x0f,0x06,0x06,0x06,0x46,0x66,0x7f,0x00}, {0x63,0x77,0x7f,0x7f,0x6b,0x63,0x63,0x00},
    {0x63,0x67,0x6f,0x7b,0x73,0x63,0x63,0x00}, {0x1c,0x36,0x63,0x63,0x63,0x36,0x1c,0x00},
    {0x3f,0x66,0x66,0x3e,0x06,0x06,0x0f,0x00}, {0x1e,0x33,0x33,0x33,0x3b,0x1e,0x38,0x00},
    {0x3f,0x66,0x66,0x3e,0x36,0x66,0x67,0x00}, {0x1e,0x33,0x07,0x0e,0x38,0x33,0x1e,0x00},
    {0x3f,0x2d,0x0c,0x0c,0x0c,0x0c,0x1e,0x00}, {0x33,0x33,0x33,0x33,0x33,0x33,0x3f,0x00},
    {0x33,0x33,0x33,0x33,0x33,0x1e,0x0c,0x00}, {0x63,0x63,0x63,0x6b,0x7f,0x77,0x63,0x00},
    {0x63,0x63,0x36,0x1c,0x1c,0x36,0x63,0x00}, {0x33,0x33,0x33,0x1e,0x0c,0x0c,0x1e,0x00},
    {0x7f,0x63,0x31,0x18,0x4c,0x66,0x7f,0x00}, {0x1e,0x06,0x06,0x06,0x06,0x06,0x1e,0x00},
    {0x03,0x06,0x0c,0x18,0x30,0x60,0x40,0x00}, {0x1e,0x18,0x18,0x18,0x18,0x18,0x1e,0x00},
    {0x08,0x1c,0x36,0x63,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff},
    {0x0c,0x0c,0x18,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x1e,0x30,0x3e,0x33,0x6e,0x00},
    {0x07,0x06,0x06,0x3e,0x66,0x66,0x3b,0x00}, {0x00,0x00,0x1e,0x33,0x03,0x33,0x1e,0x00},
    {0x38,0x30,0x30,0x3e,0x33,0x33,0x6e,0x00}, {0x00,0x00,0x1e,0x33,0x3f,0x03,0x1e,0x00},
    {0x1c,0x36,0x06,0x0f,0x06,0x06,0x0f,0x00}, {0x00,0x00,0x6e,0x33,0x33,0x3e,0x30,0x1f},
    {0x07,0x06,0x36,0x6e,0x66,0x66,0x67,0x00}, {0x0c,0x00,0x0e,0x0c,0x0c,0x0c,0x1e,0x00},
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1e}, {0x07,0x06,0x66,0x36,0x1e,0x36,0x67,0x00},
    {0x0e,0x0c,0x0c,0x0c,0x0c,0x0c,0x1e,0x00}, {0x00,0x00,0x33,0x7f,0x7f,0x6b,0x63,0x00},
    {0x00,0x00,0x1b,0x37,0x33,0x33,0x33,0x00}, {0x00,0x00,0x1e,0x33,0x33,0x33,0x1e,0x00},
    {0x00,0x00,0x3b,0x66,0x66,0x3e,0x06,0x0f}, {0x00,0x00,0x6e,0x33,0x33,0x3e,0x30,0x78},
    {0x00,0x00,0x3b,0x6e,0x66,0x06,0x0f,0x00}, {0x00,0x00,0x3e,0x03,0x1e,0x30,0x1f,0x00},
    {0x08,0x0c,0x3e,0x0c,0x0c,0x2c,0x18,0x00}, {0x00,0x00,0x33,0x33,0x33,0x33,0x6e,0x00},
    {0x00,0x00,0x33,0x33,0x33,0x1e,0x0c,0x00}, {0x00,0x00,0x63,0x6b,0x7f,0x7f,0x36,0x00},
    {0x00,0x00,0x63,0x36,0x1c,0x36,0x63,0x00}, {0x00,0x00,0x33,0x33,0x33,0x3e,0x30,0x1f},
    {0x00,0x00,0x3f,0x19,0x0c,0x00,0x3f,0x00}, {0x38,0x0c,0x0c,0x07,0x0c,0x0c,0x38,0x00},
    {0x0c,0x0c,0x0c,0x00,0x0c,0x0c,0x0c,0x00}, {0x07,0x0c,0x0c,0x38,0x0c,0x0c,0x07,0x00},
    {0x6e,0x3b,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}
};

typedef struct {
    char name[VINEFM_NAME_MAX];
    int is_dir;
    uint32_t size;
} vinefm_entry_t;

typedef struct {
    md64api_grp_video_info_t vi;
    uint32_t fmt;
    uint32_t pitch;
    uint32_t bpp_bytes;
    uint8_t *bb;
    uint32_t bb_size;
    uint32_t bb_pitch;

    cursor_img_t cursor;
    int have_cursor;

    int efd;
    int running;

    int32_t mx;
    int32_t my;
    uint8_t buttons;
    uint8_t prev_buttons;

    char cwd[VINEFM_PATH_MAX];
    vinefm_entry_t entries[VINEFM_MAX_ENTRIES];
    int entry_count;
    int selection;
    int scroll;

    char status[VINEFM_STATUS_MAX];

    char history[32][VINEFM_PATH_MAX];
    int history_count;
    int history_index;

    int modal_active;
    char modal_title[32];
    char modal_input[VINEFM_INPUT_MAX];
    int modal_len;
    int modal_mode; /* 1=rename,2=new_folder,3=copy,4=move */
    char modal_target[VINEFM_PATH_MAX];

    int clipboard_mode; /* 0=none 1=copy 2=cut */
    char clipboard_path[VINEFM_PATH_MAX];
} vinefm_state_t;

static uint32_t pack_xrgb8888(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void put_px(uint8_t *fb, uint32_t pitch, uint32_t w, uint32_t h, int32_t x, int32_t y, uint32_t argb) {
    if (x < 0 || y < 0) return;
    if ((uint32_t)x >= w || (uint32_t)y >= h) return;
    uint32_t *row = (uint32_t *)(fb + (uint64_t)y * pitch);
    row[x] = argb;
}

static void fill_rect(uint8_t *fb, uint32_t pitch, uint32_t w, uint32_t h,
                      int32_t x, int32_t y, uint32_t rw, uint32_t rh, uint32_t argb) {
    if (rw == 0 || rh == 0) return;
    if (x < 0) { rw += x; x = 0; }
    if (y < 0) { rh += y; y = 0; }
    if (x >= (int32_t)w || y >= (int32_t)h) return;
    if (x + (int32_t)rw > (int32_t)w) rw = w - (uint32_t)x;
    if (y + (int32_t)rh > (int32_t)h) rh = h - (uint32_t)y;
    for (uint32_t yy = 0; yy < rh; yy++) {
        uint32_t *row = (uint32_t *)(fb + (uint64_t)(y + (int32_t)yy) * pitch);
        for (uint32_t xx = 0; xx < rw; xx++) {
            row[x + (int32_t)xx] = argb;
        }
    }
}

static void draw_char(uint8_t *fb, uint32_t pitch, uint32_t w, uint32_t h,
                      int32_t x, int32_t y, char ch, uint32_t argb) {
    if (ch < 32 || ch > 127) ch = '?';
    const uint8_t *glyph = font8x8_basic[(int)ch - 32];
    for (int yy = 0; yy < 8; yy++) {
        uint8_t row = glyph[yy];
        for (int xx = 0; xx < 8; xx++) {
            if (row & (1u << xx)) {
                put_px(fb, pitch, w, h, x + xx, y + yy, argb);
            }
        }
    }
}

static void draw_text(uint8_t *fb, uint32_t pitch, uint32_t w, uint32_t h,
                      int32_t x, int32_t y, const char *s, uint32_t argb) {
    if (!s) return;
    int32_t cx = x;
    int32_t cy = y;
    while (*s) {
        char ch = *s++;
        if (ch == '\n') {
            cx = x;
            cy += FONT_H;
            continue;
        }
        draw_char(fb, pitch, w, h, cx, cy, ch, argb);
        cx += FONT_W;
    }
}

static void draw_text_clip(uint8_t *fb, uint32_t pitch, uint32_t w, uint32_t h,
                           int32_t x, int32_t y, const char *s, uint32_t argb,
                           int32_t clip_x, int32_t clip_y, uint32_t clip_w, uint32_t clip_h) {
    if (!s) return;
    int32_t cx = x;
    int32_t cy = y;
    int32_t clip_x2 = clip_x + (int32_t)clip_w;
    int32_t clip_y2 = clip_y + (int32_t)clip_h;
    while (*s) {
        char ch = *s++;
        if (ch == '\n') {
            cx = x;
            cy += FONT_H;
            continue;
        }
        if (cx + FONT_W >= clip_x && cx < clip_x2 && cy + FONT_H >= clip_y && cy < clip_y2) {
            draw_char(fb, pitch, w, h, cx, cy, ch, argb);
        }
        cx += FONT_W;
    }
}

static void format_size(uint32_t size, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    if (size < 1024) {
        snprintf(out, out_sz, "%u B", (unsigned)size);
    } else if (size < 1024 * 1024) {
        snprintf(out, out_sz, "%u KB", (unsigned)(size / 1024));
    } else {
        snprintf(out, out_sz, "%u MB", (unsigned)(size / (1024 * 1024)));
    }
}

static void set_status(vinefm_state_t *s, const char *msg) {
    if (!s) return;
    if (!msg) msg = "";
    strncpy(s->status, msg, sizeof(s->status) - 1);
    s->status[sizeof(s->status) - 1] = 0;
}

static int is_dot_entry(const char *name) {
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

static void path_join(const char *base, const char *name, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = 0;
    if (!base) base = "";
    strncpy(out, base, out_sz - 1);
    out[out_sz - 1] = 0;
    size_t bl = strlen(out);
    if (bl && out[bl - 1] != '/') {
        strncat(out, "/", out_sz - strlen(out) - 1);
    }
    if (name) strncat(out, name, out_sz - strlen(out) - 1);
}

static void normalize_path(char *path) {
    if (!path) return;
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') {
        path[n - 1] = 0;
        n--;
    }
}

static int entry_cmp(const vinefm_entry_t *ea, const vinefm_entry_t *eb) {
    if (ea->is_dir != eb->is_dir) return eb->is_dir - ea->is_dir;
    return strcmp(ea->name, eb->name);
}

static void sort_entries(vinefm_state_t *s) {
    if (!s) return;
    for (int i = 1; i < s->entry_count; i++) {
        vinefm_entry_t key = s->entries[i];
        int j = i - 1;
        while (j >= 0 && entry_cmp(&s->entries[j], &key) > 0) {
            s->entries[j + 1] = s->entries[j];
            j--;
        }
        s->entries[j + 1] = key;
    }
}

static void history_push(vinefm_state_t *s, const char *path) {
    if (!s || !path) return;
    if (s->history_index >= 0 && s->history_index < s->history_count) {
        if (strcmp(s->history[s->history_index], path) == 0) return;
    }
    if (s->history_index + 1 < s->history_count) {
        s->history_count = s->history_index + 1;
    }
    if (s->history_count >= 32) {
        for (int i = 1; i < 32; i++) {
            strncpy(s->history[i - 1], s->history[i], VINEFM_PATH_MAX - 1);
            s->history[i - 1][VINEFM_PATH_MAX - 1] = 0;
        }
        s->history_count = 31;
        s->history_index = 30;
    }
    strncpy(s->history[s->history_count], path, VINEFM_PATH_MAX - 1);
    s->history[s->history_count][VINEFM_PATH_MAX - 1] = 0;
    s->history_count++;
    s->history_index = s->history_count - 1;
}

static int load_dir(vinefm_state_t *s) {
    if (!s) return -1;
    normalize_path(s->cwd);
    history_push(s, s->cwd);
    int fd = opendir(s->cwd);
    if (fd < 0) {
        set_status(s, "Failed to open directory");
        return -1;
    }

    s->entry_count = 0;
    s->selection = 0;
    s->scroll = 0;

    char name[VINEFM_NAME_MAX];
    int is_dir = 0;
    uint32_t size = 0;
    while (s->entry_count < VINEFM_MAX_ENTRIES) {
        int rc = readdir(fd, name, sizeof(name), &is_dir, &size);
        if (rc == 0) break;
        if (rc < 0) break;
        if (is_dot_entry(name)) continue;

        vinefm_entry_t *e = &s->entries[s->entry_count++];
        strncpy(e->name, name, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = 0;
        e->is_dir = is_dir ? 1 : 0;
        e->size = size;
    }
    closedir(fd);

    sort_entries(s);
    return 0;
}

static void open_selection(vinefm_state_t *s) {
    if (!s) return;
    if (s->selection < 0 || s->selection >= s->entry_count) return;

    vinefm_entry_t *e = &s->entries[s->selection];
    char path[VINEFM_PATH_MAX];
    path_join(s->cwd, e->name, path, sizeof(path));

    if (e->is_dir) {
        strncpy(s->cwd, path, sizeof(s->cwd) - 1);
        s->cwd[sizeof(s->cwd) - 1] = 0;
        load_dir(s);
        return;
    }

    char *argv_exec[2] = { (char *)path, NULL };
    if (execve(path, argv_exec, NULL) < 0) {
        set_status(s, "Failed to execute file");
    }
}

static void go_up(vinefm_state_t *s) {
    if (!s) return;
    char *slash = strrchr(s->cwd, '/');
    if (slash && slash != s->cwd) {
        *slash = 0;
    } else if (slash == s->cwd) {
        s->cwd[1] = 0;
    }
    load_dir(s);
}

static int copy_file(const char *src, const char *dst) {
    int in = open(src, O_RDONLY, 0);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); return -2; }

    size_t buf_sz = 64 * 1024;
    char *buf = (char*)malloc(buf_sz);
    if (!buf) { close(in); close(out); return -3; }

    for (;;) {
        ssize_t rd = read(in, buf, buf_sz);
        if (rd == 0) break;
        if (rd < 0) { free(buf); close(in); close(out); return -4; }
        size_t off = 0;
        while (off < (size_t)rd) {
            ssize_t wr = write(out, buf + off, (size_t)rd - off);
            if (wr < 0) { free(buf); close(in); close(out); return -5; }
            off += (size_t)wr;
        }
    }

    free(buf);
    close(in);
    close(out);
    return 0;
}

static void start_modal(vinefm_state_t *s, const char *title, int mode, const char *default_text, const char *target) {
    if (!s) return;
    s->modal_active = 1;
    s->modal_mode = mode;
    strncpy(s->modal_title, title, sizeof(s->modal_title) - 1);
    s->modal_title[sizeof(s->modal_title) - 1] = 0;
    s->modal_len = 0;
    s->modal_input[0] = 0;
    if (default_text) {
        strncpy(s->modal_input, default_text, sizeof(s->modal_input) - 1);
        s->modal_input[sizeof(s->modal_input) - 1] = 0;
        s->modal_len = (int)strlen(s->modal_input);
    }
    if (target) {
        strncpy(s->modal_target, target, sizeof(s->modal_target) - 1);
        s->modal_target[sizeof(s->modal_target) - 1] = 0;
    } else {
        s->modal_target[0] = 0;
    }
}

static void close_modal(vinefm_state_t *s) {
    if (!s) return;
    s->modal_active = 0;
    s->modal_mode = 0;
    s->modal_len = 0;
    s->modal_input[0] = 0;
}

static void modal_commit(vinefm_state_t *s) {
    if (!s) return;
    char path[VINEFM_PATH_MAX];
    if (s->modal_mode == 1) { /* rename */
        path_join(s->cwd, s->modal_input, path, sizeof(path));
        int rc = copy_file(s->modal_target, path);
        if (rc == 0) {
            unlink(s->modal_target);
            set_status(s, "Renamed");
        } else {
            set_status(s, "Rename failed");
        }
    } else if (s->modal_mode == 2) { /* new folder */
        path_join(s->cwd, s->modal_input, path, sizeof(path));
        if (mkdir(path) == 0) set_status(s, "Folder created");
        else set_status(s, "Create folder failed");
    } else if (s->modal_mode == 3) { /* copy */
        path_join(s->cwd, s->modal_input, path, sizeof(path));
        if (copy_file(s->modal_target, path) == 0) set_status(s, "Copied");
        else set_status(s, "Copy failed");
    } else if (s->modal_mode == 4) { /* move */
        path_join(s->cwd, s->modal_input, path, sizeof(path));
        if (copy_file(s->modal_target, path) == 0) {
            unlink(s->modal_target);
            set_status(s, "Moved");
        } else set_status(s, "Move failed");
    }
    close_modal(s);
    load_dir(s);
}

static void delete_selection(vinefm_state_t *s) {
    if (!s) return;
    if (s->selection < 0 || s->selection >= s->entry_count) return;

    vinefm_entry_t *e = &s->entries[s->selection];
    char path[VINEFM_PATH_MAX];
    path_join(s->cwd, e->name, path, sizeof(path));

    int rc = 0;
    if (e->is_dir) {
        rc = rmdir(path);
    } else {
        rc = unlink(path);
    }

    if (rc == 0) {
        set_status(s, "Deleted");
    } else {
        set_status(s, "Delete failed");
    }
    load_dir(s);
}

static int handle_mouse_click(vinefm_state_t *s, int32_t list_x, int32_t list_y,
                              int32_t row_h, int32_t list_w, int32_t list_h) {
    if (!s) return 0;
    int32_t mx = s->mx;
    int32_t my = s->my;
    if (mx < list_x || my < list_y || mx >= list_x + list_w || my >= list_y + list_h) return 0;
    int idx = (my - list_y) / row_h + s->scroll;
    if (idx >= 0 && idx < s->entry_count) {
        s->selection = idx;
        if ((s->buttons & 1u) && !(s->prev_buttons & 1u)) {
            open_selection(s);
        }
        return 1;
    }
    return 0;
}

static void history_back(vinefm_state_t *s) {
    if (!s) return;
    if (s->history_index > 0) {
        s->history_index--;
        strncpy(s->cwd, s->history[s->history_index], sizeof(s->cwd) - 1);
        s->cwd[sizeof(s->cwd) - 1] = 0;
        load_dir(s);
    }
}

static void history_forward(vinefm_state_t *s) {
    if (!s) return;
    if (s->history_index + 1 < s->history_count) {
        s->history_index++;
        strncpy(s->cwd, s->history[s->history_index], sizeof(s->cwd) - 1);
        s->cwd[sizeof(s->cwd) - 1] = 0;
        load_dir(s);
    }
}

static void handle_sidebar_click(vinefm_state_t *s, int32_t list_y, uint32_t row_h) {
    if (!s) return;
    if (!(s->buttons & 1u) || (s->prev_buttons & 1u)) return;

    int32_t mx = s->mx;
    int32_t my = s->my;
    if (mx < 16 || mx >= 160) return;
    int idx = (my - (list_y + 6)) / (int)row_h;
    if (idx == 1) {
        strncpy(s->cwd, "/ModuOS/shared/usr", sizeof(s->cwd) - 1);
        s->cwd[sizeof(s->cwd) - 1] = 0;
        load_dir(s);
    } else if (idx == 2) {
        strncpy(s->cwd, "/", sizeof(s->cwd) - 1);
        s->cwd[sizeof(s->cwd) - 1] = 0;
        load_dir(s);
    } else if (idx == 3) {
        strncpy(s->cwd, "/Apps", sizeof(s->cwd) - 1);
        s->cwd[sizeof(s->cwd) - 1] = 0;
        load_dir(s);
    } else if (idx == 4) {
        strncpy(s->cwd, "/ModuOS/System64", sizeof(s->cwd) - 1);
        s->cwd[sizeof(s->cwd) - 1] = 0;
        load_dir(s);
    }
}

static void handle_toolbar_click(vinefm_state_t *s) {
    if (!s) return;
    if (!(s->buttons & 1u) || (s->prev_buttons & 1u)) return;

    int32_t mx = s->mx;
    int32_t my = s->my;
    if (my >= 6 && my < 6 + (FONT_H + 6)) {
        if (mx >= 8 && mx < 32) {
            history_back(s);
        } else if (mx >= 36 && mx < 60) {
            history_forward(s);
        } else if (mx >= 64 && mx < 88) {
            go_up(s);
        }
    }
}

static void handle_keyboard(vinefm_state_t *s, Event *e) {
    if (!s || !e) return;
    if (e->type == EVENT_KEY_PRESSED) {
        KeyCode key = e->data.keyboard.keycode;
        if (key == KEY_ESCAPE) {
            s->running = 0;
        } else if (key == KEY_ARROW_DOWN) {
            if (s->selection + 1 < s->entry_count) s->selection++;
        } else if (key == KEY_ARROW_UP) {
            if (s->selection > 0) s->selection--;
        } else if (key == KEY_PAGE_DOWN) {
            s->selection += 10;
            if (s->selection >= s->entry_count) s->selection = s->entry_count - 1;
        } else if (key == KEY_PAGE_UP) {
            s->selection -= 10;
            if (s->selection < 0) s->selection = 0;
        } else if (key == KEY_ENTER) {
            open_selection(s);
        } else if (key == KEY_BACKSPACE) {
            go_up(s);
        } else if (key == KEY_DELETE) {
            delete_selection(s);
        } else if (key == KEY_F2) {
            if (s->selection >= 0 && s->selection < s->entry_count) {
                vinefm_entry_t *e = &s->entries[s->selection];
                char src[VINEFM_PATH_MAX];
                path_join(s->cwd, e->name, src, sizeof(src));
                start_modal(s, "Rename", 1, e->name, src);
            }
        } else if (key == KEY_F5) {
            if (s->selection >= 0 && s->selection < s->entry_count) {
                vinefm_entry_t *e = &s->entries[s->selection];
                char src[VINEFM_PATH_MAX];
                path_join(s->cwd, e->name, src, sizeof(src));
                start_modal(s, "Copy to", 3, e->name, src);
            }
        } else if (key == KEY_F6) {
            if (s->selection >= 0 && s->selection < s->entry_count) {
                vinefm_entry_t *e = &s->entries[s->selection];
                char src[VINEFM_PATH_MAX];
                path_join(s->cwd, e->name, src, sizeof(src));
                start_modal(s, "Move to", 4, e->name, src);
            }
        } else if (key == KEY_F7) {
            start_modal(s, "New Folder", 2, "NewFolder", NULL);
        } else if (key == KEY_F5 && (e->data.keyboard.modifiers & MOD_CTRL)) {
            load_dir(s);
        }
    } else if (e->type == EVENT_CHAR_INPUT) {
        char c = e->data.keyboard.ascii;
        if (s->modal_active) {
            if (c == '\b') {
                if (s->modal_len > 0) {
                    s->modal_input[--s->modal_len] = 0;
                }
            } else if (c == '\n' || c == '\r') {
                modal_commit(s);
            } else if (c >= 32 && c < 127) {
                if (s->modal_len + 1 < (int)sizeof(s->modal_input)) {
                    s->modal_input[s->modal_len++] = c;
                    s->modal_input[s->modal_len] = 0;
                }
            }
        }
    }
}

static void update_scroll(vinefm_state_t *s, int visible_rows) {
    if (!s) return;
    if (visible_rows <= 0) return;
    if (s->selection < s->scroll) {
        s->scroll = s->selection;
    } else if (s->selection >= s->scroll + visible_rows) {
        s->scroll = s->selection - visible_rows + 1;
    }
    if (s->scroll < 0) s->scroll = 0;
    if (s->scroll > s->entry_count - visible_rows) {
        s->scroll = s->entry_count - visible_rows;
        if (s->scroll < 0) s->scroll = 0;
    }
}

static void draw_cursor(vinefm_state_t *s) {
    if (!s || !s->bb) return;
    if (s->have_cursor) {
        if (s->fmt == MD64API_GRP_FMT_XRGB8888) {
            alpha_blit_cursor_xrgb8888(s->bb, s->bb_pitch, s->vi.width, s->vi.height, &s->cursor, s->mx, s->my);
        } else if (s->fmt == MD64API_GRP_FMT_RGB565) {
            alpha_blit_cursor_rgb565(s->bb, s->bb_pitch, s->vi.width, s->vi.height, &s->cursor, s->mx, s->my);
        }
    } else {
        int32_t x = s->mx;
        int32_t y = s->my;
        for (int i = -4; i <= 4; i++) {
            put_px(s->bb, s->bb_pitch, s->vi.width, s->vi.height, x + i, y, COLOR_TEXT);
            put_px(s->bb, s->bb_pitch, s->vi.width, s->vi.height, x, y + i, COLOR_TEXT);
        }
    }
}

static void draw_ui(vinefm_state_t *s) {
    if (!s) return;

    uint32_t sw = s->vi.width;
    uint32_t sh = s->vi.height;
    uint32_t toolbar_h = FONT_H + 10;
    uint32_t path_h = FONT_H + 8;
    uint32_t header_h = toolbar_h + path_h + 8;
    uint32_t status_h = FONT_H + 8;
    uint32_t sidebar_w = 160;
    uint32_t list_x = sidebar_w + 16;
    uint32_t list_y = header_h + 8;
    uint32_t list_w = sw - list_x - 16;
    uint32_t list_h = sh - header_h - status_h - 16;
    uint32_t row_h = FONT_H + 6;
    int visible_rows = (int)(list_h / row_h);

    fill_rect(s->bb, s->bb_pitch, sw, sh, 0, 0, sw, sh, COLOR_BG);
    fill_rect(s->bb, s->bb_pitch, sw, sh, 0, 0, sw, header_h, COLOR_HEADER);
    fill_rect(s->bb, s->bb_pitch, sw, sh, 0, sh - status_h, sw, status_h, COLOR_STATUS);

    fill_rect(s->bb, s->bb_pitch, sw, sh, 16, list_y, sidebar_w - 16, list_h, COLOR_PANEL);
    fill_rect(s->bb, s->bb_pitch, sw, sh, list_x, list_y, list_w, list_h, COLOR_PANEL);

    /* Toolbar */
    fill_rect(s->bb, s->bb_pitch, sw, sh, 8, 6, 24, toolbar_h - 4, COLOR_PANEL);
    fill_rect(s->bb, s->bb_pitch, sw, sh, 36, 6, 24, toolbar_h - 4, COLOR_PANEL);
    fill_rect(s->bb, s->bb_pitch, sw, sh, 64, 6, 24, toolbar_h - 4, COLOR_PANEL);
    draw_text(s->bb, s->bb_pitch, sw, sh, 12, 8, "<", COLOR_TEXT);
    draw_text(s->bb, s->bb_pitch, sw, sh, 42, 8, ">", COLOR_TEXT_DIM);
    draw_text(s->bb, s->bb_pitch, sw, sh, 70, 8, "^", COLOR_TEXT);

    draw_text(s->bb, s->bb_pitch, sw, sh, 104, 8, "VineFM", COLOR_TEXT);

    /* Breadcrumb / path bar */
    fill_rect(s->bb, s->bb_pitch, sw, sh, 8, toolbar_h + 6, sw - 16, path_h, COLOR_PANEL);
    draw_text_clip(s->bb, s->bb_pitch, sw, sh, 12, (int32_t)(toolbar_h + 8), s->cwd, COLOR_TEXT,
                   12, (int32_t)(toolbar_h + 8), sw - 24, path_h - 2);

    char status_buf[VINEFM_STATUS_MAX];
    snprintf(status_buf, sizeof(status_buf), "%s", s->status);
    draw_text(s->bb, s->bb_pitch, sw, sh, 16, (int32_t)(sh - status_h + 4), status_buf, COLOR_TEXT);

    update_scroll(s, visible_rows);

    /* Sidebar (Places) */
    draw_text(s->bb, s->bb_pitch, sw, sh, 24, list_y + 6, "Places", COLOR_TEXT_DIM);
    draw_text(s->bb, s->bb_pitch, sw, sh, 24, list_y + 6 + row_h, "Home", COLOR_TEXT);
    draw_text(s->bb, s->bb_pitch, sw, sh, 24, list_y + 6 + row_h * 2, "Root", COLOR_TEXT);
    draw_text(s->bb, s->bb_pitch, sw, sh, 24, list_y + 6 + row_h * 3, "Apps", COLOR_TEXT);
    draw_text(s->bb, s->bb_pitch, sw, sh, 24, list_y + 6 + row_h * 4, "System", COLOR_TEXT);

    /* Details header */
    draw_text(s->bb, s->bb_pitch, sw, sh, list_x + 8, list_y - (int32_t)FONT_H - 2, "Name", COLOR_TEXT_DIM);
    draw_text(s->bb, s->bb_pitch, sw, sh, list_x + (int32_t)list_w - 160, list_y - (int32_t)FONT_H - 2, "Type", COLOR_TEXT_DIM);
    draw_text(s->bb, s->bb_pitch, sw, sh, list_x + (int32_t)list_w - 80, list_y - (int32_t)FONT_H - 2, "Size", COLOR_TEXT_DIM);

    for (int i = 0; i < visible_rows; i++) {
        int idx = s->scroll + i;
        if (idx >= s->entry_count) break;
        vinefm_entry_t *e = &s->entries[idx];
        uint32_t row_y = list_y + (uint32_t)i * row_h;
        uint32_t row_color = (idx == s->selection) ? COLOR_SELECT : COLOR_PANEL;
        fill_rect(s->bb, s->bb_pitch, sw, sh, list_x, row_y, list_w, row_h, row_color);

        char name_buf[VINEFM_NAME_MAX + 4];
        snprintf(name_buf, sizeof(name_buf), "%s%s", e->name, e->is_dir ? "/" : "");
        draw_text_clip(s->bb, s->bb_pitch, sw, sh, list_x + 8, row_y + 3, name_buf,
                       (idx == s->selection) ? COLOR_TEXT : COLOR_TEXT_DIM,
                       list_x + 8, row_y + 2, list_w - 180, row_h - 2);

        const char *type = e->is_dir ? "Folder" : "File";
        draw_text_clip(s->bb, s->bb_pitch, sw, sh, list_x + (int32_t)list_w - 160, row_y + 3, type,
                       (idx == s->selection) ? COLOR_TEXT : COLOR_TEXT_DIM,
                       list_x + (int32_t)list_w - 160, row_y + 2, 72, row_h - 2);

        char size_buf[32];
        format_size(e->size, size_buf, sizeof(size_buf));
        draw_text_clip(s->bb, s->bb_pitch, sw, sh, list_x + (int32_t)list_w - 80, row_y + 3, size_buf,
                       (idx == s->selection) ? COLOR_TEXT : COLOR_TEXT_DIM,
                       list_x + (int32_t)list_w - 80, row_y + 2, 72, row_h - 2);
    }

    if (s->modal_active) {
        uint32_t modal_w = sw / 2;
        uint32_t modal_h = FONT_H * 4 + 16;
        int32_t modal_x = (int32_t)(sw / 2 - modal_w / 2);
        int32_t modal_y = (int32_t)(sh / 2 - modal_h / 2);
        fill_rect(s->bb, s->bb_pitch, sw, sh, modal_x, modal_y, modal_w, modal_h, COLOR_HEADER);
        draw_text(s->bb, s->bb_pitch, sw, sh, modal_x + 8, modal_y + 6, s->modal_title, COLOR_TEXT);
        fill_rect(s->bb, s->bb_pitch, sw, sh, modal_x + 8, modal_y + FONT_H + 10, modal_w - 16, FONT_H + 6, COLOR_PANEL);
        draw_text(s->bb, s->bb_pitch, sw, sh, modal_x + 12, modal_y + FONT_H + 12, s->modal_input, COLOR_TEXT);
        draw_text(s->bb, s->bb_pitch, sw, sh, modal_x + 8, modal_y + FONT_H * 2 + 14,
                  "Enter=OK  Esc=Cancel", COLOR_TEXT_DIM);
    }

    draw_cursor(s);
}

static void poll_events(vinefm_state_t *s) {
    if (!s || s->efd < 0) return;
    for (;;) {
        Event e;
        ssize_t n = read(s->efd, &e, sizeof(e));
        if (n != (ssize_t)sizeof(e)) break;

        if (e.type == EVENT_MOUSE_MOVE) {
            s->mx += e.data.mouse.delta_x;
            s->my += e.data.mouse.delta_y;
            if (e.data.mouse.x || e.data.mouse.y) {
                s->mx = e.data.mouse.x;
                s->my = e.data.mouse.y;
            }
            if (s->mx < 0) s->mx = 0;
            if (s->my < 0) s->my = 0;
            if (s->mx >= (int32_t)s->vi.width) s->mx = (int32_t)s->vi.width - 1;
            if (s->my >= (int32_t)s->vi.height) s->my = (int32_t)s->vi.height - 1;
        } else if (e.type == EVENT_MOUSE_BUTTON) {
            s->buttons = e.data.mouse.buttons;
        }

        if (s->modal_active) {
            if (e.type == EVENT_KEY_PRESSED && e.data.keyboard.keycode == KEY_ESCAPE) {
                close_modal(s);
                continue;
            }
        }

        handle_keyboard(s, &e);
    }
}

static int ensure_gfx_ready(vinefm_state_t *s) {
    if (!s) return -1;
    if (md64api_grp_get_video0_info(&s->vi) != 0) return -1;
    if (s->vi.mode != MD64API_GRP_MODE_GRAPHICS) return -2;

    s->fmt = s->vi.fmt;
    if (s->fmt == MD64API_GRP_FMT_UNKNOWN) {
        if (s->vi.bpp == 32) s->fmt = MD64API_GRP_FMT_XRGB8888;
        else if (s->vi.bpp == 16) s->fmt = MD64API_GRP_FMT_RGB565;
    }
    s->bpp_bytes = (s->fmt == MD64API_GRP_FMT_RGB565) ? 2 : 4;
    s->pitch = s->vi.width * s->bpp_bytes;

    size_t size = (size_t)s->vi.height * s->pitch;
    if (!s->bb || s->bb_size < size) {
        if (s->bb) free(s->bb);
        s->bb = (uint8_t*)malloc(size);
        if (!s->bb) return -3;
        s->bb_size = (uint32_t)size;
        s->bb_pitch = s->pitch;
    }
    return 0;
}

static int init_input(vinefm_state_t *s) {
    s->efd = open("$/dev/input/event0", O_RDONLY | O_NONBLOCK, 0);
    if (s->efd < 0) {
        set_status(s, "No input device");
        return -1;
    }
    return 0;
}

static void render_frame(vinefm_state_t *s) {
    if (!s) return;
    draw_ui(s);
    (void)gfx_blit(s->bb, (uint16_t)s->vi.width, (uint16_t)s->vi.height,
                   0, 0, (uint16_t)s->bb_pitch, (uint16_t)s->fmt);
}

__attribute__((noreturn))
void _start(long argc, char **argv) {
    char **safe_argv = argv;
    if ((uintptr_t)argv >= 0x00007FFF00000000ULL) {
        safe_argv = NULL;
        argc = 0;
    }
    int rc = md_main(argc, safe_argv);
    exit(rc);
}

int md_main(long argc, char **argv) {
    if (!argv) {
        static char *fallback_argv[1] = { (char *)"vinefm" };
        argv = fallback_argv;
        argc = 1;
    }

    static vinefm_state_t state;
    vinefm_state_t *s = &state;
    memset(s, 0, sizeof(*s));
    s->running = 1;
    s->buttons = 0;
    s->prev_buttons = 0;

    strncpy(s->cwd, "/", sizeof(s->cwd) - 1);

    if (ensure_gfx_ready(s) != 0) {
        puts_raw("vinefm: graphics init failed\n");
        return 1;
    }

    init_input(s);
    s->have_cursor = (load_cursor_bmp_rgba8888("/ModuOS/shared/usr/assets/mouse/arrow.bmp", &s->cursor) == 0);
    load_dir(s);
    set_status(s, "Ready");

    while (s->running) {
        poll_events(s);

        uint32_t toolbar_h = FONT_H + 10;
        uint32_t path_h = FONT_H + 8;
        uint32_t header_h = toolbar_h + path_h + 8;
        uint32_t status_h = FONT_H + 8;
        uint32_t sidebar_w = 160;
        uint32_t list_x = sidebar_w + 16;
        uint32_t list_y = header_h + 8;
        uint32_t list_w = s->vi.width - list_x - 16;
        uint32_t list_h = s->vi.height - header_h - status_h - 16;
        uint32_t row_h = FONT_H + 6;
        handle_toolbar_click(s);
        handle_sidebar_click(s, (int32_t)list_y, row_h);
        (void)handle_mouse_click(s, (int32_t)list_x, (int32_t)list_y, (int32_t)row_h, (int32_t)list_w, (int32_t)list_h);

        render_frame(s);
        s->prev_buttons = s->buttons;
        yield();
    }

    if (s->efd >= 0) close(s->efd);
    input_flush();
    return 0;
}


