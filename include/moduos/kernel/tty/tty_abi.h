// SPDX-License-Identifier: GPL-2.0-only
//
// tty_abi.h - ModuOS TTY ABI shared between the kernel and userland.
//
// Pure data definitions (no kernel includes) so libc and apps can include it.
// Flag values and c_cc indices are Linux-compatible on purpose, so ported
// code (stty-like tools, line editors, terminal emulators) maps 1:1.
//
// ioctl transport
// ---------------
// ModuOS has no ioctl() syscall; devices expose SYS_INVOKE. Every TTY device
// ($/dev/ttyN, $/dev/tty, $/dev/console, $/dev/ptmx, $/dev/pts/N) accepts a
// tty_ioc_t as both the in and out buffer:
//
//     tty_ioc_t io = { .cmd = TTY_IOC_GETATTR };
//     invoke(fd, &io, sizeof io, &io, sizeof io);     // returns 0 or -errno
//     use(io.data) ...
//
// libc wraps this as tty_ioctl(fd, cmd, arg).

#ifndef MODUOS_KERNEL_TTY_TTY_ABI_H
#define MODUOS_KERNEL_TTY_TTY_ABI_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* termios (Linux-compatible values)                                   */
/* ------------------------------------------------------------------ */

#define TTY_NCCS 20

/* c_iflag */
#define TTY_IGNBRK  0x0001
#define TTY_BRKINT  0x0002
#define TTY_IGNPAR  0x0004
#define TTY_ISTRIP  0x0020
#define TTY_INLCR   0x0040
#define TTY_IGNCR   0x0080
#define TTY_ICRNL   0x0100
#define TTY_IXON    0x0400
#define TTY_IXANY   0x0800
#define TTY_IXOFF   0x1000

/* c_oflag */
#define TTY_OPOST   0x0001
#define TTY_ONLCR   0x0004
#define TTY_OCRNL   0x0008
#define TTY_ONOCR   0x0010
#define TTY_ONLRET  0x0020

/* c_cflag (accepted and stored; there is no real line hardware) */
#define TTY_CS8     0x0030
#define TTY_CREAD   0x0080
#define TTY_HUPCL   0x0400
#define TTY_CLOCAL  0x0800

/* c_lflag */
#define TTY_ISIG    0x0001
#define TTY_ICANON  0x0002
#define TTY_ECHO    0x0008
#define TTY_ECHOE   0x0010
#define TTY_ECHOK   0x0020
#define TTY_ECHONL  0x0040
#define TTY_NOFLSH  0x0080
#define TTY_TOSTOP  0x0100
#define TTY_ECHOCTL 0x0200
#define TTY_IEXTEN  0x8000

/* c_cc indices */
#define TTY_VINTR    0
#define TTY_VQUIT    1
#define TTY_VERASE   2
#define TTY_VKILL    3
#define TTY_VEOF     4
#define TTY_VTIME    5
#define TTY_VMIN     6
#define TTY_VSTART   8
#define TTY_VSTOP    9
#define TTY_VSUSP   10
#define TTY_VEOL    11
#define TTY_VREPRINT 12
#define TTY_VWERASE 14
#define TTY_VLNEXT  15

typedef struct {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_cc[TTY_NCCS];
} tty_termios_t;

typedef struct {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} tty_winsize_t;

/* ------------------------------------------------------------------ */
/* ioctl-style commands                                                */
/* ------------------------------------------------------------------ */

#define TTY_IOC_MAX_DATA 96

typedef struct {
    uint32_t cmd;
    int32_t  result;                    /* scalar result for int-returning cmds */
    uint8_t  data[TTY_IOC_MAX_DATA];    /* in and/or out payload                */
} tty_ioc_t;

/* termios / line discipline */
#define TTY_IOC_GETATTR      0x5401   /* out: tty_termios_t                      */
#define TTY_IOC_SETATTR      0x5402   /* in : tty_termios_t                      */
#define TTY_IOC_SETATTR_FLUSH 0x5403  /* in : tty_termios_t (discard pending in) */
#define TTY_IOC_FLUSH_IN     0x540B   /* drop unread input                       */
#define TTY_IOC_SETCTTY      0x540E   /* make this tty the caller's ctty         */
#define TTY_IOC_GETPGRP      0x540F   /* result = foreground pgrp                */
#define TTY_IOC_SETPGRP      0x5410   /* in : int32 pgrp                         */
#define TTY_IOC_GETSID       0x5429   /* result = session id of this tty         */
#define TTY_IOC_GETWINSZ     0x5413   /* out: tty_winsize_t                      */
#define TTY_IOC_SETWINSZ     0x5414   /* in : tty_winsize_t (pty master/any tty) */
#define TTY_IOC_NREAD        0x541B   /* result = bytes readable now (FIONREAD)  */
#define TTY_IOC_NONBLOCK     0x5421   /* in : int32 0/1 (FIONBIO)                */
#define TTY_IOC_ISATTY       0x5420   /* result = 1                              */
#define TTY_IOC_GETINDEX     0x5430   /* result = vt number (1-based) or pty N   */

/* pty master only */
#define PTY_IOC_GETPTN       0x5431   /* result = slave number (pts/N)           */
#define PTY_IOC_UNLOCK       0x5432   /* unlock slave for opening                */

/* virtual console (VT) control; valid on $/dev/ttyN, $/dev/tty0, $/dev/console */
#define VT_IOC_GETSTATE      0x5603   /* out: tty_vt_state_t                     */
#define VT_IOC_ACTIVATE      0x5606   /* in : int32 vt (1..TTY_NUM_VT)           */
#define VT_IOC_WAITACTIVE    0x5607   /* in : int32 vt, block until active       */
#define VT_IOC_SETMODE       0x4B3A   /* in : int32 KD_TEXT / KD_GRAPHICS        */
#define VT_IOC_GETMODE       0x4B3B   /* result = KD_TEXT / KD_GRAPHICS          */
#define VT_IOC_CLEAR         0x5610   /* clear screen + home cursor              */

#define KD_TEXT      0
#define KD_GRAPHICS  1

typedef struct {
    uint16_t active;      /* currently visible VT, 1-based */
    uint16_t count;       /* number of VTs                 */
    uint16_t mode_mask;   /* bit n set => VT n+1 is KD_GRAPHICS */
    uint16_t reserved;
} tty_vt_state_t;

/* ------------------------------------------------------------------ */
/* Device nodes                                                        */
/* ------------------------------------------------------------------ */
/*
 *   $/dev/tty        controlling terminal of the caller
 *   $/dev/console    the system console (VT1)
 *   $/dev/tty0       currently active VT
 *   $/dev/tty1..N    virtual consoles (N = TTY_NUM_VT)
 *   $/dev/ptmx       open to allocate a pty; PTY_IOC_GETPTN gives N
 *   $/dev/pts/N      pty slave
 */

#endif /* MODUOS_KERNEL_TTY_TTY_ABI_H */
