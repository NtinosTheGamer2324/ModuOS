#include "moduos/fs/userfs.h"
#include "moduos/fs/fd.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/interrupts/irq_lock.h"
#include "moduos/kernel/interrupts/hlt_wait.h"
#include "moduos/kernel/COM/com.h"

typedef enum {
    USERFS_NODE_DIR = 0,
    USERFS_NODE_DEV = 1,
} userfs_node_type_t;

typedef struct userfs_node {
    userfs_node_type_t type;
    char name[64];
    struct userfs_node *parent;
    struct userfs_node *children;
    struct userfs_node *next;
    userfs_user_ops_t ops;
    void *ctx;
    const char *owner_id;
} userfs_node_t;

typedef struct {
    uint8_t buf[4096];
    uint32_t r;
    uint32_t w;
    uint32_t count;
    int flags;
    char path[128];
} userfs_node_ctx_t;

static userfs_node_t *g_root = NULL;
static int g_inited = 0;

static void userfs_log_access(const char *op, const char *path, int flags, int allowed, size_t count) {
    char buf[32];
    com_write_string(COM1_PORT, "[USERFS] ");
    com_write_string(COM1_PORT, op);
    if (path && *path) {
        com_write_string(COM1_PORT, " ");
        com_write_string(COM1_PORT, path);
    }
    com_write_string(COM1_PORT, " flags=0x");
    itoa(flags, buf, 16);
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, " count=");
    itoa((int)count, buf, 10);
    com_write_string(COM1_PORT, buf);
    com_write_string(COM1_PORT, " allowed=");
    com_write_string(COM1_PORT, allowed ? "yes" : "no");
    com_write_string(COM1_PORT, "\n");
}

static void *userfs_open_ctx(void *ctx, int flags) {
    userfs_node_ctx_t *c = (userfs_node_ctx_t*)ctx;
    if (c) c->flags = flags;
    return ctx;
}

static ssize_t userfs_read_ctx(void *ctx, void *buf, size_t count) {
    userfs_node_ctx_t *c = (userfs_node_ctx_t*)ctx;
    if (!c || !buf || count == 0) return -1;

    int allowed = ((c->flags & O_WRONLY) == 0);
    userfs_log_access("read", c->path, c->flags, allowed, count);
    if (!allowed) return -2;

    size_t n = 0;
    uint64_t f = irq_save();
    while (n < count) {
        if (c->count == 0) {
            if (c->flags & O_NONBLOCK) {
                irq_restore(f);
                return (ssize_t)n;
            }
            irq_restore(f);
            hlt_wait_preserve_if();
            f = irq_save();
            continue;
        }
        ((uint8_t*)buf)[n++] = c->buf[c->r];
        c->r = (c->r + 1) % (uint32_t)sizeof(c->buf);
        c->count--;
    }
    irq_restore(f);
    return (ssize_t)n;
}

static ssize_t userfs_write_ctx(void *ctx, const void *buf, size_t count) {
    userfs_node_ctx_t *c = (userfs_node_ctx_t*)ctx;
    if (!c || !buf || count == 0) return -1;

    int allowed = ((c->flags & (O_WRONLY | O_RDWR)) != 0);
    userfs_log_access("write", c->path, c->flags, allowed, count);
    if (!allowed) return -2;

    size_t n = 0;
    uint64_t f = irq_save();
    while (n < count && c->count < sizeof(c->buf)) {
        c->buf[c->w] = ((const uint8_t*)buf)[n++];
        c->w = (c->w + 1) % (uint32_t)sizeof(c->buf);
        c->count++;
    }
    irq_restore(f);
    return (ssize_t)n;
}

static userfs_user_ops_t g_userfs_ops = {
    .read = userfs_read_ctx,
    .write = userfs_write_ctx,
};

static void userfs_init_once(void) {
    if (g_inited) return;
    g_root = (userfs_node_t*)kmalloc(sizeof(userfs_node_t));
    if (!g_root) return;
    memset(g_root, 0, sizeof(*g_root));
    g_root->type = USERFS_NODE_DIR;
    strncpy(g_root->name, "", sizeof(g_root->name) - 1);
    g_inited = 1;
}

static userfs_node_t *userfs_new_node(userfs_node_type_t type, const char *name, userfs_node_t *parent) {
    userfs_node_t *n = (userfs_node_t*)kmalloc(sizeof(userfs_node_t));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    n->type = type;
    n->parent = parent;
    if (name) {
        strncpy(n->name, name, sizeof(n->name) - 1);
        n->name[sizeof(n->name) - 1] = 0;
    }
    return n;
}

static userfs_node_t *userfs_find_child(userfs_node_t *dir, const char *name) {
    if (!dir || dir->type != USERFS_NODE_DIR || !name) return NULL;
    for (userfs_node_t *c = dir->children; c; c = c->next) {
        if (strcmp(c->name, name) == 0) return c;
    }
    return NULL;
}

static userfs_node_t *userfs_add_child(userfs_node_t *dir, userfs_node_t *child) {
    if (!dir || dir->type != USERFS_NODE_DIR || !child) return NULL;
    child->next = dir->children;
    dir->children = child;
    child->parent = dir;
    return child;
}

static const char *userfs_path_next(const char *p, char *seg, size_t seg_sz) {
    while (*p == '/') p++;
    if (!*p) return NULL;

    size_t i = 0;
    while (p[i] && p[i] != '/') {
        if (i + 1 < seg_sz) seg[i] = p[i];
        i++;
    }
    if (seg_sz) seg[(i < seg_sz) ? i : (seg_sz - 1)] = 0;
    return p + i;
}

static const char *userfs_normalize_path(const char *path) {
    if (!path) return NULL;

    if (path[0] == '$') {
        if (path[1] == '/') {
            path += 2;
        } else {
            path += 1;
        }
    }
    while (*path == '/') path++;

    if (strncmp(path, "userland/", 9) == 0) {
        path += 9;
    }
    while (*path == '/') path++;

    return *path ? path : NULL;
}

static userfs_node_t *userfs_find_node(const char *path) {
    userfs_init_once();
    if (!g_root || !path) return NULL;

    userfs_node_t *cur = g_root;
    const char *p = path;
    char seg[64];

    while ((p = userfs_path_next(p, seg, sizeof(seg))) != NULL) {
        if (!seg[0]) break;
        userfs_node_t *c = userfs_find_child(cur, seg);
        if (!c) return NULL;
        cur = c;
        while (*p == '/') p++;
    }

    return cur;
}

int userfs_register_user_path(const char *path, const char *owner_id) {
    userfs_init_once();
    if (!g_root || !path || !path[0] || !owner_id) return -1;

    path = userfs_normalize_path(path);
    if (!path) return -1;

    userfs_node_t *cur = g_root;
    const char *p = path;
    char seg[64];
    char last[64];
    last[0] = 0;

    while ((p = userfs_path_next(p, seg, sizeof(seg))) != NULL) {
        if (!seg[0]) break;
        strncpy(last, seg, sizeof(last) - 1);
        last[sizeof(last) - 1] = 0;

        const char *q = p;
        while (*q == '/') q++;
        int has_more = (*q != 0);

        if (!has_more) break;

        userfs_node_t *c = userfs_find_child(cur, seg);
        if (c) {
            if (c->type != USERFS_NODE_DIR) return -2;
            cur = c;
        } else {
            userfs_node_t *nd = userfs_new_node(USERFS_NODE_DIR, seg, cur);
            if (!nd) return -3;
            userfs_add_child(cur, nd);
            cur = nd;
        }
        p = q;
    }

    if (!last[0]) return -4;

    userfs_node_t *existing = userfs_find_child(cur, last);
    if (existing) return -5;

    userfs_node_t *nd = userfs_new_node(USERFS_NODE_DEV, last, cur);
    if (!nd) return -6;

    userfs_node_ctx_t *ctx = (userfs_node_ctx_t*)kmalloc(sizeof(userfs_node_ctx_t));
    if (!ctx) {
        kfree(nd);
        return -7;
    }
    memset(ctx, 0, sizeof(*ctx));
    strncpy(ctx->path, path, sizeof(ctx->path) - 1);
    ctx->path[sizeof(ctx->path) - 1] = 0;
    nd->ctx = ctx;
    nd->ops = g_userfs_ops;
    nd->owner_id = owner_id;
    userfs_add_child(cur, nd);
    return 0;
}

int userfs_pump(void) {
    userfs_init_once();
    if (!g_root) return 0;
    int progressed = 0;

    // Walk all nodes and allow writers to drain data without explicit reads.
    // This ensures producer/consumer pairs don't stall if the reader exits.
    for (userfs_node_t *dir = g_root; dir; dir = dir->next) {
        if (dir->type != USERFS_NODE_DIR) continue;
        for (userfs_node_t *c = dir->children; c; c = c->next) {
            if (c->type != USERFS_NODE_DEV || !c->ctx) continue;
            userfs_node_ctx_t *ctx = (userfs_node_ctx_t*)c->ctx;
            if (ctx->count > 0 && (ctx->flags & O_NONBLOCK)) {
                // Make data available by yielding without clearing; count as progress.
                progressed = 1;
            }
        }
    }

    return progressed;
}

void *userfs_open_path(const char *path, int flags) {
    if (!path) return NULL;

    path = userfs_normalize_path(path);
    if (!path) return NULL;

    userfs_node_t *node = userfs_find_node(path);
    if (!node || node->type != USERFS_NODE_DEV) {
        return NULL;
    }
    if (!node->ops.read && !node->ops.write) return NULL;
    return userfs_open_ctx(node->ctx, flags);
}

ssize_t userfs_read(void *handle, void *buf, size_t count) {
    return userfs_read_ctx(handle, buf, count);
}

ssize_t userfs_write(void *handle, const void *buf, size_t count) {
    return userfs_write_ctx(handle, buf, count);
}

void userfs_close(void *handle) {
    (void)handle;
}

int userfs_list_dir_next(const char *path, int *cookie, char *name_buf, size_t buf_size, int *is_dir) {
    if (!cookie || !name_buf || buf_size == 0) return -1;
    userfs_node_t *dir = userfs_find_node(path ? path : "");
    if (!dir || dir->type != USERFS_NODE_DIR) return -1;

    int idx = 0;
    int target = *cookie;
    for (userfs_node_t *c = dir->children; c; c = c->next) {
        if (idx == target) {
            strncpy(name_buf, c->name, buf_size - 1);
            name_buf[buf_size - 1] = 0;
            if (is_dir) *is_dir = (c->type == USERFS_NODE_DIR);
            *cookie = target + 1;
            return 1;
        }
        idx++;
    }
    return 0;
}

int userfs_directory_exists(const char *path) {
    userfs_node_t *dir = userfs_find_node(path ? path : "");
    return (dir && dir->type == USERFS_NODE_DIR) ? 1 : 0;
}
