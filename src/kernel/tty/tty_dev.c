// SPDX-License-Identifier: GPL-2.0-only
//
// tty_dev.c - devfs nodes for the TTY subsystem and the ioctl (SYS_INVOKE) handler.
//
//   $/dev/tty1..ttyN  $/dev/tty0  $/dev/tty  $/dev/console  $/dev/ptmx  $/dev/pts/N

#include "moduos/kernel/tty/tty.h"
#include "moduos/fs/devfs.h"
#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/memory/usercopy.h"
#include "moduos/kernel/errno.h"

#ifndef ENOTTY
#define ENOTTY 25
#endif
#define O_NOCTTY_FLAG   0x100
#define O_NONBLOCK_FLAG 0x800

extern pty_t *pty_by_index(int i);

enum { N_FIXED, N_CTTY, N_ACTIVE, N_PTMX, N_PTS };

typedef struct { int kind; int index; } tty_node_t;

typedef struct {
    int     role;        /* 0 = tty, 1 = pty master, 2 = dead */
    tty_t  *tty;
    pty_t  *pty;
    int     nonblock;
    int     counted;
    int     dead_err;
} tty_file_t;

#define N_NODES (TTY_NUM_VT + 4 + TTY_MAX_PTY)
static tty_node_t          g_nodes[N_NODES];
static devfs_device_ops_t  g_ops[N_NODES];
static char                g_names[N_NODES][8];

/* ---- open / close ---------------------------------------------------- */

static void acquire_ctty(process_t *cur, tty_t *t) {
    cur->controlling_tty = t->index + 1;
    t->session = cur->sid;
    t->fg_pgrp = cur->pgid;
}

static void *tty_dev_open(void *ctx, int flags) {
    tty_node_t *nd = (tty_node_t *)ctx;
    tty_file_t *f = (tty_file_t *)kzalloc(sizeof(*f));
    if (!f) return NULL;
    f->nonblock = (flags & O_NONBLOCK_FLAG) != 0;
    f->role = 2;
    f->dead_err = EIO;

    process_t *cur = process_get_current();
    uint64_t fl; tty_lock(&fl);

    tty_t *t = NULL;
    switch (nd->kind) {
        case N_FIXED:  t = tty_get(nd->index); break;
        case N_CTTY:   t = tty_for_process((struct process *)cur, 0); if (!t) f->dead_err = ENXIO; break;
        case N_ACTIVE: t = tty_get(vt_active() - 1); break;
        case N_PTS: {
            pty_t *p = pty_by_index(nd->index);
            if (p && p->allocated && !p->locked && p->slave) { t = p->slave; p->slave_seen = 1; }
            else f->dead_err = EIO;
            break;
        }
        case N_PTMX:
            tty_unlock(fl);
            f->pty = pty_alloc();
            tty_lock(&fl);
            if (f->pty) { f->role = 1; f->tty = f->pty->slave; }
            else f->dead_err = EBUSY;
            break;
    }

    if (nd->kind != N_PTMX && t) {
        f->role = 0;
        f->tty = t;
        t->open_count++;
        f->counted = 1;
        if (cur && cur->pid == cur->sid && cur->controlling_tty <= 0 &&
            !(flags & O_NOCTTY_FLAG) && t->session == 0)
            acquire_ctty(cur, t);
    }
    tty_unlock(fl);
    return f;
}

static int tty_dev_close(void *ctx) {
    tty_file_t *f = (tty_file_t *)ctx;
    if (!f) return 0;
    if (f->role == 1) {
        pty_master_closed(f->pty);
    } else if (f->counted && f->tty) {
        uint64_t fl; tty_lock(&fl);
        if (f->tty->open_count > 0) f->tty->open_count--;
        tty_unlock(fl);
    }
    kfree(f);
    return 0;
}

/* ---- read / write ---------------------------------------------------- */

static ssize_t tty_dev_read(void *ctx, void *buf, size_t n) {
    tty_file_t *f = (tty_file_t *)ctx;
    if (!f || f->role == 2) return -(f ? f->dead_err : EBADF);
    if (f->role == 1) return pty_master_read(f->pty, buf, n, f->nonblock);
    return tty_read(f->tty, buf, n, f->nonblock);
}

static ssize_t tty_dev_write(void *ctx, const void *buf, size_t n) {
    tty_file_t *f = (tty_file_t *)ctx;
    if (!f || f->role == 2) return -(f ? f->dead_err : EBADF);
    if (f->role == 1) return pty_master_write(f->pty, buf, n);
    return tty_write(f->tty, buf, n);
}

/* ---- ioctl ----------------------------------------------------------- */

static ssize_t tty_dev_invoke(void *ctx, const void *in_buf, size_t in_size,
                              void *out_buf, size_t out_size) {
    tty_file_t *f = (tty_file_t *)ctx;
    if (!f || f->role == 2) return -(f ? f->dead_err : EBADF);
    if (!in_buf || in_size < 8) return -EINVAL;

    tty_ioc_t io;
    size_t cp = in_size < sizeof(io) ? in_size : sizeof(io);
    memset(&io, 0, sizeof(io));
    if (usercopy_from_user(&io, in_buf, cp) != 0) return -EFAULT;

    process_t *cur = process_get_current();
    tty_t *t = f->tty;                       /* master ops act on the slave */
    int rc = 0;
    io.result = 0;

    uint64_t fl; tty_lock(&fl);
    switch (io.cmd) {
    case TTY_IOC_GETATTR:
        memcpy(io.data, &t->termios, sizeof(tty_termios_t)); break;
    case TTY_IOC_SETATTR:
    case TTY_IOC_SETATTR_FLUSH: {
        tty_termios_t nt;
        memcpy(&nt, io.data, sizeof(nt));
        tty_set_termios_locked(t, &nt, io.cmd == TTY_IOC_SETATTR_FLUSH);
        break;
    }
    case TTY_IOC_FLUSH_IN: tty_flush_input_locked(t); break;
    case TTY_IOC_SETCTTY:
        if (f->role == 1) { rc = -EINVAL; break; }
        if (!cur || cur->pid != cur->sid) { rc = -EPERM; break; }
        if (t->session && t->session != cur->sid) { rc = -EPERM; break; }
        acquire_ctty(cur, t);
        break;
    case TTY_IOC_GETPGRP: io.result = (int32_t)t->fg_pgrp; break;
    case TTY_IOC_SETPGRP: {
        int32_t pg; memcpy(&pg, io.data, sizeof(pg));
        if (pg < 0) rc = -EINVAL; else t->fg_pgrp = (uint32_t)pg;
        break;
    }
    case TTY_IOC_GETSID:  io.result = (int32_t)t->session; break;
    case TTY_IOC_GETWINSZ: memcpy(io.data, &t->winsize, sizeof(t->winsize)); break;
    case TTY_IOC_SETWINSZ: memcpy(&t->winsize, io.data, sizeof(t->winsize)); break;
    case TTY_IOC_NREAD:    io.result = (int32_t)t->rq_count; break;
    case TTY_IOC_NONBLOCK: { int32_t v; memcpy(&v, io.data, sizeof(v)); f->nonblock = v != 0; break; }
    case TTY_IOC_ISATTY:   io.result = 1; break;
    case TTY_IOC_GETINDEX:
        io.result = (t->kind == TTY_KIND_VT) ? (int32_t)t->index + 1
                                             : (int32_t)(t->index - TTY_NUM_VT);
        break;
    case PTY_IOC_GETPTN:
        if (f->role != 1) rc = -ENOTTY; else io.result = f->pty->index;
        break;
    case PTY_IOC_UNLOCK:
        if (f->role != 1) rc = -ENOTTY; else f->pty->locked = 0;
        break;

    case VT_IOC_GETSTATE: {
        tty_vt_state_t st = { (uint16_t)vt_active(), TTY_NUM_VT, vt_mode_mask(), 0 };
        memcpy(io.data, &st, sizeof(st));
        break;
    }
    case VT_IOC_ACTIVATE: {
        int32_t n; memcpy(&n, io.data, sizeof(n));
        if (vt_switch_locked(n) != 0) rc = -EINVAL;
        break;
    }
    case VT_IOC_WAITACTIVE: {
        int32_t n; memcpy(&n, io.data, sizeof(n));
        if (n < 1 || n > TTY_NUM_VT) { rc = -EINVAL; break; }
        while (vt_active() != n) {
            tty_unlock(fl);
            if (tty_signal_pending_current()) return -EINTR;
            tty_wait_a_bit();
            tty_lock(&fl);
        }
        break;
    }
    case VT_IOC_SETMODE: {
        int32_t m; memcpy(&m, io.data, sizeof(m));
        if (!t->vt) rc = -ENOTTY; else vt_set_mode_locked(t->vt, m);
        break;
    }
    case VT_IOC_GETMODE:
        if (!t->vt) rc = -ENOTTY; else io.result = vt_get_mode(t->vt);
        break;
    case VT_IOC_CLEAR:
        if (!t->vt) rc = -ENOTTY; else vt_clear_locked(t->vt);
        break;
    default:
        rc = -ENOTTY;
    }
    tty_unlock(fl);

    if (rc) return rc;
    if (out_buf && out_size) {
        size_t oc = out_size < sizeof(io) ? out_size : sizeof(io);
        if (usercopy_to_user(out_buf, &io, oc) != 0) return -EFAULT;
    }
    return 0;
}

/* ---- registration ---------------------------------------------------- */

static void add_node(int slot, const char *path_name, const char *base, int kind, int index) {
    strncpy(g_names[slot], base, sizeof(g_names[slot]) - 1);
    g_nodes[slot].kind = kind;
    g_nodes[slot].index = index;
    g_ops[slot] = (devfs_device_ops_t){
        .name = g_names[slot], .open = tty_dev_open, .read = tty_dev_read,
        .write = tty_dev_write, .close = tty_dev_close, .invoke = tty_dev_invoke,
    };
    devfs_owner_t owner = { .kind = DEVFS_OWNER_KERNEL, .id = "tty" };
    devfs_register_path(path_name, &g_ops[slot], &g_nodes[slot], owner);
}

static void itoa_small(int v, char *o) {
    char t[8]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    int i = 0; while (n) o[i++] = t[--n]; o[i] = 0;
}

void tty_dev_register(void) {
    devfs_owner_t owner = { .kind = DEVFS_OWNER_KERNEL, .id = "tty" };
    int s = 0;
    char num[8], path[24];

    for (int i = 0; i < TTY_NUM_VT; i++, s++) {
        itoa_small(i + 1, num);
        strcpy(path, "tty"); strcat(path, num);
        add_node(s, path, path, N_FIXED, i);
    }
    add_node(s++, "console", "console", N_FIXED, 0);
    add_node(s++, "tty",     "tty",     N_CTTY,  0);
    add_node(s++, "tty0",    "tty0",    N_ACTIVE, 0);
    add_node(s++, "ptmx",    "ptmx",    N_PTMX,  0);

    devfs_mkdir_p("pts", owner);
    for (int i = 0; i < TTY_MAX_PTY; i++, s++) {
        itoa_small(i, num);
        strcpy(path, "pts/"); strcat(path, num);
        add_node(s, path, num, N_PTS, i);
    }
}
