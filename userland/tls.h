// tls.h -- minimal TLS 1.2 client for ModuOS (C port of mytls.py)
//
// ECDHE (x25519 / secp256r1) + AES-128-GCM-SHA256, ECDSA or RSA server certs.
//
// Certificate validation (TLS_VERIFY_FULL):
//   * the chain the server sends is checked link by link (signatures with
//     RSA PKCS#1 v1.5 or ECDSA P-256/P-384 over SHA-256/384/512),
//   * it must end at a root/intermediate in the PEM bundle (TLS_CA_PATH),
//   * CA flag, keyUsage, pathLen, serverAuth EKU and validity dates are checked
//     (dates only once a clock was given with tls_set_time()),
//   * the leaf must match the host name,
//   * the ServerKeyExchange signature is verified against the leaf key.
// Not implemented: revocation (CRL/OCSP), name constraints, policy mapping.
#ifndef TLS_H
#define TLS_H

#include "sockets.h"
#include "crypto.h"

#define TLS_REC_MAX   18432   /* 16384 + 2048: max TLS 1.2 record body */
#define TLS_HS_MAX    65536   /* reassembly buffer for handshake messages */
#define TLS_LEAF_MAX  8192
#define TLS_SKE_MAX   1024
#define TLS_MAX_CHAIN 8       /* certificates accepted in the server's chain */
#define TLS_CA_CERT_MAX 4096  /* largest DER certificate read from the CA bundle */

/* Default trust store: a PEM bundle (curl's cacert.pem works as is). */
#ifndef TLS_CA_PATH
#define TLS_CA_PATH "/ModuOS/shared/certs/cacert.pem"
#endif

/* `verify` flags for tls_connect / tls_connect_to */
#define TLS_VERIFY_NONE   0
#define TLS_VERIFY_HOST   1   /* host name must match the leaf certificate */
#define TLS_VERIFY_CHAIN  2   /* chain must lead to a CA in the bundle */
#define TLS_VERIFY_FULL   (TLS_VERIFY_HOST | TLS_VERIFY_CHAIN)

typedef struct {
    int      fd;
    int      connected;
    int      peer_closed;
    int      clean_close;                /* server sent close_notify */
    int      ems;                        /* extended_master_secret negotiated */
    int      verify;                     /* TLS_VERIFY_* flags this connection was opened with */
    int      chain_verified;             /* 1 once the chain was validated against the bundle */
    int      dates_checked;              /* 1 if validity dates were really checked (needs tls_set_time) */
    int      chain_len;                  /* certificates the server sent */
    const char *host;
    char     errmsg[160];
    int      io_errno;                   /* errno + sockets.h message captured at the failing call */
    char     io_msg[64];

    uint8_t  client_random[32], server_random[32];
    uint8_t  ckey[16], skey[16], civ[4], siv[4];
    uint64_t cseq, sseq;
    sha256_ctx th;                       /* running hash of handshake messages */
    uint16_t cipher_suite;
    const char *curve;

    uint8_t  leaf_der[TLS_LEAF_MAX];
    size_t   leaf_len;
    uint8_t  ske[TLS_SKE_MAX];
    size_t   ske_len;

    uint8_t  ca_io[1024];                /* read buffer for the CA bundle */
    uint8_t  ca_der[TLS_CA_CERT_MAX];    /* one decoded CA certificate */

    uint8_t  hs_buf[TLS_HS_MAX];
    size_t   hs_off, hs_len;

    uint8_t  rec[TLS_REC_MAX];          /* last record read from the wire */
    uint8_t  plain[TLS_REC_MAX];        /* decrypted application data */
    size_t   plain_pos, plain_len;
    uint8_t  out[16384 + 64];            /* record being written */
} tls_t;

/* Large struct (~150 KB): declare it static or allocate it, not on the stack. */

/* Where to find trusted CAs (default TLS_CA_PATH) and what time it is, in unix
 * seconds. Without a time (or with a value before 2024) certificate validity
 * dates are NOT checked and tls_dates_checked() stays 0. Call before connecting. */
void    tls_set_ca_path(const char *path);
void    tls_set_time(int64_t unix_seconds);

/* TCP connect + full handshake. 0 on success, -1 on failure (see tls_error).
 * `verify` is a mask of TLS_VERIFY_*; TLS_VERIFY_FULL is what you want. */
int     tls_connect(tls_t *t, const char *host, uint16_t port, int timeout_ms,
                     int verify);
/* Same, but connect to `addr` (an IP string or name) while using `host` for the
 * SNI extension and the certificate name check. Use this when you resolved the
 * name yourself. */
int     tls_connect_to(tls_t *t, const char *addr, const char *host, uint16_t port,
                        int timeout_ms, int verify);
/* Encrypt and send everything. 0 on success, -1 on failure. */
int     tls_sendall(tls_t *t, const void *data, size_t len);
/* >0 bytes of app data, 0 when the server closed, <0 on error/timeout. */
ssize_t tls_recv(tls_t *t, void *buf, size_t max);
void    tls_close(tls_t *t);

const char *tls_error(const tls_t *t);
const char *tls_cipher_name(const tls_t *t);

/* 1 if the certificate chain was validated against the CA bundle. */
int     tls_chain_verified(const tls_t *t);
/* 1 if validity dates were checked as part of that (a clock was available). */
int     tls_dates_checked(const tls_t *t);

/* 1 if the peer ended the stream with close_notify (response not truncated). */
int     tls_clean_close(const tls_t *t);

#endif