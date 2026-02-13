#include "moduos/fs/userfs.h"
#include "moduos/fs/fd.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/interrupts/irq_lock.h"
#include "moduos/kernel/interrupts/hlt_wait.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/process/process.h"

/*
 * UserFS: DevFS-style tree for user processes.
 * Paths are rooted at $/user.
 */

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
    const char *owner_id;
    uint32_t perms;
    struct userfs_node *owner_next;
    void *ctx;
} userfs_node_t;

typedef struct {
    uint8_t buf[4096];
    uint32_t r;
    uint32_t w;
    uint32_t count;
    int flags;
    char path[128];
    userfs_node_t *node;
} userfs_node_ctx_t;

typedef struct userfs_handle {
    userfs_node_ctx_t *ctx;
    userfs_node_t *node;
    int flags;
} userfs_handle_t;

static userfs_node_t *g_root = NULL;
static userfs_node_t *g_owned_nodes = NULL;
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

    if (strncmp(path, "user", 4) == 0 && (path[4] == 0 || path[4] == '/')) {
        path += 4;
        while (*path == '/') path++;
    }

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

static void userfs_unlink_child(userfs_node_t *parent, userfs_node_t *child) {
    if (!parent || !child) return;
    userfs_node_t **pp = &parent->children;
    while (*pp) {
        if (*pp == child) {
            *pp = child->next;
            child->next = NULL;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void userfs_free_node(userfs_node_t *node) {
    if (!node) return;
    while (node->children) {
        userfs_node_t *c = node->children;
        node->children = c->next;
        userfs_free_node(c);
    }
    if (node->type == USERFS_NODE_DEV) {
        if (node->ctx) {
            kfree(node->ctx);
            node->ctx = NULL;
        }
        if (node->owner_id) {
            kfree((void*)node->owner_id);
            node->owner_id = NULL;
        }
    }
    kfree(node);
}

static void userfs_prune_empty_dirs(userfs_node_t *dir) {
    while (dir && dir->parent && dir->type == USERFS_NODE_DIR && dir->children == NULL) {
        userfs_node_t *parent = dir->parent;
        userfs_unlink_child(parent, dir);
        kfree(dir);
        dir = parent;
    }
}

static void userfs_remove_owner_nodes(const char *owner_id) {
    userfs_node_t **pp = &g_owned_nodes;
    while (*pp) {
        userfs_node_t *node = *pp;
        if (node->owner_id && strcmp(node->owner_id, owner_id) == 0) {
            *pp = node->owner_next;
            if (node->parent) userfs_unlink_child(node->parent, node);
            userfs_prune_empty_dirs(node->parent);
            userfs_free_node(node);
            continue;
        }
        pp = &(*pp)->owner_next;
    }
}

int userfs_register_user_path(const char *path, const char *owner_id, uint32_t perms) {
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
    ctx->node = nd;

    nd->ctx = ctx;
    nd->owner_id = owner_id;
    nd->perms = perms ? perms : USERFS_PERM_READ_WRITE;
    userfs_add_child(cur, nd);

    nd->owner_next = g_owned_nodes;
    g_owned_nodes = nd;

    return 0;
}

static int userfs_check_access(userfs_node_t *node, int flags) {
    if (!node) return 0;
    int want_read = ((flags & O_WRONLY) == 0);
    int want_write = ((flags & (O_WRONLY | O_RDWR)) != 0);

    int allow_read = (node->perms & USERFS_PERM_READ_ONLY) || (node->perms & USERFS_PERM_READ_WRITE);
    int allow_write = (node->perms & USERFS_PERM_WRITE_ONLY) || (node->perms & USERFS_PERM_READ_WRITE);

    if (want_read && !allow_read) return 0;
    if (want_write && !allow_write) return 0;
    return 1;
}

void *userfs_open_path(const char *path, int flags) {
    if (!path) return NULL;

    path = userfs_normalize_path(path);
    if (!path) return NULL;

    userfs_node_t *node = userfs_find_node(path);
    if (!node || node->type != USERFS_NODE_DEV) {
        return NULL;
    }
    if (!userfs_check_access(node, flags)) {
        userfs_log_access("open", path, flags, 0, 0);
        return NULL;
    }

    userfs_handle_t *h = (userfs_handle_t*)kmalloc(sizeof(userfs_handle_t));
    if (!h) return NULL;
    memset(h, 0, sizeof(*h));
    h->ctx = (userfs_node_ctx_t*)node->ctx;
    h->node = node;
    h->flags = flags;
    userfs_log_access("open", path, flags, 1, 0);
    return h;
}

ssize_t userfs_read(void *handle, void *buf, size_t count) {
    userfs_handle_t *h = (userfs_handle_t*)handle;
    if (!h || !h->node || !h->ctx) return -1;
    int allowed = userfs_check_access(h->node, h->flags | O_RDONLY);
    userfs_log_access("read", h->ctx->path, h->flags, allowed, count);
    if (!allowed) return -2;

    userfs_node_ctx_t *c = h->ctx;
    if (!buf || count == 0) return -1;
    size_t n = 0;
    uint64_t f = irq_save();
    while (n < count) {
        if (c->count == 0) {
            if (h->flags & O_NONBLOCK) {
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

ssize_t userfs_write(void *handle, const void *buf, size_t count) {
    userfs_handle_t *h = (userfs_handle_t*)handle;
    if (!h || !h->node || !h->ctx) return -1;
    int allowed = userfs_check_access(h->node, h->flags | O_WRONLY);
    userfs_log_access("write", h->ctx->path, h->flags, allowed, count);
    if (!allowed) return -2;

    userfs_node_ctx_t *c = h->ctx;
    if (!buf || count == 0) return -1;
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

void userfs_close(void *handle) {
    if (!handle) return;
    kfree(handle);
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

void userfs_owner_exited(const char *owner_id) {
    if (!owner_id) return;
    userfs_remove_owner_nodes(owner_id);
}
