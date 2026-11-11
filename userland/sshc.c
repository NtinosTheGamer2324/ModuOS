// sshc.c - SSH-2 client for ModuOS: password authentication, interactive shell.
//
// Usage:   sshc [-p port] [-l user] [-t term] [user@]host
// Escape:  Enter, then "~."  drops the connection (a lone "~~" sends one "~").
//
// Build:   userland/build.sh picks up *.c -> dist/sshc.sqr. The crypto library
//          is a shared lib, so build.sh needs one line in DYNAMIC_APPS:
//              sshc:crypto
//
// ── What is negotiated ──────────────────────────────────────────────────
// The algorithm set is dictated by what lib_crypto.c can do today, not by
// preference. A server that offers none of these is refused with a message.
//
//   kex          curve25519-sha256 (x25519 + SHA-256)
//   host key     rsa-sha2-256 (preferred: RSA verify is the cheaper one) or
//                ecdsa-sha2-nistp256. ssh-ed25519 is not offered because the
//                library has no Ed25519 verify.
//   cipher       aes128-gcm@openssh.com (AEAD, so no separate MAC)
//   compression  none
//   user auth    password only
//
// Host keys are pinned trust-on-first-use in SSHC_KNOWN_HOSTS, one line per
// "host:port algorithm sha256-hex". A changed key aborts the connection.
//
// ── Limits ──────────────────────────────────────────────────────────────
//  * No public-key or keyboard-interactive auth, no port forwarding, no agent.
//  * No re-keying. OpenSSH asks for one after about a gigabyte of traffic; the
//    session then ends with an error instead of continuing unprotected.
//  * Random numbers come from $/dev/md64api/urandom. It fails closed, so QEMU
//    needs "-cpu max" (RDSEED/RDRAND) or the handshake cannot start.
//  * netd has one mailbox: while sshc runs, other socket programs may see
//    ETIMEDOUT. The interactive loop polls the socket and the tty (no select).
//  * The terminal size is sent when the session starts and whenever it
//    changes; there is no SIGWINCH, so it is sampled every 500 ms.

#include "libc/libc.h"
#include "libc/tty.h"
#include "sockets.h"
#include "crypto.h"

// ── Configuration ───────────────────────────────────────────────────────

#define SSHC_PORT_DEFAULT        22
#define SSHC_CONNECT_TIMEOUT_MS  10000
#define SSHC_IDENT_TIMEOUT_MS    10000
#define SSHC_HANDSHAKE_TIMEOUT   30000
#define SSHC_AUTH_TIMEOUT        60000
#define SSHC_AUTH_ATTEMPTS       3

#define SSHC_KNOWN_HOSTS_DIR     "/ModuOS/shared/ssh"
#define SSHC_KNOWN_HOSTS         "/ModuOS/shared/ssh/known_hosts"
#define SSHC_KNOWN_HOSTS_MAX     16384

// RFC 4253 obliges every peer to accept 35000 bytes of packet.
#define SSHC_MAX_PACKET          35000
#define SSHC_RX_CAP              (SSHC_MAX_PACKET + 64)
#define SSHC_TX_CAP              2304
#define SSHC_KEXINIT_C_CAP       1024
#define SSHC_KEXINIT_S_CAP       8192

#define SSHC_LOCAL_WINDOW        (256 * 1024)
#define SSHC_LOCAL_MAX_PACKET    32768
#define SSHC_INPUT_CHUNK         256
#define SSHC_WINSIZE_POLL_MS     500

#define SSHC_VERSION_STRING      "SSH-2.0-ModuOS_sshc_1.0"

#define SSHC_KEX_OFFER           "curve25519-sha256,curve25519-sha256@libssh.org"
#define SSHC_HOSTKEY_OFFER       "rsa-sha2-256,ecdsa-sha2-nistp256"
#define SSHC_CIPHER              "aes128-gcm@openssh.com"
#define SSHC_MAC_OFFER           "hmac-sha2-256"

// ── Protocol constants (RFC 4250/4253/4252/4254, RFC 5656) ───────────────

enum {
    SSHC_MSG_DISCONNECT       = 1,
    SSHC_MSG_IGNORE           = 2,
    SSHC_MSG_UNIMPLEMENTED    = 3,
    SSHC_MSG_DEBUG            = 4,
    SSHC_MSG_SERVICE_REQUEST  = 5,
    SSHC_MSG_SERVICE_ACCEPT   = 6,
    SSHC_MSG_KEXINIT          = 20,
    SSHC_MSG_NEWKEYS          = 21,
    SSHC_MSG_ECDH_INIT        = 30,
    SSHC_MSG_ECDH_REPLY       = 31,
    SSHC_MSG_USERAUTH_REQUEST = 50,
    SSHC_MSG_USERAUTH_FAILURE = 51,
    SSHC_MSG_USERAUTH_SUCCESS = 52,
    SSHC_MSG_USERAUTH_BANNER  = 53,
    SSHC_MSG_USERAUTH_PASSWD_CHANGEREQ = 60,
    SSHC_MSG_GLOBAL_REQUEST   = 80,
    SSHC_MSG_REQUEST_FAILURE  = 82,
    SSHC_MSG_CHANNEL_OPEN     = 90,
    SSHC_MSG_CHANNEL_OPEN_OK  = 91,
    SSHC_MSG_CHANNEL_OPEN_FAIL = 92,
    SSHC_MSG_CHANNEL_ADJUST   = 93,
    SSHC_MSG_CHANNEL_DATA     = 94,
    SSHC_MSG_CHANNEL_EXT_DATA = 95,
    SSHC_MSG_CHANNEL_EOF      = 96,
    SSHC_MSG_CHANNEL_CLOSE    = 97,
    SSHC_MSG_CHANNEL_REQUEST  = 98,
    SSHC_MSG_CHANNEL_SUCCESS  = 99,
    SSHC_MSG_CHANNEL_FAILURE  = 100
};

#define SSHC_DISCONNECT_BY_APPLICATION 11
#define SSHC_OPEN_ADMIN_PROHIBITED     1
#define SSHC_LOCAL_CHANNEL             0

// ── State ───────────────────────────────────────────────────────────────

// Everything large lives in one static: the user stack is small.
typedef struct {
    int      sock;
    uint8_t  rx[SSHC_RX_CAP];
    size_t   rx_len;
    uint8_t  pkt[SSHC_RX_CAP];     // decrypted payload of the last packet
    size_t   pkt_len;
    uint8_t  tx_plain[SSHC_TX_CAP];
    uint8_t  tx[SSHC_TX_CAP + 32];

    int      enc_in;
    int      enc_out;
    uint8_t  key_in[16];
    uint8_t  key_out[16];
    uint8_t  iv_in[12];            // 4 fixed bytes + 64-bit invocation counter
    uint8_t  iv_out[12];

    char     v_c[64];
    char     v_s[256];
    uint8_t  i_c[SSHC_KEXINIT_C_CAP];
    size_t   i_c_len;
    uint8_t  i_s[SSHC_KEXINIT_S_CAP];
    size_t   i_s_len;
    uint8_t  session_id[32];

    uint32_t remote_chan;
    uint32_t remote_win;
    uint32_t local_win;
    int      channel_open;
    int      close_sent;
    int      have_exit;
    int      exit_status;

    int      tty_fd;               // $/dev/tty; -1 when it cannot be opened
    int      esc_state;            // 0 mid-line, 1 line start, 2 "~" held back
    int      quit;
    uint16_t rows;
    uint16_t cols;
} sshc_state_t;

static sshc_state_t sshc;

// fd 0 is routed through the kernel's tty_proc_read(), which always blocks and
// ignores TTY_IOC_NONBLOCK. Polling it would freeze the loop (and with it all
// remote output) until the next keypress. $/dev/tty is a real device file with
// its own non-blocking flag, so the session reads keys from that instead.
static int sshc_tty(void) { return sshc.tty_fd >= 0 ? sshc.tty_fd : 0; }
static uint8_t sshc_msg[SSHC_TX_CAP];
static char sshc_errmsg[192];
static char sshc_kh_buf[SSHC_KNOWN_HOSTS_MAX];

static int sshc_fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(sshc_errmsg, sizeof(sshc_errmsg), fmt, ap);
    va_end(ap);
    return -1;
}

static void sshc_wipe(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

static uint32_t sshc_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void sshc_put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

// ── Message writer / reader ─────────────────────────────────────────────

typedef struct {
    uint8_t *p;
    size_t   len;
    size_t   cap;
    int      bad;
} sshc_wr_t;

typedef struct {
    const uint8_t *p;
    size_t         n;
    int            bad;
} sshc_rd_t;

static void sshc_wr_init(sshc_wr_t *w) {
    w->p = sshc_msg;
    w->len = 0;
    w->cap = sizeof(sshc_msg);
    w->bad = 0;
}

static void sshc_wr_raw(sshc_wr_t *w, const void *d, size_t n) {
    if (w->len + n > w->cap) { w->bad = 1; return; }
    memcpy(w->p + w->len, d, n);
    w->len += n;
}

static void sshc_wr_u8(sshc_wr_t *w, uint8_t v) { sshc_wr_raw(w, &v, 1); }

static void sshc_wr_u32(sshc_wr_t *w, uint32_t v) {
    uint8_t b[4];
    sshc_put_be32(b, v);
    sshc_wr_raw(w, b, 4);
}

static void sshc_wr_str(sshc_wr_t *w, const void *d, size_t n) {
    sshc_wr_u32(w, (uint32_t)n);
    sshc_wr_raw(w, d, n);
}

static void sshc_wr_cstr(sshc_wr_t *w, const char *s) {
    sshc_wr_str(w, s, strlen(s));
}

static uint8_t sshc_rd_u8(sshc_rd_t *r) {
    if (r->n < 1) { r->bad = 1; return 0; }
    uint8_t v = r->p[0];
    r->p++;
    r->n--;
    return v;
}

static uint32_t sshc_rd_u32(sshc_rd_t *r) {
    if (r->n < 4) { r->bad = 1; return 0; }
    uint32_t v = sshc_be32(r->p);
    r->p += 4;
    r->n -= 4;
    return v;
}

static void sshc_rd_skip(sshc_rd_t *r, size_t n) {
    if (r->n < n) { r->bad = 1; return; }
    r->p += n;
    r->n -= n;
}

static void sshc_rd_str(sshc_rd_t *r, const uint8_t **s, size_t *len) {
    uint32_t n = sshc_rd_u32(r);
    if (r->bad || n > r->n) { r->bad = 1; *s = NULL; *len = 0; return; }
    *s = r->p;
    *len = n;
    r->p += n;
    r->n -= n;
}

// ── Name lists ──────────────────────────────────────────────────────────

static int sshc_list_has(const uint8_t *l, size_t n, const char *name) {
    size_t nl = strlen(name);
    size_t i = 0;
    while (i <= n) {
        size_t j = i;
        while (j < n && l[j] != ',') j++;
        if (j - i == nl && memcmp(l + i, name, nl) == 0) return 1;
        i = j + 1;
    }
    return 0;
}

// ── Packet layer (RFC 4253 section 6 + RFC 5647 for AES-GCM) ─────────────

static void sshc_iv_bump(uint8_t iv[12]) {
    for (int i = 11; i >= 4; i--)
        if (++iv[i] != 0) break;
}

// Padding is zero rather than random: with an AEAD cipher it is encrypted and
// authenticated, and before NEWKEYS nothing secret is on the wire.
static int sshc_tx(const uint8_t *payload, size_t len) {
    if (len == 0 || len > SSHC_TX_CAP - 48)
        return sshc_fail("outgoing message too large");

    size_t block = sshc.enc_out ? 16 : 8;
    size_t aligned = sshc.enc_out ? 1 + len : 5 + len;
    size_t pad = block - (aligned % block);
    if (pad < 4) pad += block;
    size_t plen = 1 + len + pad;
    size_t total;

    sshc_put_be32(sshc.tx, (uint32_t)plen);
    if (!sshc.enc_out) {
        sshc.tx[4] = (uint8_t)pad;
        memcpy(sshc.tx + 5, payload, len);
        memset(sshc.tx + 5 + len, 0, pad);
        total = 4 + plen;
    } else {
        sshc.tx_plain[0] = (uint8_t)pad;
        memcpy(sshc.tx_plain + 1, payload, len);
        memset(sshc.tx_plain + 1 + len, 0, pad);
        // The length field stays in clear text and is authenticated as AAD.
        if (aes128gcm_seal(sshc.key_out, sshc.iv_out, sshc.tx, 4,
                           sshc.tx_plain, plen, sshc.tx + 4) != 0)
            return sshc_fail("encryption failed");
        sshc_iv_bump(sshc.iv_out);
        total = 4 + plen + 16;
    }
    if (sock_sendall(sshc.sock, sshc.tx, total) != 0)
        return sshc_fail("send failed: %s", sock_last_message());
    return 0;
}

static int sshc_wr_send(const sshc_wr_t *w) {
    if (w->bad) return sshc_fail("outgoing message too large");
    return sshc_tx(w->p, w->len);
}

// 1: a packet is in sshc.pkt/pkt_len. 0: not complete yet. -1: fatal.
static int sshc_rx_poll(void) {
    for (;;) {
        if (sshc.rx_len >= 4) {
            uint32_t plen = sshc_be32(sshc.rx);
            size_t tag = sshc.enc_in ? 16 : 0;
            if (plen < 6 || plen > SSHC_MAX_PACKET ||
                (sshc.enc_in && (plen & 15) != 0))
                return sshc_fail("bad packet length from server");
            size_t total = 4 + (size_t)plen + tag;
            if (sshc.rx_len >= total) {
                size_t pad, payload_len;
                if (!sshc.enc_in) {
                    pad = sshc.rx[4];
                    if (pad < 4 || pad + 1 > plen)
                        return sshc_fail("bad padding from server");
                    payload_len = plen - 1 - pad;
                    memcpy(sshc.pkt, sshc.rx + 5, payload_len);
                } else {
                    if (aes128gcm_open(sshc.key_in, sshc.iv_in, sshc.rx, 4,
                                       sshc.rx + 4, plen + 16, sshc.pkt) != 0)
                        return sshc_fail("packet authentication failed");
                    sshc_iv_bump(sshc.iv_in);
                    pad = sshc.pkt[0];
                    if (pad < 4 || pad + 1 > plen)
                        return sshc_fail("bad padding from server");
                    payload_len = plen - 1 - pad;
                    memmove(sshc.pkt, sshc.pkt + 1, payload_len);
                }
                if (payload_len == 0)
                    return sshc_fail("empty packet from server");
                sshc.pkt_len = payload_len;
                memmove(sshc.rx, sshc.rx + total, sshc.rx_len - total);
                sshc.rx_len -= total;
                return 1;
            }
        }
        if (sshc.rx_len == sizeof(sshc.rx))
            return sshc_fail("receive buffer overflow");
        ssize_t n = sock_recv(sshc.sock, sshc.rx + sshc.rx_len,
                              sizeof(sshc.rx) - sshc.rx_len);
        if (n > 0) { sshc.rx_len += (size_t)n; continue; }
        if (n == 0) return sshc_fail("connection closed by server");
        if (errno == EAGAIN || errno == ETIMEDOUT) return 0;
        return sshc_fail("network error: %s", sock_last_message());
    }
}

static int sshc_rx_wait(uint32_t timeout_ms) {
    uint64_t end = time_ms() + timeout_ms;
    sock_settimeout(sshc.sock, 100);
    for (;;) {
        int r = sshc_rx_poll();
        if (r != 0) return r;
        if (time_ms() >= end) return sshc_fail("timed out waiting for server");
    }
}

// Transport noise is dropped here. >0: message type worth handling,
// 0: ignorable, -1: the server disconnected us.
static int sshc_classify(void) {
    uint8_t t = sshc.pkt[0];
    if (t == SSHC_MSG_IGNORE || t == SSHC_MSG_DEBUG ||
        t == SSHC_MSG_UNIMPLEMENTED)
        return 0;
    if (t == SSHC_MSG_DISCONNECT) {
        sshc_rd_t r = { sshc.pkt + 1, sshc.pkt_len - 1, 0 };
        const uint8_t *d = NULL;
        size_t dl = 0;
        sshc_rd_u32(&r);
        sshc_rd_str(&r, &d, &dl);
        if (r.bad) { d = NULL; dl = 0; }
        if (dl > 120) dl = 120;
        return sshc_fail("server disconnected: %.*s", (int)dl,
                         d ? (const char *)d : "");
    }
    return t;
}

static int sshc_next_msg(uint32_t timeout_ms) {
    for (;;) {
        if (sshc_rx_wait(timeout_ms) < 0) return -1;
        int t = sshc_classify();
        if (t != 0) return t;
    }
}

// ── Interactive helpers (before the tty is switched to raw) ─────────────

static int sshc_prompt(const char *text, char *buf, size_t cap, int echo) {
    termios_t saved, t;
    int muted = !echo && tty_getattr(sshc_tty(), &saved) == 0;

    printf("%s", text);
    fflush(stdout);
    if (muted) {
        t = saved;
        t.c_lflag &= ~(uint32_t)TTY_ECHO;
        tty_setattr(sshc_tty(), &t, 0);
    }
    ssize_t n = read(0, buf, cap - 1);
    if (muted) {
        tty_setattr(sshc_tty(), &saved, 0);
        printf("\n");
    }
    if (n <= 0) return -1;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    buf[n] = 0;
    return (int)n;
}

static void sshc_b64(const uint8_t *in, size_t n, char *out) {
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = tab[(v >> 18) & 63];
        out[o++] = tab[(v >> 12) & 63];
        if (i + 1 < n) out[o++] = tab[(v >> 6) & 63];
        if (i + 2 < n) out[o++] = tab[v & 63];
    }
    out[o] = 0;
}

static void sshc_hex(const uint8_t *in, size_t n, char *out) {
    static const char tab[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = tab[in[i] >> 4];
        out[2 * i + 1] = tab[in[i] & 15];
    }
    out[2 * n] = 0;
}

// ── Known hosts ─────────────────────────────────────────────────────────

// 1: pinned and equal. -1: pinned for this algorithm and different.
// 0: not pinned; *other is set when the host is known under another algorithm.
static int sshc_known_lookup(const char *id, const char *algo,
                             const char *hex, int *other) {
    *other = 0;
    int fd = open(SSHC_KNOWN_HOSTS, O_RDONLY, 0);
    if (fd < 0) return 0;
    size_t len = 0;
    for (;;) {
        ssize_t n = read(fd, sshc_kh_buf + len, sizeof(sshc_kh_buf) - 1 - len);
        if (n <= 0) break;
        len += (size_t)n;
        if (len >= sizeof(sshc_kh_buf) - 1) break;
    }
    close(fd);
    sshc_kh_buf[len] = 0;

    size_t idl = strlen(id), al = strlen(algo);
    int result = 0;
    for (char *line = sshc_kh_buf; *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (strncmp(line, id, idl) == 0 && line[idl] == ' ') {
            const char *a = line + idl + 1;
            if (strncmp(a, algo, al) == 0 && a[al] == ' ') {
                result = strncmp(a + al + 1, hex, 64) == 0 ? 1 : -1;
                if (result < 0) return -1;
            } else {
                *other = 1;
            }
        }
        if (!nl) break;
        line = nl + 1;
    }
    return result;
}

static void sshc_known_add(const char *id, const char *algo, const char *hex) {
    char line[256];
    mkdir(SSHC_KNOWN_HOSTS_DIR);
    int fd = open(SSHC_KNOWN_HOSTS, O_WRONLY | O_CREAT | O_APPEND, 0644);
    int n = snprintf(line, sizeof(line), "%s %s %s\n", id, algo, hex);
    if (fd < 0 || n <= 0 || write(fd, line, (size_t)n) != n)
        printf("sshc: warning: could not save the host key to %s\n",
               SSHC_KNOWN_HOSTS);
    if (fd >= 0) close(fd);
}

static int sshc_check_host_key(const char *host, uint16_t port,
                               const char *algo, const uint8_t *ks,
                               size_t ks_len) {
    uint8_t fp[32];
    char hex[65], b64[48], id[160], answer[16];
    int other;

    sha256(ks, ks_len, fp);
    sshc_hex(fp, 32, hex);
    sshc_b64(fp, 32, b64);
    snprintf(id, sizeof(id), "%s:%u", host, (unsigned)port);

    int known = sshc_known_lookup(id, algo, hex, &other);
    if (known > 0) return 0;
    if (known < 0) {
        printf("sshc: the %s host key of %s does not match the one pinned in\n"
               "      %s (now SHA256:%s).\n"
               "      Connection aborted. If the key changed on purpose, "
               "delete that line.\n", algo, id, SSHC_KNOWN_HOSTS, b64);
        return sshc_fail("host key mismatch");
    }
    printf("The authenticity of host '%s' can't be established.\n"
           "%s key fingerprint is SHA256:%s.\n", id, algo, b64);
    if (other)
        printf("This host is already pinned under a different key type.\n");
    if (sshc_prompt("Continue connecting (yes/no)? ", answer, sizeof(answer),
                    1) < 0 || strcmp(answer, "yes") != 0)
        return sshc_fail("host key not accepted");
    sshc_known_add(id, algo, hex);
    return 0;
}

// ── Key exchange ────────────────────────────────────────────────────────

static void sshc_hash_str(sha256_ctx *c, const void *d, size_t n) {
    uint8_t l[4];
    sshc_put_be32(l, (uint32_t)n);
    sha256_update(c, l, 4);
    sha256_update(c, d, n);
}

// Shared secret as an RFC 4251 mpint. OpenSSH treats the raw 32-byte x25519
// output as a big-endian integer without byte reversal, so the same is done
// here; interoperability depends on it.
static void sshc_hash_mpint(sha256_ctx *c, const uint8_t *be, size_t n) {
    size_t skip = 0;
    while (skip < n && be[skip] == 0) skip++;
    const uint8_t *s = be + skip;
    size_t len = n - skip;
    int lead = len > 0 && (s[0] & 0x80);
    uint8_t l[4], zero = 0;
    sshc_put_be32(l, (uint32_t)(len + (size_t)lead));
    sha256_update(c, l, 4);
    if (lead) sha256_update(c, &zero, 1);
    sha256_update(c, s, len);
}

static void sshc_kdf(const uint8_t k[32], const uint8_t h[32], char letter,
                     uint8_t out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    sshc_hash_mpint(&c, k, 32);
    sha256_update(&c, h, 32);
    sha256_update(&c, (const uint8_t *)&letter, 1);
    sha256_update(&c, sshc.session_id, 32);
    sha256_final(&c, out);
}

static int sshc_send_kexinit(void) {
    uint8_t cookie[16];
    sshc_wr_t w;

    if (random_bytes(cookie, sizeof(cookie)) != 0)
        return sshc_fail("no entropy source (QEMU needs -cpu max)");
    sshc_wr_init(&w);
    sshc_wr_u8(&w, SSHC_MSG_KEXINIT);
    sshc_wr_raw(&w, cookie, sizeof(cookie));
    sshc_wr_cstr(&w, SSHC_KEX_OFFER);
    sshc_wr_cstr(&w, SSHC_HOSTKEY_OFFER);
    sshc_wr_cstr(&w, SSHC_CIPHER);
    sshc_wr_cstr(&w, SSHC_CIPHER);
    sshc_wr_cstr(&w, SSHC_MAC_OFFER);
    sshc_wr_cstr(&w, SSHC_MAC_OFFER);
    sshc_wr_cstr(&w, "none");
    sshc_wr_cstr(&w, "none");
    sshc_wr_cstr(&w, "");
    sshc_wr_cstr(&w, "");
    sshc_wr_u8(&w, 0);
    sshc_wr_u32(&w, 0);
    if (w.bad || w.len > sizeof(sshc.i_c))
        return sshc_fail("KEXINIT too large");
    memcpy(sshc.i_c, w.p, w.len);
    sshc.i_c_len = w.len;
    return sshc_wr_send(&w);
}

// Picks the host key algorithm; the first entry of our offer that the server
// also lists wins, as RFC 4253 section 7.1 requires.
static int sshc_negotiate(const char **hostkey_alg) {
    static const char *const prefs[] = { "rsa-sha2-256",
                                         "ecdsa-sha2-nistp256" };
    sshc_rd_t r = { sshc.i_s, sshc.i_s_len, 0 };
    const uint8_t *l[10];
    size_t ln[10];

    sshc_rd_u8(&r);
    sshc_rd_skip(&r, 16);
    for (int i = 0; i < 10; i++) sshc_rd_str(&r, &l[i], &ln[i]);
    if (r.bad) return sshc_fail("malformed KEXINIT from server");

    if (!sshc_list_has(l[0], ln[0], "curve25519-sha256") &&
        !sshc_list_has(l[0], ln[0], "curve25519-sha256@libssh.org"))
        return sshc_fail("server offers no curve25519-sha256 key exchange");

    *hostkey_alg = NULL;
    for (size_t i = 0; i < sizeof(prefs) / sizeof(prefs[0]); i++) {
        if (sshc_list_has(l[1], ln[1], prefs[i])) {
            *hostkey_alg = prefs[i];
            break;
        }
    }
    if (!*hostkey_alg)
        return sshc_fail("server has no rsa-sha2-256 or ecdsa-sha2-nistp256 "
                         "host key (ssh-ed25519 only is not supported)");
    if (!sshc_list_has(l[2], ln[2], SSHC_CIPHER) ||
        !sshc_list_has(l[3], ln[3], SSHC_CIPHER))
        return sshc_fail("server does not offer " SSHC_CIPHER);
    if (!sshc_list_has(l[6], ln[6], "none") ||
        !sshc_list_has(l[7], ln[7], "none"))
        return sshc_fail("server does not allow uncompressed sessions");
    return 0;
}

// Host key blob and signature layouts: RFC 4253 section 6.6 (RSA),
// RFC 5656 section 3.1 and 3.1.2 (ECDSA). The signature covers H.
static int sshc_verify_host_sig(const char *alg, const uint8_t *ks,
                                size_t ks_len, const uint8_t *sig,
                                size_t sig_len, const uint8_t h[32]) {
    sshc_rd_t sr = { sig, sig_len, 0 };
    const uint8_t *name, *inner;
    size_t name_len, inner_len;
    mcert_t key;

    sshc_rd_str(&sr, &name, &name_len);
    sshc_rd_str(&sr, &inner, &inner_len);
    if (sr.bad || sr.n != 0 || name_len != strlen(alg) ||
        memcmp(name, alg, name_len) != 0)
        return sshc_fail("host key signature has the wrong format");

    sshc_rd_t kr = { ks, ks_len, 0 };
    const uint8_t *ktype;
    size_t ktype_len;
    sshc_rd_str(&kr, &ktype, &ktype_len);
    memset(&key, 0, sizeof(key));

    if (strcmp(alg, "rsa-sha2-256") == 0) {
        const uint8_t *e, *n;
        size_t el, nl;
        if (ktype_len != 7 || memcmp(ktype, "ssh-rsa", 7) != 0)
            return sshc_fail("host key type does not match its signature");
        sshc_rd_str(&kr, &e, &el);
        sshc_rd_str(&kr, &n, &nl);
        if (kr.bad) return sshc_fail("malformed RSA host key");
        while (el > 0 && e[0] == 0) { e++; el--; }
        while (nl > 0 && n[0] == 0) { n++; nl--; }
        if (el == 0 || nl < 256 || nl > 512)
            return sshc_fail("RSA host key must be 2048 to 4096 bits");
        key.key_type = MC_KEY_RSA;
        key.rsa_e = e; key.rsa_e_len = el;
        key.rsa_n = n; key.rsa_n_len = nl;
        return verify_signature(&key, 0x0401, h, 32, inner, inner_len) == 0
                   ? 0 : sshc_fail("host key signature is invalid");
    }

    // ecdsa-sha2-nistp256: the library wants DER, the wire has two mpints.
    const uint8_t *curve, *q, *rb, *sb;
    size_t curve_len, q_len, rl, sl;
    if (ktype_len != 19 || memcmp(ktype, "ecdsa-sha2-nistp256", 19) != 0)
        return sshc_fail("host key type does not match its signature");
    sshc_rd_str(&kr, &curve, &curve_len);
    sshc_rd_str(&kr, &q, &q_len);
    if (kr.bad || curve_len != 8 || memcmp(curve, "nistp256", 8) != 0 ||
        q_len != 65 || q[0] != 0x04)
        return sshc_fail("malformed ECDSA host key");
    key.key_type = MC_KEY_EC_P256;
    key.ec_point = q;

    sshc_rd_t ir = { inner, inner_len, 0 };
    sshc_rd_str(&ir, &rb, &rl);
    sshc_rd_str(&ir, &sb, &sl);
    if (ir.bad || rl == 0 || sl == 0 || rl > 33 || sl > 33)
        return sshc_fail("malformed ECDSA signature");

    uint8_t der[2 + 2 + 33 + 2 + 33];
    size_t o = 0;
    der[o++] = 0x30;
    der[o++] = (uint8_t)(4 + rl + sl);
    der[o++] = 0x02;
    der[o++] = (uint8_t)rl;
    memcpy(der + o, rb, rl);
    o += rl;
    der[o++] = 0x02;
    der[o++] = (uint8_t)sl;
    memcpy(der + o, sb, sl);
    o += sl;
    return verify_signature(&key, 0x0403, h, 32, der, o) == 0
               ? 0 : sshc_fail("host key signature is invalid");
}

static int sshc_kex(const char *host, uint16_t port) {
    const char *hostkey_alg;
    uint8_t priv[32], pub[32], k[32], h[32], keys[32];

    if (sshc_send_kexinit() != 0) return -1;
    if (sshc_next_msg(SSHC_HANDSHAKE_TIMEOUT) != SSHC_MSG_KEXINIT)
        return sshc_fail("expected KEXINIT from server");
    if (sshc.pkt_len > sizeof(sshc.i_s))
        return sshc_fail("server KEXINIT too large");
    memcpy(sshc.i_s, sshc.pkt, sshc.pkt_len);
    sshc.i_s_len = sshc.pkt_len;
    if (sshc_negotiate(&hostkey_alg) != 0) return -1;

    if (x25519_keypair(priv, pub) != 0)
        return sshc_fail("key generation failed (QEMU needs -cpu max)");
    sshc_wr_t w;
    sshc_wr_init(&w);
    sshc_wr_u8(&w, SSHC_MSG_ECDH_INIT);
    sshc_wr_str(&w, pub, 32);
    if (sshc_wr_send(&w) != 0) return -1;

    if (sshc_next_msg(SSHC_HANDSHAKE_TIMEOUT) != SSHC_MSG_ECDH_REPLY)
        return sshc_fail("expected key exchange reply");
    sshc_rd_t r = { sshc.pkt + 1, sshc.pkt_len - 1, 0 };
    const uint8_t *ks, *qs, *sig;
    size_t ks_len, qs_len, sig_len;
    sshc_rd_str(&r, &ks, &ks_len);
    sshc_rd_str(&r, &qs, &qs_len);
    sshc_rd_str(&r, &sig, &sig_len);
    if (r.bad || qs_len != 32) return sshc_fail("malformed key exchange reply");

    if (x25519_shared(priv, qs, k) != 0) {
        sshc_wipe(priv, sizeof(priv));
        return sshc_fail("server sent a degenerate key share");
    }
    sshc_wipe(priv, sizeof(priv));

    sha256_ctx c;
    sha256_init(&c);
    sshc_hash_str(&c, sshc.v_c, strlen(sshc.v_c));
    sshc_hash_str(&c, sshc.v_s, strlen(sshc.v_s));
    sshc_hash_str(&c, sshc.i_c, sshc.i_c_len);
    sshc_hash_str(&c, sshc.i_s, sshc.i_s_len);
    sshc_hash_str(&c, ks, ks_len);
    sshc_hash_str(&c, pub, 32);
    sshc_hash_str(&c, qs, 32);
    sshc_hash_mpint(&c, k, 32);
    sha256_final(&c, h);
    memcpy(sshc.session_id, h, 32);

    if (sshc_verify_host_sig(hostkey_alg, ks, ks_len, sig, sig_len, h) != 0 ||
        sshc_check_host_key(host, port, hostkey_alg, ks, ks_len) != 0) {
        sshc_wipe(k, sizeof(k));
        return -1;
    }

    sshc_wr_init(&w);
    sshc_wr_u8(&w, SSHC_MSG_NEWKEYS);
    if (sshc_wr_send(&w) != 0) return -1;

    // IVs: letters A/B, keys: C/D. Only the leading bytes of each hash are used.
    sshc_kdf(k, h, 'A', keys);
    memcpy(sshc.iv_out, keys, 12);
    sshc_kdf(k, h, 'B', keys);
    memcpy(sshc.iv_in, keys, 12);
    sshc_kdf(k, h, 'C', keys);
    memcpy(sshc.key_out, keys, 16);
    sshc_kdf(k, h, 'D', keys);
    memcpy(sshc.key_in, keys, 16);
    sshc_wipe(keys, sizeof(keys));
    sshc_wipe(k, sizeof(k));
    sshc.enc_out = 1;

    if (sshc_next_msg(SSHC_HANDSHAKE_TIMEOUT) != SSHC_MSG_NEWKEYS)
        return sshc_fail("expected NEWKEYS from server");
    sshc.enc_in = 1;
    return 0;
}

static int sshc_exchange_idents(void) {
    char line[256];
    char banner[sizeof(SSHC_VERSION_STRING) + 2];

    snprintf(sshc.v_c, sizeof(sshc.v_c), "%s", SSHC_VERSION_STRING);
    snprintf(banner, sizeof(banner), "%s\r\n", SSHC_VERSION_STRING);
    if (sock_sendall(sshc.sock, banner, strlen(banner)) != 0)
        return sshc_fail("send failed: %s", sock_last_message());

    sock_settimeout(sshc.sock, SSHC_IDENT_TIMEOUT_MS);
    // A server may print free-form lines before its version string.
    for (int i = 0; i < 32; i++) {
        ssize_t n = sock_recvline(sshc.sock, line, sizeof(line));
        if (n <= 0) return sshc_fail("no SSH banner from server");
        if (strncmp(line, "SSH-", 4) != 0) continue;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if (strncmp(line, "SSH-2.0-", 8) != 0 &&
            strncmp(line, "SSH-1.99-", 9) != 0)
            return sshc_fail("server speaks %s, not SSH-2", line);
        snprintf(sshc.v_s, sizeof(sshc.v_s), "%s", line);
        return 0;
    }
    return sshc_fail("no SSH banner from server");
}

// ── User authentication (RFC 4252, password method) ─────────────────────

static int sshc_auth(const char *user, const char *host) {
    sshc_wr_t w;
    char pass[256], text[200];

    sshc_wr_init(&w);
    sshc_wr_u8(&w, SSHC_MSG_SERVICE_REQUEST);
    sshc_wr_cstr(&w, "ssh-userauth");
    if (sshc_wr_send(&w) != 0) return -1;
    if (sshc_next_msg(SSHC_HANDSHAKE_TIMEOUT) != SSHC_MSG_SERVICE_ACCEPT)
        return sshc_fail("server refused the ssh-userauth service");

    for (int attempt = 0; attempt < SSHC_AUTH_ATTEMPTS; attempt++) {
        snprintf(text, sizeof(text), "%s@%s's password: ", user, host);
        int plen = sshc_prompt(text, pass, sizeof(pass), 0);
        if (plen < 0) return sshc_fail("no password entered");

        sshc_wr_init(&w);
        sshc_wr_u8(&w, SSHC_MSG_USERAUTH_REQUEST);
        sshc_wr_cstr(&w, user);
        sshc_wr_cstr(&w, "ssh-connection");
        sshc_wr_cstr(&w, "password");
        sshc_wr_u8(&w, 0);
        sshc_wr_str(&w, pass, (size_t)plen);
        int sent = sshc_wr_send(&w);
        sshc_wipe(pass, sizeof(pass));
        sshc_wipe(sshc_msg, sizeof(sshc_msg));
        if (sent != 0) return -1;

        for (;;) {
            int t = sshc_next_msg(SSHC_AUTH_TIMEOUT);
            if (t < 0) return -1;
            if (t == SSHC_MSG_USERAUTH_BANNER) {
                sshc_rd_t r = { sshc.pkt + 1, sshc.pkt_len - 1, 0 };
                const uint8_t *b;
                size_t bl;
                sshc_rd_str(&r, &b, &bl);
                if (!r.bad) {
                    if (bl > 1024) bl = 1024;
                    printf("%.*s", (int)bl, (const char *)b);
                }
                continue;
            }
            if (t == SSHC_MSG_USERAUTH_SUCCESS) return 0;
            if (t == SSHC_MSG_USERAUTH_PASSWD_CHANGEREQ)
                return sshc_fail("the server requires a password change; "
                                 "do it from another session");
            if (t != SSHC_MSG_USERAUTH_FAILURE)
                return sshc_fail("unexpected message %d during login", t);

            sshc_rd_t r = { sshc.pkt + 1, sshc.pkt_len - 1, 0 };
            const uint8_t *methods;
            size_t ml;
            sshc_rd_str(&r, &methods, &ml);
            if (r.bad) return sshc_fail("malformed authentication reply");
            if (!sshc_list_has(methods, ml, "password"))
                return sshc_fail("server does not accept password login "
                                 "(it offers: %.*s)", (int)(ml > 100 ? 100 : ml),
                                 (const char *)methods);
            break;
        }
        printf("Permission denied, please try again.\n");
    }
    return sshc_fail("permission denied");
}

// ── Session channel (RFC 4254) ──────────────────────────────────────────

static int sshc_send_channel_msg(uint8_t type) {
    sshc_wr_t w;
    sshc_wr_init(&w);
    sshc_wr_u8(&w, type);
    sshc_wr_u32(&w, sshc.remote_chan);
    return sshc_wr_send(&w);
}

static int sshc_send_close(void) {
    if (!sshc.channel_open || sshc.close_sent) return 0;
    sshc.close_sent = 1;
    return sshc_send_channel_msg(SSHC_MSG_CHANNEL_CLOSE);
}

static void sshc_out(const uint8_t *d, size_t n) {
    while (n > 0) {
        ssize_t w = write(1, d, n);
        if (w <= 0) break;
        d += w;
        n -= (size_t)w;
    }
}

// Window is topped up in half-window steps so one adjust covers many packets.
static int sshc_consume(size_t n) {
    sshc.local_win = n >= sshc.local_win ? 0 : sshc.local_win - (uint32_t)n;
    if (sshc.local_win >= SSHC_LOCAL_WINDOW / 2) return 0;
    sshc_wr_t w;
    sshc_wr_init(&w);
    sshc_wr_u8(&w, SSHC_MSG_CHANNEL_ADJUST);
    sshc_wr_u32(&w, sshc.remote_chan);
    sshc_wr_u32(&w, SSHC_LOCAL_WINDOW - sshc.local_win);
    sshc.local_win = SSHC_LOCAL_WINDOW;
    return sshc_wr_send(&w);
}

// Handles one connection-level message. 0 on success, -1 on a fatal error;
// *done is set once the channel has been closed.
static int sshc_dispatch(int type, int *done) {
    sshc_rd_t r = { sshc.pkt + 1, sshc.pkt_len - 1, 0 };
    sshc_wr_t w;
    const uint8_t *d;
    size_t dl;

    switch (type) {
    case SSHC_MSG_CHANNEL_DATA:
    case SSHC_MSG_CHANNEL_EXT_DATA:
        sshc_rd_u32(&r);
        if (type == SSHC_MSG_CHANNEL_EXT_DATA) sshc_rd_u32(&r);
        sshc_rd_str(&r, &d, &dl);
        if (r.bad) return sshc_fail("malformed channel data");
        sshc_out(d, dl);
        return sshc_consume(dl);

    case SSHC_MSG_CHANNEL_ADJUST: {
        sshc_rd_u32(&r);
        uint32_t add = sshc_rd_u32(&r);
        if (r.bad) return sshc_fail("malformed window adjust");
        sshc.remote_win = add > 0xFFFFFFFFu - sshc.remote_win
                              ? 0xFFFFFFFFu : sshc.remote_win + add;
        return 0;
    }

    case SSHC_MSG_CHANNEL_CLOSE:
        *done = 1;
        return sshc_send_close();

    case SSHC_MSG_CHANNEL_REQUEST: {
        sshc_rd_u32(&r);
        sshc_rd_str(&r, &d, &dl);
        uint8_t want = sshc_rd_u8(&r);
        if (r.bad) return sshc_fail("malformed channel request");
        if (dl == 11 && memcmp(d, "exit-status", 11) == 0) {
            uint32_t code = sshc_rd_u32(&r);
            if (!r.bad) { sshc.exit_status = (int)(code & 0xFF); sshc.have_exit = 1; }
            return 0;
        }
        if (dl == 11 && memcmp(d, "exit-signal", 11) == 0) {
            sshc.exit_status = 255;
            sshc.have_exit = 1;
            return 0;
        }
        return want ? sshc_send_channel_msg(SSHC_MSG_CHANNEL_FAILURE) : 0;
    }

    case SSHC_MSG_GLOBAL_REQUEST:
        sshc_rd_str(&r, &d, &dl);
        if (sshc_rd_u8(&r) && !r.bad) {
            sshc_wr_init(&w);
            sshc_wr_u8(&w, SSHC_MSG_REQUEST_FAILURE);
            return sshc_wr_send(&w);
        }
        return 0;

    case SSHC_MSG_CHANNEL_OPEN: {
        sshc_rd_str(&r, &d, &dl);
        uint32_t sender = sshc_rd_u32(&r);
        if (r.bad) return 0;
        sshc_wr_init(&w);
        sshc_wr_u8(&w, SSHC_MSG_CHANNEL_OPEN_FAIL);
        sshc_wr_u32(&w, sender);
        sshc_wr_u32(&w, SSHC_OPEN_ADMIN_PROHIBITED);
        sshc_wr_cstr(&w, "");
        sshc_wr_cstr(&w, "");
        return sshc_wr_send(&w);
    }

    case SSHC_MSG_KEXINIT:
        return sshc_fail("server asked to re-key, which sshc does not support");

    default:
        return 0;
    }
}

// Sends one channel request and waits for its answer (want_reply is set).
static int sshc_request(const sshc_wr_t *w, const char *what) {
    int done = 0;
    if (sshc_wr_send(w) != 0) return -1;
    for (;;) {
        int t = sshc_next_msg(SSHC_HANDSHAKE_TIMEOUT);
        if (t < 0) return -1;
        if (t == SSHC_MSG_CHANNEL_SUCCESS) return 0;
        if (t == SSHC_MSG_CHANNEL_FAILURE)
            return sshc_fail("server refused the %s request", what);
        if (sshc_dispatch(t, &done) != 0) return -1;
        if (done) return sshc_fail("channel closed during setup");
    }
}

static void sshc_query_winsize(void) {
    winsize_t ws;
    if (tty_getwinsize(sshc_tty(), &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        sshc.rows = ws.ws_row;
        sshc.cols = ws.ws_col;
    } else if (sshc.rows == 0) {
        sshc.rows = 25;
        sshc.cols = 80;
    }
}

static int sshc_open_shell(const char *term) {
    sshc_wr_t w;

    sshc_wr_init(&w);
    sshc_wr_u8(&w, SSHC_MSG_CHANNEL_OPEN);
    sshc_wr_cstr(&w, "session");
    sshc_wr_u32(&w, SSHC_LOCAL_CHANNEL);
    sshc_wr_u32(&w, SSHC_LOCAL_WINDOW);
    sshc_wr_u32(&w, SSHC_LOCAL_MAX_PACKET);
    if (sshc_wr_send(&w) != 0) return -1;

    for (;;) {
        int t = sshc_next_msg(SSHC_HANDSHAKE_TIMEOUT);
        if (t < 0) return -1;
        if (t == SSHC_MSG_CHANNEL_OPEN_OK) {
            sshc_rd_t r = { sshc.pkt + 1, sshc.pkt_len - 1, 0 };
            sshc_rd_u32(&r);
            sshc.remote_chan = sshc_rd_u32(&r);
            sshc.remote_win = sshc_rd_u32(&r);
            sshc_rd_u32(&r);
            if (r.bad) return sshc_fail("malformed channel confirmation");
            break;
        }
        if (t == SSHC_MSG_CHANNEL_OPEN_FAIL)
            return sshc_fail("server refused to open a session channel");
        int done = 0;
        if (sshc_dispatch(t, &done) != 0) return -1;
    }
    sshc.channel_open = 1;
    sshc.local_win = SSHC_LOCAL_WINDOW;

    sshc_query_winsize();
    sshc_wr_init(&w);
    sshc_wr_u8(&w, SSHC_MSG_CHANNEL_REQUEST);
    sshc_wr_u32(&w, sshc.remote_chan);
    sshc_wr_cstr(&w, "pty-req");
    sshc_wr_u8(&w, 1);
    sshc_wr_cstr(&w, term);
    sshc_wr_u32(&w, sshc.cols);
    sshc_wr_u32(&w, sshc.rows);
    sshc_wr_u32(&w, 0);
    sshc_wr_u32(&w, 0);
    sshc_wr_str(&w, "\0", 1);               // terminal modes: TTY_OP_END only
    if (sshc_request(&w, "pty") != 0) return -1;

    sshc_wr_init(&w);
    sshc_wr_u8(&w, SSHC_MSG_CHANNEL_REQUEST);
    sshc_wr_u32(&w, sshc.remote_chan);
    sshc_wr_cstr(&w, "shell");
    sshc_wr_u8(&w, 1);
    return sshc_request(&w, "shell");
}

static int sshc_send_winsize(void) {
    sshc_wr_t w;
    sshc_wr_init(&w);
    sshc_wr_u8(&w, SSHC_MSG_CHANNEL_REQUEST);
    sshc_wr_u32(&w, sshc.remote_chan);
    sshc_wr_cstr(&w, "window-change");
    sshc_wr_u8(&w, 0);
    sshc_wr_u32(&w, sshc.cols);
    sshc_wr_u32(&w, sshc.rows);
    sshc_wr_u32(&w, 0);
    sshc_wr_u32(&w, 0);
    return sshc_wr_send(&w);
}

// OpenSSH-style escape: "~." at the start of a line quits, "~~" sends one "~".
// A "~" is held back until the next byte shows what it meant.
static size_t sshc_filter_input(const uint8_t *in, size_t n, uint8_t *out) {
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = in[i];
        if (sshc.esc_state == 2) {
            if (c == '.') { sshc.quit = 1; return m; }
            out[m++] = '~';
            if (c != '~') out[m++] = c;
            sshc.esc_state = (c == '\r' || c == '\n') ? 1 : 0;
        } else if (sshc.esc_state == 1 && c == '~') {
            sshc.esc_state = 2;
        } else {
            out[m++] = c;
            sshc.esc_state = (c == '\r' || c == '\n') ? 1 : 0;
        }
    }
    return m;
}

static int sshc_send_data(const uint8_t *d, size_t n) {
    sshc_wr_t w;
    sshc_wr_init(&w);
    sshc_wr_u8(&w, SSHC_MSG_CHANNEL_DATA);
    sshc_wr_u32(&w, sshc.remote_chan);
    sshc_wr_str(&w, d, n);
    sshc.remote_win -= (uint32_t)n;
    return sshc_wr_send(&w);
}

static int sshc_session(void) {
    termios_t saved, raw;
    int32_t one = 1, zero = 0;
    uint8_t in[SSHC_INPUT_CHUNK], out[2 * SSHC_INPUT_CHUNK];
    int done = 0, rc = 0;
    int tty = sshc_tty();
    int have_tty = tty_getattr(tty, &saved) == 0;
    uint64_t next_ws = time_ms() + SSHC_WINSIZE_POLL_MS;

    if (sshc.tty_fd < 0)
        printf("sshc: warning: no controlling tty, remote output will only "
               "appear after a keypress\r\n");
    if (have_tty) {
        raw = saved;
        tty_make_raw(&raw);
        tty_setattr(tty, &raw, 0);
    }
    tty_ioctl(tty, TTY_IOC_NONBLOCK, &one, sizeof(one));
    sock_settimeout(sshc.sock, SOCK_NONBLOCK);
    sshc.esc_state = 1;

    while (!done && !sshc.quit) {
        int idle = 1;

        int r = sshc_rx_poll();
        if (r < 0) { rc = -1; break; }
        if (r > 0) {
            idle = 0;
            int t = sshc_classify();
            if (t < 0 || (t > 0 && sshc_dispatch(t, &done) != 0)) {
                rc = -1;
                break;
            }
        }

        // Keep one byte of headroom: a held-back "~" can grow the output.
        if (sshc.remote_win >= 2 && !sshc.close_sent) {
            size_t cap = sizeof(in);
            if (cap > sshc.remote_win - 1) cap = sshc.remote_win - 1;
            ssize_t n = read(tty, in, cap);
            if (n > 0) {
                idle = 0;
                size_t m = sshc_filter_input(in, (size_t)n, out);
                if (m > 0 && sshc_send_data(out, m) != 0) { rc = -1; break; }
            }
        }

        if (time_ms() >= next_ws) {
            uint16_t old_rows = sshc.rows, old_cols = sshc.cols;
            next_ws = time_ms() + SSHC_WINSIZE_POLL_MS;
            sshc_query_winsize();
            if ((sshc.rows != old_rows || sshc.cols != old_cols) &&
                sshc_send_winsize() != 0) { rc = -1; break; }
        }
        if (idle) yield();
    }

    tty_ioctl(tty, TTY_IOC_NONBLOCK, &zero, sizeof(zero));
    if (have_tty) tty_setattr(tty, &saved, 0);
    return rc;
}

static void sshc_disconnect(void) {
    sshc_wr_t w;
    sock_settimeout(sshc.sock, 2000);
    sshc_send_close();
    sshc_wr_init(&w);
    sshc_wr_u8(&w, SSHC_MSG_DISCONNECT);
    sshc_wr_u32(&w, SSHC_DISCONNECT_BY_APPLICATION);
    sshc_wr_cstr(&w, "");
    sshc_wr_cstr(&w, "");
    sshc_wr_send(&w);
    sock_close(sshc.sock);
}

// ── Entry point ─────────────────────────────────────────────────────────

static void sshc_usage(void) {
    printf("usage: sshc [-p port] [-l user] [-t term] [user@]host\n");
}

int md_main(long argc, char **argv) {
    const char *host = NULL, *user = NULL, *term = NULL;
    char userbuf[64], hostbuf[128];
    unsigned long port = SSHC_PORT_DEFAULT;

    for (long i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            user = argv[++i];
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            term = argv[++i];
        } else if (argv[i][0] != '-' && !host) {
            host = argv[i];
        } else {
            sshc_usage();
            return 2;
        }
    }
    if (!host || port == 0 || port > 65535) {
        sshc_usage();
        return 2;
    }

    const char *at = strchr(host, '@');
    if (at) {
        size_t ul = (size_t)(at - host);
        if (ul == 0 || ul >= sizeof(userbuf)) { sshc_usage(); return 2; }
        memcpy(userbuf, host, ul);
        userbuf[ul] = 0;
        user = userbuf;
        snprintf(hostbuf, sizeof(hostbuf), "%s", at + 1);
        host = hostbuf;
    }
    if (!user) user = getenv("USER");
    if (!user || !*user) {
        if (sshc_prompt("login as: ", userbuf, sizeof(userbuf), 1) <= 0) {
            printf("sshc: no user name\n");
            return 2;
        }
        user = userbuf;
    }
    if (!term) term = getenv("TERM");
    if (!term || !*term) term = "xterm";

    sshc.tty_fd = tty_open_ctty();
    printf("Connecting to %s port %lu...\n", host, port);
    sshc.sock = create_connection(host, (uint16_t)port,
                                  SSHC_CONNECT_TIMEOUT_MS);
    if (sshc.sock < 0) {
        printf("sshc: cannot connect: %s\n", sock_last_message());
        return 255;
    }

    if (sshc_exchange_idents() != 0 ||
        sshc_kex(host, (uint16_t)port) != 0 ||
        sshc_auth(user, host) != 0 ||
        sshc_open_shell(term) != 0) {
        printf("sshc: %s\n", sshc_errmsg);
        sshc_disconnect();
        if (sshc.tty_fd >= 0) close(sshc.tty_fd);
        return 255;
    }

    int rc = sshc_session();
    if (rc != 0) printf("\r\nsshc: %s\r\n", sshc_errmsg);
    sshc_disconnect();
    if (sshc.tty_fd >= 0) close(sshc.tty_fd);
    printf("\r\nConnection to %s closed.\r\n", host);
    if (rc != 0) return 255;
    return sshc.have_exit ? sshc.exit_status : 0;
}