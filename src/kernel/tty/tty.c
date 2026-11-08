// SPDX-License-Identifier: GPL-2.0-only
//
// tty.c - TTY core: line discipline, read/write, job control, keyboard routing.
//
// Locking: one global spinlock (g_tty_lock) protects every tty, VT and pty.
// Functions ending in _locked expect it to be held. Blocking readers/writers
// drop it while they wait.

#include "moduos/kernel/tty/tty.h"
#include "moduos/kernel/process/process_new.h"
#include "moduos/kernel/spinlock.h"
#include "moduos/kernel/interrupts/irq_lock.h"
#include "moduos/kernel/interrupts/hlt_wait.h"
#include "moduos/arch/AMD64/interrupts/timer.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/errno.h"
#include "moduos/kernel/COM/com.h"

#ifndef ENOTTY
#define ENOTTY 25
#endif

#define SIG_INT_NUM   2
#define SIG_QUIT_NUM  3
#define SIG_HUP_NUM   1

static spinlock_t g_tty_lock;
static int        g_lock_ready;
static int        g_ready;

static tty_t     *g_tty[TTY_MAX];

/* ------------------------------------------------------------------ */
/* locking                                                             */
/* ------------------------------------------------------------------ */

static void lock_init_once(void) {
    if (!g_lock_ready) { spinlock_init(&g_tty_lock); g_lock_ready = 1; }
}

void tty_lock(uint64_t *flags)    { lock_init_once(); spinlock_lock_irqsave(&g_tty_lock, flags); }
void tty_unlock(uint64_t flags)   { spinlock_unlock_irqrestore(&g_tty_lock, flags); }

int tty_is_ready(void) { return g_ready; }

/* ------------------------------------------------------------------ */
/* table                                                               */
/* ------------------------------------------------------------------ */

tty_t *tty_get(int index) {
    if (index < 0 || index >= TTY_MAX) return NULL;
    tty_t *t = g_tty[index];
    return (t && t->in_use) ? t : NULL;
}

/* Used by vt.c / pty.c to install their tty objects. */
struct process *tty_current_process(void) { return (struct process *)process_get_current(); }

tty_t *tty_slot_alloc(int index, int kind) {
    if (index < 0 || index >= TTY_MAX) return NULL;
    tty_t *t = g_tty[index];
    if (!t) {
        t = (tty_t *)kzalloc(sizeof(tty_t));
        if (!t) return NULL;
        g_tty[index] = t;
    } else {
        memset(t, 0, sizeof(*t));
    }
    t->index  = index;
    t->kind   = kind;
    t->in_use = 1;

    t->termios.c_iflag = TTY_ICRNL;
    t->termios.c_oflag = TTY_OPOST | TTY_ONLCR;
    t->termios.c_cflag = TTY_CS8 | TTY_CREAD | TTY_CLOCAL;
    t->termios.c_lflag = TTY_ISIG | TTY_ICANON | TTY_ECHO | TTY_ECHOE |
                         TTY_ECHOK | TTY_ECHOCTL | TTY_IEXTEN;
    t->termios.c_cc[TTY_VINTR]    = 3;     /* ^C */
    t->termios.c_cc[TTY_VQUIT]    = 28;    /* ^\ */
    t->termios.c_cc[TTY_VERASE]   = 127;   /* DEL */
    t->termios.c_cc[TTY_VKILL]    = 21;    /* ^U */
    t->termios.c_cc[TTY_VEOF]     = 4;     /* ^D */
    t->termios.c_cc[TTY_VTIME]    = 0;
    t->termios.c_cc[TTY_VMIN]     = 1;
    t->termios.c_cc[TTY_VSTART]   = 17;
    t->termios.c_cc[TTY_VSTOP]    = 19;
    t->termios.c_cc[TTY_VSUSP]    = 26;    /* ^Z (no job stop support yet) */
    t->termios.c_cc[TTY_VREPRINT] = 18;    /* ^R */
    t->termios.c_cc[TTY_VWERASE]  = 23;    /* ^W */
    t->termios.c_cc[TTY_VLNEXT]   = 22;    /* ^V */

    t->winsize.ws_row = 25;
    t->winsize.ws_col = 80;
    return t;
}

tty_t *tty_for_process(struct process *pp, int fallback_console) {
    process_t *p = (process_t *)pp;
    if (p && p->controlling_tty > 0) {
        tty_t *t = tty_get(p->controlling_tty - 1);
        if (t) return t;
    }
    return fallback_console ? tty_get(0) : NULL;
}

/* ------------------------------------------------------------------ */
/* blocking helper                                                     */
/* ------------------------------------------------------------------ */

void tty_wait_a_bit(void) {
    process_yield();
    hlt_wait_preserve_if();
}

int tty_signal_pending_current(void);
int tty_signal_pending_current(void) {
    process_t *p = process_get_current();
    return p && (p->pending_signals & ~p->blocked_signals) != 0;
}

static int signal_pending(void) {
    process_t *p = process_get_current();
    return p && (p->pending_signals & ~p->blocked_signals) != 0;
}

/* ------------------------------------------------------------------ */
/* job control: signals / hangup                                       */
/* ------------------------------------------------------------------ */

static int proc_alive(process_t *p) {
    return p && p->state != PROCESS_STATE_UNUSED &&
           p->state != PROCESS_STATE_EMBRYO &&
           p->state != PROCESS_STATE_ZOMBIE;
}

int tty_has_users(tty_t *t) {
    if (!t) return 0;
    if (t->open_count > 0) return 1;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = process_table[i];
        if (proc_alive(p) && p->controlling_tty == t->index + 1) return 1;
    }
    return 0;
}

/*
 * Deliver sig to the foreground set of t:
 *   - every live process whose controlling tty is t,
 *   - restricted to fg_pgrp when one has been set,
 *   - never PID 1,
 *   - the session leader (login/getty/terminal shell) is skipped when
 *     there is anything else to signal, so ^C kills the running program
 *     instead of the thing that respawns your shell.
 */
void tty_signal_group_locked(tty_t *t, int sig) {
    int others = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = process_table[i];
        if (!proc_alive(p) || p->controlling_tty != t->index + 1 || p->pid == 1) continue;
        if (t->fg_pgrp && p->pgid != t->fg_pgrp) continue;
        if (p->pid != p->sid) others++;
    }
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = process_table[i];
        if (!proc_alive(p) || p->controlling_tty != t->index + 1 || p->pid == 1) continue;
        if (t->fg_pgrp && p->pgid != t->fg_pgrp) continue;
        if (p->pid == p->sid && others) continue;
        (void)send_signal(p->pid, sig);
    }
}

void tty_hangup_locked(tty_t *t) {
    if (!t) return;
    t->hung_up = 1;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = process_table[i];
        if (!proc_alive(p) || p->controlling_tty != t->index + 1 || p->pid == 1) continue;
        (void)send_signal(p->pid, SIG_HUP_NUM);
    }
}

void tty_proc_exit(struct process *pp) {
    process_t *p = (process_t *)pp;
    if (!p || p->controlling_tty <= 0 || p->pid != p->sid) return;
    uint64_t f; tty_lock(&f);
    tty_t *t = tty_get(p->controlling_tty - 1);
    if (t && t->session == p->sid) {
        for (int i = 0; i < MAX_PROCESSES; i++) {
            process_t *q = process_table[i];
            if (!proc_alive(q) || q == p || q->controlling_tty != t->index + 1 || q->pid == 1) continue;
            (void)send_signal(q->pid, SIG_HUP_NUM);
        }
        t->session = 0;
        t->fg_pgrp = 0;
    }
    tty_unlock(f);
}

/* ------------------------------------------------------------------ */
/* output                                                              */
/* ------------------------------------------------------------------ */

size_t tty_output_locked(tty_t *t, const uint8_t *buf, size_t n) {
    if (!t || !n) return 0;
    if (t->kind == TTY_KIND_VT)       vt_write_locked(t->vt, buf, n);
    else if (t->kind == TTY_KIND_PTS) pty_slave_output_locked(t, buf, n);
    return n;
}

void tty_write_out_locked(tty_t *t, const uint8_t *buf, size_t n) {
    if (!(t->termios.c_oflag & TTY_OPOST)) { tty_output_locked(t, buf, n); return; }

    uint8_t out[256];
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = buf[i];
        if (c == '\n' && (t->termios.c_oflag & TTY_ONLCR)) {
            out[o++] = '\r'; out[o++] = '\n';
        } else if (c == '\r' && (t->termios.c_oflag & TTY_OCRNL)) {
            out[o++] = '\n';
        } else {
            out[o++] = c;
        }
        if (o >= sizeof(out) - 2) { tty_output_locked(t, out, o); o = 0; }
    }
    if (o) tty_output_locked(t, out, o);
}

static void echo_bytes(tty_t *t, const char *s, size_t n) {
    tty_write_out_locked(t, (const uint8_t *)s, n);
}

static void echo_char(tty_t *t, uint8_t c) {
    if (!(t->termios.c_lflag & TTY_ECHO)) {
        if (c == '\n' && (t->termios.c_lflag & TTY_ECHONL)) echo_bytes(t, "\n", 1);
        return;
    }
    if (c == '\n' || c == '\t') { echo_bytes(t, (const char *)&c, 1); return; }
    if ((t->termios.c_lflag & TTY_ECHOCTL) && (c < 32 || c == 127)) {
        char s[2] = { '^', (char)(c == 127 ? '?' : c + 64) };
        echo_bytes(t, s, 2);
        return;
    }
    echo_bytes(t, (const char *)&c, 1);
}

/* ------------------------------------------------------------------ */
/* line discipline                                                     */
/* ------------------------------------------------------------------ */

static void rq_put(tty_t *t, uint8_t c, int marker) {
    t->rq[t->rq_w] = (uint16_t)c | (marker ? 0x100 : 0);
    t->rq_w = (t->rq_w + 1) & (TTY_RQ_SIZE - 1);
    t->rq_count++;
}

void tty_flush_input_locked(tty_t *t) {
    t->rq_r = t->rq_w = t->rq_count = 0;
    t->rq_lines = 0;
    t->eof_pending = 0;
    t->line_len = 0;
    t->lnext = 0;
}

static void commit_line(tty_t *t) {
    if (t->line_len == 0) return;
    if (TTY_RQ_SIZE - t->rq_count < t->line_len) {   /* no room: drop the line */
        t->line_len = 0;
        return;
    }
    for (uint32_t i = 0; i < t->line_len; i++)
        rq_put(t, t->line[i], i + 1 == t->line_len);
    t->rq_lines++;
    t->line_len = 0;
}

/* remove the last character (whole UTF-8 sequence); returns 1 if removed */
static int line_erase_char(tty_t *t, uint8_t *removed) {
    if (!t->line_len) return 0;
    uint8_t c = t->line[--t->line_len];
    while ((c & 0xC0) == 0x80 && t->line_len) c = t->line[--t->line_len];
    if (removed) *removed = c;
    return 1;
}

static void visual_erase(tty_t *t, uint8_t c) {
    if (!(t->termios.c_lflag & TTY_ECHO)) return;
    int cols = 1;
    if ((t->termios.c_lflag & TTY_ECHOCTL) && (c < 32 || c == 127) && c != '\t') cols = 2;
    for (int i = 0; i < cols; i++) echo_bytes(t, "\b \b", 3);
}

static void line_append(tty_t *t, uint8_t c) {
    if (t->line_len + 1 >= TTY_LINE_MAX) return;     /* keep room for '\n' */
    t->line[t->line_len++] = c;
    echo_char(t, c);
}

static void ldisc_char(tty_t *t, uint8_t c) {
    tty_termios_t *tm = &t->termios;
    uint32_t lf = tm->c_lflag;

    if (tm->c_iflag & TTY_ISTRIP) c &= 0x7F;

    /* literal-next: take the next byte verbatim */
    if (t->lnext) {
        t->lnext = 0;
        if (lf & TTY_ICANON) line_append(t, c);
        else if (TTY_RQ_SIZE - t->rq_count > 0) rq_put(t, c, 0);
        return;
    }

    /* signal characters */
    if (lf & TTY_ISIG) {
        int sig = 0;
        if (tm->c_cc[TTY_VINTR] && c == tm->c_cc[TTY_VINTR])      sig = SIG_INT_NUM;
        else if (tm->c_cc[TTY_VQUIT] && c == tm->c_cc[TTY_VQUIT]) sig = SIG_QUIT_NUM;
        if (sig) {
            if (!(lf & TTY_NOFLSH)) tty_flush_input_locked(t);
            echo_char(t, c);
            echo_bytes(t, "\n", 1);
            tty_signal_group_locked(t, sig);
            return;
        }
    }

    /* CR/NL translation */
    if (c == '\r') {
        if (tm->c_iflag & TTY_IGNCR) return;
        if (tm->c_iflag & TTY_ICRNL) c = '\n';
    } else if (c == '\n' && (tm->c_iflag & TTY_INLCR)) {
        c = '\r';
    }

    if (!(lf & TTY_ICANON)) {
        if (TTY_RQ_SIZE - t->rq_count > 0) rq_put(t, c, 0);
        echo_char(t, c);
        return;
    }

    /* ---- canonical mode ---- */

    if ((lf & TTY_IEXTEN) && tm->c_cc[TTY_VLNEXT] && c == tm->c_cc[TTY_VLNEXT]) {
        t->lnext = 1;
        if (lf & TTY_ECHO) echo_bytes(t, "^\b", 2);
        return;
    }

    if (c == tm->c_cc[TTY_VERASE] || c == 0x08 || c == 0x7F) {
        uint8_t gone;
        if (line_erase_char(t, &gone)) {
            if (lf & TTY_ECHOE) visual_erase(t, gone);
            else if (lf & TTY_ECHO) echo_char(t, c);
        }
        return;
    }

    if (tm->c_cc[TTY_VKILL] && c == tm->c_cc[TTY_VKILL]) {
        uint8_t gone;
        if (lf & TTY_ECHOE) {
            while (line_erase_char(t, &gone)) visual_erase(t, gone);
        } else {
            t->line_len = 0;
            if (lf & TTY_ECHOK) echo_bytes(t, "\n", 1);
        }
        return;
    }

    if (tm->c_cc[TTY_VWERASE] && c == tm->c_cc[TTY_VWERASE] && (lf & TTY_IEXTEN)) {
        uint8_t gone;
        while (t->line_len && t->line[t->line_len - 1] == ' ') {
            line_erase_char(t, &gone);
            if (lf & TTY_ECHOE) visual_erase(t, gone);
        }
        while (t->line_len && t->line[t->line_len - 1] != ' ') {
            line_erase_char(t, &gone);
            if (lf & TTY_ECHOE) visual_erase(t, gone);
        }
        return;
    }

    if (tm->c_cc[TTY_VREPRINT] && c == tm->c_cc[TTY_VREPRINT] && (lf & TTY_IEXTEN)) {
        if (lf & TTY_ECHO) {
            echo_bytes(t, "^R\n", 3);
            echo_bytes(t, (const char *)t->line, t->line_len);
        }
        return;
    }

    if (tm->c_cc[TTY_VEOF] && c == tm->c_cc[TTY_VEOF]) {
        if (t->line_len == 0) t->eof_pending++;
        else commit_line(t);          /* line without terminator */
        return;
    }

    if (c == '\n' || (tm->c_cc[TTY_VEOL] && c == tm->c_cc[TTY_VEOL])) {
        if (t->line_len + 1 >= TTY_LINE_MAX) t->line_len = TTY_LINE_MAX - 2;
        t->line[t->line_len++] = c;
        if (c == '\n') { if (lf & (TTY_ECHO | TTY_ECHONL)) echo_bytes(t, "\n", 1); }
        else echo_char(t, c);
        commit_line(t);
        return;
    }

    line_append(t, c);
}

void tty_receive_locked(tty_t *t, const uint8_t *buf, size_t n) {
    if (!t || t->hung_up) return;
    for (size_t i = 0; i < n; i++) ldisc_char(t, buf[i]);
}

void tty_set_termios_locked(tty_t *t, const tty_termios_t *nt, int flush) {
    int was_canon = (t->termios.c_lflag & TTY_ICANON) != 0;
    t->termios = *nt;
    int is_canon = (t->termios.c_lflag & TTY_ICANON) != 0;

    if (flush) {
        tty_flush_input_locked(t);
    } else if (was_canon && !is_canon) {
        /* hand the half-typed line to raw readers */
        for (uint32_t i = 0; i < t->line_len; i++)
            if (TTY_RQ_SIZE - t->rq_count > 0) rq_put(t, t->line[i], 0);
        t->line_len = 0;
        t->rq_lines = 0;
        t->eof_pending = 0;
    } else if (!was_canon && is_canon) {
        tty_flush_input_locked(t);
    }
}

/* ------------------------------------------------------------------ */
/* read / write                                                        */
/* ------------------------------------------------------------------ */

ssize_t tty_read(tty_t *t, void *kbuf, size_t n, int nonblock) {
    if (!t || !kbuf) return -EINVAL;
    if (!n) return 0;

    uint8_t *out = (uint8_t *)kbuf;
    uint64_t deadline = 0;
    int have_deadline = 0;

    for (;;) {
        uint64_t f; tty_lock(&f);

        if (t->termios.c_lflag & TTY_ICANON) {
            if (t->rq_lines) {
                size_t i = 0;
                while (i < n && t->rq_count) {
                    uint16_t v = t->rq[t->rq_r];
                    t->rq_r = (t->rq_r + 1) & (TTY_RQ_SIZE - 1);
                    t->rq_count--;
                    out[i++] = (uint8_t)(v & 0xFF);
                    if (v & 0x100) { t->rq_lines--; break; }
                }
                tty_unlock(f);
                return (ssize_t)i;
            }
            if (t->eof_pending) { t->eof_pending--; tty_unlock(f); return 0; }
        } else {
            uint32_t vmin  = t->termios.c_cc[TTY_VMIN];
            uint32_t vtime = t->termios.c_cc[TTY_VTIME];
            uint32_t need  = vmin > n ? (uint32_t)n : vmin;

            if (t->rq_count && t->rq_count >= need) {
                size_t i = 0;
                while (i < n && t->rq_count) {
                    uint16_t v = t->rq[t->rq_r];
                    t->rq_r = (t->rq_r + 1) & (TTY_RQ_SIZE - 1);
                    t->rq_count--;
                    out[i++] = (uint8_t)(v & 0xFF);
                }
                tty_unlock(f);
                return (ssize_t)i;
            }
            if (vmin == 0) {
                if (vtime == 0) { tty_unlock(f); return 0; }
                if (!have_deadline) {
                    deadline = get_system_ticks() + ms_to_ticks((uint64_t)vtime * 100);
                    have_deadline = 1;
                } else if (get_system_ticks() >= deadline) {
                    tty_unlock(f);
                    return 0;
                }
            }
        }

        if (t->hung_up) { tty_unlock(f); return 0; }
        tty_unlock(f);

        if (nonblock) return -EAGAIN;
        if (signal_pending()) return -EINTR;
        tty_wait_a_bit();
    }
}

ssize_t tty_write(tty_t *t, const void *kbuf, size_t n) {
    if (!t || (!kbuf && n)) return -EINVAL;
    const uint8_t *src = (const uint8_t *)kbuf;
    size_t done = 0;

    while (done < n) {
        size_t chunk = n - done;
        if (chunk > 128) chunk = 128;

        uint64_t f; tty_lock(&f);

        if (t->hung_up) { tty_unlock(f); return done ? (ssize_t)done : -EIO; }

        if (t->kind == TTY_KIND_PTS && t->pty) {
            pty_t *pp = t->pty;
            if (!pp->master_open) { tty_unlock(f); return done ? (ssize_t)done : -EIO; }
            if (TTY_PTY_BUF - pp->mq_count < chunk * 2) {       /* master queue full */
                tty_unlock(f);
                if (signal_pending()) return done ? (ssize_t)done : -EINTR;
                tty_wait_a_bit();
                continue;
            }
        }

        tty_write_out_locked(t, src + done, chunk);
        tty_unlock(f);
        done += chunk;
    }
    return (ssize_t)done;
}

/* fd 0/1/2 of a process */
ssize_t tty_proc_read(struct process *p, void *kbuf, size_t n) {
    tty_t *t = tty_for_process(p, 1);
    if (!t) return -ENOTTY;
    return tty_read(t, kbuf, n, 0);
}

ssize_t tty_proc_write(struct process *p, const void *kbuf, size_t n) {
    tty_t *t = tty_for_process(p, 1);
    if (!t) return -ENOTTY;
    return tty_write(t, kbuf, n);
}

/* ------------------------------------------------------------------ */
/* sessions / process groups                                           */
/* ------------------------------------------------------------------ */

int tty_sys_setsid(void) {
    process_t *p = process_get_current();
    if (!p) return -ESRCH;
    if (p->pgid == p->pid && p->pid != 0 && p->sid == p->pid) return -EPERM;
    p->sid = p->pid;
    p->pgid = p->pid;
    p->controlling_tty = TTY_CTTY_NONE;
    return (int)p->pid;
}

int tty_sys_setpgid(int pid, int pgid) {
    process_t *self = process_get_current();
    if (!self) return -ESRCH;
    process_t *p = pid ? process_find((uint32_t)pid) : self;
    if (!p || !proc_alive(p)) return -ESRCH;
    if (p != self && p->ppid != self->pid) return -ESRCH;
    if (p->sid != self->sid && p != self) return -EPERM;
    if (p->pid == p->sid && p->sid != 0) return -EPERM;       /* session leader */
    if (pgid < 0) return -EINVAL;
    p->pgid = pgid ? (uint32_t)pgid : p->pid;
    return 0;
}

int tty_sys_getpgid(int pid) {
    process_t *self = process_get_current();
    process_t *p = pid ? process_find((uint32_t)pid) : self;
    if (!p || !proc_alive(p)) return -ESRCH;
    return (int)p->pgid;
}

int tty_sys_getsid(int pid) {
    process_t *self = process_get_current();
    process_t *p = pid ? process_find((uint32_t)pid) : self;
    if (!p || !proc_alive(p)) return -ESRCH;
    return (int)p->sid;
}

/* ------------------------------------------------------------------ */
/* keyboard                                                            */
/* ------------------------------------------------------------------ */

static uint8_t g_mods;         /* bit0 shift, bit1 ctrl, bit2 alt, bit3 super */
#define M_SHIFT 1
#define M_CTRL  2
#define M_ALT   4
#define M_SUPER 8
static int g_swallow_release = -1;   /* keycode whose release we swallow after a hotkey */

static void track_mods(const Event *e) {
    int down = (e->type == EVENT_KEY_PRESSED);
    uint8_t bit = 0;
    switch (e->data.keyboard.keycode) {
        case KEY_LEFT_SHIFT:  case KEY_RIGHT_SHIFT: bit = M_SHIFT; break;
        case KEY_LEFT_CTRL:   case KEY_RIGHT_CTRL:  bit = M_CTRL;  break;
        case KEY_LEFT_ALT:    case KEY_RIGHT_ALT:   bit = M_ALT;   break;
        case KEY_LEFT_SUPER:  case KEY_RIGHT_SUPER: bit = M_SUPER; break;
        default: return;
    }
    if (down) g_mods |= bit; else g_mods &= (uint8_t)~bit;
}

static const char *fkey_seq(KeyCode k) {
    switch (k) {
        case KEY_F1:  return "\x1bOP";    case KEY_F2:  return "\x1bOQ";
        case KEY_F3:  return "\x1bOR";    case KEY_F4:  return "\x1bOS";
        case KEY_F5:  return "\x1b[15~";  case KEY_F6:  return "\x1b[17~";
        case KEY_F7:  return "\x1b[18~";  case KEY_F8:  return "\x1b[19~";
        case KEY_F9:  return "\x1b[20~";  case KEY_F10: return "\x1b[21~";
        case KEY_F11: return "\x1b[23~";  case KEY_F12: return "\x1b[24~";
        default: return NULL;
    }
}

static const char *nav_seq(KeyCode k) {
    switch (k) {
        case KEY_ARROW_UP:    return "\x1b[A";   case KEY_ARROW_DOWN:  return "\x1b[B";
        case KEY_ARROW_RIGHT: return "\x1b[C";   case KEY_ARROW_LEFT:  return "\x1b[D";
        case KEY_HOME:        return "\x1b[H";   case KEY_END:         return "\x1b[F";
        case KEY_INSERT:      return "\x1b[2~";  case KEY_DELETE:      return "\x1b[3~";
        case KEY_PAGE_UP:     return "\x1b[5~";  case KEY_PAGE_DOWN:   return "\x1b[6~";
        default: return NULL;
    }
}

static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

/* Translate one key press into terminal bytes and run it through the line
 * discipline of the active VT. */
static void feed_key_locked(tty_t *t, const Event *e) {
    KeyCode k = e->data.keyboard.keycode;
    uint8_t buf[8];
    size_t n = 0;

    /* SHIFT+PgUp/PgDn: scrollback, never reaches the program */
    if ((g_mods & M_SHIFT) && (k == KEY_PAGE_UP || k == KEY_PAGE_DOWN)) {
        if (t->vt) vt_scroll_view_locked(t->vt, k == KEY_PAGE_UP ? (t->winsize.ws_row / 2 + 1)
                                                                : -(t->winsize.ws_row / 2 + 1));
        return;
    }

    const char *seq = nav_seq(k);
    if (!seq) seq = fkey_seq(k);
    if (seq) {
        while (*seq && n < sizeof(buf)) buf[n++] = (uint8_t)*seq++;
        /* escape sequences bypass canonical editing keys but still go through
         * the discipline so raw-mode programs and ECHO behave consistently */
        tty_receive_locked(t, buf, n);
        return;
    }

    char c = 0;
    switch (k) {
        case KEY_ENTER:     c = '\r';   break;
        case KEY_BACKSPACE: c = 0x7F;   break;
        case KEY_TAB:       c = '\t';   break;
        case KEY_ESCAPE:    c = 0x1B;   break;
        case KEY_SPACE:     c = ' ';    break;
        default:            c = e->data.keyboard.ascii; break;
    }
    if (!c) return;

    if ((e->data.keyboard.modifiers & MOD_CAPS) && is_alpha(c) && !(g_mods & M_SHIFT)) {
        /* ps2 layer folds SHIFT into ascii but not CAPS LOCK */
        c = (c >= 'a' && c <= 'z') ? (char)(c - 32) : (char)(c + 32);
    }

    if (g_mods & M_CTRL) {
        if (c >= 'a' && c <= 'z')      c = (char)(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
        else if (c == ' ' || c == '@') c = 0;
        else if (c >= '[' && c <= '_') c = (char)(c - '@');
        else if (c == '?')             c = 0x7F;
    }

    if ((g_mods & M_ALT) && !(g_mods & M_CTRL)) buf[n++] = 0x1B;   /* meta prefix */
    buf[n++] = (uint8_t)c;
    tty_receive_locked(t, buf, n);
}

unsigned tty_input_event(const Event *e) {
    if (!e) return TTY_EV_PASS;
    if (e->type != EVENT_KEY_PRESSED && e->type != EVENT_KEY_RELEASED) return TTY_EV_PASS;
    if (!g_ready) return TTY_EV_PASS;

    uint64_t f; tty_lock(&f);
    unsigned flags = TTY_EV_PASS;

    track_mods(e);

    KeyCode k = e->data.keyboard.keycode;

    /* SHIFT + SUPER + ALT + F1..Fn  ->  switch VT */
    if (k >= KEY_F1 && k <= KEY_F12) {
        int n = (int)(k - KEY_F1) + 1;
        if (e->type == EVENT_KEY_PRESSED &&
            (g_mods & (M_SHIFT | M_SUPER | M_ALT)) == (M_SHIFT | M_SUPER | M_ALT) &&
            n <= TTY_NUM_VT) {
            vt_switch_locked(n);
            g_swallow_release = (int)k;
            tty_unlock(f);
            return TTY_EV_NO_KBD0 | TTY_EV_NO_EVENT0;
        }
        if (e->type == EVENT_KEY_RELEASED && g_swallow_release == (int)k) {
            g_swallow_release = -1;
            tty_unlock(f);
            return TTY_EV_NO_KBD0 | TTY_EV_NO_EVENT0;
        }
    }

    tty_t *t = tty_get(vt_active() - 1);
    if (!t || !t->vt || vt_get_mode(t->vt) == KD_GRAPHICS) {
        /* graphics VT: raw delivery (event0 + kbd0), exactly as before */
        tty_unlock(f);
        return TTY_EV_PASS;
    }

    if (e->type == EVENT_KEY_PRESSED) feed_key_locked(t, e);

    /* A text VT is on screen. Keep the legacy byte stream quiet; hide raw
     * events from GUI clients living on another VT. */
#ifndef TTY_LEGACY_KBD0
    flags |= TTY_EV_NO_KBD0;
#endif
    if (vt_mode_mask()) flags |= TTY_EV_NO_EVENT0;

    tty_unlock(f);
    return flags;
}

/* ------------------------------------------------------------------ */
/* init                                                                */
/* ------------------------------------------------------------------ */

void tty_init(void) {
    lock_init_once();
    vt_init_all();
    pty_init();
    tty_dev_register();
    g_ready = 1;
    vt_set_ready(1);
    com_write_string(COM1_PORT, "[TTY] ready: ");
    com_write_string(COM1_PORT, "VT1..VT");
    char d[2] = { (char)('0' + TTY_NUM_VT), 0 };
    com_write_string(COM1_PORT, d);
    com_write_string(COM1_PORT, " + ptmx, switch with SHIFT+SUPER+ALT+F1..F");
    com_write_string(COM1_PORT, d);
    com_write_string(COM1_PORT, "\n");
}

void tty_display_changed(void) {
    if (!g_ready) return;
    uint64_t f; tty_lock(&f);
    vt_geometry_changed_locked();
    tty_unlock(f);
}

void tty_panic_release(void) {
    /* No locking: the machine is going down and the lock holder may be dead. */
    g_ready = 0;
    vt_set_ready(0);
}
