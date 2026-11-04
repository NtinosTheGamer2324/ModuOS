// sockets.h -- a Python-flavoured socket API on top of netd_ipc.h
//
// Python                                   This header
// ------------------------------------     ---------------------------------------------
// s = socket.socket(AF_INET, SOCK_STREAM)  int s = socket(AF_INET, SOCK_STREAM, 0);
// s.settimeout(5.0)                        sock_settimeout(s, 5000);          // ms
// s.connect(("example.com", 80))           sock_connect(s, "example.com", 80);
// s.sendall(b"GET / HTTP/1.0\r\n\r\n")     sock_sendall(s, req, len);
// data = s.recv(1024)                      n = sock_recv(s, buf, sizeof buf);
// f = s.makefile(); f.readline()           n = sock_recvline(s, line, sizeof line);
// s.close()                                sock_close(s);
// s.bind((host, port)); s.listen()         sock_bind(s, host, port); sock_listen(s, 4);
// conn, addr = s.accept()                  c = sock_accept(s, &addr);   // new socket fd
// socket.gethostbyname("example.com")      gethostbyname("example.com", ip);
// socket.create_connection((h, p), t)      create_connection(h, p, t_ms);
// s.sendto(data, (host, port))             sock_sendto(s, data, len, host, port);
// data, addr = s.recvfrom(512)             sock_recvfrom(s, buf, cap, &addr);
//
// Errors follow C/POSIX style rather than Python exceptions: functions return
// -1 and set errno (ETIMEDOUT, ECONNREFUSED, ECONNRESET, ENOTCONN, EAGAIN...).
// sock_last_message() returns netd's last human-readable message for debugging.
//
// Example:
//
//   #include "sockets.h"
//   int md_main(long argc, char **argv) {
//       int s = create_connection("example.com", 80, 5000);
//       if (s < 0) { printf("connect failed: errno=%d (%s)\n", errno, sock_last_message()); return 1; }
//       const char *req = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
//       sock_sendall(s, req, strlen(req));
//       char line[256];
//       while (sock_recvline(s, line, sizeof line) > 0) puts_raw(line);
//       sock_close(s);
//       return 0;
//   }
//
// ── Limits imposed by what netd actually offers ───────────────────────
//  * TCP servers need the v5 netd (TCP_LISTEN/ACCEPT/UNLISTEN). bind() only
//    records a port: netd listens on every address, so `host` is ignored and
//    port 0 (ephemeral) is not supported. netd allows 4 listeners, a backlog
//    of 4 and 8 connections in total. Re-listening on a port replaces a stale
//    listener (e.g. left by a crashed server) instead of failing.
//  * While a process sits in sock_accept() it keeps the single mailbox busy
//    (in 1 s slices); other socket programs may see ETIMEDOUT meanwhile.
//  * UDP: netd has no real UDP sockets. One sendto() is one full exchange:
//    netd sends the datagram, waits up to the timeout for ONE reply from the
//    same (ip, port), and sockets.h buffers that reply for the next
//    recvfrom()/recv(). sendto() therefore BLOCKS for up to the timeout.
//    If no reply arrives, sendto() still returns len and the following
//    recvfrom() fails with ETIMEDOUT.
//  * No shutdown(), no select(), no setsockopt(). One mailbox slot, so only
//    one call is in flight system-wide; concurrent clients may see ETIMEDOUT
//    when the mailbox lock can't be acquired.
//  * Single-threaded, like the rest of this libc.
//  * Behaviour below was checked against netd.c: TCP_SEND is all-or-nothing;
//    TCP_RECV with no data after its timeout returns success=1, len=0,
//    eof=0; any success=0 from TCP_* is a hard error (reset / timed out /
//    bad handle), mapped back to errno from netd's message text.
//  * netd treats timeout_ms == 0 as "use the default (3-5 s)", so non-blocking
//    polls ask for 1 ms instead.
//  * A DHCP lease renewal blocks netd for up to several seconds; a call that
//    lands then can fail with ETIMEDOUT even though nothing is wrong.
//  * netd reports "timed out" for both "send buffer stayed full" and
//    "connection died of retransmit timeout" on TCP_SEND; both give ETIMEDOUT.

#pragma once
#include "netd_ipc.h"

// ── errno fallbacks (only defined if your errno.h lacks them) ─────────
#ifndef EBADF
#define EBADF 9
#endif
#ifndef EBUSY
#define EBUSY 16
#endif
#ifndef EAGAIN
#define EAGAIN 11
#endif
#ifndef ENOENT
#define ENOENT 2
#endif
#ifndef EMFILE
#define EMFILE 24
#endif
#ifndef EPIPE
#define EPIPE 32
#endif
#ifndef EMSGSIZE
#define EMSGSIZE 90
#endif
#ifndef EPROTONOSUPPORT
#define EPROTONOSUPPORT 93
#endif
#ifndef EOPNOTSUPP
#define EOPNOTSUPP 95
#endif
#ifndef EAFNOSUPPORT
#define EAFNOSUPPORT 97
#endif
#ifndef ENETDOWN
#define ENETDOWN 100
#endif
#ifndef ECONNRESET
#define ECONNRESET 104
#endif
#ifndef EISCONN
#define EISCONN 106
#endif
#ifndef ENOTCONN
#define ENOTCONN 107
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED 111
#endif
#ifndef ENOBUFS
#define ENOBUFS 105
#endif
#ifndef EHOSTUNREACH
#define EHOSTUNREACH 113
#endif

// ── Constants ─────────────────────────────────────────────────────────
#define AF_INET       2
#define SOCK_STREAM   1
#define SOCK_DGRAM    2

#define SOCK_BLOCKING   (-1)   // timeout value: wait forever (default)
#define SOCK_NONBLOCK   0      // timeout value: poll, never wait

#define SOCK_MAX                  8
#define SOCK_FD_BASE              256    // socket "fds" are not libc file fds
#define SOCK_CONNECT_TIMEOUT_MS   10000  // used when the socket has no finite timeout
#define SOCK_SEND_TIMEOUT_MS      5000
#define SOCK_UDP_DEFAULT_WAIT_MS  5000   // sendto() reply wait on a blocking socket
#define SOCK_DNS_TIMEOUT_MS       3000
#define SOCK_DNS_RETRIES          2

typedef struct {
    uint8_t  ip[4];
    uint16_t port;
} sock_addr_t;

typedef struct {
    uint8_t ip[4];
    uint8_t gateway[4];
    uint8_t netmask[4];
    uint8_t mac[6];
    uint8_t link_up;
    uint8_t dhcp_bound;
} sock_netinfo_t;

// ── Internal state ───────────────────────────────────────────────────
typedef struct {
    uint8_t  used;
    uint8_t  type;          // SOCK_STREAM / SOCK_DGRAM
    uint8_t  connected;     // STREAM: TCP is open. DGRAM: default peer set.
    uint8_t  eof;           // STREAM: peer closed, nothing more will arrive
    uint8_t  broken;        // (unused; netd is the source of truth)
    uint8_t  listening;     // STREAM: TCP_LISTEN succeeded; `handle` is the listener
    uint16_t bound_port;    // STREAM: port recorded by sock_bind()
    uint8_t  udp_has;       // DGRAM: a buffered reply is waiting
    int      handle;        // netd tcp_handle (STREAM)
    int      pending_err;   // DGRAM: errno to report if no reply arrived
    int32_t  timeout_ms;    // SOCK_BLOCKING (-1), 0, or >0
    uint8_t  peer_ip[4];
    uint16_t peer_port;
    uint8_t  from_ip[4];    // DGRAM: source of buffered reply
    uint16_t from_port;
    uint16_t rx_off, rx_len;
    uint8_t  rx[NETD_TCP_MAX_PAYLOAD]; // STREAM read-ahead / DGRAM reply
} sock_t;

static sock_t          __socks[SOCK_MAX];
static netd_shm_t     *__sock_shm = NULL;
static netd_request_t  __sock_req;     // static: keeps ~1.5 KB off the user stack
static netd_response_t __sock_resp;
static char            __sock_msg[64];
static uint8_t         __sock_dns_ip[4];
static int             __sock_dns_set = 0;

static inline const char *sock_last_message(void) { return __sock_msg; }

// ── Small helpers ───────────────────────────────────────────────────
static inline int __sock_ci_contains(const char *h, const char *n) {
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b) {
            char x = *a, y = *b;
            if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
            if (x != y) break;
            a++; b++;
        }
        if (!*b) return 1;
    }
    return 0;
}

// Maps netd's tcp_err_text() strings back to errno:
//   "tcp: connection refused" / "reset" / "timed out" / "bad handle or connection state"
static inline int __sock_errno_from_msg(void) {
    if (__sock_ci_contains(__sock_msg, "refused")) return ECONNREFUSED;
    if (__sock_ci_contains(__sock_msg, "reset"))   return ECONNRESET;
    if (__sock_ci_contains(__sock_msg, "timed out")) return ETIMEDOUT;
    return ENOTCONN;
}

static inline sock_t *__sock_get(int fd) {
    int i = fd - SOCK_FD_BASE;
    if (i < 0 || i >= SOCK_MAX || !__socks[i].used) { errno = EBADF; return NULL; }
    return &__socks[i];
}

// ── netd mailbox plumbing ───────────────────────────────────────────
static inline int __sock_attach(void) {
    if (__sock_shm) return 0;
    int h = shm_open(NETD_IPC_SHM_NAME, O_RDWR, 0, 0);   // size 0 = attach existing
    if (h < 0) { errno = ENETDOWN; return -1; }          // netd not running
    void *p = mmap(NULL, sizeof(netd_shm_t), PROT_R | PROT_W, MAP_SHARED, h);
    if (!p || p == MAP_FAILED || (long)p < 0) { errno = ENETDOWN; return -1; }
    __sock_shm = (netd_shm_t *)p;
    return 0;
}

static inline netd_request_t *__sock_req_init(int cmd, uint32_t timeout_ms) {
    memset(&__sock_req, 0, sizeof(__sock_req));
    __sock_req.cmd = cmd;
    __sock_req.timeout_ms = timeout_ms;
    return &__sock_req;
}

// Posts __sock_req, fills __sock_resp. Returns 0 if netd answered, -1 if the
// mailbox was busy or netd never replied (errno = ETIMEDOUT).
static inline int __sock_rpc(uint32_t op_timeout_ms) {
    if (__sock_attach() != 0) return -1;
    memset(&__sock_resp, 0, sizeof(__sock_resp));
    if (netd_ipc_call(__sock_shm, &__sock_req, &__sock_resp,
                      op_timeout_ms + NETD_IPC_RESPONSE_MARGIN_MS) != 0) {
        errno = ETIMEDOUT;
        return -1;
    }
    memcpy(__sock_msg, __sock_resp.message, sizeof(__sock_msg));
    __sock_msg[sizeof(__sock_msg) - 1] = 0;
    return 0;
}

// ── Address helpers ─────────────────────────────────────────────────
// Returns 1 if `s` is a valid dotted quad (stored in out), else 0.
static inline int inet_aton(const char *s, uint8_t out[4]) {
    if (!s) return 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return 0;
        int v = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            if (++digits > 3 || v > 255) return 0;
            s++;
        }
        out[part] = (uint8_t)v;
        if (part < 3) { if (*s != '.') return 0; s++; }
    }
    return *s == 0;
}

// Returns a pointer to a static buffer (overwritten per call).
static inline const char *inet_ntoa(const uint8_t ip[4]) {
    static char buf[16];
    snprintf(buf, sizeof(buf), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    return buf;
}

// ── Network info ────────────────────────────────────────────────────
static inline int sock_get_netinfo(sock_netinfo_t *out) {
    __sock_req_init(NETD_CMD_GET_STATUS, 1000);
    if (__sock_rpc(1000) != 0) return -1;
    if (!__sock_resp.success) { errno = ENETDOWN; return -1; }
    memcpy(out->ip, __sock_resp.our_ip, 4);
    memcpy(out->gateway, __sock_resp.gateway_ip, 4);
    memcpy(out->netmask, __sock_resp.netmask, 4);
    memcpy(out->mac, __sock_resp.mac, 6);
    out->link_up = __sock_resp.link_up;
    out->dhcp_bound = __sock_resp.dhcp_bound;
    return 0;
}

static inline int get_local_ip(uint8_t ip[4]) {
    sock_netinfo_t ni;
    if (sock_get_netinfo(&ni) != 0) return -1;
    memcpy(ip, ni.ip, 4);
    return 0;
}

// ── DNS (gethostbyname) ─────────────────────────────────────────────
// netd doesn't expose the DHCP-provided DNS server, so we default to the
// gateway (what passt/SLIRP-style setups forward DNS through). Override with
// sock_set_dns() if yours differs.
static inline void sock_set_dns(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    __sock_dns_ip[0] = a; __sock_dns_ip[1] = b; __sock_dns_ip[2] = c; __sock_dns_ip[3] = d;
    __sock_dns_set = 1;
}

static inline int __dns_build(const char *name, uint16_t id, uint8_t *out, int cap) {
    int n = 0;
    if (cap < 18) return -1;
    out[n++] = (uint8_t)(id >> 8); out[n++] = (uint8_t)id;
    out[n++] = 0x01; out[n++] = 0x00;                // flags: RD
    out[n++] = 0; out[n++] = 1;                      // QDCOUNT = 1
    for (int i = 0; i < 6; i++) out[n++] = 0;        // AN/NS/AR = 0
    const char *p = name;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        int len = (int)(dot - p);
        if (len == 0 || len > 63) return -1;
        if (n + 1 + len + 5 > cap) return -1;
        out[n++] = (uint8_t)len;
        memcpy(out + n, p, (size_t)len);
        n += len;
        p = dot;
        if (*p == '.') p++;
    }
    out[n++] = 0;
    out[n++] = 0; out[n++] = 1;                      // QTYPE  = A
    out[n++] = 0; out[n++] = 1;                      // QCLASS = IN
    return n;
}

static inline int __dns_skip_name(const uint8_t *b, int len, int pos) {
    while (pos < len) {
        uint8_t c = b[pos];
        if (c == 0) return pos + 1;
        if ((c & 0xC0) == 0xC0) return (pos + 2 <= len) ? pos + 2 : -1;
        pos += 1 + c;
    }
    return -1;
}

// Returns 0 and fills ip on success; -1 on malformed/NXDOMAIN (errno=ENOENT).
static inline int __dns_parse(const uint8_t *b, int len, uint16_t id, uint8_t ip[4]) {
    if (len < 12) return -1;
    if ((uint16_t)((b[0] << 8) | b[1]) != id) return -1;
    if (!(b[2] & 0x80)) return -1;                   // not a response
    if ((b[3] & 0x0F) != 0) { errno = ENOENT; return -1; }
    int qd = (b[4] << 8) | b[5];
    int an = (b[6] << 8) | b[7];
    int pos = 12;
    for (int i = 0; i < qd; i++) {
        pos = __dns_skip_name(b, len, pos);
        if (pos < 0) return -1;
        pos += 4;
    }
    for (int i = 0; i < an; i++) {
        pos = __dns_skip_name(b, len, pos);
        if (pos < 0 || pos + 10 > len) return -1;
        int type = (b[pos] << 8) | b[pos + 1];
        int rdlen = (b[pos + 8] << 8) | b[pos + 9];
        pos += 10;
        if (pos + rdlen > len) return -1;
        if (type == 1 && rdlen == 4) { memcpy(ip, b + pos, 4); return 0; }
        pos += rdlen;                                // CNAME etc: skip
    }
    errno = ENOENT;
    return -1;
}

// Like Python's socket.gethostbyname, but writes the IPv4 into `out`.
// Dotted quads pass straight through. Returns 0 on success, -1 on failure.
static inline int gethostbyname(const char *host, uint8_t out[4]) {
    if (!host || !*host) { errno = EINVAL; return -1; }
    if (inet_aton(host, out)) return 0;

    uint8_t dns[4];
    if (__sock_dns_set) memcpy(dns, __sock_dns_ip, 4);
    else {
        sock_netinfo_t ni;
        if (sock_get_netinfo(&ni) != 0) return -1;
        memcpy(dns, ni.gateway, 4);
    }

    uint8_t q[NETD_UDP_MAX_PAYLOAD];
    uint16_t id = (uint16_t)(time_ms() ^ (uint64_t)rand());
    int qlen = __dns_build(host, id, q, (int)sizeof(q));
    if (qlen < 0) { errno = EINVAL; return -1; }

    for (int attempt = 0; attempt < SOCK_DNS_RETRIES; attempt++) {
        netd_request_t *r = __sock_req_init(NETD_CMD_UDP_SEND, SOCK_DNS_TIMEOUT_MS);
        memcpy(r->target_ip, dns, 4);
        r->udp_dst_port = 53;
        r->udp_src_port = 0;
        r->udp_payload_len = (uint16_t)qlen;
        memcpy(r->udp_payload, q, (size_t)qlen);
        if (__sock_rpc(SOCK_DNS_TIMEOUT_MS) != 0) return -1;
        if (!__sock_resp.success || __sock_resp.udp_reply_len == 0) continue;
        // netd reports the reply's real length even if truncated to 512
        int rl = __sock_resp.udp_reply_len > NETD_UDP_MAX_PAYLOAD
                     ? NETD_UDP_MAX_PAYLOAD : (int)__sock_resp.udp_reply_len;
        if (__dns_parse(__sock_resp.udp_reply, rl, id, out) == 0) return 0;
        if (errno == ENOENT) return -1;              // definitive NXDOMAIN
    }
    errno = ETIMEDOUT;
    return -1;
}

static inline int __sock_resolve(const char *host, uint8_t ip[4]) {
    return gethostbyname(host, ip);
}

// ── socket() / settimeout() / close() ───────────────────────────────
static inline int socket(int family, int type, int proto) {
    (void)proto;
    if (family != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    if (type != SOCK_STREAM && type != SOCK_DGRAM) { errno = EPROTONOSUPPORT; return -1; }
    for (int i = 0; i < SOCK_MAX; i++) {
        if (!__socks[i].used) {
            memset(&__socks[i], 0, sizeof(sock_t));
            __socks[i].used = 1;
            __socks[i].type = (uint8_t)type;
            __socks[i].handle = -1;
            __socks[i].timeout_ms = SOCK_BLOCKING;
            return SOCK_FD_BASE + i;
        }
    }
    errno = EMFILE;
    return -1;
}

// timeout_ms: SOCK_BLOCKING (-1) = forever, SOCK_NONBLOCK (0) = poll, >0 = ms.
static inline int sock_settimeout(int fd, int32_t timeout_ms) {
    sock_t *s = __sock_get(fd);
    if (!s) return -1;
    s->timeout_ms = timeout_ms < 0 ? SOCK_BLOCKING : timeout_ms;
    return 0;
}

static inline int32_t sock_gettimeout(int fd) {
    sock_t *s = __sock_get(fd);
    return s ? s->timeout_ms : -2;
}

static inline int sock_setblocking(int fd, int blocking) {
    return sock_settimeout(fd, blocking ? SOCK_BLOCKING : SOCK_NONBLOCK);
}

static inline int sock_close(int fd) {
    sock_t *s = __sock_get(fd);
    if (!s) return -1;
    if (s->type == SOCK_STREAM && s->listening && s->handle >= 0) {
        __sock_req_init(NETD_CMD_TCP_UNLISTEN, 2000)->tcp_handle = s->handle;
        __sock_rpc(2000);                            // best effort
    } else if (s->type == SOCK_STREAM && s->connected && s->handle >= 0) {
        __sock_req_init(NETD_CMD_TCP_CLOSE, 2000)->tcp_handle = s->handle;
        __sock_rpc(2000);                            // best effort
    }
    memset(s, 0, sizeof(sock_t));
    return 0;
}

// ── connect() ───────────────────────────────────────────────────────
// STREAM: performs the TCP handshake (blocks up to the socket timeout, or
//   SOCK_CONNECT_TIMEOUT_MS if the socket is blocking/non-blocking).
// DGRAM:  just remembers the default peer for send()/recv().
static inline int sock_connect(int fd, const char *host, uint16_t port) {
    sock_t *s = __sock_get(fd);
    if (!s) return -1;
    if (s->connected) { errno = EISCONN; return -1; }
    if (s->listening) { errno = EINVAL; return -1; }

    uint8_t ip[4];
    if (__sock_resolve(host, ip) != 0) return -1;

    if (s->type == SOCK_DGRAM) {
        memcpy(s->peer_ip, ip, 4);
        s->peer_port = port;
        s->connected = 1;
        return 0;
    }

    uint32_t tmo = s->timeout_ms > 0 ? (uint32_t)s->timeout_ms : SOCK_CONNECT_TIMEOUT_MS;
    netd_request_t *r = __sock_req_init(NETD_CMD_TCP_OPEN, tmo);
    memcpy(r->target_ip, ip, 4);
    r->tcp_dst_port = port;
    if (__sock_rpc(tmo) != 0) return -1;
    if (!__sock_resp.success) {
        int e = __sock_errno_from_msg();
        errno = (e == ENOTCONN) ? ENOBUFS : e;   // "bad handle" on OPEN = connection table full
        return -1;
    }
    s->handle = __sock_resp.tcp_handle;
    memcpy(s->peer_ip, ip, 4);
    s->peer_port = port;
    s->connected = 1;
    return 0;
}

// Python's socket.create_connection(): socket + settimeout + connect.
// Returns the socket fd, or -1 (socket is cleaned up).
static inline int create_connection(const char *host, uint16_t port, int32_t timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sock_settimeout(fd, timeout_ms);
    if (sock_connect(fd, host, port) != 0) {
        int saved = errno;
        sock_close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static inline int sock_getpeername(int fd, sock_addr_t *out) {
    sock_t *s = __sock_get(fd);
    if (!s) return -1;
    if (!s->connected) { errno = ENOTCONN; return -1; }
    memcpy(out->ip, s->peer_ip, 4);
    out->port = s->peer_port;
    return 0;
}

// ── TCP server: bind / listen / accept ──────────────────────────────
// bind() just records the port; netd listens on all addresses, so `host`
// ("", "0.0.0.0", "127.0.0.1", ...) is accepted and ignored.
static inline int sock_bind(int fd, const char *host, uint16_t port) {
    (void)host;
    sock_t *s = __sock_get(fd);
    if (!s) return -1;
    if (s->type != SOCK_STREAM) { errno = EOPNOTSUPP; return -1; }
    if (s->connected || s->listening || s->bound_port || port == 0) { errno = EINVAL; return -1; }
    s->bound_port = port;
    return 0;
}

// `backlog` is accepted for Python parity; netd's backlog is fixed at 4.
static inline int sock_listen(int fd, int backlog) {
    (void)backlog;
    sock_t *s = __sock_get(fd);
    if (!s) return -1;
    if (s->type != SOCK_STREAM || !s->bound_port) { errno = EINVAL; return -1; }
    if (s->listening) return 0;

    __sock_req_init(NETD_CMD_TCP_LISTEN, 1000)->tcp_dst_port = s->bound_port;
    if (__sock_rpc(1000) != 0) return -1;
    if (!__sock_resp.success) {
        int e = __sock_errno_from_msg();
        errno = (e == ENOTCONN) ? ENOBUFS : e;       // "bad handle" = listener table full
        return -1;
    }
    s->handle = __sock_resp.tcp_handle;
    s->listening = 1;
    return 0;
}

// Waits for a connection per the listener's timeout (blocking = forever,
// 0 = poll -> EAGAIN, >0 = ms -> ETIMEDOUT). Returns a NEW connected
// socket fd (blocking by default, like Python), or -1. `peer` may be NULL.
static inline int sock_accept(int fd, sock_addr_t *peer) {
    sock_t *l = __sock_get(fd);
    if (!l) return -1;
    if (l->type != SOCK_STREAM || !l->listening) { errno = EINVAL; return -1; }

    int ni = -1;                                     // reserve a slot first so an accepted
    for (int i = 0; i < SOCK_MAX; i++)               // connection is never left orphaned
        if (!__socks[i].used) { ni = i; break; }
    if (ni < 0) { errno = EMFILE; return -1; }

    int blocking = l->timeout_ms < 0;
    uint32_t slice = blocking ? 1000u : (l->timeout_ms == 0 ? 1u : (uint32_t)l->timeout_ms);

    for (;;) {
        __sock_req_init(NETD_CMD_TCP_ACCEPT, slice)->tcp_handle = l->handle;
        if (__sock_rpc(slice) != 0) return -1;
        if (!__sock_resp.success) { errno = __sock_errno_from_msg(); return -1; }

        if (__sock_resp.tcp_handle != 0) {
            sock_t *c = &__socks[ni];
            memset(c, 0, sizeof(sock_t));
            c->used = 1;
            c->type = SOCK_STREAM;
            c->connected = 1;
            c->handle = __sock_resp.tcp_handle;
            c->timeout_ms = SOCK_BLOCKING;
            memcpy(c->peer_ip, __sock_resp.peer_ip, 4);
            c->peer_port = __sock_resp.peer_port;
            if (peer) { memcpy(peer->ip, c->peer_ip, 4); peer->port = c->peer_port; }
            return SOCK_FD_BASE + ni;
        }
        if (blocking) continue;
        errno = (l->timeout_ms == 0) ? EAGAIN : ETIMEDOUT;
        return -1;
    }
}

// ── UDP: sendto() / recvfrom() ──────────────────────────────────────
static inline ssize_t __sock_udp_xchg(sock_t *s, const void *data, size_t len,
                                      const uint8_t ip[4], uint16_t port) {
    if (len > NETD_UDP_MAX_PAYLOAD) { errno = EMSGSIZE; return -1; }
    uint32_t tmo = s->timeout_ms > 0 ? (uint32_t)s->timeout_ms
                 : (s->timeout_ms < 0 ? SOCK_UDP_DEFAULT_WAIT_MS : 100);

    netd_request_t *r = __sock_req_init(NETD_CMD_UDP_SEND, tmo);
    memcpy(r->target_ip, ip, 4);
    r->udp_dst_port = port;
    r->udp_src_port = 0;
    r->udp_payload_len = (uint16_t)len;
    if (len) memcpy(r->udp_payload, data, len);
    if (__sock_rpc(tmo) != 0) return -1;

    s->udp_has = 0;
    s->pending_err = 0;
    s->rx_off = s->rx_len = 0;
    if (__sock_resp.success) {
        uint16_t n = __sock_resp.udp_reply_len;
        if (n > NETD_UDP_MAX_PAYLOAD) n = NETD_UDP_MAX_PAYLOAD;
        memcpy(s->rx, __sock_resp.udp_reply, n);
        s->rx_len = n;
        s->udp_has = 1;
        memcpy(s->from_ip, ip, 4);
        s->from_port = port;
    } else {
        s->pending_err = ETIMEDOUT;                  // reported by the next recvfrom()
    }
    return (ssize_t)len;
}

static inline ssize_t sock_sendto(int fd, const void *data, size_t len,
                                  const char *host, uint16_t port) {
    sock_t *s = __sock_get(fd);
    if (!s) return -1;
    if (s->type != SOCK_DGRAM) { errno = EOPNOTSUPP; return -1; }
    uint8_t ip[4];
    if (__sock_resolve(host, ip) != 0) return -1;
    return __sock_udp_xchg(s, data, len, ip, port);
}

// Returns bytes copied (extra bytes of an oversized datagram are discarded,
// like Python), or -1 with errno = ETIMEDOUT (no reply came) / EAGAIN
// (nothing was sent yet).
static inline ssize_t sock_recvfrom(int fd, void *buf, size_t cap, sock_addr_t *from) {
    sock_t *s = __sock_get(fd);
    if (!s) return -1;
    if (s->type != SOCK_DGRAM) { errno = EOPNOTSUPP; return -1; }
    if (!s->udp_has) {
        errno = s->pending_err ? s->pending_err : EAGAIN;
        s->pending_err = 0;
        return -1;
    }
    size_t n = s->rx_len < cap ? s->rx_len : cap;
    memcpy(buf, s->rx, n);
    if (from) { memcpy(from->ip, s->from_ip, 4); from->port = s->from_port; }
    s->udp_has = 0;
    s->rx_off = s->rx_len = 0;
    return (ssize_t)n;
}

// ── TCP: send / sendall ─────────────────────────────────────────────
// Sends at most NETD_TCP_MAX_PAYLOAD bytes (one mailbox round trip); returns
// the byte count. For UDP sockets this is sendto() to the connected peer.
static inline ssize_t sock_send(int fd, const void *buf, size_t len) {
    sock_t *s = __sock_get(fd);
    if (!s) return -1;
    if (!s->connected) { errno = ENOTCONN; return -1; }
    if (s->type == SOCK_DGRAM) return __sock_udp_xchg(s, buf, len, s->peer_ip, s->peer_port);
    if (len == 0) return 0;

    size_t n = len > NETD_TCP_MAX_PAYLOAD ? NETD_TCP_MAX_PAYLOAD : len;
    uint32_t tmo = s->timeout_ms > 0 ? (uint32_t)s->timeout_ms : SOCK_SEND_TIMEOUT_MS;
    netd_request_t *r = __sock_req_init(NETD_CMD_TCP_SEND, tmo);
    r->tcp_handle = s->handle;
    r->tcp_payload_len = (uint16_t)n;
    memcpy(r->tcp_payload, buf, n);
    if (__sock_rpc(tmo) != 0) return -1;
    if (!__sock_resp.success) {                      // netd: all-or-nothing, nothing queued
        errno = __sock_errno_from_msg();
        return -1;
    }
    return (ssize_t)n;
}

// Python's sendall: loops until everything is queued. Returns 0 or -1.
static inline int sock_sendall(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = sock_send(fd, p, len);
        if (n < 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static inline int sock_sendstr(int fd, const char *str) {
    return sock_sendall(fd, str, strlen(str));
}

// ── TCP: recv ───────────────────────────────────────────────────────
// One fetch from netd straight into dst (cap <= NETD_TCP_MAX_PAYLOAD).
// Returns bytes (>0), 0 on EOF, -1 on error/timeout.
static inline ssize_t __sock_stream_fetch(sock_t *s, uint8_t *dst, size_t cap) {
    if (cap > NETD_TCP_MAX_PAYLOAD) cap = NETD_TCP_MAX_PAYLOAD;
    int blocking = s->timeout_ms < 0;
    // netd maps timeout_ms==0 to its 5 s default, so a poll must ask for 1 ms.
    uint32_t slice = blocking ? 1000u : (s->timeout_ms == 0 ? 1u : (uint32_t)s->timeout_ms);

    for (;;) {
        netd_request_t *r = __sock_req_init(NETD_CMD_TCP_RECV, slice);
        r->tcp_handle = s->handle;
        r->tcp_recv_cap = (uint16_t)cap;
        if (__sock_rpc(slice) != 0) return -1;

        // success=0 is always a hard error here (reset / dead / bad handle).
        // netd returns buffered data before reporting a dead connection.
        if (!__sock_resp.success) { errno = __sock_errno_from_msg(); return -1; }

        uint16_t n = __sock_resp.tcp_payload_len;
        if (n > cap) n = (uint16_t)cap;
        if (n > 0) { memcpy(dst, __sock_resp.tcp_payload, n); return n; }
        if (__sock_resp.tcp_eof) { s->eof = 1; return 0; }

        // success, no data, no EOF: the wait simply expired.
        if (blocking) continue;                      // keep waiting in 1 s slices
        errno = (s->timeout_ms == 0) ? EAGAIN : ETIMEDOUT;
        return -1;
    }
}

// Returns bytes read (<= cap), 0 on EOF (stream) , -1 on error/timeout.
static inline ssize_t sock_recv(int fd, void *buf, size_t cap) {
    sock_t *s = __sock_get(fd);
    if (!s) return -1;
    if (s->type == SOCK_DGRAM) return sock_recvfrom(fd, buf, cap, NULL);
    if (!s->connected) { errno = ENOTCONN; return -1; }
    if (cap == 0) return 0;

    if (s->rx_len) {                                 // drain read-ahead first
        size_t n = s->rx_len < cap ? s->rx_len : cap;
        memcpy(buf, s->rx + s->rx_off, n);
        s->rx_off += (uint16_t)n;
        s->rx_len -= (uint16_t)n;
        return (ssize_t)n;
    }
    if (s->eof) return 0;
    return __sock_stream_fetch(s, (uint8_t *)buf, cap);
}

// Read exactly `want` bytes. Returns `want`, a smaller count if the peer
// closed early, or -1 on error/timeout with nothing read.
static inline ssize_t sock_recv_exact(int fd, void *buf, size_t want) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < want) {
        ssize_t n = sock_recv(fd, p + got, want - got);
        if (n < 0) return got ? (ssize_t)got : -1;
        if (n == 0) break;
        got += (size_t)n;
    }
    return (ssize_t)got;
}

// Read until EOF or `cap` is full (handy for HTTP/1.0 responses).
// Returns total bytes, or -1 if nothing was read and an error occurred.
static inline ssize_t sock_recv_all(int fd, void *buf, size_t cap) {
    return sock_recv_exact(fd, buf, cap);
}

// Like file.readline() on s.makefile(): reads through '\n' (included) and
// NUL-terminates. Returns line length, 0 on EOF with no data, -1 on error.
// If the line is cut short by a timeout, the partial text is returned.
static inline ssize_t sock_recvline(int fd, char *buf, size_t cap) {
    sock_t *s = __sock_get(fd);
    if (!s) return -1;
    if (s->type != SOCK_STREAM) { errno = EOPNOTSUPP; return -1; }
    if (!s->connected) { errno = ENOTCONN; return -1; }
    if (!buf || cap < 2) { errno = EINVAL; return -1; }

    size_t out = 0;
    for (;;) {
        while (s->rx_len && out < cap - 1) {
            char c = (char)s->rx[s->rx_off++];
            s->rx_len--;
            buf[out++] = c;
            if (c == '\n') { buf[out] = 0; return (ssize_t)out; }
        }
        if (out >= cap - 1 || s->eof) { buf[out] = 0; return (ssize_t)out; }

        ssize_t n = __sock_stream_fetch(s, s->rx, NETD_TCP_MAX_PAYLOAD);
        if (n < 0) { buf[out] = 0; return out ? (ssize_t)out : -1; }
        if (n == 0) { buf[out] = 0; return (ssize_t)out; }
        s->rx_off = 0;
        s->rx_len = (uint16_t)n;
    }
}

// ── ping ────────────────────────────────────────────────────────────
// Returns 0 and fills *rtt_ms on reply, -1 otherwise.
static inline int sock_ping(const char *host, uint32_t timeout_ms, uint32_t *rtt_ms) {
    uint8_t ip[4];
    if (__sock_resolve(host, ip) != 0) return -1;
    netd_request_t *r = __sock_req_init(NETD_CMD_PING, timeout_ms);
    memcpy(r->target_ip, ip, 4);
    if (__sock_rpc(timeout_ms) != 0) return -1;
    if (!__sock_resp.success) { errno = ETIMEDOUT; return -1; }
    if (rtt_ms) *rtt_ms = __sock_resp.rtt_ms;
    return 0;
}