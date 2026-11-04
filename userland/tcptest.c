// tcptest.c — exercises netd's TCP layer through the SHM mailbox.
//
// Usage:
//   tcptest [ip] [port] [--slow] [--refused-port N] [--eof-port N]
//
//   ip, port        echo server to test against. Default: netd's gateway
//                   address, port 7777.
//   --slow          also run the connection-table exhaustion/recovery test
//                   (can take 40+ seconds while old connections age out).
//   --refused-port  a port with nothing listening on the target (default 1).
//   --eof-port N    a port whose server sends some data and then closes the
//                   connection; enables the server-initiated-close test.
//
// The echo server must send back every byte it receives and keep the
// connection open until the client closes. On the host, any of these work:
//   socat TCP-LISTEN:7777,reuseaddr,fork EXEC:/bin/cat
//   ncat -l -k 7777 --exec /bin/cat
//
// For --eof-port, something like:
//   socat TCP-LISTEN:7778,reuseaddr,fork SYSTEM:'echo hello'
//
// Exit code is the number of failed tests.

#include "libc/libc.h"
#include "netd_ipc.h"

// ── Mailbox plumbing ────────────────────────────────────────────────────

static netd_shm_t *g_shm;
static netd_request_t  g_req;   // static: these are several KB each
static netd_response_t g_resp;
static char g_msg[96];          // reason for the most recent failure

static int g_pass = 0, g_fail = 0;

static void req_begin(int cmd, uint32_t timeout_ms) {
    memset(&g_req, 0, sizeof(g_req));
    g_req.cmd = cmd;
    g_req.timeout_ms = timeout_ms;
}

// Posts g_req and waits for g_resp. Returns 0 if netd answered.
static int mbox_call(void) {
    if (netd_ipc_call(g_shm, &g_req, &g_resp, g_req.timeout_ms + NETD_IPC_RESPONSE_MARGIN_MS) != 0) {
        snprintf(g_msg, sizeof(g_msg), "mailbox call timed out");
        return -1;
    }
    return 0;
}

// Like mbox_call, but also turns resp.success == 0 into a failure that
// carries netd's message.
static int mbox_ok(void) {
    if (mbox_call() != 0) return -1;
    if (!g_resp.success) {
        snprintf(g_msg, sizeof(g_msg), "%s", g_resp.message);
        return -1;
    }
    return 0;
}

// ── Small helpers ───────────────────────────────────────────────────────

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int contains(const char *hay, const char *needle) {
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return 1;
    }
    return 0;
}

static int parse_ip(const char *s, uint8_t out[4]) {
    int part = 0, val = -1;
    for (;; s++) {
        char c = *s;
        if (c >= '0' && c <= '9') {
            if (val < 0) val = 0;
            val = val * 10 + (c - '0');
            if (val > 255) return -1;
        } else if (c == '.' || c == 0) {
            if (val < 0 || part > 3) return -1;
            out[part++] = (uint8_t)val;
            val = -1;
            if (c == 0) break;
        } else {
            return -1;
        }
    }
    return part == 4 ? 0 : -1;
}

static int parse_port(const char *s) {
    if (!*s) return -1;
    int v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        if (v > 65535) return -1;
    }
    return v > 0 ? v : -1;
}

static void sleep_ms(uint32_t ms) {
    uint64_t end = time_ms() + ms;
    while (time_ms() < end) yield();
}

// Deterministic, non-repeating-enough byte pattern so reordering, loss and
// duplication all show up as mismatches. `seed` separates connections.
static uint8_t pat(uint32_t seed, uint32_t i) {
    uint32_t x = i * 2654435761u + seed * 40503u;
    return (uint8_t)((x >> 13) ^ (x >> 5) ^ i);
}

static void report(const char *name, int ok, const char *detail) {
    if (ok) { g_pass++; printf("[PASS] %s%s%s\n", name, detail ? " -- " : "", detail ? detail : ""); }
    else    { g_fail++; printf("[FAIL] %s%s%s\n", name, detail ? ": " : "", detail ? detail : g_msg); }
}

// ── TCP client wrappers (one mailbox round trip each) ───────────────────

static int t_open(const uint8_t ip[4], uint16_t port, int *handle) {
    req_begin(NETD_CMD_TCP_OPEN, 5000);
    memcpy(g_req.target_ip, ip, 4);
    g_req.tcp_dst_port = port;
    if (mbox_ok() != 0) return -1;
    *handle = g_resp.tcp_handle;
    return 0;
}

static int t_send(int h, const uint8_t *data, uint16_t len, uint32_t timeout_ms) {
    req_begin(NETD_CMD_TCP_SEND, timeout_ms);
    g_req.tcp_handle = h;
    g_req.tcp_payload_len = len;
    memcpy(g_req.tcp_payload, data, len);
    return mbox_ok();
}

static int t_recv(int h, uint8_t *out, uint16_t cap, uint32_t timeout_ms,
                  uint16_t *got, uint8_t *eof) {
    req_begin(NETD_CMD_TCP_RECV, timeout_ms);
    g_req.tcp_handle = h;
    g_req.tcp_recv_cap = cap;
    if (mbox_ok() != 0) return -1;
    *got = g_resp.tcp_payload_len;
    *eof = g_resp.tcp_eof;
    memcpy(out, g_resp.tcp_payload, *got);
    return 0;
}

static void t_close(int h) {
    req_begin(NETD_CMD_TCP_CLOSE, 1000);
    g_req.tcp_handle = h;
    mbox_call();
}

// ── Echo transfer with verification ─────────────────────────────────────

static uint8_t g_xbuf[NETD_TCP_MAX_PAYLOAD];

// Reads whatever is available (waiting up to timeout_ms), checks it against
// the pattern starting at offset *rcvd, and advances *rcvd.
// Returns 0 = ok (maybe nothing arrived), 1 = EOF, -1 = error (g_msg set).
static int consume(int h, uint32_t timeout_ms, uint32_t seed, uint32_t total, uint32_t *rcvd) {
    uint16_t got = 0;
    uint8_t eof = 0;
    if (t_recv(h, g_xbuf, sizeof(g_xbuf), timeout_ms, &got, &eof) != 0) return -1;
    if (*rcvd + got > total) {
        snprintf(g_msg, sizeof(g_msg), "received more than sent (%d extra)", (int)(*rcvd + got - total));
        return -1;
    }
    for (uint16_t i = 0; i < got; i++) {
        if (g_xbuf[i] != pat(seed, *rcvd + i)) {
            snprintf(g_msg, sizeof(g_msg), "data mismatch at offset %d (got %02x want %02x)",
                     (int)(*rcvd + i), g_xbuf[i], pat(seed, *rcvd + i));
            return -1;
        }
    }
    *rcvd += got;
    return eof ? 1 : 0;
}

// Sends `total` pattern bytes and verifies the echo. interleave=1 drains
// the echo while sending; interleave=0 sends everything first and only then
// reads, which makes the receive window close and reopen.
static int echo_xfer(int h, uint32_t total, uint32_t seed, int interleave, uint32_t *ms_out) {
    uint64_t t0 = time_ms();
    uint64_t deadline = t0 + 60000;
    uint32_t sent = 0, rcvd = 0;

    while (sent < total) {
        uint32_t chunk = total - sent;
        if (chunk > NETD_TCP_MAX_PAYLOAD) chunk = NETD_TCP_MAX_PAYLOAD;
        for (uint32_t i = 0; i < chunk; i++) g_xbuf[i] = pat(seed, sent + i);
        if (t_send(h, g_xbuf, (uint16_t)chunk, 5000) != 0) return -1;
        sent += chunk;
        if (interleave) {
            int r = consume(h, 1, seed, total, &rcvd);
            if (r < 0) return -1;
            if (r == 1 && rcvd < total) { snprintf(g_msg, sizeof(g_msg), "premature EOF"); return -1; }
        }
    }

    while (rcvd < total) {
        if (time_ms() >= deadline) {
            snprintf(g_msg, sizeof(g_msg), "timeout: sent=%d rcvd=%d of %d", (int)sent, (int)rcvd, (int)total);
            return -1;
        }
        int r = consume(h, 500, seed, total, &rcvd);
        if (r < 0) return -1;
        if (r == 1 && rcvd < total) { snprintf(g_msg, sizeof(g_msg), "premature EOF at %d", (int)rcvd); return -1; }
    }
    if (ms_out) *ms_out = (uint32_t)(time_ms() - t0);
    return 0;
}

// ── Tests ───────────────────────────────────────────────────────────────

static uint8_t g_ip[4];
static uint16_t g_port = 7777;
static uint16_t g_refused_port = 1;
static int g_eof_port = 0;
static int g_slow = 0;

static void test_status(void) {
    req_begin(NETD_CMD_GET_STATUS, 1000);
    if (mbox_ok() != 0) { report("status", 0, NULL); return; }
    char d[96];
    snprintf(d, sizeof(d), "ip=%d.%d.%d.%d gw=%d.%d.%d.%d dhcp=%d",
             g_resp.our_ip[0], g_resp.our_ip[1], g_resp.our_ip[2], g_resp.our_ip[3],
             g_resp.gateway_ip[0], g_resp.gateway_ip[1], g_resp.gateway_ip[2], g_resp.gateway_ip[3],
             g_resp.dhcp_bound);
    report("status", 1, d);
}

static void test_connect_close(void) {
    int h;
    if (t_open(g_ip, g_port, &h) != 0) { report("connect + close", 0, NULL); return; }
    t_close(h);
    report("connect + close", 1, NULL);
}

static void test_closed_handle(void) {
    int h;
    if (t_open(g_ip, g_port, &h) != 0) { report("closed handle rejected", 0, NULL); return; }
    t_close(h);

    uint8_t b = 'x';
    int send_rejected = (t_send(h, &b, 1, 500) != 0);
    uint16_t got; uint8_t eof;
    int recv_rejected = (t_recv(h, g_xbuf, 16, 100, &got, &eof) != 0);
    int double_close_ok;
    req_begin(NETD_CMD_TCP_CLOSE, 500);
    g_req.tcp_handle = h;
    double_close_ok = (mbox_call() == 0 && g_resp.success);

    if (!send_rejected)      { snprintf(g_msg, sizeof(g_msg), "send on closed handle succeeded"); report("closed handle rejected", 0, NULL); return; }
    if (!recv_rejected)      { snprintf(g_msg, sizeof(g_msg), "recv on closed handle succeeded"); report("closed handle rejected", 0, NULL); return; }
    if (!double_close_ok)    { snprintf(g_msg, sizeof(g_msg), "second close was not harmless");   report("closed handle rejected", 0, NULL); return; }
    report("closed handle rejected", 1, NULL);
}

static void test_bogus_handle(void) {
    uint8_t b = 'x';
    int rejected = (t_send(0x7FFFFF00 | 3, &b, 1, 300) != 0) && (t_send(-5, &b, 1, 300) != 0);
    if (!rejected) snprintf(g_msg, sizeof(g_msg), "garbage handle accepted");
    report("garbage handle rejected", rejected, rejected ? NULL : g_msg);
}

static void test_refused(void) {
    int h;
    uint64_t t0 = time_ms();
    if (t_open(g_ip, g_refused_port, &h) == 0) {
        t_close(h);
        snprintf(g_msg, sizeof(g_msg), "connected to port %d, which should have nothing listening", (int)g_refused_port);
        report("refused connect fails", 0, NULL);
        return;
    }
    char d[96];
    snprintf(d, sizeof(d), "\"%s\" after %d ms%s", g_msg, (int)(time_ms() - t0),
             contains(g_msg, "refused") ? "" : " (expected 'refused'; peer may be dropping instead of resetting)");
    report("refused connect fails", 1, d);
}

static void test_echo_small(void) {
    int h;
    if (t_open(g_ip, g_port, &h) != 0) { report("echo \"hello\"", 0, NULL); return; }
    static const uint8_t hello[] = "hello";
    uint32_t rcvd = 0;
    int ok = (t_send(h, hello, 5, 3000) == 0);
    uint64_t deadline = time_ms() + 5000;
    uint16_t got = 0; uint8_t eof = 0;
    uint8_t buf[16];
    uint16_t total = 0;
    while (ok && total < 5 && time_ms() < deadline) {
        if (t_recv(h, buf + total, (uint16_t)(sizeof(buf) - total), 500, &got, &eof) != 0) { ok = 0; break; }
        total += got;
        if (eof) break;
    }
    (void)rcvd;
    if (ok && total != 5) { snprintf(g_msg, sizeof(g_msg), "got %d bytes, wanted 5", (int)total); ok = 0; }
    if (ok) for (int i = 0; i < 5; i++) if (buf[i] != hello[i]) { snprintf(g_msg, sizeof(g_msg), "echo mismatch"); ok = 0; break; }
    t_close(h);
    report("echo \"hello\"", ok, NULL);
}

// Sizes straddling the interesting boundaries: one byte, the mailbox payload
// cap, the MSS, and the send/receive buffer sizes. All on one connection, so
// sequence/window state has to carry correctly from one transfer to the next.
static void test_echo_sizes(void) {
    static const uint32_t sizes[] = { 1, 536, 1023, 1024, 1025, 1460, 1461, 2920, 4095, 4096, 4097, 20000 };
    int h;
    if (t_open(g_ip, g_port, &h) != 0) { report("echo sizes (one connection)", 0, NULL); return; }
    int ok = 1;
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        if (echo_xfer(h, sizes[i], 100 + i, 1, NULL) != 0) {
            char d[96];
            snprintf(d, sizeof(d), "size %d: %s", (int)sizes[i], g_msg);
            snprintf(g_msg, sizeof(g_msg), "%s", d);
            ok = 0;
            break;
        }
    }
    t_close(h);
    report("echo sizes (one connection)", ok, NULL);
}

static void test_bulk(void) {
    const uint32_t total = 64 * 1024;
    int h;
    if (t_open(g_ip, g_port, &h) != 0) { report("bulk echo 64 KB", 0, NULL); return; }
    uint32_t ms = 0;
    int ok = (echo_xfer(h, total, 7, 1, &ms) == 0);
    t_close(h);
    if (!ok) { report("bulk echo 64 KB", 0, NULL); return; }
    char d[96];
    if (ms == 0) ms = 1;
    snprintf(d, sizeof(d), "%d ms, ~%d KB/s each way", (int)ms, (int)((uint64_t)total * 1000 / ms / 1024));
    report("bulk echo 64 KB", 1, d);
}

// Sends 16 KB without reading anything back. The echo fills our 4 KB
// receive buffer, our advertised window drops to zero, and the rest has to
// wait on the peer until we start reading and send window updates.
static void test_window_stall(void) {
    int h;
    if (t_open(g_ip, g_port, &h) != 0) { report("receive window close/reopen (16 KB)", 0, NULL); return; }
    uint32_t ms = 0;
    int ok = (echo_xfer(h, 16 * 1024, 21, 0, &ms) == 0);
    t_close(h);
    char d[48];
    snprintf(d, sizeof(d), "%d ms", (int)ms);
    report("receive window close/reopen (16 KB)", ok, ok ? d : NULL);
}

static void test_idle(void) {
    int h;
    if (t_open(g_ip, g_port, &h) != 0) { report("idle 3 s then echo", 0, NULL); return; }
    sleep_ms(3000);
    int ok = (echo_xfer(h, 700, 33, 1, NULL) == 0);
    t_close(h);
    report("idle 3 s then echo", ok, NULL);
}

#define NCONC 4
// Several connections interleaved, each with its own data pattern, to catch
// mixed-up connection state or crosstalk.
static void test_concurrent(void) {
    int h[NCONC];
    int opened = 0;
    int ok = 1;
    for (; opened < NCONC; opened++) {
        if (t_open(g_ip, g_port, &h[opened]) != 0) { ok = 0; break; }
    }
    uint32_t sent[NCONC] = {0}, rcvd[NCONC] = {0};
    const uint32_t per_conn = 8 * 1024;

    for (int round = 0; ok && round < (int)(per_conn / 1024); round++) {
        for (int c = 0; ok && c < NCONC; c++) {
            for (uint32_t i = 0; i < 1024; i++) g_xbuf[i] = pat(50 + c, sent[c] + i);
            if (t_send(h[c], g_xbuf, 1024, 5000) != 0) { ok = 0; break; }
            sent[c] += 1024;
        }
        for (int c = 0; ok && c < NCONC; c++) {
            uint64_t deadline = time_ms() + 10000;
            uint32_t want = sent[c];
            while (rcvd[c] < want) {
                if (time_ms() >= deadline) { snprintf(g_msg, sizeof(g_msg), "conn %d stalled at %d/%d", c, (int)rcvd[c], (int)want); ok = 0; break; }
                int r = consume(h[c], 500, 50 + c, per_conn, &rcvd[c]);
                if (r != 0) { if (r == 1) snprintf(g_msg, sizeof(g_msg), "conn %d: premature EOF", c); ok = 0; break; }
            }
        }
    }
    for (int c = 0; c < opened; c++) t_close(h[c]);
    char d[48];
    snprintf(d, sizeof(d), "%d connections x %d KB", NCONC, (int)(per_conn / 1024));
    report("concurrent connections", ok, ok ? d : NULL);
}

static void test_server_close(void) {
    int h;
    if (t_open(g_ip, (uint16_t)g_eof_port, &h) != 0) { report("server-initiated close -> EOF", 0, NULL); return; }
    uint64_t deadline = time_ms() + 10000;
    int total = 0, saw_eof = 0, ok = 1;
    while (time_ms() < deadline) {
        uint16_t got = 0; uint8_t eof = 0;
        if (t_recv(h, g_xbuf, sizeof(g_xbuf), 500, &got, &eof) != 0) { ok = 0; break; }
        total += got;
        if (eof) { saw_eof = 1; break; }
    }
    if (ok && !saw_eof) { snprintf(g_msg, sizeof(g_msg), "no EOF within 10 s (got %d bytes)", total); ok = 0; }

    // After EOF, sending is still allowed (CLOSE_WAIT) but the peer will
    // reset; what matters is that close still works afterwards.
    t_close(h);
    char d[48];
    snprintf(d, sizeof(d), "%d bytes then EOF", total);
    report("server-initiated close -> EOF", ok, ok ? d : NULL);
}

// Opens connections until netd refuses, checks the limit is the table size
// (8), then waits for old connections to finish closing and checks that
// slots become available again.
static void test_table_exhaustion(void) {
    int h[16];
    int n = 0;
    while (n < 16 && t_open(g_ip, g_port, &h[n]) == 0) n++;
    char d[96];
    int ok = 1;
    if (n == 0) { snprintf(g_msg, sizeof(g_msg), "could not open any connection (table still busy from earlier tests?)"); ok = 0; }
    else if (n > 8) { snprintf(g_msg, sizeof(g_msg), "opened %d connections; table should hold 8", n); ok = 0; }
    for (int i = 0; i < n; i++) t_close(h[i]);
    if (!ok) { report("connection table limit + recovery", 0, NULL); return; }

    uint64_t t0 = time_ms();
    int again = -1;
    while (time_ms() - t0 < 60000) {
        if (t_open(g_ip, g_port, &again) == 0) break;
        again = -1;
        sleep_ms(1000);
    }
    if (again < 0) {
        snprintf(g_msg, sizeof(g_msg), "opened %d, but no slot freed within 60 s", n);
        report("connection table limit + recovery", 0, NULL);
        return;
    }
    t_close(again);
    snprintf(d, sizeof(d), "opened %d, slot free again after %d ms", n, (int)(time_ms() - t0));
    report("connection table limit + recovery", 1, d);
}

// ── Main ────────────────────────────────────────────────────────────────

int md_main(long argc, char **argv) {
    int have_ip = 0, have_port = 0;

    for (long i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (streq(a, "--slow")) { g_slow = 1; }
        else if (streq(a, "--refused-port") && i + 1 < argc) {
            int p = parse_port(argv[++i]);
            if (p < 0) { printf("tcptest: bad --refused-port\n"); return 1; }
            g_refused_port = (uint16_t)p;
        }
        else if (streq(a, "--eof-port") && i + 1 < argc) {
            int p = parse_port(argv[++i]);
            if (p < 0) { printf("tcptest: bad --eof-port\n"); return 1; }
            g_eof_port = p;
        }
        else if (!have_ip && parse_ip(a, g_ip) == 0) { have_ip = 1; }
        else if (!have_port && parse_port(a) > 0) { g_port = (uint16_t)parse_port(a); have_port = 1; }
        else { printf("tcptest: unrecognized argument '%s'\n", a); return 1; }
    }

    int shm_handle = shm_open(NETD_IPC_SHM_NAME, O_RDWR, 0, 0);
    if (shm_handle < 0) {
        printf("tcptest: cannot attach to netd mailbox '%s' (is netd running?)\n", NETD_IPC_SHM_NAME);
        return 1;
    }
    g_shm = (netd_shm_t *)mmap(NULL, sizeof(netd_shm_t), PROT_R | PROT_W, MAP_SHARED, shm_handle);
    if (g_shm == MAP_FAILED) { printf("tcptest: mmap of netd mailbox failed\n"); return 1; }

    if (!have_ip) {
        req_begin(NETD_CMD_GET_STATUS, 1000);
        if (mbox_ok() != 0) { printf("tcptest: GET_STATUS failed: %s\n", g_msg); return 1; }
        memcpy(g_ip, g_resp.gateway_ip, 4);
    }

    printf("tcptest: echo server %d.%d.%d.%d:%d, refused port %d%s\n",
           g_ip[0], g_ip[1], g_ip[2], g_ip[3], (int)g_port, (int)g_refused_port,
           g_slow ? ", slow tests on" : "");

    test_status();
    test_connect_close();
    test_closed_handle();
    test_bogus_handle();
    test_refused();
    test_echo_small();
    test_echo_sizes();
    test_bulk();
    test_window_stall();
    test_idle();
    test_concurrent();
    if (g_eof_port) test_server_close();
    else printf("[SKIP] server-initiated close (no --eof-port)\n");
    if (g_slow) test_table_exhaustion();
    else printf("[SKIP] connection table limit + recovery (needs --slow)\n");

    printf("tcptest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}