/* tty.h - ModuOS TTY helpers (termios-lite, ptys, VT switching).
 *
 * Header-only on top of invoke(); see include/moduos/kernel/tty/tty_abi.h
 * for the ABI. Every function returns 0 (or a value >= 0) on success and
 * -1 on failure.
 */
#ifndef _MODUOS_TTY_H
#define _MODUOS_TTY_H

#include <stdint.h>
#include <stddef.h>
#include "../../include/moduos/kernel/tty/tty_abi.h"
#include "unistd.h"
#include "fcntl.h"
#include "moduos.h"

typedef tty_termios_t termios_t;
typedef tty_winsize_t winsize_t;

/* Raw ioctl. 'arg' is copied in/out of io.data (up to TTY_IOC_MAX_DATA). */
static inline int tty_ioctl(int fd, uint32_t cmd, void *arg, size_t arg_sz) {
    tty_ioc_t io;
    unsigned char *p = (unsigned char *)&io;
    for (size_t i = 0; i < sizeof io; i++) p[i] = 0;
    io.cmd = cmd;
    if (arg && arg_sz) {
        if (arg_sz > TTY_IOC_MAX_DATA) return -1;
        const unsigned char *s = (const unsigned char *)arg;
        for (size_t i = 0; i < arg_sz; i++) io.data[i] = s[i];
    }
    ssize_t r = invoke(fd, &io, sizeof io, &io, sizeof io);
    if (r < 0) return -1;
    if (arg && arg_sz) {
        unsigned char *d = (unsigned char *)arg;
        for (size_t i = 0; i < arg_sz; i++) d[i] = io.data[i];
    }
    return io.result;
}

static inline int tty_isatty(int fd)   { return tty_ioctl(fd, TTY_IOC_ISATTY, 0, 0) == 1; }
static inline int tty_getattr(int fd, termios_t *t) { return tty_ioctl(fd, TTY_IOC_GETATTR, t, sizeof *t); }
static inline int tty_setattr(int fd, const termios_t *t, int flush) {
    termios_t tmp = *t;
    return tty_ioctl(fd, flush ? TTY_IOC_SETATTR_FLUSH : TTY_IOC_SETATTR, &tmp, sizeof tmp);
}
static inline int tty_getwinsize(int fd, winsize_t *w) { return tty_ioctl(fd, TTY_IOC_GETWINSZ, w, sizeof *w); }
static inline int tty_setwinsize(int fd, const winsize_t *w) {
    winsize_t t = *w; return tty_ioctl(fd, TTY_IOC_SETWINSZ, &t, sizeof t);
}

/* cfmakeraw-style helpers */
static inline void tty_make_raw(termios_t *t) {
    t->c_iflag &= ~(TTY_ISTRIP | TTY_INLCR | TTY_IGNCR | TTY_ICRNL | TTY_IXON | TTY_BRKINT);
    t->c_oflag &= ~TTY_OPOST;
    t->c_lflag &= ~(TTY_ECHO | TTY_ECHONL | TTY_ICANON | TTY_ISIG | TTY_IEXTEN);
    t->c_cc[TTY_VMIN] = 1; t->c_cc[TTY_VTIME] = 0;
}

/* Open the caller's controlling terminal ($/dev/tty). */
static inline int tty_open_ctty(void) { return open("$/dev/tty", O_RDWR, 0); }

/* Make 'fd' (a $/dev/ttyN or pts) the controlling tty of the caller.
 * The caller must be a session leader without a ctty (call setsid() first). */
static inline int tty_set_ctty(int fd) { return tty_ioctl(fd, TTY_IOC_SETCTTY, 0, 0); }

/* Pseudo terminals. Returns master fd; *slave_path receives "$/dev/pts/N". */
static inline int tty_openpty(char *slave_path, size_t sz) {
    int m = open("$/dev/ptmx", O_RDWR, 0);
    if (m < 0) return -1;
    int n = tty_ioctl(m, PTY_IOC_GETPTN, 0, 0);
    if (n < 0 || tty_ioctl(m, PTY_IOC_UNLOCK, 0, 0) < 0) { close(m); return -1; }
    if (slave_path && sz >= 16) {
        const char *pre = "$/dev/pts/";
        size_t i = 0;
        while (pre[i]) { slave_path[i] = pre[i]; i++; }
        char tmp[8]; int k = 0;
        do { tmp[k++] = (char)('0' + n % 10); n /= 10; } while (n);
        while (k) slave_path[i++] = tmp[--k];
        slave_path[i] = 0;
    }
    return m;
}

/* Virtual consoles (1-based). */
static inline int vt_activate(int vt) {
    int fd = open("$/dev/tty0", O_RDWR, 0);
    if (fd < 0) return -1;
    int32_t v = vt;
    int r = tty_ioctl(fd, VT_IOC_ACTIVATE, &v, sizeof v);
    close(fd);
    return r;
}
static inline int vt_get_active(void) {
    int fd = open("$/dev/tty0", O_RDWR, 0);
    if (fd < 0) return -1;
    tty_vt_state_t st;
    int r = tty_ioctl(fd, VT_IOC_GETSTATE, &st, sizeof st);
    close(fd);
    return r < 0 ? -1 : (int)st.active;
}

#endif
