/*
 * mdparted_gui.c — mdparted, ported from the REPL to an mdw window.
 *
 * Same engine as the console tool: it reads the drive KV blob out of
 * $/dev/vDriveN and edits MBR partition entries through raw
 * vdrive_read_sector()/vdrive_write_sector(). Nothing about the on-disk
 * behaviour changed. What changed is the front end -- instead of a
 * prompt loop it's an mdw client that:
 *   - draws itself into its SHM pixel buffer,
 *   - takes clicks from the per-window mouse ring (wm_mouse_ring_pop),
 *   - takes typing from the per-window key ring (wm_key_ring_pop),
 *   - reports damage when something actually changed.
 *
 * Destructive actions (Delete, MkLabel) still require a typed "yes",
 * exactly as the console version did: a single mis-click must never be
 * able to rewrite a partition table.
 *
 * As in the console tool, formatting (mkfs) is deliberately NOT here.
 *
 * Note on Tab: mdw consumes Tab for window cycling, so it never reaches
 * a client -- use the mouse to move focus.
 *
 * Usage:
 *   mdparted_gui                 → enumerates $/dev/, opens the first drive
 *   mdparted_gui $/dev/vDrive1   → opens that drive directly
 */

#include "libc/libc.h"
#include "libc/string.h"
#include "lib_fnt.h"
#include "wm_protocol.h"
#include "wm_client_resize.h"
#include <stdint.h>

#define FONT_PRIMARY  "/ModuOS/shared/assets/fonts/Terminus.fnt"
#define FONT_FALLBACK "/ModuOS/shared/assets/fonts/Unicode.fnt"

/* -- Limits ---------------------------------------------------------- */
#define MAX_PARTS     4
#define MAX_DRIVES    16
#define SECTOR_SIZE   512

/* -- Window geometry ------------------------------------------------- */
#define WIN_W   620
#define WIN_H   430

#define PAD           12
#define HDR_Y         10
#define HDR_LINE2_Y   26
#define TABLE_HDR_Y   52
#define TABLE_Y       70
#define ROW_H         18
#define BTN_ROW1_Y    154
#define BTN_ROW2_Y    182
#define BTN_W         92
#define BTN_H         24
#define BTN_GAP       8
#define PROMPT_Y      218
#define FIELD_Y       234
#define FIELD_H       20
#define LOG_Y         266
#define LOG_LINE_H    14
#define LOG_LINES     10

/* -- Palette (0xAARRGGBB, all opaque: window is created without
 *    WM_FLAG_HAS_ALPHA so mdw takes its memcpy blit path) ----------- */
#define C_BG                0xFF1B1F27u
#define C_ROW               0xFF232935u
#define C_ROW_SEL           0xFF2F6FE0u
#define C_ROW_HOVER         0xFF2C3446u
#define C_TEXT              0xFFDCE2EEu
#define C_TEXT_DIM          0xFF8C94A6u
#define C_ACCENT            0xFF6FB2FFu
#define C_OK                0xFF5FD37Fu
#define C_WARN              0xFFE8C468u
#define C_ERR               0xFFE0483Fu
#define C_BTN               0xFF303848u
#define C_BTN_HOVER         0xFF3D4759u
#define C_BTN_DOWN          0xFF232B39u
#define C_BTN_DANGER        0xFF5A2E2Bu
#define C_BTN_DANGER_HOVER  0xFF7A3B37u
#define C_BTN_EDGE          0xFF454F63u
#define C_FIELD             0xFF11151Cu
#define C_RULE              0xFF39415Bu

/* -- Model ----------------------------------------------------------- */

typedef struct {
    int      present;
    int      index;
    uint64_t lba_start;
    uint64_t size_mb;
    char     fs[16];
    char     fstype[16];
    int      mounted;
    int      slot;
} part_t;

typedef struct {
    char     dev_path[128];
    char     model[48];
    char     type[24];
    char     serial[24];
    uint64_t capacity_mb;
    int      read_only;
    part_t   parts[MAX_PARTS];
} drive_t;

/* -- KV parser (same approach as lsblk / console mdparted) ----------- */

static char g_kv[4096];
static int  g_kv_len;

static void kv_load(int fd) {
    g_kv_len = 0;
    char tmp[256];
    ssize_t n;
    while ((n = read(fd, tmp, sizeof(tmp))) > 0) {
        if (g_kv_len + (int)n < (int)sizeof(g_kv) - 1) {
            memcpy(g_kv + g_kv_len, tmp, n);
            g_kv_len += (int)n;
        }
    }
    g_kv[g_kv_len] = '\0';
}

static int kv_get(const char *key, char *out, int out_sz) {
    const char *p = g_kv;
    int klen = strlen(key);
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, key, klen) == 0) {
            const char *q = p + klen;
            while (*q == ' ' || *q == ':' || *q == '\t') q++;
            int i = 0;
            while (*q && *q != '\n' && i < out_sz - 1) out[i++] = *q++;
            out[i] = '\0';
            return 1;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    out[0] = '\0';
    return 0;
}

static uint64_t kv_get_u64(const char *key) {
    char tmp[32];
    kv_get(key, tmp, sizeof(tmp));
    uint64_t v = 0;
    for (int i = 0; tmp[i] >= '0' && tmp[i] <= '9'; i++) v = v * 10 + (uint64_t)(tmp[i] - '0');
    return v;
}

static int kv_get_int(const char *key) { return (int)kv_get_u64(key); }

/* "part" + N + "_lba" etc, without leaning on snprintf's format engine. */
static void kvkey(char *out, int sz, const char *pfx, int n, const char *sfx) {
    int i = 0;
    while (*pfx && i < sz - 1) out[i++] = *pfx++;
    if (i < sz - 1) out[i++] = (char)('0' + n);
    while (*sfx && i < sz - 1) out[i++] = *sfx++;
    out[i] = '\0';
}

static void drive_load(drive_t *d) {
    for (int i = 0; i < MAX_PARTS; i++) d->parts[i].present = 0;

    int fd = open(d->dev_path, O_RDONLY, 0);
    if (fd < 0) return;
    kv_load(fd);
    close(fd);

    kv_get("model",  d->model,  sizeof(d->model));
    kv_get("type",   d->type,   sizeof(d->type));
    kv_get("serial", d->serial, sizeof(d->serial));
    d->capacity_mb = kv_get_u64("capacity_mb");
    d->read_only   = kv_get_int("read_only");

    for (int p = 1; p <= MAX_PARTS; p++) {
        char key[40];
        kvkey(key, sizeof(key), "part", p, "_present");
        if (!kv_get_int(key)) { d->parts[p-1].present = 0; continue; }

        part_t *pt = &d->parts[p-1];
        pt->present = 1;
        pt->index   = p;
        kvkey(key, sizeof(key), "part", p, "_lba");     pt->lba_start = kv_get_u64(key);
        kvkey(key, sizeof(key), "part", p, "_mb");      pt->size_mb   = kv_get_u64(key);
        kvkey(key, sizeof(key), "part", p, "_fs");      kv_get(key, pt->fs,     sizeof(pt->fs));
        kvkey(key, sizeof(key), "part", p, "_fstype");  kv_get(key, pt->fstype, sizeof(pt->fstype));
        kvkey(key, sizeof(key), "part", p, "_mounted"); pt->mounted   = kv_get_int(key);
        kvkey(key, sizeof(key), "part", p, "_slot");    pt->slot      = kv_get_int(key);
    }
}

/* -- Tiny string helpers --------------------------------------------- */

static int sl(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void scat(char *dst, int cap, const char *s) {
    int n = sl(dst);
    while (*s && n < cap - 1) dst[n++] = *s++;
    dst[n] = '\0';
}

static void scat_u(char *dst, int cap, uint64_t v) {
    char t[24];
    int n = 0;
    if (v == 0) t[n++] = '0';
    while (v > 0 && n < (int)sizeof(t)) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    int d = sl(dst);
    for (int i = n - 1; i >= 0 && d < cap - 1; i--) dst[d++] = t[i];
    dst[d] = '\0';
}

static void scat_hex2(char *dst, int cap, uint8_t v) {
    const char *H = "0123456789ABCDEF";
    char t[5];
    t[0] = '0'; t[1] = 'x'; t[2] = H[(v >> 4) & 0xF]; t[3] = H[v & 0xF]; t[4] = '\0';
    scat(dst, cap, t);
}

static void sset(char *dst, int cap, const char *s) { dst[0] = '\0'; scat(dst, cap, s); }

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int parse_u(const char *s, uint64_t *out) {
    int any = 0;
    uint64_t v = 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint64_t)(*s++ - '0'); any = 1; }
    *out = v;
    return any;
}

/* Accepts 0x-prefixed hex or plain decimal, like the console tool did. */
static int parse_type_byte(const char *s, uint8_t *out) {
    while (*s == ' ') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        uint32_t v = 0;
        int any = 0;
        while (*s) {
            int d;
            if (*s >= '0' && *s <= '9')      d = *s - '0';
            else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
            else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
            else break;
            v = v * 16 + (uint32_t)d; any = 1; s++;
        }
        if (!any) return 0;
        *out = (uint8_t)v;
        return 1;
    }
    uint64_t v;
    if (!parse_u(s, &v)) return 0;
    *out = (uint8_t)v;
    return 1;
}

static void fmt_mb(uint64_t mb, char *out, int cap) {
    out[0] = '\0';
    if (mb == 0) { scat(out, cap, "<1M"); return; }
    if (mb < 1024) { scat_u(out, cap, mb); scat(out, cap, "M"); }
    else           { scat_u(out, cap, mb / 1024); scat(out, cap, "G"); }
}

/* Extract numeric vdrive id from "$/dev/vDrive0" → 0 */
static uint8_t vid_from_path(const char *path) {
    const char *p = path;
    while (*p) {
        if (strncmp(p, "vDrive", 6) == 0) {
            p += 6;
            uint64_t v; parse_u(p, &v);
            return (uint8_t)v;
        }
        p++;
    }
    return 0;
}

/* -- Drawing into our own SHM window --------------------------------- */

static uint32_t   *g_buf;
static fnt_font_t *g_font;

/* Row width of the ACTUAL mapped buffer -- equals WIN_W normally, but
 * after MAXIMIZE/FULLSCREEN mdw reallocates to a bigger segment whose row
 * stride is the new (larger) width, not WIN_W. This UI's layout stays
 * fixed-size (letterboxed top-left); only pixel-indexing math needs the
 * real stride. */
static int g_stride = WIN_W;

static void fill_rect(int x, int y, int w, int h, uint32_t col) {
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > WIN_W) x1 = WIN_W;
    if (y1 > WIN_H) y1 = WIN_H;
    for (int yy = y; yy < y1; yy++)
        for (int xx = x; xx < x1; xx++)
            g_buf[(size_t)yy * g_stride + xx] = col;
}

static void frame_rect(int x, int y, int w, int h, uint32_t col) {
    fill_rect(x, y, w, 1, col);
    fill_rect(x, y + h - 1, w, 1, col);
    fill_rect(x, y, 1, h, col);
    fill_rect(x + w - 1, y, 1, h, col);
}

static int text_w(const char *s) {
    if (!g_font || !s) return 0;
    int w = 0;
    while (*s) {
        fnt_glyph_t *gl = fnt_get_glyph(g_font, (uint32_t)(unsigned char)*s);
        if (gl) w += gl->width;
        s++;
    }
    return w;
}

static void draw_text(int x, int y, const char *s, uint32_t col) {
    if (!g_font || !s) return;
    int cx = x;
    while (*s) {
        fnt_glyph_t *gl = fnt_get_glyph(g_font, (uint32_t)(unsigned char)*s);
        if (gl) {
            for (int dy = 0; dy < gl->bitmap_height; dy++)
                for (int dx = 0; dx < gl->bitmap_width; dx++)
                    if (fnt_get_pixel(gl, dx, dy)) {
                        int px = cx + dx, py = y + dy;
                        if (px >= 0 && px < WIN_W && py >= 0 && py < WIN_H)
                            g_buf[(size_t)py * g_stride + px] = col;
                    }
            cx += gl->width;
        }
        s++;
    }
}

static void draw_text_center(int x, int y, int w, const char *s, uint32_t col) {
    draw_text(x + (w - text_w(s)) / 2, y, s, col);
}

static fnt_font_t *try_load_font(const char *path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return NULL;
    long fsz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    fnt_font_t *out = NULL;
    if (fsz > 0 && fsz < 4 * 1024 * 1024) {
        void *data = malloc((size_t)fsz);
        if (data) {
            size_t got = 0;
            while (got < (size_t)fsz) {
                ssize_t r = read(fd, (uint8_t *)data + got, (size_t)fsz - got);
                if (r <= 0) break;
                got += (size_t)r;
            }
            if (got == (size_t)fsz) out = fnt_load_font(data, got);
            free(data);
        }
    }
    close(fd);
    return out;
}

/* -- UI state -------------------------------------------------------- */

enum {
    BTN_PREV = 0, BTN_NEXT, BTN_REFRESH, BTN_MKPART,
    BTN_MOUNT, BTN_UNMOUNT, BTN_MKLABEL, BTN_DELETE, BTN_QUIT,
    BTN_COUNT
};

typedef struct { int x, y, w, h; const char *label; int danger; } btn_t;
static btn_t g_btns[BTN_COUNT];

typedef enum { MODE_IDLE, MODE_INPUT_MKPART, MODE_CONFIRM } ui_mode_t;
typedef enum { PEND_NONE, PEND_MKLABEL, PEND_DELETE } pending_t;

static struct {
    drive_t   drives[MAX_DRIVES];
    int       ndrives;
    int       cur;        /* index into drives[] */
    int       sel_part;   /* 0-based, -1 = none  */

    ui_mode_t mode;
    pending_t pending;
    char      field[96];
    char      prompt[128];

    char      log[LOG_LINES][96];
    uint32_t  log_col[LOG_LINES];
    int       log_count;

    int       hover_btn;  /* BTN_* or -1 */
    int       hover_row;  /* 0-based or -1 */
    int       active_btn; /* button the press landed on, -1 = none */
    int       dirty;
    int       quit;
} ui;

static void log_line(const char *text, uint32_t col) {
    if (ui.log_count == LOG_LINES) {
        for (int i = 1; i < LOG_LINES; i++) {
            sset(ui.log[i-1], sizeof(ui.log[0]), ui.log[i]);
            ui.log_col[i-1] = ui.log_col[i];
        }
        ui.log_count--;
    }
    sset(ui.log[ui.log_count], sizeof(ui.log[0]), text);
    ui.log_col[ui.log_count] = col;
    ui.log_count++;
    ui.dirty = 1;
}

static drive_t *curd(void) { return &ui.drives[ui.cur]; }

/* -- Disk operations (identical semantics to the console tool) ------- */

static int guard_writable(void) {
    if (curd()->read_only) { log_line("Drive is read-only.", C_ERR); return 0; }
    return 1;
}

static void do_mklabel(void) {
    if (!guard_writable()) return;

    uint8_t vid = vid_from_path(curd()->dev_path);
    uint8_t mbr[SECTOR_SIZE];

    int r = vdrive_read_sector(vid, 0, mbr);
    if (r < 0) { log_line("Failed to read MBR.", C_ERR); return; }

    /* Preserve bootstrap code (bytes 0-445), zero the four entries, restamp. */
    memset(mbr + 446, 0, 64);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    r = vdrive_write_sector(vid, 0, mbr);
    if (r < 0) { log_line("Failed to write MBR.", C_ERR); return; }

    log_line("MBR partition table cleared.", C_OK);
    drive_load(curd());
    ui.sel_part = -1;
}

static void do_mkpart(uint64_t start_mb, uint64_t size_mb, uint8_t type_byte) {
    if (!guard_writable()) return;

    if (size_mb == 0) { log_line("Size must be > 0.", C_ERR); return; }
    if (start_mb + size_mb > curd()->capacity_mb) {
        char m[96];
        sset(m, sizeof(m), "Exceeds drive capacity (");
        scat_u(m, sizeof(m), curd()->capacity_mb);
        scat(m, sizeof(m), "M).");
        log_line(m, C_ERR);
        return;
    }

    int slot = -1;
    for (int i = 0; i < MAX_PARTS; i++) if (!curd()->parts[i].present) { slot = i; break; }
    if (slot < 0) { log_line("MBR already has 4 primary partitions.", C_ERR); return; }

    uint32_t lba_start = (uint32_t)((start_mb * 1024ULL * 1024ULL) / SECTOR_SIZE);
    uint32_t lba_count = (uint32_t)((size_mb  * 1024ULL * 1024ULL) / SECTOR_SIZE);

    uint8_t vid = vid_from_path(curd()->dev_path);
    uint8_t mbr[SECTOR_SIZE];
    int r = vdrive_read_sector(vid, 0, mbr);
    if (r < 0) { log_line("Failed to read MBR.", C_ERR); return; }

    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        log_line("No MBR signature — stamping one (MkLabel first next time).", C_WARN);
        mbr[510] = 0x55;
        mbr[511] = 0xAA;
    }

    /* MBR entry: status, CHS first, type, CHS last, LBA start, LBA count.
     * CHS fields get the standard LBA-only placeholder. */
    uint8_t *e = mbr + 446 + slot * 16;
    e[0]  = 0x00;
    e[1]  = 0xFE; e[2] = 0xFF; e[3] = 0xFF;
    e[4]  = type_byte;
    e[5]  = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
    e[8]  = (uint8_t)(lba_start);
    e[9]  = (uint8_t)(lba_start >> 8);
    e[10] = (uint8_t)(lba_start >> 16);
    e[11] = (uint8_t)(lba_start >> 24);
    e[12] = (uint8_t)(lba_count);
    e[13] = (uint8_t)(lba_count >> 8);
    e[14] = (uint8_t)(lba_count >> 16);
    e[15] = (uint8_t)(lba_count >> 24);

    r = vdrive_write_sector(vid, 0, mbr);
    if (r < 0) { log_line("Failed to write MBR.", C_ERR); return; }

    char m[96];
    sset(m, sizeof(m), "Partition ");
    scat_u(m, sizeof(m), (uint64_t)(slot + 1));
    scat(m, sizeof(m), " created: start=");
    scat_u(m, sizeof(m), start_mb);
    scat(m, sizeof(m), "M size=");
    scat_u(m, sizeof(m), size_mb);
    scat(m, sizeof(m), "M type=");
    scat_hex2(m, sizeof(m), type_byte);
    log_line(m, C_OK);
    log_line("Use a separate mkfs tool to format it.", C_TEXT_DIM);

    drive_load(curd());
    ui.sel_part = slot;
}

static void do_delete(int pidx0) {
    if (!guard_writable()) return;
    if (pidx0 < 0 || pidx0 >= MAX_PARTS) return;

    part_t *pt = &curd()->parts[pidx0];
    if (!pt->present) { log_line("No such partition.", C_ERR); return; }
    if (pt->mounted)  { log_line("Partition is mounted — unmount first.", C_ERR); return; }

    uint8_t vid = vid_from_path(curd()->dev_path);
    uint8_t mbr[SECTOR_SIZE];
    int r = vdrive_read_sector(vid, 0, mbr);
    if (r < 0) { log_line("Failed to read MBR.", C_ERR); return; }

    memset(mbr + 446 + pidx0 * 16, 0, 16);

    r = vdrive_write_sector(vid, 0, mbr);
    if (r < 0) { log_line("Failed to write MBR.", C_ERR); return; }

    char m[96];
    sset(m, sizeof(m), "Partition ");
    scat_u(m, sizeof(m), (uint64_t)(pidx0 + 1));
    scat(m, sizeof(m), " removed.");
    log_line(m, C_OK);
    drive_load(curd());
}

static void do_mount(int pidx0) {
    part_t *pt = &curd()->parts[pidx0];
    if (!pt->present) { log_line("No such partition.", C_ERR); return; }
    if (pt->mounted)  { log_line("Already mounted.", C_WARN); return; }

    uint8_t vid = vid_from_path(curd()->dev_path);
    int r = mount_drive((int)vid, (uint32_t)pt->lba_start, 0); /* 0 = autodetect fs */
    if (r < 0) { log_line("Mount failed.", C_ERR); return; }

    char m[96];
    sset(m, sizeof(m), "Mounted at slot ");
    scat_u(m, sizeof(m), (uint64_t)r);
    scat(m, sizeof(m), ".");
    log_line(m, C_OK);
    drive_load(curd());
}

static void do_unmount(int pidx0) {
    part_t *pt = &curd()->parts[pidx0];
    if (!pt->present) { log_line("No such partition.", C_ERR); return; }
    if (!pt->mounted) { log_line("Not mounted.", C_WARN); return; }

    int was = pt->slot;
    int r = unmount_slot(pt->slot);
    if (r < 0) { log_line("Unmount failed.", C_ERR); return; }

    char m[96];
    sset(m, sizeof(m), "Unmounted (was slot ");
    scat_u(m, sizeof(m), (uint64_t)was);
    scat(m, sizeof(m), ").");
    log_line(m, C_OK);
    drive_load(curd());
}

/* -- Drive enumeration ----------------------------------------------- */

static int enumerate_drives(void) {
    int count = 0;
    int dir = opendir("$/dev/");
    if (dir < 0) return 0;

    char entry[96];
    int is_dir_flag;
    uint32_t entry_size;

    while (readdir(dir, entry, sizeof(entry), &is_dir_flag, &entry_size) > 0) {
        if (is_dir_flag) continue;
        if (strncmp(entry, "vDrive", 6) != 0) continue;
        if (count >= MAX_DRIVES) break;

        /* Kernel bug: $/dev/ may list each vDrive twice — deduplicate by name */
        char candidate[128];
        sset(candidate, sizeof(candidate), "$/dev/");
        scat(candidate, sizeof(candidate), entry);

        int seen = 0;
        for (int i = 0; i < count; i++)
            if (str_eq(ui.drives[i].dev_path, candidate)) { seen = 1; break; }
        if (seen) continue;

        sset(ui.drives[count].dev_path, sizeof(ui.drives[0].dev_path), candidate);
        drive_load(&ui.drives[count]);
        count++;
    }
    closedir(dir);
    return count;
}

/* -- Layout + hit testing -------------------------------------------- */

static void set_btn(int id, int x, int y, int w, int h, const char *label, int danger) {
    g_btns[id].x = x; g_btns[id].y = y; g_btns[id].w = w; g_btns[id].h = h;
    g_btns[id].label = label; g_btns[id].danger = danger;
}

static void layout_buttons(void) {
    /* Drive stepper, top-right */
    set_btn(BTN_PREV, WIN_W - PAD - 68, HDR_Y - 2, 32, 20, "<", 0);
    set_btn(BTN_NEXT, WIN_W - PAD - 32, HDR_Y - 2, 32, 20, ">", 0);

    int x = PAD;
    set_btn(BTN_REFRESH, x, BTN_ROW1_Y, BTN_W, BTN_H, "Refresh", 0); x += BTN_W + BTN_GAP;
    set_btn(BTN_MKPART,  x, BTN_ROW1_Y, BTN_W, BTN_H, "MkPart",  0); x += BTN_W + BTN_GAP;
    set_btn(BTN_MOUNT,   x, BTN_ROW1_Y, BTN_W, BTN_H, "Mount",   0); x += BTN_W + BTN_GAP;
    set_btn(BTN_UNMOUNT, x, BTN_ROW1_Y, BTN_W, BTN_H, "Unmount", 0);

    x = PAD;
    set_btn(BTN_MKLABEL, x, BTN_ROW2_Y, BTN_W, BTN_H, "MkLabel", 1); x += BTN_W + BTN_GAP;
    set_btn(BTN_DELETE,  x, BTN_ROW2_Y, BTN_W, BTN_H, "Delete",  1);
    set_btn(BTN_QUIT, WIN_W - PAD - BTN_W, BTN_ROW2_Y, BTN_W, BTN_H, "Quit", 0);
}

static int hit_button(int x, int y) {
    for (int i = 0; i < BTN_COUNT; i++) {
        btn_t *b = &g_btns[i];
        if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) return i;
    }
    return -1;
}

static int hit_row(int x, int y) {
    if (x < PAD || x > WIN_W - PAD) return -1;
    if (y < TABLE_Y || y >= TABLE_Y + MAX_PARTS * ROW_H) return -1;
    return (y - TABLE_Y) / ROW_H;
}

/* -- Rendering ------------------------------------------------------- */

static void draw_button(int id) {
    btn_t *b = &g_btns[id];
    int hot  = (ui.hover_btn == id);
    int down = (ui.active_btn == id && hot);

    uint32_t bg;
    if (down)            bg = C_BTN_DOWN;
    else if (b->danger)  bg = hot ? C_BTN_DANGER_HOVER : C_BTN_DANGER;
    else                 bg = hot ? C_BTN_HOVER : C_BTN;

    fill_rect(b->x, b->y, b->w, b->h, bg);
    frame_rect(b->x, b->y, b->w, b->h, hot ? C_ACCENT : C_BTN_EDGE);
    draw_text_center(b->x, b->y + (b->h - 8) / 2, b->w, b->label, C_TEXT);
}

static void render(void) {
    drive_t *d = curd();
    char line[160];

    fill_rect(0, 0, WIN_W, WIN_H, C_BG);

    /* -- Header -- */
    sset(line, sizeof(line), d->dev_path);
    scat(line, sizeof(line), "   ");
    scat(line, sizeof(line), d->model[0] ? d->model : "(unknown model)");
    draw_text(PAD, HDR_Y, line, C_ACCENT);

    char cap[16];
    fmt_mb(d->capacity_mb, cap, sizeof(cap));
    sset(line, sizeof(line), "type=");
    scat(line, sizeof(line), d->type[0] ? d->type : "?");
    scat(line, sizeof(line), "  serial=");
    scat(line, sizeof(line), d->serial[0] ? d->serial : "?");
    scat(line, sizeof(line), "  capacity=");
    scat(line, sizeof(line), cap);
    scat(line, sizeof(line), "  read_only=");
    scat(line, sizeof(line), d->read_only ? "yes" : "no");
    draw_text(PAD, HDR_LINE2_Y, line, d->read_only ? C_WARN : C_TEXT_DIM);

    draw_button(BTN_PREV);
    draw_button(BTN_NEXT);

    /* -- Partition table -- */
    fill_rect(PAD, TABLE_HDR_Y - 4, WIN_W - 2 * PAD, 1, C_RULE);
    draw_text(PAD + 8,   TABLE_HDR_Y, "#",         C_TEXT_DIM);
    draw_text(PAD + 44,  TABLE_HDR_Y, "START LBA", C_TEXT_DIM);
    draw_text(PAD + 160, TABLE_HDR_Y, "SIZE",      C_TEXT_DIM);
    draw_text(PAD + 240, TABLE_HDR_Y, "FS",        C_TEXT_DIM);
    draw_text(PAD + 340, TABLE_HDR_Y, "MOUNTED",   C_TEXT_DIM);
    draw_text(PAD + 440, TABLE_HDR_Y, "SLOT",      C_TEXT_DIM);

    for (int i = 0; i < MAX_PARTS; i++) {
        part_t *pt = &d->parts[i];
        int ry = TABLE_Y + i * ROW_H;
        int selected = (ui.sel_part == i);
        uint32_t rowbg = selected ? C_ROW_SEL : (ui.hover_row == i ? C_ROW_HOVER : C_ROW);
        fill_rect(PAD, ry, WIN_W - 2 * PAD, ROW_H - 2, rowbg);

        char c[24];
        sset(c, sizeof(c), ""); scat_u(c, sizeof(c), (uint64_t)(i + 1));
        draw_text(PAD + 8, ry + 5, c, selected ? C_TEXT : C_WARN);

        if (!pt->present) {
            draw_text(PAD + 44, ry + 5, "(free)", selected ? C_TEXT : C_TEXT_DIM);
            continue;
        }

        sset(c, sizeof(c), ""); scat_u(c, sizeof(c), pt->lba_start);
        draw_text(PAD + 44, ry + 5, c, C_TEXT);

        char sz[16]; fmt_mb(pt->size_mb, sz, sizeof(sz));
        draw_text(PAD + 160, ry + 5, sz, C_TEXT);

        /* Mounted partitions report the fs the kernel actually detected;
         * otherwise fall back to whatever the MBR type string claims. */
        const char *fs_show = (pt->mounted && pt->fstype[0]) ? pt->fstype
                            : (pt->fs[0] ? pt->fs : "-");
        draw_text(PAD + 240, ry + 5, fs_show, C_TEXT);

        draw_text(PAD + 340, ry + 5, pt->mounted ? "yes" : "no",
                   pt->mounted ? (selected ? C_TEXT : C_OK) : C_TEXT_DIM);

        if (pt->mounted) { sset(c, sizeof(c), ""); scat_u(c, sizeof(c), (uint64_t)pt->slot); }
        else             { sset(c, sizeof(c), "-"); }
        draw_text(PAD + 440, ry + 5, c, C_TEXT);
    }

    /* -- Buttons -- */
    draw_button(BTN_REFRESH);
    draw_button(BTN_MKPART);
    draw_button(BTN_MOUNT);
    draw_button(BTN_UNMOUNT);
    draw_button(BTN_MKLABEL);
    draw_button(BTN_DELETE);
    draw_button(BTN_QUIT);

    /* -- Prompt + text field -- */
    if (ui.mode == MODE_IDLE) {
        draw_text(PAD, PROMPT_Y, "Click a row to select a partition. Destructive actions ask for confirmation.", C_TEXT_DIM);
    } else {
        draw_text(PAD, PROMPT_Y, ui.prompt, ui.mode == MODE_CONFIRM ? C_WARN : C_TEXT);
        fill_rect(PAD, FIELD_Y, WIN_W - 2 * PAD, FIELD_H, C_FIELD);
        frame_rect(PAD, FIELD_Y, WIN_W - 2 * PAD, FIELD_H, C_ACCENT);
        draw_text(PAD + 6, FIELD_Y + 6, ui.field, C_TEXT);
        fill_rect(PAD + 6 + text_w(ui.field) + 1, FIELD_Y + 4, 1, FIELD_H - 8, C_ACCENT); /* caret */
        draw_text(PAD, FIELD_Y + FIELD_H + 4, "Enter = run,  Esc = cancel", C_TEXT_DIM);
    }

    /* -- Log -- */
    fill_rect(PAD, LOG_Y - 6, WIN_W - 2 * PAD, 1, C_RULE);
    for (int i = 0; i < ui.log_count; i++)
        draw_text(PAD, LOG_Y + i * LOG_LINE_H, ui.log[i], ui.log_col[i]);
}

/* -- Actions --------------------------------------------------------- */

static void begin_input(const char *prompt, const char *preset) {
    ui.mode = MODE_INPUT_MKPART;
    sset(ui.prompt, sizeof(ui.prompt), prompt);
    sset(ui.field, sizeof(ui.field), preset ? preset : "");
    ui.dirty = 1;
}

static void begin_confirm(const char *prompt, pending_t what) {
    ui.mode = MODE_CONFIRM;
    ui.pending = what;
    sset(ui.prompt, sizeof(ui.prompt), prompt);
    sset(ui.field, sizeof(ui.field), "");
    ui.dirty = 1;
}

static void end_input(void) {
    ui.mode = MODE_IDLE;
    ui.pending = PEND_NONE;
    ui.field[0] = '\0';
    ui.dirty = 1;
}

static int require_selection(void) {
    if (ui.sel_part < 0) { log_line("Select a partition row first.", C_WARN); return 0; }
    return 1;
}

static void switch_drive(int delta) {
    if (ui.ndrives <= 1) { log_line("Only one drive available.", C_TEXT_DIM); return; }
    ui.cur = (ui.cur + delta + ui.ndrives) % ui.ndrives;
    ui.sel_part = -1;
    drive_load(curd());
    char m[128];
    sset(m, sizeof(m), "Switched to ");
    scat(m, sizeof(m), curd()->dev_path);
    log_line(m, C_ACCENT);
}

/* Parse "<start_mb> <size_mb> [type]" out of the text field. */
static void run_mkpart_field(const char *text) {
    char buf[96];
    sset(buf, sizeof(buf), text);

    char *tok[3]; int nt = 0;
    char *p = buf;
    while (*p && nt < 3) {
        while (*p == ' ') p++;
        if (!*p) break;
        tok[nt++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
    }

    if (nt < 2) { log_line("Need: <start_mb> <size_mb> [type]", C_ERR); return; }

    uint64_t start_mb, size_mb;
    if (!parse_u(tok[0], &start_mb) || !parse_u(tok[1], &size_mb)) {
        log_line("Start and size must be numbers (MB).", C_ERR);
        return;
    }
    uint8_t type_byte = 0x0B; /* FAT32 LBA — same default as the console tool */
    if (nt >= 3 && !parse_type_byte(tok[2], &type_byte)) {
        log_line("Bad type byte — use 0x0B, 0x83, 0x07 or a decimal.", C_ERR);
        return;
    }
    do_mkpart(start_mb, size_mb, type_byte);
}

static void on_button(int id) {
    switch (id) {
        case BTN_PREV:    switch_drive(-1); break;
        case BTN_NEXT:    switch_drive(+1); break;
        case BTN_REFRESH: drive_load(curd()); log_line("Reloaded partition table.", C_TEXT_DIM); break;

        case BTN_MKPART:
            begin_input("mkpart — <start_mb> <size_mb> [type]   (0x0B FAT32, 0x0E FAT16, 0x83 Linux, 0x07 NTFS)",
                         "1 64 0x0B");
            break;

        case BTN_MKLABEL:
            begin_confirm("This destroys ALL partition info on this drive. Type 'yes' to confirm:", PEND_MKLABEL);
            break;

        case BTN_DELETE:
            if (!require_selection()) break;
            if (!curd()->parts[ui.sel_part].present) { log_line("That slot is already free.", C_WARN); break; }
            begin_confirm("Delete the selected partition? Type 'yes' to confirm:", PEND_DELETE);
            break;

        case BTN_MOUNT:   if (require_selection()) do_mount(ui.sel_part);   break;
        case BTN_UNMOUNT: if (require_selection()) do_unmount(ui.sel_part); break;
        case BTN_QUIT:    ui.quit = 1; break;
        default: break;
    }
    ui.dirty = 1;
}

/* -- Input ----------------------------------------------------------- */

static void handle_mouse(const wm_mouse_event_t *m) {
    if (m->type == WM_MOUSE_LEAVE) {
        if (ui.hover_btn != -1 || ui.hover_row != -1 || ui.active_btn != -1) ui.dirty = 1;
        ui.hover_btn = -1;
        ui.hover_row = -1;
        ui.active_btn = -1;
        return;
    }

    int hb = hit_button(m->x, m->y);
    int hr = (hb < 0) ? hit_row(m->x, m->y) : -1;
    if (hb != ui.hover_btn || hr != ui.hover_row) {
        ui.hover_btn = hb;
        ui.hover_row = hr;
        ui.dirty = 1;
    }

    if (m->type == WM_MOUSE_DOWN) {
        if (m->changed & WM_MBTN_LEFT) {
            ui.active_btn = hb;
            if (hb < 0 && hr >= 0) { ui.sel_part = (ui.sel_part == hr) ? -1 : hr; }
            ui.dirty = 1;
        }
        return;
    }

    if (m->type == WM_MOUSE_UP) {
        if (m->changed & WM_MBTN_LEFT) {
            /* Commit only if the release lands on the same button the press did.
             * That's the standard "drag off to abort" escape hatch, and it matters
             * here because two of these buttons rewrite a partition table. */
            int was = ui.active_btn;
            ui.active_btn = -1;
            if (was >= 0 && was == hb) on_button(was);
            ui.dirty = 1;
        }
        return;
    }
}

static void handle_key(const KeyboardEventData *k) {
    if (ui.mode == MODE_IDLE) return; /* nothing focusable outside the field */

    char c = (char)k->ascii;

    if (c == 27) { end_input(); log_line("Cancelled.", C_TEXT_DIM); return; }

    if (c == 8 || c == 127) { /* backspace / delete */
        int n = sl(ui.field);
        if (n > 0) { ui.field[n-1] = '\0'; ui.dirty = 1; }
        return;
    }

    if (c == '\n' || c == '\r') {
        ui_mode_t was  = ui.mode;
        pending_t what = ui.pending;
        int       sel  = ui.sel_part;
        char text[96]; sset(text, sizeof(text), ui.field);
        end_input();

        if (was == MODE_CONFIRM) {
            if (!str_eq(text, "yes")) { log_line("Aborted — nothing written.", C_TEXT_DIM); return; }
            if (what == PEND_MKLABEL)     do_mklabel();
            else if (what == PEND_DELETE) do_delete(sel);
        } else {
            run_mkpart_field(text);
        }
        return;
    }

    if (c >= 32 && c < 127) {
        int n = sl(ui.field);
        if (n < (int)sizeof(ui.field) - 1) { ui.field[n] = c; ui.field[n+1] = '\0'; ui.dirty = 1; }
    }
}

/* -- Entry point ----------------------------------------------------- */

int md_main(long argc, char **argv) {
    memset(&ui, 0, sizeof(ui));
    ui.sel_part   = -1;
    ui.hover_btn  = -1;
    ui.hover_row  = -1;
    ui.active_btn = -1;
    ui.mode       = MODE_IDLE;

    /* Drive selection: an explicit path wins, otherwise enumerate $/dev/. */
    if (argc >= 2 && argv[1] && argv[1][0]) {
        sset(ui.drives[0].dev_path, sizeof(ui.drives[0].dev_path), argv[1]);
        drive_load(&ui.drives[0]);
        ui.ndrives = 1;
    } else {
        ui.ndrives = enumerate_drives();
    }
    if (ui.ndrives == 0) {
        puts("mdparted: no block devices found\n");
        return 1;
    }
    ui.cur = 0;

    wm_shm_t *mb = wm_ipc_connect(3000); /* mdw may still be starting up — poll, don't fail on the first race */
    if (!mb) {
        puts("mdparted: mdw never came up (waited 3s)\n");
        return 1;
    }

    wm_request_t req; memset(&req, 0, sizeof(req));
    req.cmd   = WM_CMD_CREATE;
    req.w     = WIN_W;
    req.h     = WIN_H;
    req.flags = 0;                    /* fully opaque -> memcpy blit path in mdw */
    req.pid   = (uint32_t)getpid();   /* lets mdw reap this window if we crash */
    { const char *t = "mdparted"; int i = 0; while (t[i] && i < 63) { req.title[i] = t[i]; i++; } req.title[i] = 0; }

    wm_response_t resp;
    if (wm_ipc_call(mb, &req, &resp, 3000) != 0 || !resp.success) {
        printf("mdparted: create failed: %s\n", resp.message);
        return 1;
    }

    int shmfd = shm_open(resp.shm_name, O_RDWR, 0, 0);
    if (shmfd < 0) { puts("mdparted: shm_open failed\n"); return 1; }
    g_buf = (uint32_t *)mmap(NULL, (size_t)resp.shm_size, PROT_R | PROT_W, MAP_SHARED, shmfd);
    if (g_buf == MAP_FAILED) { puts("mdparted: mmap failed\n"); return 1; }

    /* Input rings. The protocol treats both as optional, but this app is
     * useless without the mouse one, so a failure there is fatal rather
     * than a window that silently ignores every click. */
    wm_key_ring_t *keys = NULL;
    int keyfd = shm_open(resp.key_shm_name, O_RDWR, 0, 0);
    if (keyfd >= 0) {
        void *p = mmap(NULL, (size_t)resp.key_shm_size, PROT_R | PROT_W, MAP_SHARED, keyfd);
        if (p != MAP_FAILED) keys = (wm_key_ring_t *)p;
    }
    wm_mouse_ring_t *mice = NULL;
    int mousefd = shm_open(resp.mouse_shm_name, O_RDWR, 0, 0);
    if (mousefd >= 0) {
        void *p = mmap(NULL, (size_t)resp.mouse_shm_size, PROT_R | PROT_W, MAP_SHARED, mousefd);
        if (p != MAP_FAILED) mice = (wm_mouse_ring_t *)p;
    }
    if (!mice) { puts("mdparted: no mouse ring (mdw too old?)\n"); return 1; }
    if (!keys)   puts("mdparted: no key ring — text entry disabled\n");

    g_font = try_load_font(FONT_PRIMARY);
    if (!g_font) g_font = try_load_font(FONT_FALLBACK);

    layout_buttons();
    wm_set_cursor(mb, resp.win_id, WM_CURSOR_POINTER);

    log_line("mdparted — ModuOS Partition Editor (GUI)", C_ACCENT);
    log_line("Formatting is not done here; use a separate mkfs tool.", C_TEXT_DIM);
    if (curd()->read_only) log_line("This drive is read-only: edits are blocked.", C_WARN);

    wm_client_surface_t surf;
    wm_client_surface_init(&surf, g_buf, resp.shm_name, resp.shm_size, WIN_W, WIN_H);

    render();
    wm_damage(mb, resp.win_id, 0, 0, WIN_W, WIN_H);

    /* Nothing here animates, so this app repaints only on input. A full
     * -window damage rect per change is fine at human click rates; precise
     * sub-rect damage (like demo_notes.c's uptime line) only pays off for
     * something redrawing continuously. */
    uint64_t next_poll = time_ms();
    uint64_t next_resize_poll = time_ms();
    while (!ui.quit) {
        wm_mouse_event_t m;
        while (wm_mouse_ring_pop(mice, &m)) handle_mouse(&m);

        if (keys) {
            KeyboardEventData k;
            while (wm_key_ring_pop(keys, &k)) handle_key(&k);
        }

        /* mdw draws this window's titlebar itself, so its own maximize/
         * restore button never round-trips through our own wm_ipc_call --
         * polling wm_list_windows() is the only way we find out the
         * content buffer got reallocated. */
        if (time_ms() >= next_resize_poll) {
            int rc = wm_client_surface_poll(mb, resp.win_id, &surf);
            if (rc < 0) break; /* window torn down some other way */
            if (rc > 0) {
                g_buf = surf.buf;
                g_stride = surf.w;
                /* Fixed table/log layout: letterbox rather than reflow --
                 * clear the whole (possibly now much bigger) buffer, then
                 * redraw the normal content anchored top-left. */
                wm_client_surface_clear(&surf, C_BG);
                ui.dirty = 1;
            }
            next_resize_poll = time_ms() + 200;
        }

        if (surf.minimized) {
            next_poll = time_ms();
            yield();
            continue;
        }

        if (ui.dirty) {
            render();
            wm_damage(mb, resp.win_id, 0, 0, WIN_W, WIN_H);
            ui.dirty = 0;
        }

        /* sleep() is unreliable right now — pace with time_ms()+yield(). */
        next_poll += 8;
        while (time_ms() < next_poll) yield();
        if (time_ms() > next_poll + 200) next_poll = time_ms(); /* resync after a long stall */
    }

    wm_request_t close_req; memset(&close_req, 0, sizeof(close_req));
    close_req.cmd    = WM_CMD_CLOSE;
    close_req.win_id = resp.win_id;
    wm_response_t cresp;
    wm_ipc_call(mb, &close_req, &cresp, 3000);

    if (surf.buf) munmap(surf.buf, (size_t)surf.shm_size); /* NOT resp.shm_size -- may be stale after a resize */
    if (g_font) fnt_free_font(g_font);
    return 0;
}