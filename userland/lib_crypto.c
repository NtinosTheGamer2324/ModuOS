// lib_crypto.c -- crypto primitives for lib_tls.c (shared as crypto.sqrl, no entry point)
//
// SHA-256 comes from sha256.h. Everything else is here: HMAC, AES-128-GCM,
// x25519, P-256 (ECDH + ECDSA verify), RSA verify (PKCS#1 v1.5 + PSS), a DER
// walker for X.509 leaf certs, and a random source (the md64api urandom node).
//
// Written for clarity first: bignums are 32-bit limbs with Montgomery
// multiplication, GHASH is bit-serial, scalar multiplication is plain
// double-and-add. None of it is constant-time. Needs memcpy/memset/memmove
// from string.h (sha256.h already pulls that in).
#include "sockets.h"   /* pulls in libc.h: open/read/close for random_bytes() */
#include "crypto.h"

static int bytes_eq(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

/* ================================================================ HMAC */
void hmac_sha256(const uint8_t *key, size_t klen,
                 const uint8_t *msg, size_t mlen, uint8_t out[32]) {
    uint8_t k[64], pad[64], inner[32];
    sha256_ctx c;
    memset(k, 0, sizeof(k));
    if (klen > 64) sha256(key, klen, k);
    else if (klen) memcpy(k, key, klen);

    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&c);
    sha256_update(&c, pad, 64);
    sha256_update(&c, msg, mlen);
    sha256_final(&c, inner);

    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&c);
    sha256_update(&c, pad, 64);
    sha256_update(&c, inner, 32);
    sha256_final(&c, out);
}

/* ================================================================ AES-128 */
static uint8_t g_sbox[256];
static int     g_sbox_ok;

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
static uint8_t rotl8(uint8_t x, int s) { return (uint8_t)((x << s) | (x >> (8 - s))); }

/* S-box generated from the GF(2^8) inverse + affine map (no table to mistype). */
static void sbox_init(void) {
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1b : 0));      /* p *= 3 */
        q ^= (uint8_t)(q << 1); q ^= (uint8_t)(q << 2); q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;                                    /* q /= 3 */
        uint8_t x = (uint8_t)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4));
        g_sbox[p] = x ^ 0x63;
    } while (p != 1);
    g_sbox[0] = 0x63;
    g_sbox_ok = 1;
}

typedef struct { uint8_t rk[176]; } aes128_t;

static void aes128_expand(aes128_t *a, const uint8_t key[16]) {
    if (!g_sbox_ok) sbox_init();
    memcpy(a->rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 4; i < 44; i++) {
        uint8_t t[4];
        memcpy(t, a->rk + 4 * (i - 1), 4);
        if ((i & 3) == 0) {
            uint8_t u = t[0];
            t[0] = g_sbox[t[1]] ^ rcon;
            t[1] = g_sbox[t[2]];
            t[2] = g_sbox[t[3]];
            t[3] = g_sbox[u];
            rcon = xtime(rcon);
        }
        for (int j = 0; j < 4; j++) a->rk[4 * i + j] = a->rk[4 * (i - 4) + j] ^ t[j];
    }
}

static void aes128_encrypt(const aes128_t *a, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ a->rk[i];
    for (int round = 1; round <= 10; round++) {
        for (int c = 0; c < 4; c++)                       /* SubBytes + ShiftRows */
            for (int r = 0; r < 4; r++)
                t[r + 4 * c] = g_sbox[s[r + 4 * ((c + r) & 3)]];
        if (round < 10) {                                 /* MixColumns */
            for (int c = 0; c < 4; c++) {
                uint8_t a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3];
                s[4 * c + 0] = xtime(a0) ^ xtime(a1) ^ a1 ^ a2 ^ a3;
                s[4 * c + 1] = a0 ^ xtime(a1) ^ xtime(a2) ^ a2 ^ a3;
                s[4 * c + 2] = a0 ^ a1 ^ xtime(a2) ^ xtime(a3) ^ a3;
                s[4 * c + 3] = xtime(a0) ^ a0 ^ a1 ^ a2 ^ xtime(a3);
            }
        } else {
            memcpy(s, t, 16);
        }
        for (int i = 0; i < 16; i++) s[i] ^= a->rk[16 * round + i];
    }
    memcpy(out, s, 16);
}

/* ================================================================ GCM */
typedef struct { uint64_t hi, lo; } u128;

static u128 load128(const uint8_t b[16]) {
    u128 r = {0, 0};
    for (int i = 0; i < 8; i++) { r.hi = (r.hi << 8) | b[i]; r.lo = (r.lo << 8) | b[8 + i]; }
    return r;
}

/* Multiplication in GF(2^128) with the GCM bit order. */
static u128 gf_mul(u128 x, u128 h) {
    u128 z = {0, 0}, v = h;
    for (int i = 0; i < 128; i++) {
        uint64_t bit = (i < 64) ? (x.hi >> (63 - i)) & 1 : (x.lo >> (127 - i)) & 1;
        if (bit) { z.hi ^= v.hi; z.lo ^= v.lo; }
        uint64_t lsb = v.lo & 1;
        v.lo = (v.lo >> 1) | (v.hi << 63);
        v.hi >>= 1;
        if (lsb) v.hi ^= 0xe100000000000000ULL;
    }
    return z;
}

static void ghash_update(u128 *y, u128 h, const uint8_t *data, size_t len) {
    while (len) {
        uint8_t blk[16];
        size_t n = len < 16 ? len : 16;
        memset(blk, 0, 16);
        memcpy(blk, data, n);
        u128 b = load128(blk);
        y->hi ^= b.hi; y->lo ^= b.lo;
        *y = gf_mul(*y, h);
        data += n; len -= n;
    }
}

static void gcm_ctr(const aes128_t *a, const uint8_t nonce[12], uint32_t ctr,
                    const uint8_t *in, size_t len, uint8_t *out) {
    uint8_t cb[16], ks[16];
    memcpy(cb, nonce, 12);
    while (len) {
        cb[12] = (uint8_t)(ctr >> 24); cb[13] = (uint8_t)(ctr >> 16);
        cb[14] = (uint8_t)(ctr >> 8);  cb[15] = (uint8_t)ctr;
        aes128_encrypt(a, cb, ks);
        size_t n = len < 16 ? len : 16;
        for (size_t i = 0; i < n; i++) out[i] = in[i] ^ ks[i];
        in += n; out += n; len -= n; ctr++;
    }
}

static void gcm_tag(const aes128_t *a, const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                    const uint8_t *ct, size_t ct_len, uint8_t tag[16]) {
    uint8_t zero[16], hb[16], j0[16], ek[16], lens[16];
    memset(zero, 0, 16);
    aes128_encrypt(a, zero, hb);
    u128 h = load128(hb), y = {0, 0};
    ghash_update(&y, h, aad, aad_len);
    ghash_update(&y, h, ct, ct_len);
    uint64_t abits = (uint64_t)aad_len * 8, cbits = (uint64_t)ct_len * 8;
    for (int i = 0; i < 8; i++) { lens[i] = (uint8_t)(abits >> (56 - 8 * i)); lens[8 + i] = (uint8_t)(cbits >> (56 - 8 * i)); }
    ghash_update(&y, h, lens, 16);

    memcpy(j0, nonce, 12);
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
    aes128_encrypt(a, j0, ek);
    for (int i = 0; i < 8; i++) {
        tag[i]     = ek[i]     ^ (uint8_t)(y.hi >> (56 - 8 * i));
        tag[8 + i] = ek[8 + i] ^ (uint8_t)(y.lo >> (56 - 8 * i));
    }
}

int aes128gcm_seal(const uint8_t key[16], const uint8_t nonce[12],
                   const uint8_t *aad, size_t aad_len,
                   const uint8_t *pt, size_t pt_len, uint8_t *out) {
    aes128_t a;
    aes128_expand(&a, key);
    gcm_ctr(&a, nonce, 2, pt, pt_len, out);
    gcm_tag(&a, nonce, aad, aad_len, out, pt_len, out + pt_len);
    return 0;
}

int aes128gcm_open(const uint8_t key[16], const uint8_t nonce[12],
                   const uint8_t *aad, size_t aad_len,
                   const uint8_t *ct, size_t ct_len, uint8_t *out) {
    if (ct_len < 16) return -1;
    size_t n = ct_len - 16;
    aes128_t a;
    uint8_t tag[16];
    aes128_expand(&a, key);
    gcm_tag(&a, nonce, aad, aad_len, ct, n, tag);
    if (!bytes_eq(tag, ct + n, 16)) return -1;
    gcm_ctr(&a, nonce, 2, ct, n, out);
    return 0;
}

/* ================================================================ RNG */
#define CRYPTO_URANDOM_PATH "$/dev/md64api/urandom"

/* Fails closed: any open error or short read is an error, never a fallback. */
int random_bytes(void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    int fd = open(CRYPTO_URANDOM_PATH, O_RDONLY, 0);
    if (fd < 0) return -1;
    while (len) {
        ssize_t r = read(fd, p, len);
        if (r <= 0) { close(fd); return -1; }
        p += (size_t)r;
        len -= (size_t)r;
    }
    close(fd);
    return 0;
}

/* ================================================================ x25519 (TweetNaCl-style) */
typedef int64_t gf[16];
static const gf x_121665 = {0xDB41, 1};

static void x_car(gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * 65536;
    }
}
static void x_sel(gf p, gf q, int b) {
    int64_t c = ~(int64_t)(b - 1);
    for (int i = 0; i < 16; i++) { int64_t t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}
static void x_pack(uint8_t *o, const gf n) {
    gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    x_car(t); x_car(t); x_car(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        x_sel(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) { o[2 * i] = (uint8_t)(t[i] & 0xff); o[2 * i + 1] = (uint8_t)(t[i] >> 8); }
}
static void x_unpack(gf o, const uint8_t *n) {
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}
static void x_add(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void x_sub(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
static void x_mul(gf o, const gf a, const gf b) {
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    x_car(o); x_car(o);
}
static void x_sqr(gf o, const gf a) { x_mul(o, a, a); }
static void x_inv(gf o, const gf in) {
    gf c;
    for (int a = 0; a < 16; a++) c[a] = in[a];
    for (int a = 253; a >= 0; a--) {
        x_sqr(c, c);
        if (a != 2 && a != 4) x_mul(c, c, in);
    }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

static void x25519_scalarmult(uint8_t *q, const uint8_t *n, const uint8_t *p) {
    uint8_t z[32];
    int64_t x[80];
    gf a, b, c, d, e, f;
    for (int i = 0; i < 31; i++) z[i] = n[i];
    z[31] = (n[31] & 127) | 64;
    z[0] &= 248;
    x_unpack(x, p);
    for (int i = 0; i < 16; i++) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        x_sel(a, b, r); x_sel(c, d, r);
        x_add(e, a, c); x_sub(a, a, c); x_add(c, b, d); x_sub(b, b, d);
        x_sqr(d, e); x_sqr(f, a); x_mul(a, c, a); x_mul(c, b, e);
        x_add(e, a, c); x_sub(a, a, c); x_sqr(b, a); x_sub(c, d, f);
        x_mul(a, c, x_121665); x_add(a, a, d); x_mul(c, c, a); x_mul(a, d, f);
        x_mul(d, b, x); x_sqr(b, e);
        x_sel(a, b, r); x_sel(c, d, r);
    }
    for (int i = 0; i < 16; i++) { x[i + 16] = a[i]; x[i + 32] = c[i]; x[i + 48] = b[i]; x[i + 64] = d[i]; }
    x_inv(x + 32, x + 32);
    x_mul(x + 16, x + 16, x + 32);
    x_pack(q, x + 16);
}

int x25519_keypair(uint8_t priv[32], uint8_t pub[32]) {
    static const uint8_t base[32] = {9};
    if (random_bytes(priv, 32) != 0) return -1;
    x25519_scalarmult(pub, priv, base);
    return 0;
}

int x25519_shared(const uint8_t priv[32], const uint8_t peer[32], uint8_t out[32]) {
    uint8_t acc = 0;
    x25519_scalarmult(out, priv, peer);
    for (int i = 0; i < 32; i++) acc |= out[i];
    return acc ? 0 : -1;
}

/* ================================================================ bignum + Montgomery field */
#define BN_MAX 128                       /* 4096 bits */

typedef struct {
    int      n;
    uint32_t m[BN_MAX];                  /* modulus (odd) */
    uint32_t r2[BN_MAX];                 /* R^2 mod m, R = 2^(32n) */
    uint32_t one[BN_MAX];                /* R mod m = 1 in Montgomery form */
    uint32_t minv;                       /* -m^-1 mod 2^32 */
} fld_t;

static int bn_cmp(const uint32_t *a, const uint32_t *b, int n) {
    for (int i = n - 1; i >= 0; i--) {
        if (a[i] > b[i]) return 1;
        if (a[i] < b[i]) return -1;
    }
    return 0;
}
static uint32_t bn_sub(uint32_t *r, const uint32_t *a, const uint32_t *b, int n) {
    uint64_t borrow = 0;
    for (int i = 0; i < n; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        r[i] = (uint32_t)d;
        borrow = (d >> 63) & 1;
    }
    return (uint32_t)borrow;
}
static uint32_t bn_add(uint32_t *r, const uint32_t *a, const uint32_t *b, int n) {
    uint64_t carry = 0;
    for (int i = 0; i < n; i++) {
        uint64_t s = (uint64_t)a[i] + b[i] + carry;
        r[i] = (uint32_t)s;
        carry = s >> 32;
    }
    return (uint32_t)carry;
}
static int bn_is_zero(const uint32_t *a, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v |= a[i];
    return v == 0;
}
static void bn_from_be(uint32_t *r, int n, const uint8_t *b, size_t len) {
    memset(r, 0, (size_t)n * 4);
    for (size_t i = 0; i < len; i++) {
        size_t pos = len - 1 - i;                        /* byte weight 256^i */
        if (i / 4 < (size_t)n) r[i / 4] |= (uint32_t)b[pos] << (8 * (i % 4));
    }
}
static void bn_to_be(uint8_t *out, size_t len, const uint32_t *a, int n) {
    for (size_t i = 0; i < len; i++)
        out[len - 1 - i] = (i / 4 < (size_t)n) ? (uint8_t)(a[i / 4] >> (8 * (i % 4))) : 0;
}

static void fld_dbl_mod(uint32_t *x, const uint32_t *m, int n) {
    uint32_t carry = 0;
    for (int i = 0; i < n; i++) {
        uint32_t nc = x[i] >> 31;
        x[i] = (x[i] << 1) | carry;
        carry = nc;
    }
    if (carry || bn_cmp(x, m, n) >= 0) bn_sub(x, x, m, n);
}

static void fld_init(fld_t *f, const uint32_t *m, int n) {
    f->n = n;
    memcpy(f->m, m, (size_t)n * 4);
    uint32_t inv = m[0];
    for (int i = 0; i < 5; i++) inv *= 2 - m[0] * inv;
    f->minv = (uint32_t)0 - inv;
    memset(f->one, 0, sizeof(f->one));
    f->one[0] = 1;
    for (int i = 0; i < 32 * n; i++) fld_dbl_mod(f->one, f->m, n);        /* R mod m */
    memcpy(f->r2, f->one, (size_t)n * 4);
    for (int i = 0; i < 32 * n; i++) fld_dbl_mod(f->r2, f->m, n);         /* R^2 mod m */
}

/* Montgomery product: r = a*b/R mod m (CIOS). r may alias a or b. */
static void fld_mul(const fld_t *f, uint32_t *r, const uint32_t *a, const uint32_t *b) {
    int n = f->n;
    uint32_t t[BN_MAX + 2];
    memset(t, 0, (size_t)(n + 2) * 4);
    for (int i = 0; i < n; i++) {
        uint64_t c = 0, s;
        for (int j = 0; j < n; j++) {
            s = (uint64_t)t[j] + (uint64_t)a[j] * b[i] + c;
            t[j] = (uint32_t)s;
            c = s >> 32;
        }
        s = (uint64_t)t[n] + c;
        t[n] = (uint32_t)s;
        t[n + 1] = (uint32_t)(s >> 32);

        uint32_t mq = t[0] * f->minv;
        s = (uint64_t)t[0] + (uint64_t)mq * f->m[0];
        c = s >> 32;
        for (int j = 1; j < n; j++) {
            s = (uint64_t)t[j] + (uint64_t)mq * f->m[j] + c;
            t[j - 1] = (uint32_t)s;
            c = s >> 32;
        }
        s = (uint64_t)t[n] + c;
        t[n - 1] = (uint32_t)s;
        t[n] = t[n + 1] + (uint32_t)(s >> 32);
    }
    if (t[n] || bn_cmp(t, f->m, n) >= 0) bn_sub(t, t, f->m, n);
    memcpy(r, t, (size_t)n * 4);
}

static void fld_add(const fld_t *f, uint32_t *r, const uint32_t *a, const uint32_t *b) {
    uint32_t carry = bn_add(r, a, b, f->n);
    if (carry || bn_cmp(r, f->m, f->n) >= 0) bn_sub(r, r, f->m, f->n);
}
static void fld_sub(const fld_t *f, uint32_t *r, const uint32_t *a, const uint32_t *b) {
    if (bn_sub(r, a, b, f->n)) bn_add(r, r, f->m, f->n);
}
static void fld_to_mont(const fld_t *f, uint32_t *r, const uint32_t *a) { fld_mul(f, r, a, f->r2); }
static void fld_from_mont(const fld_t *f, uint32_t *r, const uint32_t *a) {
    uint32_t one[BN_MAX];
    memset(one, 0, (size_t)f->n * 4);
    one[0] = 1;
    fld_mul(f, r, a, one);
}
/* r = a^e (a in Montgomery form, e little-endian limbs) */
static void fld_pow(const fld_t *f, uint32_t *r, const uint32_t *a, const uint32_t *e, int en) {
    uint32_t acc[BN_MAX], base[BN_MAX];
    memcpy(acc, f->one, (size_t)f->n * 4);
    memcpy(base, a, (size_t)f->n * 4);
    for (int i = en * 32 - 1; i >= 0; i--) {
        fld_mul(f, acc, acc, acc);
        if ((e[i >> 5] >> (i & 31)) & 1) fld_mul(f, acc, acc, base);
    }
    memcpy(r, acc, (size_t)f->n * 4);
}
static void fld_inv(const fld_t *f, uint32_t *r, const uint32_t *a) {      /* m prime */
    uint32_t e[BN_MAX], two[BN_MAX];
    memset(two, 0, (size_t)f->n * 4);
    two[0] = 2;
    bn_sub(e, f->m, two, f->n);
    fld_pow(f, r, a, e, f->n);
}

/* ================================================================ P-256 */
static const uint8_t P256_P[32]  = {0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                                    0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
static const uint8_t P256_N[32]  = {0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
                                    0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51};
static const uint8_t P256_B[32]  = {0x5a,0xc6,0x35,0xd8,0xaa,0x3a,0x93,0xe7,0xb3,0xeb,0xbd,0x55,0x76,0x98,0x86,0xbc,
                                    0x65,0x1d,0x06,0xb0,0xcc,0x53,0xb0,0xf6,0x3b,0xce,0x3c,0x3e,0x27,0xd2,0x60,0x4b};
static const uint8_t P256_GX[32] = {0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,0xf8,0xbc,0xe6,0xe5,0x63,0xa4,0x40,0xf2,
                                    0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,0xf4,0xa1,0x39,0x45,0xd8,0x98,0xc2,0x96};
static const uint8_t P256_GY[32] = {0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,0x8e,0xe7,0xeb,0x4a,0x7c,0x0f,0x9e,0x16,
                                    0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,0xcb,0xb6,0x40,0x68,0x37,0xbf,0x51,0xf5};

typedef struct { uint32_t x[8], y[8], z[8]; } pt_t;       /* Jacobian, Montgomery form; z==0 is infinity */

static fld_t    g_fp, g_fn;                               /* field mod p, field mod group order n */
static uint32_t g_b_m[8], g_gx_m[8], g_gy_m[8];
static int      g_ecc_ready;

static void ecc_init(void) {
    if (g_ecc_ready) return;
    uint32_t t[8];
    bn_from_be(t, 8, P256_P, 32); fld_init(&g_fp, t, 8);
    bn_from_be(t, 8, P256_N, 32); fld_init(&g_fn, t, 8);
    bn_from_be(t, 8, P256_B, 32);  fld_to_mont(&g_fp, g_b_m, t);
    bn_from_be(t, 8, P256_GX, 32); fld_to_mont(&g_fp, g_gx_m, t);
    bn_from_be(t, 8, P256_GY, 32); fld_to_mont(&g_fp, g_gy_m, t);
    g_ecc_ready = 1;
}

#define FM(r, a, b) fld_mul(&g_fp, r, a, b)
#define FA(r, a, b) fld_add(&g_fp, r, a, b)
#define FS(r, a, b) fld_sub(&g_fp, r, a, b)

static void pt_set_inf(pt_t *p) {
    memcpy(p->x, g_fp.one, 32);
    memcpy(p->y, g_fp.one, 32);
    memset(p->z, 0, 32);
}

static void pt_dbl(pt_t *r, const pt_t *p) {
    if (bn_is_zero(p->z, 8)) { *r = *p; return; }
    uint32_t delta[8], gamma[8], beta[8], alpha[8], t1[8], t2[8], b2[8], b4[8], b8[8];
    uint32_t x3[8], y3[8], z3[8], g2[8], g4[8], g8[8];
    FM(delta, p->z, p->z);
    FM(gamma, p->y, p->y);
    FM(beta, p->x, gamma);
    FS(t1, p->x, delta);
    FA(t2, p->x, delta);
    FM(alpha, t1, t2);
    FA(t1, alpha, alpha);
    FA(alpha, t1, alpha);                                 /* alpha = 3(x-delta)(x+delta) */
    FA(b2, beta, beta); FA(b4, b2, b2); FA(b8, b4, b4);
    FM(t1, alpha, alpha);
    FS(x3, t1, b8);                                       /* x3 = alpha^2 - 8 beta */
    FA(t1, p->y, p->z);
    FM(t2, t1, t1);
    FS(t2, t2, gamma);
    FS(z3, t2, delta);                                    /* z3 = (y+z)^2 - gamma - delta */
    FS(t1, b4, x3);
    FM(t2, alpha, t1);
    FM(g2, gamma, gamma);
    FA(g4, g2, g2); FA(g4, g4, g4); FA(g8, g4, g4);
    FS(y3, t2, g8);                                       /* y3 = alpha(4beta - x3) - 8 gamma^2 */
    memcpy(r->x, x3, 32); memcpy(r->y, y3, 32); memcpy(r->z, z3, 32);
}

static void pt_add(pt_t *r, const pt_t *p, const pt_t *q) {
    if (bn_is_zero(p->z, 8)) { *r = *q; return; }
    if (bn_is_zero(q->z, 8)) { *r = *p; return; }
    uint32_t z1z1[8], z2z2[8], u1[8], u2[8], s1[8], s2[8], h[8], rr[8], i[8], j[8], v[8], t[8], t2[8];
    uint32_t x3[8], y3[8], z3[8];
    FM(z1z1, p->z, p->z);
    FM(z2z2, q->z, q->z);
    FM(u1, p->x, z2z2);
    FM(u2, q->x, z1z1);
    FM(t, q->z, z2z2); FM(s1, p->y, t);
    FM(t, p->z, z1z1); FM(s2, q->y, t);
    FS(h, u2, u1);
    FS(rr, s2, s1);
    if (bn_is_zero(h, 8)) {
        if (bn_is_zero(rr, 8)) { pt_dbl(r, p); return; }
        pt_set_inf(r);
        return;
    }
    FA(rr, rr, rr);
    FA(t, h, h); FM(i, t, t);
    FM(j, h, i);
    FM(v, u1, i);
    FM(t, rr, rr); FS(t, t, j); FA(t2, v, v); FS(x3, t, t2);
    FS(t, v, x3); FM(t, rr, t);
    FM(t2, s1, j); FA(t2, t2, t2); FS(y3, t, t2);
    FA(t, p->z, q->z); FM(t, t, t); FS(t, t, z1z1); FS(t, t, z2z2); FM(z3, t, h);
    memcpy(r->x, x3, 32); memcpy(r->y, y3, 32); memcpy(r->z, z3, 32);
}

/* r = k * (px, py); k plain little-endian limbs, px/py Montgomery affine */
static void ec_mul(pt_t *r, const uint32_t k[8], const uint32_t *px, const uint32_t *py) {
    pt_t acc, base;
    memcpy(base.x, px, 32); memcpy(base.y, py, 32); memcpy(base.z, g_fp.one, 32);
    pt_set_inf(&acc);
    for (int i = 255; i >= 0; i--) {
        pt_dbl(&acc, &acc);
        if ((k[i >> 5] >> (i & 31)) & 1) pt_add(&acc, &acc, &base);
    }
    *r = acc;
}

/* Jacobian -> affine, plain (non-Montgomery) limbs. Fails on infinity. */
static int pt_affine(const pt_t *p, uint32_t x[8], uint32_t y[8]) {
    if (bn_is_zero(p->z, 8)) return -1;
    uint32_t zi[8], zi2[8], zi3[8], t[8];
    fld_inv(&g_fp, zi, p->z);
    FM(zi2, zi, zi);
    FM(zi3, zi2, zi);
    FM(t, p->x, zi2); fld_from_mont(&g_fp, x, t);
    FM(t, p->y, zi3); fld_from_mont(&g_fp, y, t);
    return 0;
}

/* Validate and load an uncompressed point into Montgomery coordinates. */
static int pt_load(const uint8_t *b, uint32_t xm[8], uint32_t ym[8]) {
    uint32_t x[8], y[8], l[8], r[8], t[8];
    if (b[0] != 4) return -1;
    bn_from_be(x, 8, b + 1, 32);
    bn_from_be(y, 8, b + 33, 32);
    if (bn_cmp(x, g_fp.m, 8) >= 0 || bn_cmp(y, g_fp.m, 8) >= 0) return -1;
    fld_to_mont(&g_fp, xm, x);
    fld_to_mont(&g_fp, ym, y);
    FM(l, ym, ym);                                        /* y^2 */
    FM(t, xm, xm); FM(r, t, xm);                          /* x^3 */
    FA(t, xm, xm); FA(t, t, xm);                          /* 3x */
    FS(r, r, t);
    FA(r, r, g_b_m);                                      /* x^3 - 3x + b */
    return bn_cmp(l, r, 8) == 0 ? 0 : -1;
}

int p256_keypair(uint8_t priv[32], uint8_t pub[65]) {
    ecc_init();
    uint32_t k[8], x[8], y[8];
    pt_t p;
    for (int tries = 0; tries < 100; tries++) {
        if (random_bytes(priv, 32) != 0) return -1;
        bn_from_be(k, 8, priv, 32);
        if (bn_is_zero(k, 8) || bn_cmp(k, g_fn.m, 8) >= 0) continue;
        ec_mul(&p, k, g_gx_m, g_gy_m);
        if (pt_affine(&p, x, y) != 0) continue;
        pub[0] = 4;
        bn_to_be(pub + 1, 32, x, 8);
        bn_to_be(pub + 33, 32, y, 8);
        return 0;
    }
    return -1;
}

int p256_shared(const uint8_t priv[32], const uint8_t peer[65], uint8_t out[32]) {
    ecc_init();
    uint32_t k[8], xm[8], ym[8], x[8], y[8];
    pt_t p;
    if (pt_load(peer, xm, ym) != 0) return -1;
    bn_from_be(k, 8, priv, 32);
    if (bn_is_zero(k, 8) || bn_cmp(k, g_fn.m, 8) >= 0) return -1;
    ec_mul(&p, k, xm, ym);
    if (pt_affine(&p, x, y) != 0) return -1;
    bn_to_be(out, 32, x, 8);
    return 0;
}

/* ================================================================ DER / X.509 */
static int der_read(const uint8_t **pp, const uint8_t *end, uint8_t *tag, const uint8_t **val, size_t *len) {
    const uint8_t *p = *pp;
    if (end - p < 2) return -1;
    *tag = p[0];
    size_t l = p[1];
    p += 2;
    if (l & 0x80) {
        int nb = (int)(l & 0x7f);
        if (nb == 0 || nb > 3 || end - p < nb) return -1;
        l = 0;
        for (int i = 0; i < nb; i++) l = (l << 8) | *p++;
    }
    if ((size_t)(end - p) < l) return -1;
    *val = p;
    *len = l;
    *pp = p + l;
    return 0;
}

static int der_expect(const uint8_t **pp, const uint8_t *end, uint8_t want, const uint8_t **val, size_t *len) {
    uint8_t tag;
    if (der_read(pp, end, &tag, val, len) != 0 || tag != want) return -1;
    return 0;
}

static void strip_zeros(const uint8_t **p, size_t *len) {
    while (*len > 1 && **p == 0) { (*p)++; (*len)--; }
}

static int parse_spki(mcert_t *c, const uint8_t *v, size_t l) {
    static const uint8_t oid_rsa[9]  = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01};
    static const uint8_t oid_ec[7]   = {0x2a,0x86,0x48,0xce,0x3d,0x02,0x01};
    static const uint8_t oid_p256[8] = {0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07};
    const uint8_t *p = v, *end = v + l, *alg, *key, *oid, *par;
    size_t alen, klen, olen, plen;
    if (der_expect(&p, end, 0x30, &alg, &alen) != 0) return -1;
    if (der_expect(&p, end, 0x03, &key, &klen) != 0 || klen < 1 || key[0] != 0) return -1;
    key++; klen--;

    const uint8_t *ap = alg, *aend = alg + alen;
    if (der_expect(&ap, aend, 0x06, &oid, &olen) != 0) return -1;

    if (olen == 9 && bytes_eq(oid, oid_rsa, 9)) {
        const uint8_t *kp = key, *kend = key + klen, *seq, *n, *e;
        size_t slen, nlen, elen;
        if (der_expect(&kp, kend, 0x30, &seq, &slen) != 0) return -1;
        const uint8_t *sp = seq, *send = seq + slen;
        if (der_expect(&sp, send, 0x02, &n, &nlen) != 0) return -1;
        if (der_expect(&sp, send, 0x02, &e, &elen) != 0) return -1;
        strip_zeros(&n, &nlen);
        strip_zeros(&e, &elen);
        c->key_type = MC_KEY_RSA;
        c->rsa_n = n; c->rsa_n_len = nlen;
        c->rsa_e = e; c->rsa_e_len = elen;
    } else if (olen == 7 && bytes_eq(oid, oid_ec, 7)) {
        if (der_expect(&ap, aend, 0x06, &par, &plen) == 0 && plen == 8 && bytes_eq(par, oid_p256, 8) &&
            klen == 65 && key[0] == 4) {
            c->key_type = MC_KEY_EC_P256;
            c->ec_point = key;
        }
    }
    return 0;
}

int mcert_parse(mcert_t *c, const uint8_t *der, size_t der_len) {
    memset(c, 0, sizeof(*c));
    c->der = der;
    c->der_len = der_len;
    const uint8_t *p = der, *end = der + der_len, *v;
    size_t l;
    uint8_t tag;

    if (der_expect(&p, end, 0x30, &v, &l) != 0) return -1;          /* Certificate */
    p = v; end = v + l;
    if (der_expect(&p, end, 0x30, &v, &l) != 0) return -1;          /* TBSCertificate */
    p = v; end = v + l;

    if (p < end && *p == 0xa0 && der_read(&p, end, &tag, &v, &l) != 0) return -1;   /* [0] version */
    if (der_expect(&p, end, 0x02, &v, &l) != 0) return -1;          /* serial */
    if (der_expect(&p, end, 0x30, &v, &l) != 0) return -1;          /* signature alg */
    if (der_expect(&p, end, 0x30, &v, &l) != 0) return -1;          /* issuer */
    if (der_expect(&p, end, 0x30, &v, &l) != 0) return -1;          /* validity */
    if (der_expect(&p, end, 0x30, &v, &l) != 0) return -1;          /* subject */
    c->subject = v; c->subject_len = l;
    if (der_expect(&p, end, 0x30, &v, &l) != 0) return -1;          /* SubjectPublicKeyInfo */
    if (parse_spki(c, v, l) != 0) return -1;

    while (p < end) {                                               /* [1] [2] [3] */
        if (der_read(&p, end, &tag, &v, &l) != 0) return -1;
        if (tag == 0xa3) {
            const uint8_t *ip = v, *iend = v + l, *seq;
            size_t slen;
            if (der_expect(&ip, iend, 0x30, &seq, &slen) != 0) return -1;
            c->exts = seq; c->exts_len = slen;
        }
    }
    return 0;
}

static char lc(char ch) { return (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch; }

static int name_matches(const uint8_t *name, size_t nlen, const char *host) {
    size_t hlen = 0;
    while (host[hlen]) hlen++;
    if (nlen >= 2 && name[0] == '*' && name[1] == '.') {
        size_t dot = 0;
        while (dot < hlen && host[dot] != '.') dot++;
        if (dot == 0 || dot >= hlen) return 0;                      /* needs a non-empty first label */
        const char *rest = host + dot + 1;
        size_t rlen = hlen - dot - 1;
        if (rlen != nlen - 2) return 0;
        for (size_t i = 0; i < rlen; i++) if (lc(rest[i]) != lc((char)name[2 + i])) return 0;
        return 1;
    }
    if (nlen != hlen) return 0;
    for (size_t i = 0; i < nlen; i++) if (lc((char)name[i]) != lc(host[i])) return 0;
    return 1;
}

int mcert_match_host(const mcert_t *c, const char *host) {
    static const uint8_t oid_san[3] = {0x55, 0x1d, 0x11};
    static const uint8_t oid_cn[3]  = {0x55, 0x04, 0x03};
    int have_san = 0;

    const uint8_t *p = c->exts, *end = c->exts + c->exts_len;
    while (c->exts && p < end) {
        const uint8_t *ext, *oid, *val;
        size_t elen, olen, vlen;
        uint8_t tag;
        if (der_expect(&p, end, 0x30, &ext, &elen) != 0) break;
        const uint8_t *ep = ext, *eend = ext + elen;
        if (der_expect(&ep, eend, 0x06, &oid, &olen) != 0) continue;
        if (olen != 3 || !bytes_eq(oid, oid_san, 3)) continue;
        if (der_read(&ep, eend, &tag, &val, &vlen) != 0) continue;
        if (tag == 0x01 && der_read(&ep, eend, &tag, &val, &vlen) != 0) continue;   /* skip critical flag */
        if (tag != 0x04) continue;
        const uint8_t *sp = val, *send = val + vlen, *names;
        size_t nlen;
        if (der_expect(&sp, send, 0x30, &names, &nlen) != 0) continue;
        have_san = 1;
        const uint8_t *np = names, *nend = names + nlen, *nv;
        size_t nvl;
        while (np < nend && der_read(&np, nend, &tag, &nv, &nvl) == 0)
            if (tag == 0x82 && name_matches(nv, nvl, host)) return 1;
    }
    if (have_san) return 0;

    /* no SAN extension: subject CN (legacy) */
    p = c->subject; end = c->subject + c->subject_len;
    while (p && p < end) {
        const uint8_t *set, *seq, *oid, *val;
        size_t slen, qlen, olen, vlen;
        uint8_t tag;
        if (der_expect(&p, end, 0x31, &set, &slen) != 0) break;
        const uint8_t *sp = set, *send = set + slen;
        if (der_expect(&sp, send, 0x30, &seq, &qlen) != 0) continue;
        const uint8_t *qp = seq, *qend = seq + qlen;
        if (der_expect(&qp, qend, 0x06, &oid, &olen) != 0) continue;
        if (olen == 3 && bytes_eq(oid, oid_cn, 3) && der_read(&qp, qend, &tag, &val, &vlen) == 0 &&
            name_matches(val, vlen, host)) return 1;
    }
    return 0;
}

/* ================================================================ signatures */
static int ecdsa_p256_sha256_verify(const mcert_t *cert, const uint8_t *msg, size_t msg_len,
                                    const uint8_t *sig, size_t sig_len) {
    if (cert->key_type != MC_KEY_EC_P256) return -1;
    ecc_init();

    const uint8_t *p = sig, *end = sig + sig_len, *seq, *rb, *sb;
    size_t slen, rl, sl;
    if (der_expect(&p, end, 0x30, &seq, &slen) != 0) return -1;
    const uint8_t *sp = seq, *send = seq + slen;
    if (der_expect(&sp, send, 0x02, &rb, &rl) != 0) return -1;
    if (der_expect(&sp, send, 0x02, &sb, &sl) != 0) return -1;
    strip_zeros(&rb, &rl); strip_zeros(&sb, &sl);
    if (rl > 32 || sl > 32) return -1;

    uint32_t r[8], s[8], z[8], qx[8], qy[8];
    bn_from_be(r, 8, rb, rl);
    bn_from_be(s, 8, sb, sl);
    if (bn_is_zero(r, 8) || bn_is_zero(s, 8) || bn_cmp(r, g_fn.m, 8) >= 0 || bn_cmp(s, g_fn.m, 8) >= 0) return -1;
    if (pt_load(cert->ec_point, qx, qy) != 0) return -1;

    uint8_t h[32];
    sha256(msg, msg_len, h);
    bn_from_be(z, 8, h, 32);
    if (bn_cmp(z, g_fn.m, 8) >= 0) bn_sub(z, z, g_fn.m, 8);

    uint32_t sm[8], w[8], zm[8], rm[8], u1m[8], u2m[8], u1[8], u2[8];
    fld_to_mont(&g_fn, sm, s);
    fld_inv(&g_fn, w, sm);
    fld_to_mont(&g_fn, zm, z);
    fld_to_mont(&g_fn, rm, r);
    fld_mul(&g_fn, u1m, zm, w);
    fld_mul(&g_fn, u2m, rm, w);
    fld_from_mont(&g_fn, u1, u1m);
    fld_from_mont(&g_fn, u2, u2m);

    pt_t a, b;
    ec_mul(&a, u1, g_gx_m, g_gy_m);
    ec_mul(&b, u2, qx, qy);
    pt_add(&a, &a, &b);
    uint32_t x[8], y[8];
    if (pt_affine(&a, x, y) != 0) return -1;
    if (bn_cmp(x, g_fn.m, 8) >= 0) bn_sub(x, x, g_fn.m, 8);
    return bn_cmp(x, r, 8) == 0 ? 0 : -1;
}

static fld_t g_frsa;

/* em = sig^e mod n, written as k big-endian bytes. */
static int rsa_public(const mcert_t *c, const uint8_t *sig, size_t sig_len, uint8_t *em, size_t *k_out) {
    if (c->key_type != MC_KEY_RSA) return -1;
    size_t k = c->rsa_n_len;
    if (k < 64 || k > 512 || sig_len != k || c->rsa_e_len == 0 || c->rsa_e_len > 4) return -1;
    if (!(c->rsa_n[k - 1] & 1)) return -1;

    int nl = (int)((k + 3) / 4);
    uint32_t mod[BN_MAX], x[BN_MAX], xm[BN_MAX], acc[BN_MAX];
    bn_from_be(mod, nl, c->rsa_n, k);
    bn_from_be(x, nl, sig, k);
    if (bn_cmp(x, mod, nl) >= 0) return -1;

    uint32_t e = 0;
    for (size_t i = 0; i < c->rsa_e_len; i++) e = (e << 8) | c->rsa_e[i];
    if (e < 3 || !(e & 1)) return -1;

    fld_init(&g_frsa, mod, nl);
    fld_to_mont(&g_frsa, xm, x);
    memcpy(acc, xm, (size_t)nl * 4);
    int top = 31;
    while (!((e >> top) & 1)) top--;
    for (int i = top - 1; i >= 0; i--) {
        fld_mul(&g_frsa, acc, acc, acc);
        if ((e >> i) & 1) fld_mul(&g_frsa, acc, acc, xm);
    }
    fld_from_mont(&g_frsa, acc, acc);
    bn_to_be(em, k, acc, nl);
    *k_out = k;
    return 0;
}

static int rsa_pkcs1_sha256_verify(const mcert_t *c, const uint8_t *msg, size_t msg_len,
                                   const uint8_t *sig, size_t sig_len) {
    static const uint8_t prefix[19] = {0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20};
    uint8_t em[512], want[512], h[32];
    size_t k;
    if (rsa_public(c, sig, sig_len, em, &k) != 0) return -1;
    sha256(msg, msg_len, h);
    if (k < 3 + 8 + 19 + 32) return -1;
    size_t ps = k - 3 - 19 - 32;
    want[0] = 0; want[1] = 1;
    memset(want + 2, 0xff, ps);
    want[2 + ps] = 0;
    memcpy(want + 3 + ps, prefix, 19);
    memcpy(want + 3 + ps + 19, h, 32);
    return bytes_eq(em, want, k) ? 0 : -1;
}

static void mgf1_sha256(const uint8_t *seed, size_t seed_len, uint8_t *out, size_t len) {
    uint8_t blk[32];
    for (uint32_t ctr = 0; len; ctr++) {
        sha256_ctx c;
        uint8_t cb[4] = {(uint8_t)(ctr >> 24), (uint8_t)(ctr >> 16), (uint8_t)(ctr >> 8), (uint8_t)ctr};
        sha256_init(&c);
        sha256_update(&c, seed, seed_len);
        sha256_update(&c, cb, 4);
        sha256_final(&c, blk);
        size_t n = len < 32 ? len : 32;
        memcpy(out, blk, n);
        out += n; len -= n;
    }
}

/* RSASSA-PSS, SHA-256, MGF1-SHA-256, salt length 32 (what TLS 1.2 rsa_pss_rsae_sha256 uses) */
static int rsa_pss_sha256_verify(const mcert_t *c, const uint8_t *msg, size_t msg_len,
                                 const uint8_t *sig, size_t sig_len) {
    uint8_t em_full[512], mask[512], mhash[32], mprime[72], hh[32];
    size_t k;
    if (rsa_public(c, sig, sig_len, em_full, &k) != 0) return -1;

    int topbyte_bits = 0;
    for (uint8_t t = c->rsa_n[0]; t; t >>= 1) topbyte_bits++;
    size_t modbits = 8 * (k - 1) + (size_t)topbyte_bits;
    size_t embits = modbits - 1;
    size_t emlen = (embits + 7) / 8;
    if (emlen > k || (k > emlen && em_full[0] != 0)) return -1;
    const uint8_t *em = em_full + (k - emlen);

    const size_t hlen = 32, slen = 32;
    if (emlen < hlen + slen + 2 || em[emlen - 1] != 0xbc) return -1;
    size_t dblen = emlen - hlen - 1;
    const uint8_t *H = em + dblen;
    unsigned topbits = (unsigned)(8 * emlen - embits);
    uint8_t topmask = topbits ? (uint8_t)(0xff << (8 - topbits)) : 0;
    if (em[0] & topmask) return -1;

    mgf1_sha256(H, hlen, mask, dblen);
    uint8_t db[512];
    for (size_t i = 0; i < dblen; i++) db[i] = em[i] ^ mask[i];
    db[0] &= (uint8_t)~topmask;
    size_t padlen = dblen - slen - 1;
    for (size_t i = 0; i < padlen; i++) if (db[i] != 0) return -1;
    if (db[padlen] != 1) return -1;

    sha256(msg, msg_len, mhash);
    memset(mprime, 0, 8);
    memcpy(mprime + 8, mhash, 32);
    memcpy(mprime + 40, db + dblen - slen, slen);
    sha256(mprime, 72, hh);
    return bytes_eq(hh, H, hlen) ? 0 : -1;
}

int verify_signature(const mcert_t *leaf, uint16_t scheme, const uint8_t *msg, size_t msg_len,
                     const uint8_t *sig, size_t sig_len) {
    switch (scheme) {
    case 0x0403: return ecdsa_p256_sha256_verify(leaf, msg, msg_len, sig, sig_len);
    case 0x0804: return rsa_pss_sha256_verify(leaf, msg, msg_len, sig, sig_len);
    case 0x0401: return rsa_pkcs1_sha256_verify(leaf, msg, msg_len, sig, sig_len);
    default:     return -1;
    }
}