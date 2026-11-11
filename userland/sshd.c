// sshd.c - SSH-2 daemon for ModuOS: password login, interactive shell on a pty.
//
// Usage:   sshd [-p port]            (must run as root, default port 22)
// Build:   userland/build.sh picks up *.c -> dist/sshd.sqr. The crypto library
//          is a shared lib, so build.sh needs one line in DYNAMIC_APPS:
//              sshd:crypto
// Run:     as root, e.g. "service /Apps/sshd.sqr" in the autostart file. Root
//          is only needed to read the host key and drop to the login user.
//
// What it does: owns the TCP session, does the SSH transport (key exchange,
// AES-GCM), authenticates through userctl (UCTL_CMD_LOGIN, the same call
// login.c and ursd.c use), then runs the stock shell on a kernel pty with the
// uid/gid userctl reported. The shell and line discipline do all parsing,
// echo, editing and ^C; the daemon only moves bytes between channel and pty.
//
// ── Algorithms ──────────────────────────────────────────────────────────
// Dictated by lib_crypto.c, which has no signing code except what is here.
//
//   kex          curve25519-sha256
//   host key     ssh-ed25519 (signed by the Ed25519 code in this file)
//   cipher       aes128-gcm@openssh.com
//   compression  none
//   user auth    password
//
// OpenSSH clients negotiate this out of the box. sshc.c cannot connect to
// this daemon yet: it only verifies RSA and ECDSA host keys.
//
// ── Host key ────────────────────────────────────────────────────────────
// SSHD_HOSTKEY_PATH holds the 32-byte Ed25519 seed. It is generated on first
// start (random_bytes, so QEMU needs "-cpu max") and reused afterwards; the
// fingerprint is printed at start-up so a client can be checked against it.
// If the seed cannot be saved the daemon still runs, but the key then changes
// on every start and clients will see a host key mismatch.
//
// ── Security notes ──────────────────────────────────────────────────────
//  * Root login is refused (SSHD_ALLOW_ROOT 0); a refused root login looks
//    exactly like a wrong password to the client and is logged here.
//  * 3 password tries per connection, a delay after each miss, and a cooldown
//    after the limit. The whole login must finish within the grace time.
//  * Unlike ursd the traffic is encrypted, so any peer address is accepted.
//    netd listens on every interface; bind it to the QEMU hostfwd you want.
//
// ── Limits (netd/libc, not choices) ─────────────────────────────────────
//  * Single-threaded, one netd mailbox: one connection at a time, and extra
//    connections are accepted and closed at once.
//  * One session channel per connection. No exec, subsystems, forwarding,
//    agent, X11 or env; those requests get a polite refusal.
//  * No public-key or keyboard-interactive auth.
//  * No re-keying. OpenSSH asks for one after about a gigabyte of traffic;
//    the session then ends with an error instead of continuing unprotected.
//  * No SIGWINCH, so window-change only updates the pty's winsize for
//    programs that query it.

#include "sockets.h"
#include "userctl_ipc.h"
#include "libc/tty.h"
#include "crypto.h"

// ── Configuration ───────────────────────────────────────────────────────

#define SSHD_PORT_DEFAULT        22
#ifndef SSHD_SHELL_PATH
#define SSHD_SHELL_PATH          "/Apps/zenith5.1.sqr"
#endif
#define SSHD_VERSION_STRING      "SSH-2.0-ModuOS_sshd_1.0"

#define SSHD_HOSTKEY_DIR         "/ModuOS/System64/ssh"
#define SSHD_HOSTKEY_PATH        "/ModuOS/System64/ssh/ssh_host_ed25519_key"

// A uid 0 account is refused by default: flip only if you accept root over
// the network.
#define SSHD_ALLOW_ROOT          1

#define SSHD_MAX_AUTH_TRIES      3
#define SSHD_FAIL_DELAY_S        1
#define SSHD_COOLDOWN_S          2
#define SSHD_LOGIN_GRACE_MS      60000
#define SSHD_IDENT_TIMEOUT_MS    10000
#define SSHD_HANDSHAKE_TIMEOUT   30000
#define SSHD_BUSY_POLL_MS        300
#define SSHD_PUMP_MAX            8192    // pty bytes per loop, keeps ^C responsive
#define SSHD_OUT_CHUNK           1024
#define SSHD_STOP_WAIT_MS        1500
#define SSHD_CLOSE_WAIT_MS       2000
#define SSHD_ACCEPT_FAIL_MAX     5

#define SSHD_DEFAULT_ROWS        25
#define SSHD_DEFAULT_COLS        80
#define SSHD_DIM_MAX             1000

// RFC 4253 obliges every peer to accept 35000 bytes of packet.
#define SSHD_MAX_PACKET          35000
#define SSHD_RX_CAP              (SSHD_MAX_PACKET + 64)
#define SSHD_TX_CAP              2304
#define SSHD_KEXINIT_C_CAP       8192
#define SSHD_KEXINIT_S_CAP       1024

#define SSHD_LOCAL_WINDOW        (256 * 1024)
#define SSHD_LOCAL_MAX_PACKET    32768
#define SSHD_LOCAL_CHANNEL       0

// ── Protocol constants (RFC 4250/4252/4253/4254) ────────────────────────

enum {
    SSHD_MSG_DISCONNECT       = 1,
    SSHD_MSG_IGNORE           = 2,
    SSHD_MSG_UNIMPLEMENTED    = 3,
    SSHD_MSG_DEBUG            = 4,
    SSHD_MSG_SERVICE_REQUEST  = 5,
    SSHD_MSG_SERVICE_ACCEPT   = 6,
    SSHD_MSG_KEXINIT          = 20,
    SSHD_MSG_NEWKEYS          = 21,
    SSHD_MSG_ECDH_INIT        = 30,
    SSHD_MSG_ECDH_REPLY       = 31,
    SSHD_MSG_USERAUTH_REQUEST = 50,
    SSHD_MSG_USERAUTH_FAILURE = 51,
    SSHD_MSG_USERAUTH_SUCCESS = 52,
    SSHD_MSG_GLOBAL_REQUEST   = 80,
    SSHD_MSG_REQUEST_FAILURE  = 82,
    SSHD_MSG_CHANNEL_OPEN     = 90,
    SSHD_MSG_CHANNEL_OPEN_OK  = 91,
    SSHD_MSG_CHANNEL_OPEN_FAIL = 92,
    SSHD_MSG_CHANNEL_ADJUST   = 93,
    SSHD_MSG_CHANNEL_DATA     = 94,
    SSHD_MSG_CHANNEL_EXT_DATA = 95,
    SSHD_MSG_CHANNEL_EOF      = 96,
    SSHD_MSG_CHANNEL_CLOSE    = 97,
    SSHD_MSG_CHANNEL_REQUEST  = 98,
    SSHD_MSG_CHANNEL_SUCCESS  = 99,
    SSHD_MSG_CHANNEL_FAILURE  = 100
};

#define SSHD_DISCONNECT_PROTOCOL_ERROR   2
#define SSHD_DISCONNECT_NO_MORE_AUTH     14
#define SSHD_DISCONNECT_BY_APPLICATION   11
#define SSHD_OPEN_UNKNOWN_TYPE           3
#define SSHD_OPEN_RESOURCE_SHORTAGE      4

// ── Ed25519 signing (TweetNaCl construction, RFC 8032) ──────────────────
// lib_crypto.c can verify RSA/ECDSA but has nothing that signs, so the host
// key signature lives here. Field elements are 16 limbs of 16 bits in int64,
// carried after every multiplication. Written for size and clarity, not
// speed: one signature costs two fixed-base scalar multiplications.

typedef int64_t ed_gf[16];

static const ed_gf ed_gf0 = { 0 };
static const ed_gf ed_gf1 = { 1 };
static const ed_gf ed_D2 = {
    0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
    0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406 };
static const ed_gf ed_X = {
    0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
    0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169 };
static const ed_gf ed_Y = {
    0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
    0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666 };
// Group order l = 2^252 + 27742317777372353535851937790883648493.
static const int64_t ed_L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0x10 };

static void ed_set(ed_gf r, const ed_gf a) {
    for (int i = 0; i < 16; i++) r[i] = a[i];
}

static void ed_car(ed_gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (int64_t)1 << 16;
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

static void ed_sel(ed_gf p, ed_gf q, int b) {
    int64_t c = ~((int64_t)b - 1);
    for (int i = 0; i < 16; i++) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void ed_pack25519(uint8_t *o, const ed_gf n) {
    ed_gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    ed_car(t);
    ed_car(t);
    ed_car(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        ed_sel(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i] = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static uint8_t ed_parity(const ed_gf a) {
    uint8_t d[32];
    ed_pack25519(d, a);
    return d[0] & 1;
}

static void ed_add_gf(ed_gf o, const ed_gf a, const ed_gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

static void ed_sub_gf(ed_gf o, const ed_gf a, const ed_gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void ed_mul(ed_gf o, const ed_gf a, const ed_gf b) {
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    ed_car(o);
    ed_car(o);
}

static void ed_sqr(ed_gf o, const ed_gf a) { ed_mul(o, a, a); }

static void ed_inv(ed_gf o, const ed_gf in) {
    ed_gf c;
    ed_set(c, in);
    for (int a = 253; a >= 0; a--) {
        ed_sqr(c, c);
        if (a != 2 && a != 4) ed_mul(c, c, in);
    }
    ed_set(o, c);
}

// Extended twisted-Edwards point addition (a = -1).
static void ed_point_add(ed_gf p[4], ed_gf q[4]) {
    ed_gf a, b, c, d, t, e, f, g, h;
    ed_sub_gf(a, p[1], p[0]);
    ed_sub_gf(t, q[1], q[0]);
    ed_mul(a, a, t);
    ed_add_gf(b, p[0], p[1]);
    ed_add_gf(t, q[0], q[1]);
    ed_mul(b, b, t);
    ed_mul(c, p[3], q[3]);
    ed_mul(c, c, ed_D2);
    ed_mul(d, p[2], q[2]);
    ed_add_gf(d, d, d);
    ed_sub_gf(e, b, a);
    ed_sub_gf(f, d, c);
    ed_add_gf(g, d, c);
    ed_add_gf(h, b, a);
    ed_mul(p[0], e, f);
    ed_mul(p[1], h, g);
    ed_mul(p[2], g, f);
    ed_mul(p[3], e, h);
}

static void ed_cswap(ed_gf p[4], ed_gf q[4], uint8_t b) {
    for (int i = 0; i < 4; i++) ed_sel(p[i], q[i], b);
}

static void ed_pack(uint8_t *r, ed_gf p[4]) {
    ed_gf tx, ty, zi;
    ed_inv(zi, p[2]);
    ed_mul(tx, p[0], zi);
    ed_mul(ty, p[1], zi);
    ed_pack25519(r, ty);
    r[31] ^= (uint8_t)(ed_parity(tx) << 7);
}

static void ed_scalarmult(ed_gf p[4], ed_gf q[4], const uint8_t *s) {
    ed_set(p[0], ed_gf0);
    ed_set(p[1], ed_gf1);
    ed_set(p[2], ed_gf1);
    ed_set(p[3], ed_gf0);
    for (int i = 255; i >= 0; --i) {
        uint8_t b = (uint8_t)((s[i / 8] >> (i & 7)) & 1);
        ed_cswap(p, q, b);
        ed_point_add(q, p);
        ed_point_add(p, p);
        ed_cswap(p, q, b);
    }
}

static void ed_scalarbase(ed_gf p[4], const uint8_t *s) {
    ed_gf q[4];
    ed_set(q[0], ed_X);
    ed_set(q[1], ed_Y);
    ed_set(q[2], ed_gf1);
    ed_mul(q[3], ed_X, ed_Y);
    ed_scalarmult(p, q, s);
}

// r = x mod l, for a 512-bit x held in 64 signed limbs of 8 bits.
static void ed_modl(uint8_t *r, int64_t x[64]) {
    int64_t carry, i, j;
    for (i = 63; i >= 32; --i) {
        carry = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * ed_L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (j = 0; j < 32; j++) {
        x[j] += carry - (x[31] >> 4) * ed_L[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (j = 0; j < 32; j++) x[j] -= carry * ed_L[j];
    for (i = 0; i < 32; i++) {
        x[i + 1] += x[i] >> 8;
        r[i] = (uint8_t)(x[i] & 255);
    }
}

static void ed_reduce(uint8_t r[64]) {
    int64_t x[64];
    for (int i = 0; i < 64; i++) x[i] = (int64_t)r[i];
    for (int i = 0; i < 64; i++) r[i] = 0;
    ed_modl(r, x);
}

static void sshd_wipe(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

// Expanded secret: SHA-512(seed) with the scalar half clamped as RFC 8032 asks.
static void ed25519_expand(const uint8_t seed[32], uint8_t d[64]) {
    sha512_hash(seed, 32, d);
    d[0] &= 248;
    d[31] &= 127;
    d[31] |= 64;
}

static void ed25519_pubkey(const uint8_t seed[32], uint8_t pk[32]) {
    uint8_t d[64];
    ed_gf p[4];
    ed25519_expand(seed, d);
    ed_scalarbase(p, d);
    ed_pack(pk, p);
    sshd_wipe(d, sizeof(d));
}

// Signs a 32-byte message (the exchange hash) into sig[64].
static void ed25519_sign32(const uint8_t seed[32], const uint8_t pk[32],
                           const uint8_t msg[32], uint8_t sig[64]) {
    uint8_t d[64], r[64], h[64], buf[96];
    int64_t x[64];
    ed_gf p[4];

    ed25519_expand(seed, d);
    memcpy(buf, d + 32, 32);
    memcpy(buf + 32, msg, 32);
    sha512_hash(buf, 64, r);
    ed_reduce(r);
    ed_scalarbase(p, r);
    ed_pack(sig, p);

    memcpy(buf, sig, 32);
    memcpy(buf + 32, pk, 32);
    memcpy(buf + 64, msg, 32);
    sha512_hash(buf, 96, h);
    ed_reduce(h);

    for (int i = 0; i < 64; i++) x[i] = 0;
    for (int i = 0; i < 32; i++) x[i] = (int64_t)r[i];
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 32; j++) x[i + j] += (int64_t)h[i] * (int64_t)d[j];
    ed_modl(sig + 32, x);

    sshd_wipe(d, sizeof(d));
    sshd_wipe(r, sizeof(r));
    sshd_wipe(h, sizeof(h));
    sshd_wipe(buf, sizeof(buf));
}

// ── State ───────────────────────────────────────────────────────────────

typedef struct {
    int      master;
    int      pid;
} sshd_shell_t;

typedef enum {
    SSHD_END_NONE = 0,     // handshake and login succeeded
    SSHD_END_CLIENT,       // peer closed or vanished
    SSHD_END_ERROR,        // protocol error or local failure
    SSHD_END_AUTHFAIL,     // too many wrong passwords
    SSHD_END_SHELL         // the shell exited
} sshd_end_t;

// Everything large lives in one static: the user stack is small.
typedef struct {
    int      sock;
    uint8_t  rx[SSHD_RX_CAP];
    size_t   rx_len;
    uint8_t  pkt[SSHD_RX_CAP];     // decrypted payload of the last packet
    size_t   pkt_len;
    uint8_t  tx_plain[SSHD_TX_CAP];
    uint8_t  tx[SSHD_TX_CAP + 32];

    int      enc_in;
    int      enc_out;
    uint8_t  key_in[16];
    uint8_t  key_out[16];
    uint8_t  iv_in[12];            // 4 fixed bytes + 64-bit invocation counter
    uint8_t  iv_out[12];

    char     v_c[256];
    char     v_s[64];
    uint8_t  i_c[SSHD_KEXINIT_C_CAP];
    size_t   i_c_len;
    uint8_t  i_s[SSHD_KEXINIT_S_CAP];
    size_t   i_s_len;

    char     user[UCTL_NAME_MAX];
    int      uid;
    int      gid;

    uint32_t remote_chan;
    uint32_t remote_win;
    uint32_t remote_maxpkt;
    uint32_t local_win;
    int      chan_open;
    int      close_sent;
    int      have_pty;
    char     term[32];
    uint16_t rows;
    uint16_t cols;
    sshd_shell_t sh;
} sshd_state_t;

static sshd_state_t sshd;
static uint8_t sshd_msg[SSHD_TX_CAP];
static char sshd_errmsg[192];

static uint8_t sshd_seed[32];
static uint8_t sshd_pub[32];
static uint8_t sshd_hostblob[4 + 11 + 4 + 32];
static uctl_shm_t *sshd_uctl;

static int sshd_fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(sshd_errmsg, sizeof(sshd_errmsg), fmt, ap);
    va_end(ap);
    return -1;
}

static uint32_t sshd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void sshd_put_be32(uint8_t *p, uint32_t v) {
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
} sshd_wr_t;

typedef struct {
    const uint8_t *p;
    size_t         n;
    int            bad;
} sshd_rd_t;

static void sshd_wr_init(sshd_wr_t *w) {
    w->p = sshd_msg;
    w->len = 0;
    w->cap = sizeof(sshd_msg);
    w->bad = 0;
}

static void sshd_wr_raw(sshd_wr_t *w, const void *d, size_t n) {
    if (w->len + n > w->cap) { w->bad = 1; return; }
    memcpy(w->p + w->len, d, n);
    w->len += n;
}

static void sshd_wr_u8(sshd_wr_t *w, uint8_t v) { sshd_wr_raw(w, &v, 1); }

static void sshd_wr_u32(sshd_wr_t *w, uint32_t v) {
    uint8_t b[4];
    sshd_put_be32(b, v);
    sshd_wr_raw(w, b, 4);
}

static void sshd_wr_str(sshd_wr_t *w, const void *d, size_t n) {
    sshd_wr_u32(w, (uint32_t)n);
    sshd_wr_raw(w, d, n);
}

static void sshd_wr_cstr(sshd_wr_t *w, const char *s) {
    sshd_wr_str(w, s, strlen(s));
}

static uint8_t sshd_rd_u8(sshd_rd_t *r) {
    if (r->n < 1) { r->bad = 1; return 0; }
    uint8_t v = r->p[0];
    r->p++;
    r->n--;
    return v;
}

static uint32_t sshd_rd_u32(sshd_rd_t *r) {
    if (r->n < 4) { r->bad = 1; return 0; }
    uint32_t v = sshd_be32(r->p);
    r->p += 4;
    r->n -= 4;
    return v;
}

static void sshd_rd_skip(sshd_rd_t *r, size_t n) {
    if (r->n < n) { r->bad = 1; return; }
    r->p += n;
    r->n -= n;
}

static void sshd_rd_str(sshd_rd_t *r, const uint8_t **s, size_t *len) {
    uint32_t n = sshd_rd_u32(r);
    if (r->bad || n > r->n) { r->bad = 1; *s = NULL; *len = 0; return; }
    *s = r->p;
    *len = n;
    r->p += n;
    r->n -= n;
}

static int sshd_str_is(const uint8_t *s, size_t n, const char *lit) {
    return strlen(lit) == n && memcmp(s, lit, n) == 0;
}

// Picks the first token of the client's name-list that we also support (RFC
// 4253 section 7.1: the client's order wins). *first tells whether that token
// was the client's first choice, which is what makes its guessed packet right.
static int sshd_pick(const uint8_t *cl, size_t cn, const char *const *ours,
                     size_t n_ours, const char **chosen, int *first) {
    size_t i = 0;
    int idx = 0;
    while (i <= cn) {
        size_t j = i;
        while (j < cn && cl[j] != ',') j++;
        for (size_t k = 0; k < n_ours; k++) {
            if (strlen(ours[k]) == j - i && memcmp(cl + i, ours[k], j - i) == 0) {
                *chosen = ours[k];
                *first = idx == 0;
                return 1;
            }
        }
        i = j + 1;
        idx++;
    }
    return 0;
}

static int sshd_list_has(const uint8_t *l, size_t n, const char *name) {
    const char *one[1] = { name };
    const char *c;
    int f;
    return sshd_pick(l, n, one, 1, &c, &f);
}

// ── Packet layer (RFC 4253 section 6 + RFC 5647 for AES-GCM) ────────────

static void sshd_iv_bump(uint8_t iv[12]) {
    for (int i = 11; i >= 4; i--)
        if (++iv[i] != 0) break;
}

// Padding is zero rather than random: with an AEAD cipher it is encrypted and
// authenticated, and before NEWKEYS nothing secret is on the wire.
static int sshd_tx(const uint8_t *payload, size_t len) {
    if (len == 0 || len > SSHD_TX_CAP - 48)
        return sshd_fail("outgoing message too large");

    size_t block = sshd.enc_out ? 16 : 8;
    size_t aligned = sshd.enc_out ? 1 + len : 5 + len;
    size_t pad = block - (aligned % block);
    if (pad < 4) pad += block;
    size_t plen = 1 + len + pad;
    size_t total;

    sshd_put_be32(sshd.tx, (uint32_t)plen);
    if (!sshd.enc_out) {
        sshd.tx[4] = (uint8_t)pad;
        memcpy(sshd.tx + 5, payload, len);
        memset(sshd.tx + 5 + len, 0, pad);
        total = 4 + plen;
    } else {
        sshd.tx_plain[0] = (uint8_t)pad;
        memcpy(sshd.tx_plain + 1, payload, len);
        memset(sshd.tx_plain + 1 + len, 0, pad);
        // The length field stays in clear text and is authenticated as AAD.
        if (aes128gcm_seal(sshd.key_out, sshd.iv_out, sshd.tx, 4,
                           sshd.tx_plain, plen, sshd.tx + 4) != 0)
            return sshd_fail("encryption failed");
        sshd_iv_bump(sshd.iv_out);
        total = 4 + plen + 16;
    }
    if (sock_sendall(sshd.sock, sshd.tx, total) != 0)
        return sshd_fail("send failed: %s", sock_last_message());
    return 0;
}

static int sshd_wr_send(const sshd_wr_t *w) {
    if (w->bad) return sshd_fail("outgoing message too large");
    return sshd_tx(w->p, w->len);
}

// 1: a packet is in sshd.pkt/pkt_len. 0: not complete yet. -1: fatal.
static int sshd_rx_poll(void) {
    for (;;) {
        if (sshd.rx_len >= 4) {
            uint32_t plen = sshd_be32(sshd.rx);
            size_t tag = sshd.enc_in ? 16 : 0;
            if (plen < 6 || plen > SSHD_MAX_PACKET ||
                (sshd.enc_in && (plen & 15) != 0))
                return sshd_fail("bad packet length from client");
            size_t total = 4 + (size_t)plen + tag;
            if (sshd.rx_len >= total) {
                size_t pad, payload_len;
                if (!sshd.enc_in) {
                    pad = sshd.rx[4];
                    if (pad < 4 || pad + 1 > plen)
                        return sshd_fail("bad padding from client");
                    payload_len = plen - 1 - pad;
                    memcpy(sshd.pkt, sshd.rx + 5, payload_len);
                } else {
                    if (aes128gcm_open(sshd.key_in, sshd.iv_in, sshd.rx, 4,
                                       sshd.rx + 4, plen + 16, sshd.pkt) != 0)
                        return sshd_fail("packet authentication failed");
                    sshd_iv_bump(sshd.iv_in);
                    pad = sshd.pkt[0];
                    if (pad < 4 || pad + 1 > plen)
                        return sshd_fail("bad padding from client");
                    payload_len = plen - 1 - pad;
                    memmove(sshd.pkt, sshd.pkt + 1, payload_len);
                }
                if (payload_len == 0)
                    return sshd_fail("empty packet from client");
                sshd.pkt_len = payload_len;
                memmove(sshd.rx, sshd.rx + total, sshd.rx_len - total);
                sshd.rx_len -= total;
                return 1;
            }
        }
        if (sshd.rx_len == sizeof(sshd.rx))
            return sshd_fail("receive buffer overflow");
        ssize_t n = sock_recv(sshd.sock, sshd.rx + sshd.rx_len,
                              sizeof(sshd.rx) - sshd.rx_len);
        if (n > 0) { sshd.rx_len += (size_t)n; continue; }
        if (n == 0) return sshd_fail("connection closed by client");
        if (errno == EAGAIN || errno == ETIMEDOUT) return 0;
        return sshd_fail("network error: %s", sock_last_message());
    }
}

static int sshd_rx_wait(uint32_t timeout_ms) {
    uint64_t end = time_ms() + timeout_ms;
    sock_settimeout(sshd.sock, 100);
    for (;;) {
        int r = sshd_rx_poll();
        if (r != 0) return r;
        if (time_ms() >= end) return sshd_fail("timed out waiting for client");
    }
}

// Transport noise is dropped here. >0: message type worth handling,
// 0: ignorable, -1: the client disconnected.
static int sshd_classify(void) {
    uint8_t t = sshd.pkt[0];
    if (t == SSHD_MSG_IGNORE || t == SSHD_MSG_DEBUG ||
        t == SSHD_MSG_UNIMPLEMENTED)
        return 0;
    if (t == SSHD_MSG_DISCONNECT) {
        sshd_rd_t r = { sshd.pkt + 1, sshd.pkt_len - 1, 0 };
        const uint8_t *d = NULL;
        size_t dl = 0;
        sshd_rd_u32(&r);
        sshd_rd_str(&r, &d, &dl);
        if (r.bad) { d = NULL; dl = 0; }
        if (dl > 120) dl = 120;
        return sshd_fail("client disconnected: %.*s", (int)dl,
                         d ? (const char *)d : "");
    }
    return t;
}

static int sshd_next_msg(uint32_t timeout_ms) {
    for (;;) {
        if (sshd_rx_wait(timeout_ms) < 0) return -1;
        int t = sshd_classify();
        if (t != 0) return t;
    }
}

static void sshd_send_disconnect(uint32_t reason, const char *text) {
    sshd_wr_t w;
    sshd_wr_init(&w);
    sshd_wr_u8(&w, SSHD_MSG_DISCONNECT);
    sshd_wr_u32(&w, reason);
    sshd_wr_cstr(&w, text);
    sshd_wr_cstr(&w, "");
    sshd_wr_send(&w);
}

// ── Text helpers ────────────────────────────────────────────────────────

static void sshd_b64(const uint8_t *in, size_t n, char *out) {
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

// ── Host key ────────────────────────────────────────────────────────────

static int sshd_hostkey_load(void) {
    int fd = open(SSHD_HOSTKEY_PATH, O_RDONLY, 0);
    if (fd >= 0) {
        size_t got = 0;
        while (got < sizeof(sshd_seed)) {
            ssize_t n = read(fd, sshd_seed + got, sizeof(sshd_seed) - got);
            if (n <= 0) break;
            got += (size_t)n;
        }
        close(fd);
        if (got != sizeof(sshd_seed)) {
            printf("sshd: %s is not a 32-byte seed; delete it to regenerate\n",
                   SSHD_HOSTKEY_PATH);
            return -1;
        }
    } else {
        if (random_bytes(sshd_seed, sizeof(sshd_seed)) != 0) {
            printf("sshd: no entropy source for the host key (QEMU needs "
                   "-cpu max)\n");
            return -1;
        }
        mkdir(SSHD_HOSTKEY_DIR);
        fd = open(SSHD_HOSTKEY_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0 || write(fd, sshd_seed, sizeof(sshd_seed)) !=
                          (ssize_t)sizeof(sshd_seed))
            printf("sshd: warning: cannot save the host key to %s; it will "
                   "change on every start\n", SSHD_HOSTKEY_PATH);
        else
            printf("sshd: generated a new host key in %s\n", SSHD_HOSTKEY_PATH);
        if (fd >= 0) close(fd);
    }

    ed25519_pubkey(sshd_seed, sshd_pub);
    sshd_put_be32(sshd_hostblob, 11);
    memcpy(sshd_hostblob + 4, "ssh-ed25519", 11);
    sshd_put_be32(sshd_hostblob + 15, 32);
    memcpy(sshd_hostblob + 19, sshd_pub, 32);
    return 0;
}

// ── Key exchange ────────────────────────────────────────────────────────

static void sshd_hash_str(sha256_ctx *c, const void *d, size_t n) {
    uint8_t l[4];
    sshd_put_be32(l, (uint32_t)n);
    sha256_update(c, l, 4);
    sha256_update(c, d, n);
}

// Shared secret as an RFC 4251 mpint. OpenSSH treats the raw 32-byte x25519
// output as a big-endian integer without byte reversal, so the same is done
// here; interoperability depends on it.
static void sshd_hash_mpint(sha256_ctx *c, const uint8_t *be, size_t n) {
    size_t skip = 0;
    while (skip < n && be[skip] == 0) skip++;
    const uint8_t *s = be + skip;
    size_t len = n - skip;
    int lead = len > 0 && (s[0] & 0x80);
    uint8_t l[4], zero = 0;
    sshd_put_be32(l, (uint32_t)(len + (size_t)lead));
    sha256_update(c, l, 4);
    if (lead) sha256_update(c, &zero, 1);
    sha256_update(c, s, len);
}

static void sshd_kdf(const uint8_t k[32], const uint8_t h[32], char letter,
                     const uint8_t session_id[32], uint8_t out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    sshd_hash_mpint(&c, k, 32);
    sha256_update(&c, h, 32);
    sha256_update(&c, (const uint8_t *)&letter, 1);
    sha256_update(&c, session_id, 32);
    sha256_final(&c, out);
}

static int sshd_send_kexinit(void) {
    uint8_t cookie[16];
    sshd_wr_t w;

    if (random_bytes(cookie, sizeof(cookie)) != 0)
        return sshd_fail("no entropy source (QEMU needs -cpu max)");
    sshd_wr_init(&w);
    sshd_wr_u8(&w, SSHD_MSG_KEXINIT);
    sshd_wr_raw(&w, cookie, sizeof(cookie));
    sshd_wr_cstr(&w, "curve25519-sha256,curve25519-sha256@libssh.org");
    sshd_wr_cstr(&w, "ssh-ed25519");
    sshd_wr_cstr(&w, "aes128-gcm@openssh.com");
    sshd_wr_cstr(&w, "aes128-gcm@openssh.com");
    sshd_wr_cstr(&w, "hmac-sha2-256");
    sshd_wr_cstr(&w, "hmac-sha2-256");
    sshd_wr_cstr(&w, "none");
    sshd_wr_cstr(&w, "none");
    sshd_wr_cstr(&w, "");
    sshd_wr_cstr(&w, "");
    sshd_wr_u8(&w, 0);
    sshd_wr_u32(&w, 0);
    if (w.bad || w.len > sizeof(sshd.i_s))
        return sshd_fail("KEXINIT too large");
    memcpy(sshd.i_s, w.p, w.len);
    sshd.i_s_len = w.len;
    return sshd_wr_send(&w);
}

// Checks the client's KEXINIT against what this daemon speaks. *discard is
// set when the client guessed wrong and a packet must be dropped (RFC 4253
// section 7).
static int sshd_negotiate(int *discard) {
    static const char *const kex_ours[] = { "curve25519-sha256",
                                            "curve25519-sha256@libssh.org" };
    static const char *const host_ours[] = { "ssh-ed25519" };
    sshd_rd_t r = { sshd.i_c, sshd.i_c_len, 0 };
    const uint8_t *l[10];
    size_t ln[10];
    const char *chosen;
    int kex_first, host_first;

    sshd_rd_u8(&r);
    sshd_rd_skip(&r, 16);
    for (int i = 0; i < 10; i++) sshd_rd_str(&r, &l[i], &ln[i]);
    uint8_t follows = sshd_rd_u8(&r);
    if (r.bad) return sshd_fail("malformed KEXINIT from client");

    if (!sshd_pick(l[0], ln[0], kex_ours, 2, &chosen, &kex_first))
        return sshd_fail("client offers no curve25519-sha256 key exchange");
    if (!sshd_pick(l[1], ln[1], host_ours, 1, &chosen, &host_first))
        return sshd_fail("client does not accept an ssh-ed25519 host key");
    if (!sshd_list_has(l[2], ln[2], "aes128-gcm@openssh.com") ||
        !sshd_list_has(l[3], ln[3], "aes128-gcm@openssh.com"))
        return sshd_fail("client does not offer aes128-gcm@openssh.com");
    if (!sshd_list_has(l[6], ln[6], "none") ||
        !sshd_list_has(l[7], ln[7], "none"))
        return sshd_fail("client does not allow uncompressed sessions");

    *discard = follows && !(kex_first && host_first);
    return 0;
}

static int sshd_kex(void) {
    uint8_t priv[32], pub[32], k[32], h[32], keys[32], sig[64];
    int discard = 0;

    if (sshd_send_kexinit() != 0) return -1;
    if (sshd_next_msg(SSHD_HANDSHAKE_TIMEOUT) != SSHD_MSG_KEXINIT)
        return sshd_fail("expected KEXINIT from client");
    if (sshd.pkt_len > sizeof(sshd.i_c))
        return sshd_fail("client KEXINIT too large");
    memcpy(sshd.i_c, sshd.pkt, sshd.pkt_len);
    sshd.i_c_len = sshd.pkt_len;
    if (sshd_negotiate(&discard) != 0) return -1;
    if (discard && sshd_next_msg(SSHD_HANDSHAKE_TIMEOUT) < 0) return -1;

    if (sshd_next_msg(SSHD_HANDSHAKE_TIMEOUT) != SSHD_MSG_ECDH_INIT)
        return sshd_fail("expected key exchange init");
    sshd_rd_t r = { sshd.pkt + 1, sshd.pkt_len - 1, 0 };
    const uint8_t *qc;
    size_t qc_len;
    sshd_rd_str(&r, &qc, &qc_len);
    if (r.bad || qc_len != 32) return sshd_fail("malformed key exchange init");

    if (x25519_keypair(priv, pub) != 0)
        return sshd_fail("key generation failed (QEMU needs -cpu max)");
    if (x25519_shared(priv, qc, k) != 0) {
        sshd_wipe(priv, sizeof(priv));
        return sshd_fail("client sent a degenerate key share");
    }
    sshd_wipe(priv, sizeof(priv));

    sha256_ctx c;
    sha256_init(&c);
    sshd_hash_str(&c, sshd.v_c, strlen(sshd.v_c));
    sshd_hash_str(&c, sshd.v_s, strlen(sshd.v_s));
    sshd_hash_str(&c, sshd.i_c, sshd.i_c_len);
    sshd_hash_str(&c, sshd.i_s, sshd.i_s_len);
    sshd_hash_str(&c, sshd_hostblob, sizeof(sshd_hostblob));
    sshd_hash_str(&c, qc, 32);
    sshd_hash_str(&c, pub, 32);
    sshd_hash_mpint(&c, k, 32);
    sha256_final(&c, h);

    ed25519_sign32(sshd_seed, sshd_pub, h, sig);

    uint8_t sigblob[4 + 11 + 4 + 64];
    sshd_put_be32(sigblob, 11);
    memcpy(sigblob + 4, "ssh-ed25519", 11);
    sshd_put_be32(sigblob + 15, 64);
    memcpy(sigblob + 19, sig, 64);

    sshd_wr_t w;
    sshd_wr_init(&w);
    sshd_wr_u8(&w, SSHD_MSG_ECDH_REPLY);
    sshd_wr_str(&w, sshd_hostblob, sizeof(sshd_hostblob));
    sshd_wr_str(&w, pub, 32);
    sshd_wr_str(&w, sigblob, sizeof(sigblob));
    if (sshd_wr_send(&w) != 0) return -1;

    sshd_wr_init(&w);
    sshd_wr_u8(&w, SSHD_MSG_NEWKEYS);
    if (sshd_wr_send(&w) != 0) return -1;

    // The first exchange hash is also the session id. Server side of RFC 4253
    // section 7.2: IV/key "B"/"D" protect what we send, "A"/"C" what we read.
    sshd_kdf(k, h, 'A', h, keys);
    memcpy(sshd.iv_in, keys, 12);
    sshd_kdf(k, h, 'B', h, keys);
    memcpy(sshd.iv_out, keys, 12);
    sshd_kdf(k, h, 'C', h, keys);
    memcpy(sshd.key_in, keys, 16);
    sshd_kdf(k, h, 'D', h, keys);
    memcpy(sshd.key_out, keys, 16);
    sshd_wipe(keys, sizeof(keys));
    sshd_wipe(k, sizeof(k));
    sshd.enc_out = 1;

    if (sshd_next_msg(SSHD_HANDSHAKE_TIMEOUT) != SSHD_MSG_NEWKEYS)
        return sshd_fail("expected NEWKEYS from client");
    sshd.enc_in = 1;
    return 0;
}

static int sshd_exchange_idents(void) {
    char line[256];
    char banner[sizeof(SSHD_VERSION_STRING) + 2];

    snprintf(sshd.v_s, sizeof(sshd.v_s), "%s", SSHD_VERSION_STRING);
    snprintf(banner, sizeof(banner), "%s\r\n", SSHD_VERSION_STRING);
    if (sock_sendall(sshd.sock, banner, strlen(banner)) != 0)
        return sshd_fail("send failed: %s", sock_last_message());

    sock_settimeout(sshd.sock, SSHD_IDENT_TIMEOUT_MS);
    ssize_t n = sock_recvline(sshd.sock, line, sizeof(line));
    if (n <= 0 || strncmp(line, "SSH-", 4) != 0)
        return sshd_fail("no SSH banner from client");
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = 0;
    if (strncmp(line, "SSH-2.0-", 8) != 0 && strncmp(line, "SSH-1.99-", 9) != 0)
        return sshd_fail("client speaks %s, not SSH-2", line);
    snprintf(sshd.v_c, sizeof(sshd.v_c), "%s", line);
    return 0;
}

// ── User authentication (RFC 4252, password method) ─────────────────────

// 0 on success. Unknown user and wrong password are indistinguishable here
// on purpose (userctl returns the same error for both).
static int sshd_login_check(const char *user, const char *pass,
                            uctl_response_t *resp) {
    static uctl_request_t req;
    if (!sshd_uctl) sshd_uctl = uctl_attach();
    if (!sshd_uctl) return -1;

    memset(&req, 0, sizeof(req));
    req.cmd = UCTL_CMD_LOGIN;
    strlcpy(req.arg_user, user, sizeof(req.arg_user));
    strlcpy(req.arg_pass, pass, sizeof(req.arg_pass));
    int rc = uctl_call(sshd_uctl, &req, resp);
    uctl_scrub(&req, sizeof(req));
    return rc;
}

static int sshd_send_auth_failure(void) {
    sshd_wr_t w;
    sshd_wr_init(&w);
    sshd_wr_u8(&w, SSHD_MSG_USERAUTH_FAILURE);
    sshd_wr_cstr(&w, "password");
    sshd_wr_u8(&w, 0);
    return sshd_wr_send(&w);
}

// Names the userctl field limits: ulen 1..31 printable, plen 1..63 no NUL.
static int sshd_credentials_ok(const uint8_t *user, size_t ul,
                               const uint8_t *pass, size_t pl) {
    if (ul == 0 || ul >= UCTL_NAME_MAX || pl == 0 || pl >= UCTL_PASS_MAX)
        return 0;
    for (size_t i = 0; i < ul; i++)
        if (user[i] <= 0x20 || user[i] >= 0x7F) return 0;
    for (size_t i = 0; i < pl; i++)
        if (pass[i] == 0) return 0;
    return 1;
}

// 0: logged in. -1: connection must end (sshd_errmsg set). -2: too many tries.
static int sshd_auth(void) {
    sshd_wr_t w;
    uint64_t deadline = time_ms() + SSHD_LOGIN_GRACE_MS;
    int failures = 0;

    if (sshd_next_msg(SSHD_HANDSHAKE_TIMEOUT) != SSHD_MSG_SERVICE_REQUEST)
        return sshd_fail("expected a service request");
    {
        sshd_rd_t r = { sshd.pkt + 1, sshd.pkt_len - 1, 0 };
        const uint8_t *svc;
        size_t sl;
        sshd_rd_str(&r, &svc, &sl);
        if (r.bad || !sshd_str_is(svc, sl, "ssh-userauth"))
            return sshd_fail("unsupported service requested");
    }
    sshd_wr_init(&w);
    sshd_wr_u8(&w, SSHD_MSG_SERVICE_ACCEPT);
    sshd_wr_cstr(&w, "ssh-userauth");
    if (sshd_wr_send(&w) != 0) return -1;

    for (;;) {
        uint64_t now = time_ms();
        if (now >= deadline) return sshd_fail("login grace time expired");
        int t = sshd_next_msg((uint32_t)(deadline - now));
        if (t < 0) return -1;
        if (t != SSHD_MSG_USERAUTH_REQUEST)
            return sshd_fail("unexpected message %d before login", t);

        sshd_rd_t r = { sshd.pkt + 1, sshd.pkt_len - 1, 0 };
        const uint8_t *user, *svc, *method;
        size_t ul, sl, ml;
        sshd_rd_str(&r, &user, &ul);
        sshd_rd_str(&r, &svc, &sl);
        sshd_rd_str(&r, &method, &ml);
        if (r.bad || !sshd_str_is(svc, sl, "ssh-connection"))
            return sshd_fail("malformed or unsupported login request");

        if (!sshd_str_is(method, ml, "password")) {
            // "none" is how clients ask which methods exist; it is free.
            if (sshd_send_auth_failure() != 0) return -1;
            continue;
        }

        uint8_t change = sshd_rd_u8(&r);
        const uint8_t *pass;
        size_t pl;
        sshd_rd_str(&r, &pass, &pl);
        if (r.bad) return sshd_fail("malformed password request");

        char ubuf[UCTL_NAME_MAX], pbuf[UCTL_PASS_MAX];
        uctl_response_t resp;
        int ok = -1;
        if (!change && sshd_credentials_ok(user, ul, pass, pl)) {
            memcpy(ubuf, user, ul);
            ubuf[ul] = 0;
            memcpy(pbuf, pass, pl);
            pbuf[pl] = 0;
            ok = sshd_login_check(ubuf, pbuf, &resp);
            sshd_wipe(pbuf, sizeof(pbuf));
        }
        sshd_wipe(sshd.pkt, sizeof(sshd.pkt));   // held the password

        if (ok == 0 && resp.uid == 0 && !SSHD_ALLOW_ROOT) {
            printf("sshd: root login refused (SSHD_ALLOW_ROOT=0)\n");
            ok = -1;
        }
        if (ok == 0) {
            memcpy(sshd.user, ubuf, ul + 1);
            sshd.uid = (int)resp.uid;
            sshd.gid = (int)resp.gid;
            sshd_wr_init(&w);
            sshd_wr_u8(&w, SSHD_MSG_USERAUTH_SUCCESS);
            return sshd_wr_send(&w);
        }

        failures++;
        printf("sshd: auth failed (%d/%d)\n", failures, SSHD_MAX_AUTH_TRIES);
        if (failures >= SSHD_MAX_AUTH_TRIES) {
            sshd_send_disconnect(SSHD_DISCONNECT_NO_MORE_AUTH,
                                 "too many authentication failures");
            return -2;
        }
        sleep(SSHD_FAIL_DELAY_S);                // throttles guessing
        if (sshd_send_auth_failure() != 0) return -1;
    }
}

// ── Shell on a pty ──────────────────────────────────────────────────────

static void sshd_clamp_winsize(uint16_t *rows, uint16_t *cols) {
    if (*rows == 0 || *rows > SSHD_DIM_MAX) *rows = SSHD_DEFAULT_ROWS;
    if (*cols == 0 || *cols > SSHD_DIM_MAX) *cols = SSHD_DEFAULT_COLS;
}

static void sshd_pty_set_winsize(int master, uint16_t rows, uint16_t cols) {
    winsize_t ws = { rows, cols, 0, 0 };
    tty_setwinsize(master, &ws);
}

// Same sequence getty, mdterm and ursd use. ModuOS keeps one global fd table,
// so the child must not close the master and must not dup2; the shell reaches
// the pty through its controlling tty instead. gid is dropped before uid, as
// login.c does, since setgid needs the privilege setuid gives up.
static __attribute__((noreturn)) void sshd_shell_child(const char *slave) {
    if (setsid() < 0) _exit(2);
    int slave_fd = open(slave, O_RDWR, 0);
    if (slave_fd < 0) _exit(3);
    if (tty_set_ctty(slave_fd) < 0) _exit(4);
    if (setgid(sshd.gid) != 0 || setuid(sshd.uid) != 0) _exit(126);

    char env_user[48], env_logname[52], env_term[48];
    snprintf(env_user, sizeof(env_user), "USER=%s", sshd.user);
    snprintf(env_logname, sizeof(env_logname), "LOGNAME=%s", sshd.user);
    snprintf(env_term, sizeof(env_term), "TERM=%s", sshd.term);
    char *argv[] = { (char *)SSHD_SHELL_PATH, NULL };
    char *envp[] = { env_user, env_logname, env_term, NULL };
    execve(SSHD_SHELL_PATH, argv, envp);
    _exit(127);
}

static int sshd_shell_spawn(void) {
    char slave[32];
    sshd.sh.master = -1;
    sshd.sh.pid = -1;

    int master = tty_openpty(slave, sizeof(slave));
    if (master < 0) return -1;
    sshd_pty_set_winsize(master, sshd.rows, sshd.cols);

    int pid = fork();
    if (pid < 0) {
        close(master);
        return -1;
    }
    if (pid == 0) sshd_shell_child(slave);

    int one = 1;
    tty_ioctl(master, TTY_IOC_NONBLOCK, &one, sizeof(one));
    sshd.sh.master = master;
    sshd.sh.pid = pid;
    return 0;
}

// Returns 1 once the child has been reaped (or is not ours to reap).
static int sshd_wait_exit(int pid, uint32_t timeout_ms, int *status) {
    uint64_t deadline = time_ms() + timeout_ms;
    for (;;) {
        if (waitpid(pid, status, WNOHANG) != 0) return 1;
        if (time_ms() >= deadline) return 0;
        yield();
    }
}

// Always reaps: closing the master hangs up the slave (SIGHUP to everything
// on the pty), then escalate for a shell that ignores it.
static void sshd_shell_stop(void) {
    int status = 0;

    if (sshd.sh.master >= 0) {
        close(sshd.sh.master);
        sshd.sh.master = -1;
    }
    if (sshd.sh.pid <= 0) return;

    if (!sshd_wait_exit(sshd.sh.pid, 0, &status)) {
        kill(sshd.sh.pid, SIGHUP);
        kill(sshd.sh.pid, SIGTERM);
        if (!sshd_wait_exit(sshd.sh.pid, SSHD_STOP_WAIT_MS, &status)) {
            kill(sshd.sh.pid, SIGKILL);
            if (!sshd_wait_exit(sshd.sh.pid, SSHD_STOP_WAIT_MS, &status))
                printf("sshd: shell pid %d would not die\n", sshd.sh.pid);
        }
    }
    sshd.sh.pid = -1;
}

static int sshd_shell_write(const uint8_t *p, size_t len) {
    while (len) {
        ssize_t w = write(sshd.sh.master, p, len);
        if (w <= 0) return -1;
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

// ── Session channel (RFC 4254) ──────────────────────────────────────────

static int sshd_chan_msg(uint8_t type) {
    sshd_wr_t w;
    sshd_wr_init(&w);
    sshd_wr_u8(&w, type);
    sshd_wr_u32(&w, sshd.remote_chan);
    return sshd_wr_send(&w);
}

static int sshd_send_close(void) {
    if (!sshd.chan_open || sshd.close_sent) return 0;
    sshd.close_sent = 1;
    return sshd_chan_msg(SSHD_MSG_CHANNEL_CLOSE);
}

static int sshd_reply(uint8_t want, int ok) {
    if (!want) return 0;
    return sshd_chan_msg(ok ? SSHD_MSG_CHANNEL_SUCCESS
                            : SSHD_MSG_CHANNEL_FAILURE);
}

// Window is topped up in half-window steps so one adjust covers many packets.
static int sshd_consume(size_t n) {
    sshd.local_win = n >= sshd.local_win ? 0 : sshd.local_win - (uint32_t)n;
    if (sshd.local_win >= SSHD_LOCAL_WINDOW / 2) return 0;
    sshd_wr_t w;
    sshd_wr_init(&w);
    sshd_wr_u8(&w, SSHD_MSG_CHANNEL_ADJUST);
    sshd_wr_u32(&w, sshd.remote_chan);
    sshd_wr_u32(&w, SSHD_LOCAL_WINDOW - sshd.local_win);
    sshd.local_win = SSHD_LOCAL_WINDOW;
    return sshd_wr_send(&w);
}

static int sshd_open_reject(uint32_t sender, uint32_t reason) {
    sshd_wr_t w;
    sshd_wr_init(&w);
    sshd_wr_u8(&w, SSHD_MSG_CHANNEL_OPEN_FAIL);
    sshd_wr_u32(&w, sender);
    sshd_wr_u32(&w, reason);
    sshd_wr_cstr(&w, "");
    sshd_wr_cstr(&w, "");
    return sshd_wr_send(&w);
}

// TERM only ever reaches the shell's environment, so keep it to the
// characters a terminfo name uses.
static void sshd_set_term(const uint8_t *t, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n && o < sizeof(sshd.term) - 1; i++) {
        uint8_t c = t[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) { o = 0; break; }
        sshd.term[o++] = (char)c;
    }
    if (o == 0) {
        strlcpy(sshd.term, "linux", sizeof(sshd.term));
    } else {
        sshd.term[o] = 0;
    }
}

static int sshd_open_session(sshd_rd_t *r) {
    sshd_wr_t w;
    uint32_t sender = sshd_rd_u32(r);
    uint32_t window = sshd_rd_u32(r);
    uint32_t maxpkt = sshd_rd_u32(r);
    if (r->bad) return sshd_fail("malformed channel open");

    if (sshd.chan_open) return sshd_open_reject(sender, SSHD_OPEN_RESOURCE_SHORTAGE);

    sshd.chan_open = 1;
    sshd.remote_chan = sender;
    sshd.remote_win = window;
    sshd.remote_maxpkt = maxpkt < 64 ? 64 : maxpkt;
    sshd.local_win = SSHD_LOCAL_WINDOW;
    sshd.rows = SSHD_DEFAULT_ROWS;
    sshd.cols = SSHD_DEFAULT_COLS;
    strlcpy(sshd.term, "linux", sizeof(sshd.term));

    sshd_wr_init(&w);
    sshd_wr_u8(&w, SSHD_MSG_CHANNEL_OPEN_OK);
    sshd_wr_u32(&w, sender);
    sshd_wr_u32(&w, SSHD_LOCAL_CHANNEL);
    sshd_wr_u32(&w, SSHD_LOCAL_WINDOW);
    sshd_wr_u32(&w, SSHD_LOCAL_MAX_PACKET);
    return sshd_wr_send(&w);
}

static int sshd_channel_request(sshd_rd_t *r) {
    uint32_t recipient = sshd_rd_u32(r);
    const uint8_t *req;
    size_t rl;
    sshd_rd_str(r, &req, &rl);
    uint8_t want = sshd_rd_u8(r);
    if (r->bad) return sshd_fail("malformed channel request");
    if (!sshd.chan_open || recipient != SSHD_LOCAL_CHANNEL)
        return 0;

    if (sshd_str_is(req, rl, "pty-req")) {
        const uint8_t *term;
        size_t tl;
        sshd_rd_str(r, &term, &tl);
        uint32_t cols = sshd_rd_u32(r);
        uint32_t rows = sshd_rd_u32(r);
        if (r->bad || sshd.sh.pid > 0) return sshd_reply(want, 0);
        sshd_set_term(term, tl);
        sshd.cols = cols > SSHD_DIM_MAX ? 0 : (uint16_t)cols;
        sshd.rows = rows > SSHD_DIM_MAX ? 0 : (uint16_t)rows;
        sshd_clamp_winsize(&sshd.rows, &sshd.cols);
        sshd.have_pty = 1;
        return sshd_reply(want, 1);
    }
    if (sshd_str_is(req, rl, "shell")) {
        if (sshd.sh.pid > 0) return sshd_reply(want, 0);
        if (sshd_shell_spawn() != 0) {
            printf("sshd: cannot start shell (pty or fork failed)\n");
            return sshd_reply(want, 0);
        }
        return sshd_reply(want, 1);
    }
    if (sshd_str_is(req, rl, "window-change")) {
        uint32_t cols = sshd_rd_u32(r);
        uint32_t rows = sshd_rd_u32(r);
        if (r->bad || sshd.sh.master < 0) return 0;
        sshd.cols = cols > SSHD_DIM_MAX ? 0 : (uint16_t)cols;
        sshd.rows = rows > SSHD_DIM_MAX ? 0 : (uint16_t)rows;
        sshd_clamp_winsize(&sshd.rows, &sshd.cols);
        sshd_pty_set_winsize(sshd.sh.master, sshd.rows, sshd.cols);
        return 0;
    }
    if (sshd_str_is(req, rl, "signal") || sshd_str_is(req, rl, "eow@openssh.com"))
        return 0;

    // env, exec, subsystem, x11-req, agent forwarding, keepalives, ...
    return sshd_reply(want, 0);
}

// Handles one connection-level message. 0 on success, 1 when the client closed
// the channel, -1 on a fatal error.
static int sshd_dispatch(int type) {
    sshd_rd_t r = { sshd.pkt + 1, sshd.pkt_len - 1, 0 };
    sshd_wr_t w;
    const uint8_t *d;
    size_t dl;

    switch (type) {
    case SSHD_MSG_CHANNEL_OPEN: {
        sshd_rd_str(&r, &d, &dl);
        if (r.bad) return sshd_fail("malformed channel open");
        if (sshd_str_is(d, dl, "session")) return sshd_open_session(&r);
        uint32_t sender = sshd_rd_u32(&r);
        return r.bad ? 0 : sshd_open_reject(sender, SSHD_OPEN_UNKNOWN_TYPE);
    }

    case SSHD_MSG_CHANNEL_REQUEST:
        return sshd_channel_request(&r);

    case SSHD_MSG_CHANNEL_DATA:
    case SSHD_MSG_CHANNEL_EXT_DATA:
        sshd_rd_u32(&r);
        if (type == SSHD_MSG_CHANNEL_EXT_DATA) sshd_rd_u32(&r);
        sshd_rd_str(&r, &d, &dl);
        if (r.bad) return sshd_fail("malformed channel data");
        if (type == SSHD_MSG_CHANNEL_DATA && sshd.sh.master >= 0 &&
            sshd_shell_write(d, dl) != 0)
            return 1;
        return sshd_consume(dl);

    case SSHD_MSG_CHANNEL_ADJUST: {
        sshd_rd_u32(&r);
        uint32_t add = sshd_rd_u32(&r);
        if (r.bad) return sshd_fail("malformed window adjust");
        sshd.remote_win = add > 0xFFFFFFFFu - sshd.remote_win
                              ? 0xFFFFFFFFu : sshd.remote_win + add;
        return 0;
    }

    case SSHD_MSG_CHANNEL_CLOSE:
        sshd_send_close();
        return 1;

    case SSHD_MSG_GLOBAL_REQUEST:
        sshd_rd_str(&r, &d, &dl);
        if (sshd_rd_u8(&r) && !r.bad) {
            sshd_wr_init(&w);
            sshd_wr_u8(&w, SSHD_MSG_REQUEST_FAILURE);
            return sshd_wr_send(&w);
        }
        return 0;

    case SSHD_MSG_KEXINIT:
        return sshd_fail("client asked to re-key, which sshd does not support");

    default:
        return 0;
    }
}

// 0 normally, -1 on a send failure; *eof is set once the slave is gone.
static int sshd_pump_output(int *eof) {
    uint8_t buf[SSHD_OUT_CHUNK];
    size_t total = 0;

    while (total < SSHD_PUMP_MAX && sshd.remote_win > 0) {
        size_t cap = sizeof(buf);
        if (cap > sshd.remote_win) cap = sshd.remote_win;
        if (cap > sshd.remote_maxpkt) cap = sshd.remote_maxpkt;
        ssize_t n = read(sshd.sh.master, buf, cap);
        if (n < 0) break;                 // EAGAIN: queue is empty
        if (n == 0) {
            *eof = 1;                     // slave gone: the shell exited
            break;
        }
        sshd_wr_t w;
        sshd_wr_init(&w);
        sshd_wr_u8(&w, SSHD_MSG_CHANNEL_DATA);
        sshd_wr_u32(&w, sshd.remote_chan);
        sshd_wr_str(&w, buf, (size_t)n);
        sshd.remote_win -= (uint32_t)n;
        if (sshd_wr_send(&w) != 0) return -1;
        total += (size_t)n;
    }
    return (int)total;
}

static int sshd_exit_code(int status) {
    if (status == 0) return 0;
    if ((status & 0x7F) != 0) return 128 + (status & 0x7F);
    return (status >> 8) & 0xFF;
}

// Tells the client the shell ended: exit-status, EOF, CLOSE.
static int sshd_finish_channel(int status) {
    sshd_wr_t w;
    sshd_wr_init(&w);
    sshd_wr_u8(&w, SSHD_MSG_CHANNEL_REQUEST);
    sshd_wr_u32(&w, sshd.remote_chan);
    sshd_wr_cstr(&w, "exit-status");
    sshd_wr_u8(&w, 0);
    sshd_wr_u32(&w, (uint32_t)sshd_exit_code(status));
    if (sshd_wr_send(&w) != 0) return -1;
    if (sshd_chan_msg(SSHD_MSG_CHANNEL_EOF) != 0) return -1;
    return sshd_send_close();
}

// Only one session fits the single netd mailbox, so a second connection is
// closed at once instead of silently sitting in netd's backlog.
static void sshd_reject_extra(int listener) {
    sock_addr_t peer;
    int sock = sock_accept(listener, &peer);
    if (sock >= 0) sock_close(sock);
}

static sshd_end_t sshd_session(int listener) {
    uint64_t next_busy = time_ms() + SSHD_BUSY_POLL_MS;
    uint64_t chan_deadline = time_ms() + SSHD_LOGIN_GRACE_MS;

    sock_settimeout(sshd.sock, SOCK_NONBLOCK);
    sock_settimeout(listener, SOCK_NONBLOCK);

    for (;;) {
        int idle = 1;

        int r = sshd_rx_poll();
        if (r < 0) return SSHD_END_CLIENT;
        if (r > 0) {
            idle = 0;
            int t = sshd_classify();
            if (t < 0) return SSHD_END_CLIENT;
            if (t > 0) {
                int d = sshd_dispatch(t);
                if (d > 0) return SSHD_END_CLIENT;
                if (d < 0) {
                    sshd_send_disconnect(SSHD_DISCONNECT_PROTOCOL_ERROR,
                                         "protocol error");
                    return SSHD_END_ERROR;
                }
            }
        }

        if (sshd.sh.master >= 0) {
            int eof = 0;
            int pumped = sshd_pump_output(&eof);
            if (pumped < 0) return SSHD_END_CLIENT;
            if (pumped > 0) idle = 0;

            int status = 0;
            int gone = eof;
            if (!gone && waitpid(sshd.sh.pid, &status, WNOHANG) != 0) {
                sshd.sh.pid = -1;         // reaped here, nothing left to stop
                gone = 1;
            } else if (gone && sshd.sh.pid > 0) {
                if (sshd_wait_exit(sshd.sh.pid, SSHD_STOP_WAIT_MS, &status))
                    sshd.sh.pid = -1;
            }
            if (gone) {
                int dummy = 0;
                if (sshd_pump_output(&dummy) < 0) return SSHD_END_CLIENT;
                if (sshd_finish_channel(status) != 0) return SSHD_END_CLIENT;

                // Give the client a moment to answer with its own CLOSE.
                uint64_t until = time_ms() + SSHD_CLOSE_WAIT_MS;
                while (time_ms() < until) {
                    int rr = sshd_rx_poll();
                    if (rr < 0) break;
                    if (rr > 0) {
                        int t = sshd_classify();
                        if (t < 0 || t == SSHD_MSG_CHANNEL_CLOSE) break;
                    } else {
                        yield();
                    }
                }
                sshd_send_disconnect(SSHD_DISCONNECT_BY_APPLICATION, "");
                return SSHD_END_SHELL;
            }
        } else if (!sshd.chan_open && time_ms() >= chan_deadline) {
            sshd_send_disconnect(SSHD_DISCONNECT_PROTOCOL_ERROR,
                                 "no session opened");
            return SSHD_END_ERROR;
        }

        if (time_ms() >= next_busy) {
            next_busy = time_ms() + SSHD_BUSY_POLL_MS;
            sshd_reject_extra(listener);
        }
        if (idle) yield();
    }
}

// ── Connection / main ───────────────────────────────────────────────────

static sshd_end_t sshd_serve(int sock, int listener, const sock_addr_t *peer) {
    printf("sshd: connection from %s:%d\n", inet_ntoa(peer->ip),
           (int)peer->port);

    memset(&sshd, 0, sizeof(sshd));
    sshd.sock = sock;
    sshd.sh.master = -1;
    sshd.sh.pid = -1;

    sshd_end_t end = SSHD_END_ERROR;
    if (sshd_exchange_idents() != 0 || sshd_kex() != 0) {
        printf("sshd: handshake failed: %s\n", sshd_errmsg);
        goto out;
    }
    int auth = sshd_auth();
    if (auth == -2) {
        end = SSHD_END_AUTHFAIL;
        goto out;
    }
    if (auth != 0) {
        printf("sshd: login failed: %s\n", sshd_errmsg);
        goto out;
    }

    printf("sshd: login ok: %s (uid %d)\n", sshd.user, sshd.uid);
    end = sshd_session(listener);
    if (end == SSHD_END_ERROR) printf("sshd: session error: %s\n", sshd_errmsg);
    printf("sshd: session for %s ended\n", sshd.user);

out:
    sshd_shell_stop();
    sshd_wipe(&sshd, sizeof(sshd));
    return end;
}

static int sshd_listen_open(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    // The host argument is ignored by sockets.h (netd has one interface and
    // listens on it); it is passed for the reader.
    if (sock_bind(fd, "0.0.0.0", port) != 0 || sock_listen(fd, 4) != 0) {
        int saved = errno;
        sock_close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

int md_main(long argc, char **argv) {
    unsigned long port = SSHD_PORT_DEFAULT;

    for (long i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = strtoul(argv[++i], NULL, 10);
        } else {
            printf("usage: sshd [-p port]\n");
            return 2;
        }
    }
    if (port == 0 || port > 65535) {
        printf("usage: sshd [-p port]\n");
        return 2;
    }
    if (geteuid() != 0) {
        printf("sshd: must run as root to switch to the login user\n");
        return 1;
    }
    if (sshd_hostkey_load() != 0) return 1;

    {
        uint8_t fp[32];
        char b64[48];
        sha256(sshd_hostblob, sizeof(sshd_hostblob), fp);
        sshd_b64(fp, 32, b64);
        printf("sshd: host key ssh-ed25519 SHA256:%s\n", b64);
    }

    int listener = sshd_listen_open((int)port);
    if (listener < 0) {
        printf("sshd: cannot listen on port %lu (errno=%d, %s)\n", port, errno,
               sock_last_message());
        return 1;
    }
    printf("sshd: listening on port %lu\n", port);

    int accept_failures = 0;
    for (;;) {
        sock_addr_t peer;
        sock_settimeout(listener, SOCK_BLOCKING);
        int sock = sock_accept(listener, &peer);
        if (sock < 0) {
            printf("sshd: accept failed (errno=%d, %s)\n", errno,
                   sock_last_message());
            sleep(1);
            // netd restarting leaves a dead listener handle behind.
            if (++accept_failures >= SSHD_ACCEPT_FAIL_MAX) {
                sock_close(listener);
                listener = sshd_listen_open((int)port);
                if (listener < 0) {
                    printf("sshd: cannot re-listen, exiting\n");
                    return 1;
                }
                accept_failures = 0;
            }
            continue;
        }
        accept_failures = 0;

        sshd_end_t end = sshd_serve(sock, listener, &peer);
        sock_close(sock);
        if (end == SSHD_END_AUTHFAIL) sleep(SSHD_COOLDOWN_S);
    }
    return 0;
}