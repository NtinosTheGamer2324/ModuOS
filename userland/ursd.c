// ursd.c - URS (Unsecure Remote Shell) daemon for ModuOS.
//
// !! URS HAS NO ENCRYPTION. Passwords and every keystroke cross the wire in
// !! clear text. It exists for a QEMU development setup only:
// !!
// !!   Windows client -> 127.0.0.1:8080 (QEMU hostfwd, loopback only)
// !!                  -> guest 10.0.2.15:8080 (this daemon)
// !!
// !! Never forward it on 0.0.0.0, never bridge/tap the guest NIC to a LAN.
//
// Build:   userland/build.sh picks it up (*.c -> dist/ursd.sqr), no changes.
// Run:     as root, e.g. "service /Apps/ursd.sqr" in the autostart file. Root
//          is only needed to drop to the authenticated user's uid/gid.
//
// What it does: owns the TCP session, authenticates through userctl
// (UCTL_CMD_LOGIN, the same call login.c uses), then runs the stock shell
// (zenith5.1) on a kernel pty with the uid/gid userctl reported. The shell
// and line discipline do all parsing, echo, editing, ^C and permissions; the
// daemon only moves bytes between the socket and the pty master.
//
// ── Wire protocol (version 1) ───────────────────────────────────────────
// All integers big-endian. Every message is one frame:
//
//   offset 0  u8  'U'
//   offset 1  u8  'R'
//   offset 2  u8  type
//   offset 3  u8  version (1)
//   offset 4  u16 payload length (0..1024)
//   offset 6  payload
//
// Anything else (bad magic/version/type, length > 1024) is a protocol error:
// the daemon sends ERROR and drops the connection. Lengths are checked
// before the body is waited for, so a peer can't make us buffer more than
// one maximum frame.
//
//   type  name       dir   payload
//   0x01  HELLO      S->C  ASCII banner
//   0x02  AUTH_REQ   C->S  u16 rows, u16 cols, u8 ulen, user, u8 plen, pass
//   0x03  AUTH_OK    S->C  (empty)  the shell is starting
//   0x04  AUTH_FAIL  S->C  u8 attempts left
//   0x05  INPUT      C->S  raw bytes for the pty (what a keyboard would send)
//   0x06  OUTPUT     S->C  raw bytes from the pty (UTF-8 + VT100/ANSI)
//   0x07  WINSIZE    C->S  u16 rows, u16 cols
//   0x08  CLOSE      both  S->C: u8 reason. C->S: empty
//   0x09  ERROR      S->C  u8 code, ASCII text; the connection is dropped
//
// Sequence: HELLO, AUTH_REQ (up to 3 tries), AUTH_OK, then INPUT/OUTPUT/
// WINSIZE until CLOSE. ulen is 1..31 and plen 1..63 (userctl's field sizes).
//
// ── Security notes ──────────────────────────────────────────────────────
// netd listens on every address and sockets.h ignores bind()'s host
// argument, so the listener cannot be pinned to an interface here. Instead
// the daemon fails closed on both ends of the QEMU path:
//   * at start-up it refuses to run unless netd reports guest IP 10.0.2.15
//     and gateway 10.0.2.2 (QEMU "-netdev user"), and
//   * every accepted connection must come from 10.0.2.2, the address QEMU
//     user networking uses for host-forwarded connections; others are
//     closed without a byte sent.
// This is only as strong as "the host forward is 127.0.0.1-only", which is
// the QEMU command line's job (see README).
//
// Limits (netd/libc, not choices): single-threaded, so the main loop polls
// socket and pty; one netd mailbox slot, so one session at a time (extra
// connections get ERROR/BUSY); no SIGWINCH, so WINSIZE only updates the
// pty's winsize for programs that query it.

#include "sockets.h"
#include "userctl_ipc.h"
#include "libc/tty.h"

// ── Configuration ───────────────────────────────────────────────────────

#define URS_PORT              8080
#define URS_GUEST_IP_STR      "10.0.2.15"
#define URS_PEER_IP_STR       "10.0.2.2"
#define URS_SHELL_PATH        "/Apps/zenith5.1.sqr"

// A uid 0 account is refused by default: a remote shell with no encryption
// shouldn't be root. Flip only if you accept that.
#define URS_ALLOW_ROOT        1 // allowed

#define URS_MAX_AUTH_ATTEMPTS 3
#define URS_FAIL_DELAY_S      1
#define URS_COOLDOWN_S        2       // after a connection exhausts its tries
#define URS_AUTH_TIMEOUT_MS   20000   // whole handshake, slow-peer cutoff
#define URS_HANDSHAKE_POLL_MS 200
#define URS_BUSY_POLL_MS      300
#define URS_PUMP_MAX          8192    // pty bytes per loop, keeps ^C responsive
#define URS_STOP_WAIT_MS      1500
#define URS_ACCEPT_FAIL_MAX   5

#define URS_DEFAULT_ROWS      25
#define URS_DEFAULT_COLS      80
#define URS_DIM_MAX           1000

// ── Protocol ────────────────────────────────────────────────────────────

#define URS_MAGIC_0           'U'
#define URS_MAGIC_1           'R'
#define URS_VERSION           1
#define URS_HDR_LEN           6
#define URS_MAX_PAYLOAD       1024

typedef enum {
    URS_MSG_HELLO     = 0x01,
    URS_MSG_AUTH_REQ  = 0x02,
    URS_MSG_AUTH_OK   = 0x03,
    URS_MSG_AUTH_FAIL = 0x04,
    URS_MSG_INPUT     = 0x05,
    URS_MSG_OUTPUT    = 0x06,
    URS_MSG_WINSIZE   = 0x07,
    URS_MSG_CLOSE     = 0x08,
    URS_MSG_ERROR     = 0x09,
    URS_MSG_COUNT
} urs_msg_t;

typedef enum {
    URS_ERR_PROTOCOL  = 1,
    URS_ERR_TOO_LARGE = 2,
    URS_ERR_BUSY      = 3,
    URS_ERR_FORBIDDEN = 4,
    URS_ERR_INTERNAL  = 5,
    URS_ERR_TIMEOUT   = 6
} urs_err_t;

typedef enum {
    URS_CLOSE_SHELL_EXIT = 0,
    URS_CLOSE_AUTH_LIMIT = 1,
    URS_CLOSE_CLIENT     = 2,
    URS_CLOSE_SERVER     = 3
} urs_close_t;

#define URS_RX_NEED           0
#define URS_RX_FRAME          1
#define URS_RX_BAD           (-1)
#define URS_RX_TOO_BIG       (-2)

// Room for one maximum frame plus a full netd read on top of it.
#define URS_RX_CAP            (2 * (URS_HDR_LEN + URS_MAX_PAYLOAD))

// ── State ───────────────────────────────────────────────────────────────

typedef enum {
    URS_END_NONE = 0,
    URS_END_CLIENT,
    URS_END_SHELL,
    URS_END_ERROR,
    URS_END_AUTHFAIL
} urs_end_t;

typedef struct {
    char     user[UCTL_NAME_MAX];
    char     pass[UCTL_PASS_MAX];
    uint16_t rows;
    uint16_t cols;
} urs_auth_t;

typedef struct {
    int master;
    int pid;
} urs_shell_t;

// Kept out of the stack: the user stack is small and these are ~3 KB.
static uint8_t  urs_rx_buf[URS_RX_CAP];
static size_t   urs_rx_len;
static uint8_t  urs_payload[URS_MAX_PAYLOAD];
static uint8_t  urs_tx_buf[URS_HDR_LEN + URS_MAX_PAYLOAD];

static uint8_t  urs_guest_ip[4];
static uint8_t  urs_peer_ip[4];
static uctl_shm_t *urs_uctl;      // mapped once: every attach burns a handle

// ── Framing ─────────────────────────────────────────────────────────────

static uint16_t urs_get_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int urs_send_frame(int sock, uint8_t type, const void *payload,
                          size_t len) {
    if (len > URS_MAX_PAYLOAD) return -1;
    urs_tx_buf[0] = URS_MAGIC_0;
    urs_tx_buf[1] = URS_MAGIC_1;
    urs_tx_buf[2] = type;
    urs_tx_buf[3] = URS_VERSION;
    urs_tx_buf[4] = (uint8_t)(len >> 8);
    urs_tx_buf[5] = (uint8_t)len;
    if (len) memcpy(urs_tx_buf + URS_HDR_LEN, payload, len);
    return sock_sendall(sock, urs_tx_buf, URS_HDR_LEN + len);
}

static void urs_send_error(int sock, uint8_t code, const char *text) {
    uint8_t msg[1 + 120];
    size_t n = strnlen(text, sizeof(msg) - 1);
    msg[0] = code;
    memcpy(msg + 1, text, n);
    urs_send_frame(sock, URS_MSG_ERROR, msg, 1 + n);
}

static void urs_send_close(int sock, uint8_t reason) {
    urs_send_frame(sock, URS_MSG_CLOSE, &reason, 1);
}

// Pops one complete frame out of urs_rx_buf into urs_payload. The vacated
// bytes are wiped because the buffer may have held a password.
static int urs_rx_next(uint8_t *type, size_t *len) {
    if (urs_rx_len < URS_HDR_LEN) return URS_RX_NEED;
    if (urs_rx_buf[0] != URS_MAGIC_0 || urs_rx_buf[1] != URS_MAGIC_1 ||
        urs_rx_buf[3] != URS_VERSION)
        return URS_RX_BAD;

    uint8_t t = urs_rx_buf[2];
    size_t n = urs_get_be16(urs_rx_buf + 4);
    if (t == 0 || t >= URS_MSG_COUNT) return URS_RX_BAD;
    if (n > URS_MAX_PAYLOAD) return URS_RX_TOO_BIG;
    if (urs_rx_len < URS_HDR_LEN + n) return URS_RX_NEED;

    memcpy(urs_payload, urs_rx_buf + URS_HDR_LEN, n);
    size_t used = URS_HDR_LEN + n;
    size_t rest = urs_rx_len - used;
    memmove(urs_rx_buf, urs_rx_buf + used, rest);
    uctl_scrub(urs_rx_buf + rest, used);
    urs_rx_len = rest;

    *type = t;
    *len = n;
    return URS_RX_FRAME;
}

// One netd read into the frame buffer. 1 = bytes arrived, 0 = nothing yet,
// -1 = peer closed or the connection failed. A poll that finds nothing
// reports EAGAIN/ETIMEDOUT; sockets.h also uses ETIMEDOUT when the netd
// mailbox was momentarily busy, which is equally "try again".
static int urs_rx_fill(int sock) {
    size_t room = URS_RX_CAP - urs_rx_len;
    if (room == 0) return -1;     // unreachable: the parser rejects first
    ssize_t n = sock_recv(sock, urs_rx_buf + urs_rx_len, room);
    if (n > 0) {
        urs_rx_len += (size_t)n;
        return 1;
    }
    if (n == 0) return -1;
    return (errno == EAGAIN || errno == ETIMEDOUT) ? 0 : -1;
}

static void urs_rx_reset(void) {
    uctl_scrub(urs_rx_buf, sizeof(urs_rx_buf));
    uctl_scrub(urs_payload, sizeof(urs_payload));
    urs_rx_len = 0;
}

static void urs_report_rx_error(int sock, int rc) {
    if (rc == URS_RX_TOO_BIG)
        urs_send_error(sock, URS_ERR_TOO_LARGE, "frame too large");
    else
        urs_send_error(sock, URS_ERR_PROTOCOL, "malformed frame");
}

// ── Authentication ──────────────────────────────────────────────────────

static void urs_clamp_winsize(uint16_t *rows, uint16_t *cols) {
    if (*rows == 0 || *rows > URS_DIM_MAX) *rows = URS_DEFAULT_ROWS;
    if (*cols == 0 || *cols > URS_DIM_MAX) *cols = URS_DEFAULT_COLS;
}

// AUTH_REQ payload: u16 rows, u16 cols, u8 ulen, user, u8 plen, pass. The
// length must match exactly; userctl's buffers are the hard upper bounds.
static int urs_auth_parse(const uint8_t *p, size_t n, urs_auth_t *out) {
    if (n < 4 + 1 + 1 + 1 + 1) return -1;
    size_t ulen = p[4];
    if (ulen == 0 || ulen >= UCTL_NAME_MAX) return -1;
    if (n < 5 + ulen + 1) return -1;
    size_t plen = p[5 + ulen];
    if (plen == 0 || plen >= UCTL_PASS_MAX) return -1;
    if (n != 5 + ulen + 1 + plen) return -1;

    const uint8_t *user = p + 5;
    const uint8_t *pass = p + 5 + ulen + 1;
    for (size_t i = 0; i < ulen; i++)
        if (user[i] <= 0x20 || user[i] >= 0x7F) return -1;
    for (size_t i = 0; i < plen; i++)
        if (pass[i] == 0) return -1;

    memset(out, 0, sizeof(*out));
    memcpy(out->user, user, ulen);
    memcpy(out->pass, pass, plen);
    out->rows = urs_get_be16(p);
    out->cols = urs_get_be16(p + 2);
    urs_clamp_winsize(&out->rows, &out->cols);
    return 0;
}

// 0 on success. Unknown user and wrong password are indistinguishable here
// on purpose (userctl returns the same error for both).
static int urs_auth_check(const urs_auth_t *auth, uctl_response_t *resp) {
    static uctl_request_t req;
    if (!urs_uctl) urs_uctl = uctl_attach();
    if (!urs_uctl) return -1;

    memset(&req, 0, sizeof(req));
    req.cmd = UCTL_CMD_LOGIN;
    strlcpy(req.arg_user, auth->user, sizeof(req.arg_user));
    strlcpy(req.arg_pass, auth->pass, sizeof(req.arg_pass));
    int rc = uctl_call(urs_uctl, &req, resp);
    uctl_scrub(&req, sizeof(req));
    return rc;
}

static int urs_peer_allowed(const sock_addr_t *peer) {
    return memcmp(peer->ip, urs_peer_ip, 4) == 0;
}

// HELLO, then AUTH_REQ frames until one succeeds, the try limit is hit, or
// the handshake deadline passes. Returns URS_END_NONE on success.
static urs_end_t urs_handshake(int sock, urs_auth_t *auth,
                               uctl_response_t *resp) {
    static const char banner[] =
        "URS/1 ModuOS - UNENCRYPTED development shell, local use only";
    int failures = 0;

    sock_settimeout(sock, URS_HANDSHAKE_POLL_MS);
    if (urs_send_frame(sock, URS_MSG_HELLO, banner, sizeof(banner) - 1) != 0)
        return URS_END_CLIENT;

    uint64_t deadline = time_ms() + URS_AUTH_TIMEOUT_MS;
    for (;;) {
        for (;;) {
            uint8_t type;
            size_t len;
            int rc = urs_rx_next(&type, &len);
            if (rc == URS_RX_NEED) break;
            if (rc < 0) {
                urs_report_rx_error(sock, rc);
                return URS_END_ERROR;
            }
            if (type != URS_MSG_AUTH_REQ) {
                urs_send_error(sock, URS_ERR_PROTOCOL, "authenticate first");
                return URS_END_ERROR;
            }

            int parsed = urs_auth_parse(urs_payload, len, auth);
            uctl_scrub(urs_payload, sizeof(urs_payload));
            if (parsed != 0) {
                urs_send_error(sock, URS_ERR_PROTOCOL, "bad AUTH_REQ");
                return URS_END_ERROR;
            }

            int ok = urs_auth_check(auth, resp);
            uctl_scrub(auth->pass, sizeof(auth->pass));
            if (ok == 0) {
                if (resp->uid == 0 && !URS_ALLOW_ROOT) {
                    printf("ursd: root login refused (URS_ALLOW_ROOT=0)\n");
                    urs_send_error(sock, URS_ERR_FORBIDDEN,
                                   "root login is disabled over URS");
                    return URS_END_ERROR;
                }
                if (urs_send_frame(sock, URS_MSG_AUTH_OK, NULL, 0) != 0)
                    return URS_END_CLIENT;
                return URS_END_NONE;
            }

            failures++;
            int left = URS_MAX_AUTH_ATTEMPTS - failures;
            printf("ursd: auth failed (%d/%d)\n", failures,
                   URS_MAX_AUTH_ATTEMPTS);
            uint8_t left_u8 = (uint8_t)left;
            urs_send_frame(sock, URS_MSG_AUTH_FAIL, &left_u8, 1);
            if (left <= 0) {
                urs_send_close(sock, URS_CLOSE_AUTH_LIMIT);
                return URS_END_AUTHFAIL;
            }
            sleep(URS_FAIL_DELAY_S);      // throttles guessing
        }

        if (time_ms() >= deadline) {
            urs_send_error(sock, URS_ERR_TIMEOUT, "authentication timed out");
            return URS_END_ERROR;
        }
        if (urs_rx_fill(sock) < 0) return URS_END_CLIENT;
    }
}

// ── Shell on a pty ──────────────────────────────────────────────────────

static void urs_pty_set_winsize(int master, uint16_t rows, uint16_t cols) {
    winsize_t ws = { rows, cols, 0, 0 };
    tty_setwinsize(master, &ws);
}

// Same sequence getty and mdterm use. ModuOS keeps one global fd table, so
// the child must not close the master and must not dup2 (mdterm.c documents
// both); the shell reaches the pty through its controlling tty instead.
// gid is dropped before uid, as login.c does, since setgid needs the
// privilege setuid gives up.
static __attribute__((noreturn)) void urs_shell_child(const char *slave,
                                                      const char *user,
                                                      int uid, int gid) {
    if (setsid() < 0) _exit(2);
    int slave_fd = open(slave, O_RDWR, 0);
    if (slave_fd < 0) _exit(3);
    if (tty_set_ctty(slave_fd) < 0) _exit(4);
    if (setgid(gid) != 0 || setuid(uid) != 0) _exit(126);

    char env_user[48], env_logname[52];
    snprintf(env_user, sizeof(env_user), "USER=%s", user);
    snprintf(env_logname, sizeof(env_logname), "LOGNAME=%s", user);
    char *argv[] = { (char *)URS_SHELL_PATH, NULL };
    char *envp[] = { env_user, env_logname, (char *)"TERM=linux", NULL };
    execve(URS_SHELL_PATH, argv, envp);
    _exit(127);
}

static int urs_shell_spawn(urs_shell_t *sh, const char *user, int uid,
                           int gid, uint16_t rows, uint16_t cols) {
    char slave[32];
    sh->master = -1;
    sh->pid = -1;

    int master = tty_openpty(slave, sizeof(slave));
    if (master < 0) return -1;
    urs_pty_set_winsize(master, rows, cols);

    int pid = fork();
    if (pid < 0) {
        close(master);
        return -1;
    }
    if (pid == 0) urs_shell_child(slave, user, uid, gid);

    int one = 1;
    tty_ioctl(master, TTY_IOC_NONBLOCK, &one, sizeof(one));
    sh->master = master;
    sh->pid = pid;
    return 0;
}

// Returns 1 once the child has been reaped (or is not ours to reap).
static int urs_wait_exit(int pid, uint32_t timeout_ms, int *status) {
    uint64_t deadline = time_ms() + timeout_ms;
    for (;;) {
        if (waitpid(pid, status, WNOHANG) != 0) return 1;
        if (time_ms() >= deadline) return 0;
        yield();
    }
}

// Always reaps: closing the master hangs up the slave (SIGHUP to everything
// on the pty), then escalate for a shell that ignores it.
static void urs_shell_stop(urs_shell_t *sh) {
    int status = 0;

    if (sh->master >= 0) {
        close(sh->master);
        sh->master = -1;
    }
    if (sh->pid <= 0) return;

    if (!urs_wait_exit(sh->pid, 0, &status)) {
        kill(sh->pid, SIGHUP);
        kill(sh->pid, SIGTERM);
        if (!urs_wait_exit(sh->pid, URS_STOP_WAIT_MS, &status)) {
            kill(sh->pid, SIGKILL);
            if (!urs_wait_exit(sh->pid, URS_STOP_WAIT_MS, &status))
                printf("ursd: shell pid %d would not die\n", sh->pid);
        }
    }
    sh->pid = -1;
}

static int urs_shell_write(int master, const uint8_t *p, size_t len) {
    while (len) {
        ssize_t w = write(master, p, len);
        if (w <= 0) return -1;
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

// ── Session loop ────────────────────────────────────────────────────────

// 0 = keep going, 1 = client asked to close, -1 = protocol error (reported).
static int urs_session_dispatch(int sock, const urs_shell_t *sh, uint8_t type,
                                size_t len) {
    switch (type) {
    case URS_MSG_INPUT:
        if (urs_shell_write(sh->master, urs_payload, len) != 0) return 1;
        return 0;
    case URS_MSG_WINSIZE: {
        if (len != 4) {
            urs_send_error(sock, URS_ERR_PROTOCOL, "bad WINSIZE");
            return -1;
        }
        uint16_t rows = urs_get_be16(urs_payload);
        uint16_t cols = urs_get_be16(urs_payload + 2);
        urs_clamp_winsize(&rows, &cols);
        urs_pty_set_winsize(sh->master, rows, cols);
        return 0;
    }
    case URS_MSG_CLOSE:
        return 1;
    default:
        urs_send_error(sock, URS_ERR_PROTOCOL, "unexpected message");
        return -1;
    }
}

// Only one session fits the single netd mailbox, so a second connection is
// told so instead of silently sitting in netd's backlog.
static void urs_reject_extra(int listener) {
    sock_addr_t peer;
    int sock = sock_accept(listener, &peer);
    if (sock < 0) return;
    if (urs_peer_allowed(&peer))
        urs_send_error(sock, URS_ERR_BUSY, "another session is active");
    sock_close(sock);
}

// Everything the shell wrote before it died still has to reach the client.
static int urs_pump_output(int sock, int master, int *eof) {
    uint8_t buf[URS_MAX_PAYLOAD];
    size_t total = 0;

    while (total < URS_PUMP_MAX) {
        ssize_t n = read(master, buf, sizeof(buf));
        if (n < 0) break;                 // EAGAIN: queue is empty
        if (n == 0) {
            *eof = 1;                     // slave gone: the shell exited
            break;
        }
        if (urs_send_frame(sock, URS_MSG_OUTPUT, buf, (size_t)n) != 0)
            return -1;
        total += (size_t)n;
    }
    return (int)total;
}

static urs_end_t urs_session_run(int sock, int listener, urs_shell_t *sh) {
    uint64_t next_busy = time_ms() + URS_BUSY_POLL_MS;

    sock_settimeout(sock, SOCK_NONBLOCK);
    sock_settimeout(listener, SOCK_NONBLOCK);

    for (;;) {
        int idle = 1;

        int filled = urs_rx_fill(sock);
        if (filled < 0) return URS_END_CLIENT;
        if (filled > 0) idle = 0;

        for (;;) {
            uint8_t type;
            size_t len;
            int rc = urs_rx_next(&type, &len);
            if (rc == URS_RX_NEED) break;
            if (rc < 0) {
                urs_report_rx_error(sock, rc);
                return URS_END_ERROR;
            }
            int d = urs_session_dispatch(sock, sh, type, len);
            if (d > 0) return URS_END_CLIENT;
            if (d < 0) return URS_END_ERROR;
        }

        int eof = 0;
        int pumped = urs_pump_output(sock, sh->master, &eof);
        if (pumped < 0) return URS_END_CLIENT;
        if (pumped > 0) idle = 0;

        int status = 0;
        int gone = eof;
        if (!gone && waitpid(sh->pid, &status, WNOHANG) != 0) {
            sh->pid = -1;                 // reaped here, nothing left to stop
            gone = 1;
        }
        if (gone) {
            int dummy = 0;
            if (urs_pump_output(sock, sh->master, &dummy) < 0)
                return URS_END_CLIENT;
            urs_send_close(sock, URS_CLOSE_SHELL_EXIT);
            return URS_END_SHELL;
        }

        if (time_ms() >= next_busy) {
            next_busy = time_ms() + URS_BUSY_POLL_MS;
            urs_reject_extra(listener);
        }
        if (idle) yield();
    }
}

// ── Connection / main ───────────────────────────────────────────────────

static urs_end_t urs_serve(int sock, int listener, const sock_addr_t *peer) {
    urs_auth_t auth;
    uctl_response_t resp;
    urs_shell_t sh;

    if (!urs_peer_allowed(peer)) {
        printf("ursd: refused connection from %s (not the QEMU gateway)\n",
               inet_ntoa(peer->ip));
        return URS_END_ERROR;
    }
    printf("ursd: connection from %s:%d\n", inet_ntoa(peer->ip),
           (int)peer->port);

    urs_rx_reset();
    urs_end_t end = urs_handshake(sock, &auth, &resp);
    if (end != URS_END_NONE) {
        urs_rx_reset();
        return end;
    }

    printf("ursd: login ok: %s (uid %d)\n", auth.user, (int)resp.uid);
    if (urs_shell_spawn(&sh, auth.user, (int)resp.uid, (int)resp.gid,
                        auth.rows, auth.cols) != 0) {
        printf("ursd: cannot start shell (pty or fork failed)\n");
        urs_send_error(sock, URS_ERR_INTERNAL, "cannot start shell");
        urs_rx_reset();
        return URS_END_ERROR;
    }

    end = urs_session_run(sock, listener, &sh);
    urs_shell_stop(&sh);
    urs_rx_reset();
    printf("ursd: session for %s ended\n", auth.user);
    return end;
}

// Refuses to run unless the guest looks like QEMU user networking; see the
// security notes at the top for why this is the enforcement point.
static int urs_check_interface(void) {
    sock_netinfo_t ni;

    if (!inet_aton(URS_GUEST_IP_STR, urs_guest_ip) ||
        !inet_aton(URS_PEER_IP_STR, urs_peer_ip)) {
        printf("ursd: bad address constants\n");
        return -1;
    }
    if (sock_get_netinfo(&ni) != 0) {
        printf("ursd: netd not answering (errno=%d), refusing to start\n",
               errno);
        return -1;
    }
    if (!ni.link_up || memcmp(ni.ip, urs_guest_ip, 4) != 0 ||
        memcmp(ni.gateway, urs_peer_ip, 4) != 0) {
        // inet_ntoa() returns one static buffer, hence two printfs.
        printf("ursd: guest ip %s", inet_ntoa(ni.ip));
        printf(" gateway %s, expected %s / %s; refusing to start\n",
               inet_ntoa(ni.gateway), URS_GUEST_IP_STR, URS_PEER_IP_STR);
        return -1;
    }
    return 0;
}

static int urs_listen_open(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    // The host argument is ignored by sockets.h (netd has one interface and
    // listens on it); it is passed for the reader, enforcement is above.
    if (sock_bind(fd, URS_GUEST_IP_STR, URS_PORT) != 0 ||
        sock_listen(fd, 4) != 0) {
        int saved = errno;
        sock_close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

int md_main(long argc, char **argv) {
    (void)argc;
    (void)argv;

    if (geteuid() != 0) {
        printf("ursd: must run as root to switch to the login user\n");
        return 1;
    }
    if (urs_check_interface() != 0) return 1;

    int listener = urs_listen_open();
    if (listener < 0) {
        printf("ursd: cannot listen on port %d (errno=%d, %s)\n", URS_PORT,
               errno, sock_last_message());
        return 1;
    }
    printf("ursd: listening on %s:%d (UNENCRYPTED, QEMU loopback only)\n",
           URS_GUEST_IP_STR, URS_PORT);

    int accept_failures = 0;
    for (;;) {
        sock_addr_t peer;
        sock_settimeout(listener, SOCK_BLOCKING);
        int sock = sock_accept(listener, &peer);
        if (sock < 0) {
            printf("ursd: accept failed (errno=%d, %s)\n", errno,
                   sock_last_message());
            sleep(1);
            // netd restarting leaves a dead listener handle behind.
            if (++accept_failures >= URS_ACCEPT_FAIL_MAX) {
                sock_close(listener);
                listener = urs_listen_open();
                if (listener < 0) {
                    printf("ursd: cannot re-listen, exiting\n");
                    return 1;
                }
                accept_failures = 0;
            }
            continue;
        }
        accept_failures = 0;

        urs_end_t end = urs_serve(sock, listener, &peer);
        sock_close(sock);
        if (end == URS_END_AUTHFAIL) sleep(URS_COOLDOWN_S);
    }
    return 0;
}