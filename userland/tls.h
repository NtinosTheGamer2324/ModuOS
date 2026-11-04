// tls.h -- minimal TLS 1.2 client for ModuOS (C port of mytls.py)
//
// ECDHE (x25519 / secp256r1) + AES-128-GCM-SHA256, ECDSA or RSA server certs.
//
// WARNING: the certificate CHAIN is not validated against a trust store. Only
// the hostname and the ServerKeyExchange signature (against the leaf cert) are
// checked, so this does not protect against an active man-in-the-middle.
#ifndef TLS_H
#define TLS_H

#include "sockets.h"
#include "crypto.h"

#define TLS_REC_MAX   18432   /* 16384 + 2048: max TLS 1.2 record body */
#define TLS_HS_MAX    65536   /* reassembly buffer for handshake messages */
#define TLS_LEAF_MAX  8192
#define TLS_SKE_MAX   1024

typedef struct {
    int      fd;
    int      connected;
    int      peer_closed;
    int      clean_close;                /* server sent close_notify */
    int      ems;                        /* extended_master_secret negotiated */
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

    uint8_t  hs_buf[TLS_HS_MAX];
    size_t   hs_off, hs_len;

    uint8_t  rec[TLS_REC_MAX];          /* last record read from the wire */
    uint8_t  plain[TLS_REC_MAX];        /* decrypted application data */
    size_t   plain_pos, plain_len;
    uint8_t  out[16384 + 64];            /* record being written */
} tls_t;

/* Large struct (~140 KB): declare it static or allocate it, not on the stack. */

/* TCP connect + full handshake. 0 on success, -1 on failure (see tls_error). */
int     tls_connect(tls_t *t, const char *host, uint16_t port, int timeout_ms,
                     int verify_hostname);
/* Same, but connect to `addr` (an IP string or name) while using `host` for the
 * SNI extension and the certificate name check. Use this when you resolved the
 * name yourself. */
int     tls_connect_to(tls_t *t, const char *addr, const char *host, uint16_t port,
                        int timeout_ms, int verify_hostname);
/* Encrypt and send everything. 0 on success, -1 on failure. */
int     tls_sendall(tls_t *t, const void *data, size_t len);
/* >0 bytes of app data, 0 when the server closed, <0 on error/timeout. */
ssize_t tls_recv(tls_t *t, void *buf, size_t max);
void    tls_close(tls_t *t);

const char *tls_error(const tls_t *t);
const char *tls_cipher_name(const tls_t *t);

/* 1 if the peer ended the stream with close_notify (response not truncated). */
int     tls_clean_close(const tls_t *t);

#endif