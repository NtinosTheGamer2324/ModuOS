// tls.c -- minimal TLS 1.2 client for ModuOS (C port of mytls.py)
// Protocol logic only: records, handshake messages, PRF, key schedule, and the
// certificate-chain check against a PEM trust store. All primitives come from
// crypto.h.
#include "tls.h"

enum { CT_CCS = 20, CT_ALERT = 21, CT_HANDSHAKE = 22, CT_APPDATA = 23 };
enum { HS_CLIENT_HELLO = 1, HS_SERVER_HELLO = 2, HS_CERTIFICATE = 11,
       HS_SERVER_KEY_EXCHANGE = 12, HS_CERT_REQUEST = 13,
       HS_SERVER_HELLO_DONE = 14, HS_CLIENT_KEY_EXCHANGE = 16, HS_FINISHED = 20 };
enum { GROUP_X25519 = 0x001d, GROUP_SECP256R1 = 0x0017 };

/* ------------------------------------------------------------ helpers */
static void     put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void     put24(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 16); put16(p + 1, v); }
static unsigned get16(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }
static unsigned get24(const uint8_t *p) { return ((unsigned)p[0] << 16) | get16(p + 1); }
static void     put64(uint8_t *p, uint64_t v) {
    for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; }
}

static int ct_equal(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

static const char *alert_name(unsigned c) {
    switch (c) {
    case 0:   return "close_notify";
    case 10:  return "unexpected_message";
    case 20:  return "bad_record_mac";
    case 40:  return "handshake_failure";
    case 42:  return "bad_certificate";
    case 47:  return "illegal_parameter";
    case 48:  return "unknown_ca";
    case 50:  return "decode_error";
    case 51:  return "decrypt_error";
    case 70:  return "protocol_version";
    case 71:  return "insufficient_security";
    case 80:  return "internal_error";
    case 86:  return "inappropriate_fallback";
    case 112: return "unrecognized_name";
    default:  return "unknown";
    }
}

static int fail(tls_t *t, const char *msg) {
    snprintf(t->errmsg, sizeof(t->errmsg), "%s", msg);
    return -1;
}

static int fail_alert(tls_t *t, unsigned code) {
    snprintf(t->errmsg, sizeof(t->errmsg), "server alert: %s (%u)", alert_name(code), code);
    return -1;
}

/* sockets.h keeps its state in per-translation-unit statics, so errno and
 * sock_last_message() are only meaningful inside this file, right after the
 * failing call. Capture them here; callers must not read their own copies. */
static void note_io_error(tls_t *t) {
    t->io_errno = errno;
    snprintf(t->io_msg, sizeof(t->io_msg), "%s", sock_last_message());
}

static void note_io_text(tls_t *t, const char *text) {
    t->io_errno = 0;
    snprintf(t->io_msg, sizeof(t->io_msg), "%s", text);
}

static int fail_io(tls_t *t, const char *what) {
    snprintf(t->errmsg, sizeof(t->errmsg), "%s: errno=%d (%s)", what, t->io_errno, t->io_msg);
    return -1;
}

const char *tls_error(const tls_t *t) { return t->errmsg; }

int tls_clean_close(const tls_t *t) { return t->clean_close; }
int tls_chain_verified(const tls_t *t) { return t->chain_verified; }
int tls_dates_checked(const tls_t *t) { return t->dates_checked; }

/* Configuration. Lives in this translation unit, so set it through these calls. */
static const char *g_ca_path = TLS_CA_PATH;
static int64_t     g_now;                                /* unix seconds, 0 = unknown */

void tls_set_ca_path(const char *path) { g_ca_path = (path && *path) ? path : TLS_CA_PATH; }
void tls_set_time(int64_t unix_seconds) { g_now = unix_seconds; }

/* An unset RTC tends to read as 1970 or the build date; do not trust anything before 2024. */
static int64_t clock_now(void) { return g_now >= 1704067200LL ? g_now : 0; }

const char *tls_cipher_name(const tls_t *t) {
    switch (t->cipher_suite) {
    case 0xc02b: return "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256";
    case 0xc02f: return "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256";
    default:     return "none";
    }
}

/* TLS 1.2 PRF (HMAC-SHA256). label+seed must fit in 160 bytes. */
static void prf(const uint8_t *secret, size_t slen, const char *label,
                const uint8_t *seed, size_t seed_len, uint8_t *out, size_t olen) {
    uint8_t ls[160];
    size_t  ll = strlen(label);
    memcpy(ls, label, ll);
    memcpy(ls + ll, seed, seed_len);
    size_t n = ll + seed_len;

    uint8_t a[32], blk[32 + sizeof(ls)], tmp[32];
    hmac_sha256(secret, slen, ls, n, a);                 /* A(1) */
    size_t done = 0;
    while (done < olen) {
        memcpy(blk, a, 32);
        memcpy(blk + 32, ls, n);
        hmac_sha256(secret, slen, blk, 32 + n, tmp);     /* HMAC(secret, A(i) + seed) */
        size_t take = (olen - done < 32) ? olen - done : 32;
        memcpy(out + done, tmp, take);
        done += take;
        memcpy(blk, a, 32);
        hmac_sha256(secret, slen, blk, 32, a);           /* A(i+1) */
    }
}

static int looks_like_ip(const char *h) {
    for (; *h; h++)
        if (!((*h >= '0' && *h <= '9') || *h == '.' || *h == ':')) return 0;
    return 1;
}

/* ------------------------------------------------------------ record layer */
/* 0 ok, -1 error/timeout, -2 clean EOF before the first byte. */
static int read_exact(tls_t *t, uint8_t *p, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = sock_recv(t->fd, p + got, n - got);
        if (r == 0) {
            note_io_text(t, got == 0 ? "connection closed by server" : "connection closed mid-record");
            return got == 0 ? -2 : -1;
        }
        if (r < 0) { note_io_error(t); return -1; }
        got += (size_t)r;
    }
    return 0;
}

/* Reads one record body into t->rec. */
static int read_record(tls_t *t, uint8_t *type, size_t *len) {
    uint8_t hdr[5];
    int r = read_exact(t, hdr, 5);
    if (r != 0) return r;
    size_t n = get16(hdr + 3);
    if (hdr[1] != 3 || n > TLS_REC_MAX) { fail(t, "malformed TLS record header"); return -1; }
    if (n) {
        r = read_exact(t, t->rec, n);
        if (r == -2) r = -1;
        if (r != 0) return r;
    }
    *type = hdr[0];
    *len = n;
    return 0;
}

/* AES-128-GCM record protection (RFC 5288): writes a full record into dst. */
static size_t seal_record(tls_t *t, uint8_t *dst, uint8_t type, const uint8_t *pt, size_t n) {
    uint8_t nonce[12], aad[13];
    memcpy(nonce, t->civ, 4);
    put64(nonce + 4, t->cseq);
    put64(aad, t->cseq);
    aad[8] = type; aad[9] = 3; aad[10] = 3;
    put16(aad + 11, (unsigned)n);

    dst[0] = type; dst[1] = 3; dst[2] = 3;
    put16(dst + 3, (unsigned)(8 + n + 16));
    memcpy(dst + 5, nonce + 4, 8);                       /* explicit nonce */
    aes128gcm_seal(t->ckey, nonce, aad, sizeof(aad), pt, n, dst + 13);
    t->cseq++;
    return 5 + 8 + n + 16;
}

/* Decrypts t->rec (n bytes) into `out`; returns plaintext length or -1. */
static int open_record(tls_t *t, uint8_t type, size_t n, uint8_t *out) {
    if (n < 8 + 16) return fail(t, "encrypted record too short");
    uint8_t nonce[12], aad[13];
    memcpy(nonce, t->siv, 4);
    memcpy(nonce + 4, t->rec, 8);
    put64(aad, t->sseq);
    aad[8] = type; aad[9] = 3; aad[10] = 3;
    put16(aad + 11, (unsigned)(n - 24));
    if (aes128gcm_open(t->skey, nonce, aad, sizeof(aad), t->rec + 8, n - 8, out) != 0)
        return fail(t, "bad_record_mac: could not decrypt server record");
    t->sseq++;
    return (int)(n - 24);
}

/* Next plaintext handshake message, reassembled across records. The pointer
 * stays valid until the next call. */
static int next_hs_msg(tls_t *t, uint8_t **msg, size_t *len) {
    if (t->hs_off) {                                     /* drop the message returned last time */
        memmove(t->hs_buf, t->hs_buf + t->hs_off, t->hs_len - t->hs_off);
        t->hs_len -= t->hs_off;
        t->hs_off = 0;
    }
    for (;;) {
        if (t->hs_len >= 4) {
            size_t ln = get24(t->hs_buf + 1);
            if (4 + ln > TLS_HS_MAX) return fail(t, "handshake message too large");
            if (t->hs_len >= 4 + ln) {
                *msg = t->hs_buf;
                *len = 4 + ln;
                t->hs_off = 4 + ln;
                return 0;
            }
        }
        uint8_t type; size_t n;
        int r = read_record(t, &type, &n);
        if (r == -2) return fail(t, "connection closed during handshake");
        if (r != 0) { if (!t->errmsg[0]) fail_io(t, "receive error during handshake"); return -1; }
        if (type == CT_ALERT) return fail_alert(t, n >= 2 ? t->rec[1] : 0);
        if (type != CT_HANDSHAKE) return fail(t, "unexpected record type during handshake");
        if (t->hs_len + n > TLS_HS_MAX) return fail(t, "handshake flight too large");
        memcpy(t->hs_buf + t->hs_len, t->rec, n);
        t->hs_len += n;
    }
}


/* ------------------------------------------------------------ trust store + chain validation */
typedef struct {
    int      fd;
    int      err;
    uint8_t *io;  size_t io_cap, io_len, io_pos;
    uint8_t *der; size_t der_cap;
} pem_t;

static int pem_getc(pem_t *r) {
    if (r->io_pos == r->io_len) {
        ssize_t n = read(r->fd, r->io, r->io_cap);
        if (n < 0) r->err = 1;
        if (n <= 0) return -1;
        r->io_len = (size_t)n;
        r->io_pos = 0;
    }
    return r->io[r->io_pos++];
}

static int b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int has_prefix(const char *s, const char *p) {
    while (*p) if (*s++ != *p++) return 0;
    return 1;
}

/* Next certificate from a PEM bundle, DER-decoded into r->der.
 * Returns its length, 0 at end of file, -1 on a read error. Text between the
 * blocks (curl's "Name\n=====" headers, comments) is ignored; certificates that
 * are malformed or larger than the buffer are skipped. */
static long pem_next(pem_t *r) {
    char line[128];
    int in_cert = 0, overflow = 0, bad = 0, bits = 0;
    uint32_t acc = 0;
    size_t dl = 0;
    for (;;) {
        size_t ll = 0;
        int c, got = 0;
        while ((c = pem_getc(r)) >= 0) {
            got = 1;
            if (c == '\n') break;
            if (ll < sizeof(line) - 1) line[ll++] = (char)c;
        }
        if (c < 0 && !got) return r->err ? -1 : 0;
        line[ll] = 0;

        if (has_prefix(line, "-----BEGIN CERTIFICATE-----")) {      /* also restarts a truncated block */
            in_cert = 1; overflow = 0; bad = 0; bits = 0; acc = 0; dl = 0;
        } else if (!in_cert) {
            /* text between blocks */
        } else if (has_prefix(line, "-----END CERTIFICATE-----")) {
            if (!overflow && !bad && dl > 0) return (long)dl;
            in_cert = 0;
        } else if (has_prefix(line, "-----")) {
            in_cert = 0;                                 /* some other block: give up on this one */
        } else {
            for (size_t i = 0; i < ll; i++) {
                int ch = (unsigned char)line[i];
                if (ch == '=') break;
                if (ch == ' ' || ch == '\r' || ch == '\t') continue;
                int v = b64_val(ch);
                if (v < 0) { bad = 1; break; }
                acc = (acc << 6) | (uint32_t)v;
                bits += 6;
                if (bits >= 8) {
                    bits -= 8;
                    if (dl < r->der_cap) r->der[dl++] = (uint8_t)(acc >> bits);
                    else overflow = 1;
                    acc &= (1u << bits) - 1;
                }
            }
        }
        if (c < 0) return r->err ? -1 : 0;
    }
}

enum { PC_OK = 0, PC_EXPIRED, PC_NOT_YET, PC_NOT_CA, PC_KEY_USAGE, PC_PATHLEN, PC_EKU };

static const char *pc_text(int code) {
    switch (code) {
    case PC_EXPIRED:   return "certificate has expired";
    case PC_NOT_YET:   return "certificate is not yet valid";
    case PC_NOT_CA:    return "issuer is not a CA (basicConstraints)";
    case PC_KEY_USAGE: return "issuer may not sign certificates (keyUsage)";
    case PC_PATHLEN:   return "CA path length constraint exceeded";
    case PC_EKU:       return "certificate is not valid for server authentication";
    default:           return "ok";
    }
}

/* Constraint checks for chain[path[0..upto]] ending at `anchor`. */
static int check_path(const mcert_t *ch, const int *path, int upto, const mcert_t *anchor,
                      int64_t now, const mcert_t **bad) {
    for (int j = 0; j <= upto; j++) {
        const mcert_t *c = &ch[path[j]];
        *bad = c;
        if (now) {
            int d = mcert_check_dates(c, now);
            if (d == 1) return PC_NOT_YET;
            if (d == 2) return PC_EXPIRED;
        }
        if (j == 0) {
            if (c->has_eku && !c->eku_server_auth) return PC_EKU;
            continue;
        }
        /* certificates above the leaf act as CAs */
        if (!c->has_bc || !c->is_ca) return PC_NOT_CA;
        if (c->has_ku && !((c->key_usage >> MC_KU_KEY_CERT_SIGN) & 1)) return PC_KEY_USAGE;
        if (c->path_len >= 0 && (j - 1) > c->path_len) return PC_PATHLEN;
    }
    *bad = anchor;
    if (anchor->has_bc && !anchor->is_ca) return PC_NOT_CA;
    if (anchor->path_len >= 0 && upto > anchor->path_len) return PC_PATHLEN;
    return PC_OK;
}

/* Validate the server's chain (ders[0] is the leaf) against the PEM bundle.
 * `ders` point into hs_buf, so this must run while the Certificate message is current. */
static int verify_chain(tls_t *t, const uint8_t *const *ders, const size_t *lens, int n) {
    mcert_t ch[TLS_MAX_CHAIN];
    int path[TLS_MAX_CHAIN], plen = 0, used = 0;
    char label[64];

    for (int i = 0; i < n; i++)
        if (mcert_parse(&ch[i], ders[i], lens[i]) != 0) {
            snprintf(t->errmsg, sizeof(t->errmsg), "certificate #%d in the server chain is malformed", i);
            return -1;
        }
    const int64_t now = clock_now();
    t->dates_checked = now != 0;

    /* 1. Order the chain: follow issuer links from the leaf through the certs the server sent. */
    path[plen++] = 0;
    used = 1;
    for (;;) {
        const mcert_t *cur = &ch[path[plen - 1]];
        int next = -1;
        for (int k = 1; k < n; k++) {
            if (used & (1 << k)) continue;
            if (!mcert_name_equal(cur->issuer, cur->issuer_len, ch[k].subject, ch[k].subject_len)) continue;
            if (mcert_verify_issued_by(cur, &ch[k]) != 0) continue;
            next = k;
            break;
        }
        if (next < 0) break;
        path[plen++] = next;
        used |= 1 << next;
    }

    /* 2. Stream the bundle; the first CA that signed one of path[] and passes the constraints wins. */
    int fd = open(g_ca_path, O_RDONLY, 0);
    if (fd < 0) {
        snprintf(t->errmsg, sizeof(t->errmsg), "cannot open CA bundle '%s' (use -k to skip verification)", g_ca_path);
        return -1;
    }
    pem_t pr;
    memset(&pr, 0, sizeof(pr));
    pr.fd = fd;
    pr.io = t->ca_io;   pr.io_cap = sizeof(t->ca_io);
    pr.der = t->ca_der; pr.der_cap = sizeof(t->ca_der);

    int found = 0, ncerts = 0, path_code = PC_OK;
    char path_who[64] = "";
    for (;;) {
        long dl = pem_next(&pr);
        if (dl <= 0) break;
        mcert_t s;
        if (mcert_parse(&s, t->ca_der, (size_t)dl) != 0) continue;
        ncerts++;
        for (int i = 0; i < plen && !found; i++) {
            const mcert_t *c = &ch[path[i]];
            if (!mcert_name_equal(c->issuer, c->issuer_len, s.subject, s.subject_len)) continue;
            if (mcert_verify_issued_by(c, &s) != 0) continue;
            if (s.has_ku && !((s.key_usage >> MC_KU_KEY_CERT_SIGN) & 1)) continue;
            const mcert_t *bad = NULL;
            int code;
            if (now && mcert_check_dates(&s, now)) { code = mcert_check_dates(&s, now) == 1 ? PC_NOT_YET : PC_EXPIRED; bad = &s; }
            else code = check_path(ch, path, i, &s, now, &bad);
            if (code == PC_OK) { found = 1; break; }
            if (path_code == PC_OK && bad) {
                path_code = code;
                mcert_name_label(bad->subject, bad->subject_len, path_who, sizeof(path_who));
            }
        }
        if (found) break;
    }
    int io_err = pr.err;
    close(fd);

    if (found) { t->chain_verified = 1; return 0; }

    if (io_err)
        snprintf(t->errmsg, sizeof(t->errmsg), "read error on CA bundle '%s'", g_ca_path);
    else if (ncerts == 0)
        snprintf(t->errmsg, sizeof(t->errmsg), "no certificates found in CA bundle '%s'", g_ca_path);
    else if (path_code != PC_OK)
        snprintf(t->errmsg, sizeof(t->errmsg), "%s: %s", path_who, pc_text(path_code));
    else {
        const mcert_t *last = &ch[path[plen - 1]];
        if (mcert_name_equal(last->issuer, last->issuer_len, last->subject, last->subject_len)) {
            mcert_name_label(last->subject, last->subject_len, label, sizeof(label));
            snprintf(t->errmsg, sizeof(t->errmsg), "self-signed certificate '%s' is not in the CA bundle", label);
        } else {
            mcert_name_label(last->issuer, last->issuer_len, label, sizeof(label));
            snprintf(t->errmsg, sizeof(t->errmsg),
                     "unable to verify the server certificate: issuer '%s' is not trusted (or the signature is bad)", label);
        }
    }
    return -1;
}

/* ------------------------------------------------------------ handshake */
static int build_client_hello(tls_t *t, uint8_t *ch, size_t *out_len) {
    size_t hl = strlen(t->host);
    if (hl == 0 || hl > 200) return fail(t, "bad host name length");

    uint8_t *p = ch + 4;                                 /* handshake header filled in last */
    *p++ = 3; *p++ = 3;
    memcpy(p, t->client_random, 32); p += 32;
    *p++ = 0;                                            /* empty session id */
    put16(p, 4); p += 2;
    *p++ = 0xc0; *p++ = 0x2b; *p++ = 0xc0; *p++ = 0x2f;  /* ECDSA / RSA, AES128-GCM */
    *p++ = 1; *p++ = 0;                                  /* null compression */

    uint8_t *ext_len = p; p += 2;
    uint8_t *ext0 = p;

    if (!looks_like_ip(t->host)) {                       /* SNI */
        put16(p, 0x0000); p += 2;
        put16(p, (unsigned)hl + 5); p += 2;
        put16(p, (unsigned)hl + 3); p += 2;
        *p++ = 0;
        put16(p, (unsigned)hl); p += 2;
        memcpy(p, t->host, hl); p += hl;
    }
    /* supported_groups: x25519, secp256r1 (P-256 is needed for ECDSA certs) */
    put16(p, 0x000a); p += 2; put16(p, 6); p += 2; put16(p, 4); p += 2;
    put16(p, GROUP_X25519); p += 2; put16(p, GROUP_SECP256R1); p += 2;
    /* ec_point_formats: uncompressed */
    put16(p, 0x000b); p += 2; put16(p, 2); p += 2; *p++ = 1; *p++ = 0;
    /* signature_algorithms */
    /* only what lib_crypto.c can verify */
    static const uint8_t sigs[] = {
        0x04, 0x03,                                      /* ecdsa_secp256r1_sha256 */
        0x08, 0x04,                                      /* rsa_pss_rsae_sha256 */
        0x04, 0x01,                                      /* rsa_pkcs1_sha256 */
    };
    put16(p, 0x000d); p += 2;
    put16(p, sizeof(sigs) + 2); p += 2;
    put16(p, sizeof(sigs)); p += 2;
    memcpy(p, sigs, sizeof(sigs)); p += sizeof(sigs);
    /* extended_master_secret (RFC 7627) */
    put16(p, 0x0017); p += 2; put16(p, 0); p += 2;

    put16(ext_len, (unsigned)(p - ext0));
    ch[0] = HS_CLIENT_HELLO;
    put24(ch + 1, (unsigned)(p - (ch + 4)));
    *out_len = (size_t)(p - ch);
    return 0;
}

static int do_handshake(tls_t *t, int verify) {
    uint8_t ch[512];
    size_t  ch_len;

    if (random_bytes(t->client_random, 32) != 0) return fail(t, "no random source");
    if (build_client_hello(t, ch, &ch_len) != 0) return -1;
    sha256_init(&t->th);
    sha256_update(&t->th, ch, ch_len);

    uint8_t rec_hdr[5] = { CT_HANDSHAKE, 3, 1 };
    put16(rec_hdr + 3, (unsigned)ch_len);
    memcpy(t->out, rec_hdr, 5);
    memcpy(t->out + 5, ch, ch_len);
    if (sock_sendall(t->fd, t->out, 5 + ch_len) != 0) { note_io_error(t); return fail_io(t, "failed to send ClientHello"); }

    int got_hello = 0, got_cert = 0, got_ske = 0;

    /* ---- ServerHello ... ServerHelloDone ---- */
    for (;;) {
        uint8_t *msg = NULL; size_t mlen = 0;
        if (next_hs_msg(t, &msg, &mlen) != 0) return -1;
        sha256_update(&t->th, msg, mlen);
        uint8_t type = msg[0];
        const uint8_t *body = msg + 4;
        size_t blen = mlen - 4;

        if (type == HS_SERVER_HELLO) {
            if (blen < 35) return fail(t, "ServerHello truncated");
            size_t sid = body[34];
            if (sid > 32 || blen < 35 + sid + 3) return fail(t, "ServerHello malformed");
            if (body[0] != 3 || body[1] != 3) return fail(t, "server did not pick TLS 1.2");
            memcpy(t->server_random, body + 2, 32);
            unsigned cipher = get16(body + 35 + sid);
            if (cipher != 0xc02b && cipher != 0xc02f) return fail(t, "server picked unsupported cipher");
            if (body[35 + sid + 2] != 0) return fail(t, "server picked compression");
            t->cipher_suite = (uint16_t)cipher;

            t->ems = 0;
            size_t eo = 35 + sid + 3;
            if (blen >= eo + 2) {
                size_t el = get16(body + eo);
                eo += 2;
                if (eo + el > blen) return fail(t, "ServerHello extensions malformed");
                size_t eend = eo + el;
                while (eo + 4 <= eend) {
                    unsigned et = get16(body + eo);
                    size_t   en = get16(body + eo + 2);
                    eo += 4;
                    if (eo + en > eend) return fail(t, "ServerHello extension malformed");
                    if (et == 0x0017) t->ems = 1;
                    eo += en;
                }
            }
            got_hello = 1;

        } else if (type == HS_CERTIFICATE) {
            if (blen < 3) return fail(t, "Certificate truncated");
            size_t total = get24(body);
            if (3 + total > blen) return fail(t, "Certificate list malformed");
            size_t pos = 3;
            int n = 0;
            const uint8_t *cp[TLS_MAX_CHAIN];
            size_t cl[TLS_MAX_CHAIN];
            while (pos + 3 <= 3 + total) {
                size_t ln = get24(body + pos);
                pos += 3;
                if (pos + ln > 3 + total) return fail(t, "Certificate entry malformed");
                if (n == 0) {                            /* copy the leaf; the rest is checked in place */
                    if (ln > TLS_LEAF_MAX) return fail(t, "leaf certificate too large");
                    memcpy(t->leaf_der, body + pos, ln);
                    t->leaf_len = ln;
                }
                if (n < TLS_MAX_CHAIN) { cp[n] = body + pos; cl[n] = ln; }
                n++;
                pos += ln;
            }
            got_cert = n > 0;
            t->chain_len = n;
            if (got_cert && (verify & TLS_VERIFY_CHAIN)) {
                if (n > TLS_MAX_CHAIN) return fail(t, "server sent too many certificates");
                if (verify_chain(t, cp, cl, n) != 0) return -1;
            }

        } else if (type == HS_SERVER_KEY_EXCHANGE) {
            if (blen > TLS_SKE_MAX) return fail(t, "ServerKeyExchange too large");
            memcpy(t->ske, body, blen);
            t->ske_len = blen;
            got_ske = 1;

        } else if (type == HS_CERT_REQUEST) {
            return fail(t, "server requested a client certificate (unsupported)");

        } else if (type == HS_SERVER_HELLO_DONE) {
            break;
        }
        /* other message types are hashed and ignored */
    }
    if (!got_hello || !got_cert || !got_ske) return fail(t, "server flight missing messages");
    if ((verify & TLS_VERIFY_CHAIN) && !t->chain_verified) return fail(t, "certificate chain was not verified");

    /* ---- leaf certificate ---- */
    mcert_t leaf;
    if (mcert_parse(&leaf, t->leaf_der, t->leaf_len) != 0) return fail(t, "could not parse server certificate");
    if ((verify & TLS_VERIFY_HOST) && !mcert_match_host(&leaf, t->host)) return fail(t, "certificate is not valid for this host");

    /* ---- ServerKeyExchange: parse + verify signature ---- */
    const uint8_t *ske = t->ske;
    size_t sl = t->ske_len;
    if (sl < 4 || ske[0] != 3) return fail(t, "ServerKeyExchange: not a named_curve");
    unsigned curve_id = get16(ske + 1);
    size_t pub_len = ske[3];
    if (curve_id == GROUP_X25519 && pub_len != 32) return fail(t, "ServerKeyExchange: bad x25519 key");
    else if (curve_id == GROUP_SECP256R1 && pub_len != 65) return fail(t, "ServerKeyExchange: bad P-256 key");
    else if (curve_id != GROUP_X25519 && curve_id != GROUP_SECP256R1) return fail(t, "ServerKeyExchange: unsupported curve");
    size_t pos = 4 + pub_len;
    if (sl < pos + 4) return fail(t, "ServerKeyExchange truncated");
    unsigned scheme = get16(ske + pos);
    size_t sig_len = get16(ske + pos + 2);
    if (sl < pos + 4 + sig_len) return fail(t, "ServerKeyExchange signature truncated");
    const uint8_t *server_pub = ske + 4;
    const uint8_t *sig = ske + pos + 4;

    uint8_t signed_buf[32 + 32 + 4 + 65];
    memcpy(signed_buf, t->client_random, 32);
    memcpy(signed_buf + 32, t->server_random, 32);
    memcpy(signed_buf + 64, ske, 4 + pub_len);
    if (verify_signature(&leaf, (uint16_t)scheme, signed_buf, 64 + 4 + pub_len, sig, sig_len) != 0)
        return fail(t, "ServerKeyExchange signature invalid");

    /* ---- ECDHE ---- */
    uint8_t priv[32], client_pub[65], premaster[32];
    size_t  client_pub_len;
    if (curve_id == GROUP_X25519) {
        if (x25519_keypair(priv, client_pub) != 0) return fail(t, "x25519 keygen failed");
        if (x25519_shared(priv, server_pub, premaster) != 0) return fail(t, "x25519 key exchange failed");
        client_pub_len = 32;
        t->curve = "x25519";
    } else {
        if (p256_keypair(priv, client_pub) != 0) return fail(t, "P-256 keygen failed");
        if (p256_shared(priv, server_pub, premaster) != 0) return fail(t, "P-256 key exchange failed");
        client_pub_len = 65;
        t->curve = "secp256r1";
    }

    uint8_t cke[4 + 1 + 65];
    cke[0] = HS_CLIENT_KEY_EXCHANGE;
    put24(cke + 1, (unsigned)(1 + client_pub_len));
    cke[4] = (uint8_t)client_pub_len;
    memcpy(cke + 5, client_pub, client_pub_len);
    size_t cke_len = 5 + client_pub_len;
    sha256_update(&t->th, cke, cke_len);

    /* ---- key schedule ---- */
    uint8_t seed[64], master[48], kb[40];
    if (t->ems) {
        /* session_hash covers the handshake up to and including ClientKeyExchange. */
        uint8_t sh[32];
        sha256_ctx s0 = t->th;
        sha256_final(&s0, sh);
        prf(premaster, 32, "extended master secret", sh, 32, master, 48);
    } else {
        memcpy(seed, t->client_random, 32); memcpy(seed + 32, t->server_random, 32);
        prf(premaster, 32, "master secret", seed, 64, master, 48);
    }
    memcpy(seed, t->server_random, 32); memcpy(seed + 32, t->client_random, 32);
    prf(master, 48, "key expansion", seed, 64, kb, 40);
    memcpy(t->ckey, kb, 16);      memcpy(t->skey, kb + 16, 16);
    memcpy(t->civ, kb + 32, 4);   memcpy(t->siv, kb + 36, 4);
    t->cseq = t->sseq = 0;

    /* ---- client Finished ---- */
    uint8_t hash[32], fin[16];
    sha256_ctx snap = t->th;                             /* snapshot, keep the running hash */
    sha256_final(&snap, hash);
    fin[0] = HS_FINISHED; put24(fin + 1, 12);
    prf(master, 48, "client finished", hash, 32, fin + 4, 12);
    sha256_update(&t->th, fin, sizeof(fin));

    size_t o = 0;                                        /* CKE + CCS + Finished in one send */
    t->out[o++] = CT_HANDSHAKE; t->out[o++] = 3; t->out[o++] = 3;
    put16(t->out + o, (unsigned)cke_len); o += 2;
    memcpy(t->out + o, cke, cke_len); o += cke_len;
    t->out[o++] = CT_CCS; t->out[o++] = 3; t->out[o++] = 3; t->out[o++] = 0; t->out[o++] = 1; t->out[o++] = 1;
    o += seal_record(t, t->out + o, CT_HANDSHAKE, fin, sizeof(fin));
    if (sock_sendall(t->fd, t->out, o) != 0) { note_io_error(t); return fail_io(t, "failed to send ClientKeyExchange/Finished"); }

    /* ---- server ChangeCipherSpec (possibly preceded by a NewSessionTicket) ---- */
    for (;;) {
        uint8_t type; size_t n;
        int r = read_record(t, &type, &n);
        if (r != 0) { if (!t->errmsg[0]) fail_io(t, "connection lost before server ChangeCipherSpec"); return -1; }
        if (type == CT_CCS) break;
        if (type == CT_HANDSHAKE) { sha256_update(&t->th, t->rec, n); continue; }
        if (type == CT_ALERT) return fail_alert(t, n >= 2 ? t->rec[1] : 0);
        return fail(t, "unexpected record before server ChangeCipherSpec");
    }

    /* ---- server Finished (encrypted) ---- */
    uint8_t type; size_t n;
    int r = read_record(t, &type, &n);
    if (r != 0) { if (!t->errmsg[0]) fail_io(t, "connection lost before server Finished"); return -1; }
    int pl = open_record(t, type, n, t->plain);
    if (pl < 0) return -1;
    if (type == CT_ALERT) return fail_alert(t, pl >= 2 ? t->plain[1] : 0);
    if (type != CT_HANDSHAKE || pl != 16 || t->plain[0] != HS_FINISHED) return fail(t, "expected encrypted server Finished");
    sha256_final(&t->th, hash);                          /* handshake is over; consume the hash */
    uint8_t expected[12];
    prf(master, 48, "server finished", hash, 32, expected, 12);
    if (!ct_equal(t->plain + 4, expected, 12)) return fail(t, "server Finished verify_data mismatch");
    return 0;
}

/* ------------------------------------------------------------ public API */
int tls_connect_to(tls_t *t, const char *addr, const char *host, uint16_t port,
                    int timeout_ms, int verify) {
    memset(t, 0, sizeof(*t));
    t->host = host;                                      /* SNI + certificate name check */
    t->verify = verify;
    t->fd = create_connection(addr, port, timeout_ms);   /* where we actually connect */
    if (t->fd < 0) { note_io_error(t); return fail_io(t, "TCP connect failed"); }
    if (do_handshake(t, verify) != 0) {
        sock_close(t->fd);
        t->fd = -1;
        return -1;
    }
    t->connected = 1;
    return 0;
}

int tls_connect(tls_t *t, const char *host, uint16_t port, int timeout_ms, int verify) {
    return tls_connect_to(t, host, host, port, timeout_ms, verify);
}

int tls_sendall(tls_t *t, const void *data, size_t len) {
    if (!t->connected) return fail(t, "not connected");
    const uint8_t *p = (const uint8_t *)data;
    while (len) {
        size_t chunk = len > 16384 ? 16384 : len;
        size_t total = seal_record(t, t->out, CT_APPDATA, p, chunk);
        if (sock_sendall(t->fd, t->out, total) != 0) { note_io_error(t); return fail_io(t, "send failed"); }
        p += chunk;
        len -= chunk;
    }
    return 0;
}

ssize_t tls_recv(tls_t *t, void *buf, size_t max) {
    if (!t->connected) return fail(t, "not connected");
    for (;;) {
        if (t->plain_pos < t->plain_len) {
            size_t n = t->plain_len - t->plain_pos;
            if (n > max) n = max;
            memcpy(buf, t->plain + t->plain_pos, n);
            t->plain_pos += n;
            return (ssize_t)n;
        }
        if (t->peer_closed) return 0;

        uint8_t type; size_t n;
        int r = read_record(t, &type, &n);
        if (r == -2) { t->peer_closed = 1; return 0; }   /* EOF without close_notify; callers check tls_clean_close() */
        if (r != 0) { if (!t->errmsg[0]) fail_io(t, "receive error"); return -1; }
        int pl = open_record(t, type, n, t->plain);
        if (pl < 0) return -1;
        if (type == CT_APPDATA) { t->plain_pos = 0; t->plain_len = (size_t)pl; continue; }
        t->plain_pos = t->plain_len = 0;
        if (type == CT_ALERT) {
            if (pl >= 2 && t->plain[1] == 0) { t->clean_close = 1; t->peer_closed = 1; return 0; }
            return fail_alert(t, pl >= 2 ? t->plain[1] : 0);
        }
        /* other post-handshake records are ignored */
    }
}

void tls_close(tls_t *t) {
    if (t->fd >= 0) {
        if (t->connected) {
            static const uint8_t close_notify[2] = { 1, 0 };
            size_t total = seal_record(t, t->out, CT_ALERT, close_notify, 2);
            sock_sendall(t->fd, t->out, total);
        }
        sock_close(t->fd);
    }
    t->fd = -1;
    t->connected = 0;
}