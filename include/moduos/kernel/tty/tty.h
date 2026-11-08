// SPDX-License-Identifier: GPL-2.0-only
//
// tty.h - ModuOS kernel TTY subsystem.
//
//   keyboard ──► tty_input_event() ──► line discipline ──► read queue ──► read(fd 0)
//                                          │ echo
//   write(fd 1/2) ──► opost ──► output sink ──┬─► VT   : VT100 cell grid ─► framebuffer
//                                             └─► PTS  : pty master queue ─► terminal app
//
// Virtual consoles (VT1..VTn) are switched with SHIFT+SUPER+ALT+F1..Fn.
// Pseudo terminals (pty) give user-space terminal emulators the same
// line discipline / session / job-control behaviour a VT has.

#ifndef MODUOS_KERNEL_TTY_TTY_H
#define MODUOS_KERNEL_TTY_TTY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "moduos/kernel/tty/tty_abi.h"
#include "moduos/kernel/events/events.h"

/* Same definition as fs/fd.h (identical typedef redeclaration is valid C11). */
typedef int64_t ssize_t;

#ifdef __cplusplus
extern "C" {
#endif

/* ---- configuration ------------------------------------------------ */

#ifndef TTY_NUM_VT
#define TTY_NUM_VT      4        /* SHIFT+SUPER+ALT+F1..F4 */
#endif
#define TTY_MAX_PTY     16
#define TTY_MAX         (TTY_NUM_VT + TTY_MAX_PTY)

#define TTY_RQ_SIZE     4096     /* read queue entries (power of two)   */
#define TTY_LINE_MAX    2048     /* canonical edit buffer               */
#define TTY_PTY_BUF     4096     /* pty master queue (power of two)     */

/* process_t::controlling_tty is (tty index + 1); 0 / -1 mean "none". */
#define TTY_CTTY_NONE        0
#define TTY_CTTY_CONSOLE     1   /* VT1 */
#define TTY_CTTY_FROM_INDEX(i) ((i) + 1)

/* ---- return flags of tty_input_event() ---------------------------- */

#define TTY_EV_PASS          0           /* deliver as before (kbd0 + event0) */
#define TTY_EV_NO_KBD0       (1u << 0)   /* do not feed the legacy kbd0 byte stream */
#define TTY_EV_NO_EVENT0     (1u << 1)   /* do not feed $/dev/input/event0          */

/* ---- core types --------------------------------------------------- */

typedef struct tty  tty_t;
typedef struct vt   vt_t;
typedef struct pty  pty_t;

enum { TTY_KIND_VT = 1, TTY_KIND_PTS = 2 };

struct tty {
    int            index;            /* slot in the global table             */
    int            kind;
    int            in_use;

    tty_termios_t  termios;
    tty_winsize_t  winsize;

    /* read queue: low byte = data, bit 8 = "last byte of a line" marker */
    uint16_t       rq[TTY_RQ_SIZE];
    uint32_t       rq_r, rq_w, rq_count;
    uint32_t       rq_lines;         /* complete canonical lines queued      */
    uint32_t       eof_pending;      /* ^D on an empty line                  */

    /* canonical edit line */
    uint8_t        line[TTY_LINE_MAX];
    uint32_t       line_len;
    int            lnext;            /* ^V pending                           */

    /* job control */
    uint32_t       session;          /* sid that owns this tty (0 = none)    */
    uint32_t       fg_pgrp;          /* foreground pgrp (0 = unset)          */

    int            open_count;       /* open file objects                    */
    int            hung_up;

    vt_t          *vt;
    pty_t         *pty;
};

struct pty {
    int       allocated;
    int       locked;                /* slave not yet unlocked               */
    int       master_open;
    int       slave_seen;            /* slave was opened / made ctty at least once */
    int       index;                 /* pts/N                                */
    tty_t    *slave;
    uint8_t   mq[TTY_PTY_BUF];       /* slave output, read by the master     */
    uint32_t  mq_r, mq_w, mq_count;
};

/* ---- lifecycle ---------------------------------------------------- */

/* Call once after devfs/input/VGA are up and before PID 1 is created. */
void tty_init(void);
int  tty_is_ready(void);

/* Re-read the display geometry (mode change / font change) and repaint. */
void tty_display_changed(void);

/* Give the screen back to the raw VGA console (panic path). */
void tty_panic_release(void);

/* ---- keyboard ----------------------------------------------------- */

/* Called for every input event before it reaches kbd0/event0.
 * Returns TTY_EV_* flags. */
unsigned tty_input_event(const Event *e);

/* ---- process stdio (fd 0/1/2) ------------------------------------- */

struct process;
ssize_t tty_proc_read (struct process *p, void *kbuf, size_t n);
ssize_t tty_proc_write(struct process *p, const void *kbuf, size_t n);

/* Session leader exited: hang up its controlling tty. */
void tty_proc_exit(struct process *p);

/* ---- session syscalls (setsid & co) ------------------------------- */

int tty_sys_setsid(void);
int tty_sys_setpgid(int pid, int pgid);
int tty_sys_getpgid(int pid);
int tty_sys_getsid(int pid);

/* ---- virtual consoles --------------------------------------------- */

int  vt_switch(int vt_number_1based);
int  vt_active(void);                       /* 1-based */
void vt_graphics_client_open(void);         /* mvc device opened  */
void vt_graphics_client_close(void);        /* mvc device closed  */

/* ---- internal (tty.c <-> vt.c <-> pty.c <-> tty_dev.c) ------------ */

tty_t *tty_get(int index);
tty_t *tty_slot_alloc(int index, int kind);
struct process *tty_current_process(void);
tty_t *tty_for_process(struct process *p, int fallback_console);

void   tty_lock(uint64_t *flags);
void   tty_unlock(uint64_t flags);

/* All *_locked functions require the tty lock to be held. */
void   tty_receive_locked(tty_t *t, const uint8_t *buf, size_t n);
size_t tty_output_locked (tty_t *t, const uint8_t *buf, size_t n); /* after OPOST */
void   tty_write_out_locked(tty_t *t, const uint8_t *buf, size_t n); /* runs OPOST  */
void   tty_flush_input_locked(tty_t *t);
void   tty_set_termios_locked(tty_t *t, const tty_termios_t *nt, int flush);
void   tty_signal_group_locked(tty_t *t, int sig);
void   tty_hangup_locked(tty_t *t);
int    tty_has_users(tty_t *t);

ssize_t tty_read (tty_t *t, void *kbuf, size_t n, int nonblock);
ssize_t tty_write(tty_t *t, const void *kbuf, size_t n);
void    tty_wait_a_bit(void);
int     tty_signal_pending_current(void);

/* vt.c */
void   vt_init_all(void);
void   vt_write_locked(vt_t *v, const uint8_t *buf, size_t n);
void   vt_scroll_view_locked(vt_t *v, int delta_lines);
void   vt_clear_locked(vt_t *v);
int    vt_get_mode(vt_t *v);
void   vt_set_mode_locked(vt_t *v, int kd_mode);
int    vt_number(vt_t *v);                  /* 1-based */
int    vt_switch_locked(int n);
void   vt_redraw_active_locked(void);
void   vt_geometry_changed_locked(void);
void   vt_set_ready(int ready);
uint16_t vt_mode_mask(void);

/* pty.c */
void   pty_init(void);
pty_t *pty_alloc(void);
void   pty_master_closed(pty_t *p);
void   pty_slave_output_locked(tty_t *slave, const uint8_t *buf, size_t n);
ssize_t pty_master_read(pty_t *p, void *kbuf, size_t n, int nonblock);
ssize_t pty_master_write(pty_t *p, const void *kbuf, size_t n);

/* tty_dev.c */
void   tty_dev_register(void);

#ifdef __cplusplus
}
#endif

#endif /* MODUOS_KERNEL_TTY_TTY_H */
