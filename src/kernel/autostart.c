#include "moduos/kernel/exec.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/process/process.h"
#include "moduos/fs/hvfs.h"
#include "moduos/fs/fs.h"
#include "moduos/fs/devfs.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"
#include <stddef.h>
#include <stdint.h>

// Autostart list file: one absolute path per line.
// Priority: earlier lines start first.
#define AUTOSTART_PATH "/ModuOS/System64/auto/autostart"

#define AUTOSTART_ONCE_MAX 64

static int is_space(char c) {
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n');
}

static char *trim(char *s) {
    if (!s) return s;
    while (*s && is_space(*s)) s++;
    char *e = s;
    while (*e) e++;
    while (e > s && is_space(e[-1])) { e[-1] = 0; e--; }
    return s;
}

static char *next_token(char **cursor) {
    if (!cursor || !*cursor) return NULL;
    char *p = *cursor;
    while (*p && is_space(*p)) p++;
    if (!*p) { *cursor = p; return NULL; }
    char *start = p;
    while (*p && !is_space(*p)) p++;
    if (*p) { *p = 0; p++; }
    *cursor = p;
    return start;
}

static void strip_comments_inplace(char *line, int *in_block_comment) {
    if (!line || !in_block_comment) return;
    char *in = line;
    char *out = line;
    while (*in) {
        if (*in_block_comment) {
            if (in[0] == '*' && in[1] == '/') {
                *in_block_comment = 0;
                in += 2;
                continue;
            }
            in++;
            continue;
        }

        if (in[0] == '/' && in[1] == '*') {
            *in_block_comment = 1;
            in += 2;
            continue;
        }
        if (in[0] == '/' && in[1] == '/') {
            break; // line comment
        }
        *out++ = *in++;
    }
    *out = 0;
}

static int line_has_blocking_suffix(char *line) {
    if (!line) return 0;
    char *end = line + strlen(line);
    while (end > line && is_space(end[-1])) end--;
    if (end > line && end[-1] == '$') {
        end--;
        while (end > line && is_space(end[-1])) end--;
        *end = 0;
        return 1;
    }
    return 0;
}

static int parse_time_ms(const char *s, uint64_t *out_ms) {
    if (!s || !*s || !out_ms) return -1;
    uint64_t val = 0;
    const char *p = s;
    while (*p >= '0' && *p <= '9') {
        val = (val * 10) + (uint64_t)(*p - '0');
        p++;
    }
    if (p == s) return -1;

    if (*p == 0) {
        *out_ms = val;
        return 0;
    }
    if (p[0] == 'm' && p[1] == 's' && p[2] == 0) {
        *out_ms = val;
        return 0;
    }
    if (p[0] == 's' && p[1] == 0) {
        *out_ms = val * 1000ULL;
        return 0;
    }
    return -1;
}

static int path_seen_once(const char *path) {
    static char *once_list[AUTOSTART_ONCE_MAX];
    static int once_count = 0;

    if (!path || !*path) return 0;

    for (int i = 0; i < once_count; i++) {
        if (once_list[i] && strcmp(once_list[i], path) == 0) return 1;
    }

    if (once_count < AUTOSTART_ONCE_MAX) {
        size_t len = strlen(path);
        char *copy = (char*)kmalloc(len + 1);
        if (copy) {
            memcpy(copy, path, len + 1);
            once_list[once_count++] = copy;
        }
    }
    return 0;
}

static int condition_exists(fs_mount_t *boot_mount, const char *path) {
    if (!boot_mount || !path || !*path) return 0;
    return fs_file_exists(boot_mount, path) ? 1 : 0;
}

static int condition_fs(const char *fs_name) {
    if (!fs_name || !*fs_name) return 0;
    int count = fs_get_mount_count();
    for (int i = 0; i < count; i++) {
        fs_mount_t *m = fs_get_mount(i);
        if (!m || !m->valid) continue;
        const char *name = fs_type_name(m->type);
        if (name && strcmp(name, fs_name) == 0) return 1;
    }
    return 0;
}

static int condition_dev(const char *dev_path) {
    if (!dev_path || !*dev_path) return 0;
    if (strncmp(dev_path, "$/dev/", 6) != 0) {
        return 0;
    }
    const char *path = dev_path + 6;
    void *h = devfs_open_path(path, 0);
    if (!h) return 0;
    devfs_close(h);
    return 1;
}

static uint32_t find_pid_by_name(const char *name) {
    if (!name || !*name) return 0;
    for (uint32_t pid = 1; pid < MAX_PROCESSES; pid++) {
        process_t *p = process_get_by_pid(pid);
        if (!p) continue;
        if (strcmp(p->name, name) == 0) return p->pid;
    }
    return 0;
}

static void wait_for_pid(uint32_t pid) {
    if (pid == 0) return;
    while (1) {
        process_t *p = process_get_by_pid(pid);
        if (!p || p->state == PROCESS_STATE_ZOMBIE || p->state == PROCESS_STATE_TERMINATED) {
            break;
        }
        process_yield();
    }
}

void kernel_run_autostart(int boot_slot) {
    void *buf = NULL;
    size_t sz = 0;

    int rc = hvfs_read(boot_slot, AUTOSTART_PATH, &buf, &sz);
    if (rc != 0 || !buf || sz == 0) {
        com_write_string(COM1_PORT, "[AUTO] no autostart list\n");
        return;
    }

    fs_mount_t *boot_mount = fs_get_mount(boot_slot);

    // Ensure NUL terminated
    char *text = (char*)kmalloc(sz + 1);
    if (!text) {
        hvfs_free(boot_slot, AUTOSTART_PATH, buf);
        return;
    }
    for (size_t i = 0; i < sz; i++) text[i] = ((char*)buf)[i];
    text[sz] = 0;
    hvfs_free(boot_slot, AUTOSTART_PATH, buf);

    com_write_string(COM1_PORT, "[AUTO] running autostart entries\n");

    int in_block_comment = 0;
    int in_conditional_block = 0;
    int conditional_skip = 0;
    const char *section = "default";

    char *p = text;
    while (*p) {
        // Split line
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p == '\n') { *p = 0; p++; }

        strip_comments_inplace(line, &in_block_comment);
        line = trim(line);
        if (!*line) continue;
        if (line[0] == '#') continue;

        if (in_conditional_block) {
            if (strcmp(line, "}") == 0) {
                in_conditional_block = 0;
                conditional_skip = 0;
                continue;
            }
            if (conditional_skip) {
                continue;
            }
        }

        int suffix_blocking = line_has_blocking_suffix(line);
        line = trim(line);
        if (!*line) continue;

        // Section headers
        if ((strncmp(line, "early:", 6) == 0) || (strncmp(line, "late:", 5) == 0)) {
            section = line;
            com_write_string(COM1_PORT, "[AUTO] section: ");
            com_write_string(COM1_PORT, section);
            com_write_string(COM1_PORT, "\n");
            continue;
        }

        // Labels
        if (line[0] == '@') {
            char *colon = line;
            while (*colon && *colon != ':') colon++;
            if (*colon == ':') {
                *colon = 0;
                com_write_string(COM1_PORT, "[AUTO] label: ");
                com_write_string(COM1_PORT, line + 1);
                com_write_string(COM1_PORT, "\n");
                continue;
            }
        }

        // Conditional prefix
        if (line[0] == '?') {
            char *cursor = line + 1;
            char *cond = next_token(&cursor);
            char *arg = next_token(&cursor);
            if (!cond || !arg) continue;

            int ok = 0;
            if (strcmp(cond, "exists") == 0) {
                ok = condition_exists(boot_mount, arg);
            } else if (strcmp(cond, "fs") == 0) {
                ok = condition_fs(arg);
            } else if (strcmp(cond, "dev") == 0) {
                ok = condition_dev(arg);
            }

            char *rest = trim(cursor);
            if (rest && rest[0] == '{' && rest[1] == 0) {
                in_conditional_block = 1;
                conditional_skip = ok ? 0 : 1;
                continue;
            }

            if (!ok) continue;
            line = rest;
            if (!line || !*line) continue;
        }

        char *cursor = line;
        char *token = next_token(&cursor);
        if (!token) continue;

        // sleep/delay directive
        if (strcmp(token, "sleep") == 0 || strcmp(token, "delay") == 0) {
            char *arg = next_token(&cursor);
            uint64_t ms = 0;
            if (arg && parse_time_ms(arg, &ms) == 0) {
                com_write_string(COM1_PORT, "[AUTO] sleep ");
                char tmp[32];
                utoa((uint32_t)ms, tmp, 10);
                com_write_string(COM1_PORT, tmp);
                com_write_string(COM1_PORT, " ms\n");
                process_sleep(ms);
            }
            continue;
        }

        // wait directive
        if (strcmp(token, "wait") == 0) {
            char *arg = next_token(&cursor);
            if (arg && *arg) {
                uint32_t pid = 0;
                if (*arg >= '0' && *arg <= '9') {
                    pid = (uint32_t)atoi(arg);
                } else {
                    pid = find_pid_by_name(arg);
                }
                if (pid) {
                    com_write_string(COM1_PORT, "[AUTO] wait pid ");
                    char tmp[16];
                    utoa(pid, tmp, 10);
                    com_write_string(COM1_PORT, tmp);
                    com_write_string(COM1_PORT, "\n");
                    wait_for_pid(pid);
                }
            }
            continue;
        }

        // Parse modifiers
        int explicit_mode = 0;
        int blocking = 0;
        int once = 0;

        while (token) {
            if (strcmp(token, "fg") == 0) {
                blocking = 1;
                explicit_mode = 1;
            } else if (strcmp(token, "bg") == 0) {
                blocking = 0;
                explicit_mode = 1;
            } else if (strcmp(token, "once") == 0) {
                once = 1;
            } else {
                break;
            }
            token = next_token(&cursor);
        }

        if (!token) continue;

        // Restore whitespace between token and cursor so args remain intact
        if (cursor && cursor > token) {
            char *restore = cursor - 1;
            if (*restore == 0) *restore = ' ';
        }

        char *cmd = token;
        if (!explicit_mode && suffix_blocking) {
            blocking = 1;
        }

        cmd = trim(cmd);
        if (!*cmd) continue;

        if (once && path_seen_once(cmd)) {
            continue;
        }

        com_write_string(COM1_PORT, "[AUTO] start: ");
        com_write_string(COM1_PORT, cmd);
        if (blocking) {
            com_write_string(COM1_PORT, " (wait)");
        }
        com_write_string(COM1_PORT, "\n");

        if (blocking) {
            exec(cmd);
        } else {
            int pid = exec_async(cmd);
            (void)pid;
        }
    }

    kfree(text);
}
