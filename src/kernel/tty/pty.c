// SPDX-License-Identifier: GPL-2.0-only
//
// pty.c - pseudo terminals. A terminal emulator opens $/dev/ptmx (master),
// asks for the slave number, then starts a shell with that slave as its
// controlling tty. Keys written to the master go through the slave's line
// discipline; everything the slave writes is read back from the master.

#include "moduos/kernel/tty/tty.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/errno.h"

static pty_t *g_pty[TTY_MAX_PTY];

void pty_init(void) {
    for (int i = 0; i < TTY_MAX_PTY; i++) g_pty[i] = NULL;
}

pty_t *pty_by_index(int i);
pty_t *pty_by_index(int i) {
    if (i < 0 || i >= TTY_MAX_PTY) return NULL;
    return g_pty[i];
}

pty_t *pty_alloc(void) {
    uint64_t f; tty_lock(&f);
    for (int i = 0; i < TTY_MAX_PTY; i++) {
        pty_t *p = g_pty[i];
        if (p && (p->allocated || (p->slave && tty_has_users(p->slave)))) continue;
        if (!p) {
            p = (pty_t *)kzalloc(sizeof(pty_t));
            if (!p) break;
            g_pty[i] = p;
        }
        memset(p, 0, sizeof(*p));
        p->slave = tty_slot_alloc(TTY_NUM_VT + i, TTY_KIND_PTS);
        if (!p->slave) break;
        p->slave->pty = p;
        p->index = i;
        p->allocated = 1;
        p->locked = 1;
        p->master_open = 1;
        tty_unlock(f);
        return p;
    }
    tty_unlock(f);
    return NULL;
}

void pty_slave_output_locked(tty_t *slave, const uint8_t *buf, size_t n) {
    pty_t *p = slave->pty;
    if (!p || !p->master_open) return;
    for (size_t i = 0; i < n; i++) {
        if (p->mq_count >= TTY_PTY_BUF) return;          /* echo overflow: drop */
        p->mq[p->mq_w] = buf[i];
        p->mq_w = (p->mq_w + 1) & (TTY_PTY_BUF - 1);
        p->mq_count++;
    }
}

ssize_t pty_master_read(pty_t *p, void *kbuf, size_t n, int nonblock) {
    if (!p || !kbuf) return -EINVAL;
    if (!n) return 0;
    uint8_t *out = (uint8_t *)kbuf;
    extern int tty_signal_pending_current(void);

    for (;;) {
        uint64_t f; tty_lock(&f);
        if (p->mq_count) {
            size_t i = 0;
            while (i < n && p->mq_count) {
                out[i++] = p->mq[p->mq_r];
                p->mq_r = (p->mq_r + 1) & (TTY_PTY_BUF - 1);
                p->mq_count--;
            }
            tty_unlock(f);
            return (ssize_t)i;
        }
        if (p->slave && tty_has_users(p->slave)) p->slave_seen = 1;
        if (p->slave_seen && !(p->slave && tty_has_users(p->slave))) { tty_unlock(f); return 0; }
        tty_unlock(f);

        if (nonblock) return -EAGAIN;
        if (tty_signal_pending_current()) return -EINTR;
        tty_wait_a_bit();
    }
}

ssize_t pty_master_write(pty_t *p, const void *kbuf, size_t n) {
    if (!p || (!kbuf && n)) return -EINVAL;
    const uint8_t *src = (const uint8_t *)kbuf;
    size_t done = 0;
    while (done < n) {
        size_t chunk = n - done;
        if (chunk > 128) chunk = 128;
        uint64_t f; tty_lock(&f);
        tty_receive_locked(p->slave, src + done, chunk);
        tty_unlock(f);
        done += chunk;
    }
    return (ssize_t)done;
}

void pty_master_closed(pty_t *p) {
    if (!p) return;
    uint64_t f; tty_lock(&f);
    p->master_open = 0;
    if (p->slave) tty_hangup_locked(p->slave);
    p->allocated = 0;
    tty_unlock(f);
}
