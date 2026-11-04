/*
 * mdbindkeys.c — global hotkey daemon for ModuOS.
 * Reads /ModuOS/shared/config/.mdbk.ini and launches the mapped .sqr app
 * when the key combo is pressed. Uses mdw's windowless key feed
 * (WM_CMD_KEYFEED_OPEN) — no window, no root, just mdw's SHM ring.
 *
 * Config format, one binding per line:
 *   KEY+OTHERKEY+...="/path/to/sqr/app"
 * Example:
 *   SUPER + R = "/Apps/mdrun.sqr"
 *
 * Recognized modifier tokens: CTRL, CONTROL, ALT, SHIFT, SUPER, WIN, CMD.
 * The one non-modifier token must be a single ASCII character.
 * Lines starting with # or ; are comments. Malformed lines are skipped.
 */

#include "libc/libc.h"
#include "libc/string.h"
#include "wm_protocol.h"
#include <stdint.h>

#define MDBK_CONFIG      "/ModuOS/shared/config/.mdbk.ini"
#define MDBK_MAX_BIND    64
#define MDBK_MAX_PATH    256
#define MDBK_REFIRE_MS   250

typedef struct {
    uint32_t mods;
    char     key;
    char     path[MDBK_MAX_PATH];
} mdbk_binding_t;

static mdbk_binding_t g_bind[MDBK_MAX_BIND];
static uint64_t       g_last_fire[MDBK_MAX_BIND];
static int            g_bind_count = 0;

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void sset(char *d, int cap, const char *s) { int n = 0; while (*s && n < cap - 1) d[n++] = *s++; d[n] = 0; }
static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static int streq_ci(const char *a, const char *b) {
    while (*a && *b) { if (up(*a) != up(*b)) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

static int ends_with_sqr(const char *s) {
    int n = slen(s);
    if (n < 4) return 0;
    return s[n - 4] == '.' && up(s[n - 3]) == 'S' && up(s[n - 2]) == 'Q' && up(s[n - 1]) == 'R';
}

static int parse_line(char *line, mdbk_binding_t *out) {
    char *eq = 0;
    for (char *p = line; *p; p++) if (*p == '=') { eq = p; break; }
    if (!eq) return 0;
    *eq = 0;
    char *lhs = line;
    char *rhs = eq + 1;

    while (*rhs && is_space(*rhs)) rhs++;
    int rn = slen(rhs);
    while (rn > 0 && is_space(rhs[rn - 1])) rn--;
    rhs[rn] = 0;
    if (rn >= 2 && rhs[0] == '"' && rhs[rn - 1] == '"') { rhs[rn - 1] = 0; rhs++; }
    if (!rhs[0] || !ends_with_sqr(rhs)) return 0;

    out->mods = 0;
    out->key = 0;

    char *p = lhs;
    while (*p) {
        while (*p && is_space(*p)) p++;
        if (!*p) break;
        char tok[32]; int tn = 0;
        while (*p && *p != '+' && tn < (int)sizeof(tok) - 1) {
            if (!is_space(*p)) tok[tn++] = *p;
            p++;
        }
        tok[tn] = 0;
        if (*p == '+') p++;
        if (!tok[0]) continue;

        if (streq_ci(tok, "CTRL") || streq_ci(tok, "CONTROL"))      out->mods |= MOD_CTRL;
        else if (streq_ci(tok, "ALT"))                              out->mods |= MOD_ALT;
        else if (streq_ci(tok, "SHIFT"))                            out->mods |= MOD_SHIFT;
        else if (streq_ci(tok, "SUPER") || streq_ci(tok, "WIN") ||
                 streq_ci(tok, "CMD"))                              out->mods |= MOD_SHIFT;
        else if (tn == 1)                                           out->key = up(tok[0]);
        else return 0;
    }
    if (!out->key) return 0;

    sset(out->path, sizeof(out->path), rhs);
    return 1;
}

static void load_config(void) {
    int fd = open(MDBK_CONFIG, O_RDONLY, 0);
    if (fd < 0) { puts("mdbindkeys: cannot open " MDBK_CONFIG "\n"); return; }

    long fsz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (fsz <= 0 || fsz > 64 * 1024) { close(fd); return; }

    char *data = (char *)malloc((size_t)fsz + 1);
    if (!data) { close(fd); return; }

    size_t got = 0;
    while (got < (size_t)fsz) {
        ssize_t r = read(fd, data + got, (size_t)fsz - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    data[got] = 0;

    char *p = data;
    while (*p && g_bind_count < MDBK_MAX_BIND) {
        char *line = p;
        while (*p && *p != '\n') p++;
        int had_nl = (*p == '\n');
        *p = 0;
        if (had_nl) p++;

        char *ls = line;
        while (*ls && is_space(*ls)) ls++;
        if (ls[0] && ls[0] != '#' && ls[0] != ';') {
            mdbk_binding_t b; memset(&b, 0, sizeof(b));
            if (parse_line(ls, &b)) g_bind[g_bind_count++] = b;
        }
    }
    free(data);
}

static void launch(const char *path) {
    int pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        char argv0[MDBK_MAX_PATH];
        sset(argv0, sizeof(argv0), path);
        char *argv[2]; argv[0] = argv0; argv[1] = NULL;
        execve(path, argv, NULL);
        exit(1);
    }
}

static void handle_key(const KeyboardEventData *k) {
    char c = up((char)k->ascii);
    uint64_t now = time_ms();

    for (int i = 0; i < g_bind_count; i++) {
        mdbk_binding_t *b = &g_bind[i];
        if (b->key != c) continue;
        if ((k->modifiers & b->mods) != b->mods) continue;
        if (now - g_last_fire[i] < MDBK_REFIRE_MS) continue;
        g_last_fire[i] = now;
        launch(b->path);
    }
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    load_config();
    if (g_bind_count == 0) puts("mdbindkeys: no bindings loaded\n");

    wm_shm_t *mb = wm_ipc_connect(3000);
    if (!mb) { puts("mdbindkeys: mdw never came up (waited 3s)\n"); return 1; }

    wm_key_ring_t *feed = NULL;
    int feed_id = -1;
    if (wm_keyfeed_open(mb, &feed, &feed_id) != 0) {
        puts("mdbindkeys: keyfeed open failed\n");
        return 1;
    }

    for (;;) {
        KeyboardEventData k;
        while (wm_key_ring_pop(feed, &k)) handle_key(&k);
        yield();
    }

    wm_keyfeed_close(mb, feed_id);
    return 0;
}